// ============================================================
//  task_sensor.cpp —— 多传感器轮询（业务层实现）
//
//  文件按顺序分五段，一页能读完：
//    ① 解码 + 换算      按表里的格式把寄存器变成物理量
//    ② 一次事务         全工程唯一的发帧入口（保证总线间隙）
//    ③ 排程             谁到点读谁 / 判离线 / 自动恢复
//    ④ 显示             整张表、单行、排程体检
//    ⑤ 标定             把"标准值 − 实测值"补进 offset
// ============================================================

#include "../inc/task_sensor.h"
#include "../../04_driver/inc/uart.h"
#include "../../03_protocol/inc/modbus_rtu.h"
#include <string.h>

// ---------------- 运行状态 ----------------
// 每个"量"一份（一行配置一个）
typedef struct {
    uint16_t raw;        // 最近读到的原始值（核说明书用）
    float    value;      // 换算后的物理量
    uint8_t  valid;      // 是否成功读到过
    uint8_t  ok;         // 本次读成功了吗
    uint8_t  err;        // 本次错误码
    uint32_t last_ms;    // 上次读取时刻（判周期用）
} SensorState;

// 每个"设备"（从站）一份：在线/离线是【设备】的状态，不是某个量的状态
//   —— 一台变送器坏了，它上面的湿度和温度两个量一起离线。
typedef struct {
    uint8_t  online;      // 1 = 在线
    uint8_t  err_streak;  // 连续失败次数（到 SENSOR_OFFLINE_ERR 判离线）
    uint32_t last_ms;     // 上次跟它说话的时刻（判"该探一次了"）
} SlaveState;

static SensorState s_val[SENSOR_MAX];
static SlaveState  s_slv[SENSOR_MAX + 1];    // 下标直接用从站号（1~16 够用）
static uint8_t     s_count = 0;              // 表里几行
static uint8_t     s_scan  = 0;              // 这一轮从哪一行开始扫（轮转，保证公平）
static uint32_t    s_last_bus_us = 0;        // 上次事务结束时刻（微秒，算间隙）
static uint32_t    s_txn = 0;                // 累计事务数（time 命令显示）
static uint8_t     s_enabled = 1;            // ★ 采集总开关：1 = 读，0 = 暂停（云端可控）

// 取第 i 行的配置（少写一层括号，读起来干净些）
static const SensorDesc* Row(uint8_t i) { return &User_Sensors()[i]; }

// ============================================================
//  ① 解码 + 换算
// ============================================================
// 按表里的格式把寄存器变成"原始值"
static float Decode(const SensorDesc* d, const uint16_t* regs) {
    switch (d->data_type) {
        case DT_U16: return (float)regs[0];                       // 无符号 16 位
        case DT_S16: return (float)(int16_t)regs[0];              // ★ 有符号：0xFF9B 是 −101 不是 65435
        case DT_U32_ABCD:                                         // 32 位无符号，高字在前
            return (float)(((uint32_t)regs[0] << 16) | regs[1]);
        case DT_F32_ABCD: {                                       // 32 位浮点，高字在前
            uint32_t v = ((uint32_t)regs[0] << 16) | regs[1];
            float f;  memcpy(&f, &v, 4);
            return f;
        }
        default: return 0.0f;
    }
}

// 物理量 = 原始值 × scale + offset（offset 由 cal 标定写入）
static float Apply(const SensorDesc* d, float raw) { return raw * d->scale + d->offset; }

// 这一行多久读一次（表里填 0 就用默认周期）
static uint32_t Period(uint8_t i) {
    uint32_t p = Row(i)->period_ms;
    return p ? p : SENSOR_DEFAULT_PERIOD;
}

// 相邻两行能不能合并成一帧读（同一台设备 + 地址紧挨着 + 都是 16 位）
//   ★ 这就是把"5 次事务"压成"4 次"的地方：少一次事务就少一个 200 ms 间隙
static uint8_t CanPair(uint8_t i) {
    if ((uint8_t)(i + 1) >= s_count) return 0;
    const SensorDesc* a = Row(i);
    const SensorDesc* b = Row((uint8_t)(i + 1));
    return User_RegCount(a->data_type) == 1 && User_RegCount(b->data_type) == 1 &&
           a->slave == b->slave && a->reg_type == b->reg_type &&
           b->reg_addr == (uint16_t)(a->reg_addr + 1);
}

// 把一次成功的结果存下来（解码和换算都走 Apply，只有这一个出口）
static void Save(uint8_t i, const uint16_t* regs) {
    const SensorDesc* d = Row(i);
    s_val[i].raw      = regs[0];
    s_val[i].value    = Apply(d, Decode(d, regs));
    s_val[i].valid    = 1;
    s_val[i].last_ms  = millis();
}

// ============================================================
//  ② 一次事务：全工程唯一的发帧入口
//     ★ 任何要跟从站说话的地方都走它 —— 只有它能保证总线间隙。
//       绕过它就等于和自动轮读"背靠背"发帧，从站会漏答。
// ============================================================
static uint8_t BusRead(uint8_t slave, uint8_t reg_type, uint16_t addr, uint16_t qty,
                       uint16_t* out) {
    uint32_t gap  = SENSOR_BUS_GAP_MS * 1000UL;
    uint32_t used = (uint32_t)(micros() - s_last_bus_us);
    if (used < gap) delayMicroseconds(gap - used);      // 不够间隙就先等一会儿

    s_txn++;
    uint8_t err = Modbus_ReadRegs(slave, reg_type, addr, qty, out);
    s_last_bus_us = micros();                            // 间隙从这一刻重新算
    return err;
}

// ---- 写一个线圈（继电器）：★ 同样守 200 ms 间隙 ----
//  ★ 这一步不能省：协议层只等 3.5 个字符（≈3.6 ms），若按钮恰好按在传感器刚读完的瞬间，
//    两帧会背靠背，从站会漏答 —— 表现就是"偶尔按了没反应"，而且极难复现。
uint8_t Sensor_BusWriteCoil(uint8_t slave, uint16_t coil, uint8_t on) {
    uint32_t gap  = SENSOR_BUS_GAP_MS * 1000UL;
    uint32_t used = (uint32_t)(micros() - s_last_bus_us);
    if (used < gap) delayMicroseconds(gap - used);

    s_txn++;
    uint8_t err = Modbus_WriteCoil(slave, coil, on);
    s_last_bus_us = micros();
    return err;
}

// ---- 记账：一次事务的结果写进"量"和"设备"两级状态 ----
// 返回 1 = 这台设备的上线/离线状态变了（需要打印一条日志）
static uint8_t Account(uint8_t i, uint8_t err) {
    SensorState* st = &s_val[i];
    SlaveState*  sl = &s_slv[Row(i)->slave];

    st->ok  = (err == MB_OK);
    st->err = err;
    sl->last_ms = millis();

    if (err == MB_OK) {
        sl->err_streak = 0;
        if (!sl->online) { sl->online = 1; return 1; }    // 原来是离线的 → 恢复了
        return 0;
    }
    if (sl->err_streak < 255) sl->err_streak++;
    if (sl->err_streak >= SENSOR_OFFLINE_ERR && sl->online) { sl->online = 0; return 1; }
    return 0;
}

// ---- 读一行（能跟下一行合并就一帧读两个量）----
static void ReadRow(uint8_t i) {
    const SensorDesc* a = Row(i);
    uint8_t  pair = CanPair(i);
    uint8_t  need = pair ? 2 : User_RegCount(a->data_type);
    uint16_t reg[2] = {0, 0};

    uint8_t err     = BusRead(a->slave, a->reg_type, a->reg_addr, need, reg);
    uint8_t changed = Account(i, err);
    if (pair) Account((uint8_t)(i + 1), err);          // 同一台设备，结果一样

    if (err != MB_OK) {
        if (changed)
            UART0.printf("[离线] 从站 %u 连续 %u 次无应答（%s），先查线／地址／波特率\n",
                         a->slave, SENSOR_OFFLINE_ERR, Modbus_ErrStr(err));
        return;                                        // ★ 读不到就保持旧值，不瞎算
    }

    Save(i, reg);                                      // 第 1 个寄存器 → 第 i 行
    if (pair) Save((uint8_t)(i + 1), &reg[1]);         // 第 2 个寄存器 → 第 i+1 行

    if (changed) UART0.printf("[恢复] 从站 %u（%s）重新在线\n", a->slave, a->name);
}

// ---- 这一行到点该读了吗 ----
static uint8_t IsDue(uint8_t i) {
    SlaveState* sl = &s_slv[Row(i)->slave];
    // 离线：不按表里的周期，而是每 SENSOR_PROBE_MS 探一次（探到就自动恢复）
    if (!sl->online) return (uint32_t)(millis() - sl->last_ms) >= SENSOR_PROBE_MS;
    return (uint32_t)(millis() - s_val[i].last_ms) >= Period(i);
}

// ============================================================
//  ③ 排程主循环
//  ------------------------------------------------------------
//  每次 loop 最多做【一次】Modbus 事务 —— 绝不能一圈读完所有传感器：
//  离线设备每次要等满 2000 ms，读一圈就会把命令台和 MQTT 全卡住。
//  顺序用"轮转起点"保证公平：这轮从第 0 行起扫，下轮从第 1 行起扫……
// ============================================================
void Sensor_Init(void) {
    s_count = User_SensorCount();
    if (s_count > SENSOR_MAX) s_count = SENSOR_MAX;

    memset(s_val, 0, sizeof(s_val));
    memset(s_slv, 0, sizeof(s_slv));
    for (uint8_t i = 1; i <= SENSOR_MAX; i++) s_slv[i].online = 1;   // 先当在线，读失败再判

    s_scan = 0;
    s_txn  = 0;
    // 开机第一次事务不用等间隙：把"上次事务时刻"往前提一个间隙就够了
    s_last_bus_us = micros() - SENSOR_BUS_GAP_MS * 1000UL;

    Sensor_PrintTable();            // 开机先亮一次整张表
}

void Sensor_Loop(void) {
    if (s_count == 0) return;
    if (!s_enabled) return;                   // ★ 采集被云端关掉：不碰总线（其他任务不受影响）

    // ① 总线间隙不够就先返回（★ 不 delay，别的任务照常跑）
    if ((uint32_t)(micros() - s_last_bus_us) < SENSOR_BUS_GAP_MS * 1000UL) return;

    // ② 从轮转起点扫一圈，找第一个"到点该读"的
    uint8_t hit = 0xFF;
    for (uint8_t n = 0; n < s_count; n++) {
        uint8_t i = (uint8_t)((s_scan + n) % s_count);
        if (IsDue(i)) { hit = i; break; }
    }
    if (hit == 0xFF) return;                  // 这一圈暂时没有到点的

    // ③ 做一次事务，下次从它的下一行开始扫
    ReadRow(hit);
    s_scan = (uint8_t)((hit + 1) % s_count);
}

// ============================================================
//  ④ 取数据（命令台与上云层只认这几个）
// ============================================================
uint8_t Sensor_Read(uint8_t idx) {            // 立刻读一行（cal 标定前会先调它）
    if (idx >= s_count) return MB_ERR_PARAM;
    ReadRow(idx);
    return s_val[idx].ok ? MB_OK : s_val[idx].err;
}

const SensorDesc* Sensor_Desc(uint8_t idx) { return (idx >= s_count) ? NULL : Row(idx); }
uint8_t           Sensor_Count(void)       { return s_count; }

uint8_t Sensor_Value(uint8_t idx, float* out) {
    if (idx >= s_count || out == NULL || !s_val[idx].valid) return 0;
    *out = s_val[idx].value;
    return 1;
}

uint8_t Sensor_IsOnline(uint8_t idx) {
    return (idx >= s_count) ? 0 : s_slv[Row(idx)->slave].online;
}

// 有几个量在线（汇总行与 time 命令都要用）
static uint8_t OnlineCount(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < s_count; i++) if (s_slv[Row(i)->slave].online) n++;
    return n;
}

// ============================================================
//  ⑤ 显示（sensor / sensor <行号> / time 三条命令的屏幕）
// ============================================================
static void Print_Head(void) {
    UART0.println(" #  名称     从站 功能码 寄存器  格式  系数   偏置   原始值     换算值      状态");
    UART0.println("--- -------- ---- ------ ------- ----- ------ ------ --------- --------- --------");
}

// 打一行：配置 + 本次原始值 + 换算值 + 状态。★ 原始值和状态是排查的唯一依据
static void Print_Row(uint8_t i) {
    const SensorDesc* d  = Row(i);
    SensorState*      st = &s_val[i];

    UART0.printf(" %u  %-8s %u   0x0%X   0x%04X   %-4s  %5.2f  %5.2f   0x%04X(%-6d)",
                 i + 1, d->name, d->slave, d->reg_type, d->reg_addr,
                 User_DataTypeName(d->data_type), d->scale, d->offset,
                 (unsigned)st->raw, (int)(int16_t)st->raw);

    if (st->valid) UART0.printf(" %9.2f %-4s", st->value, d->unit);   // 读成功过才有换算值
    else           UART0.print("     --         ");

    if (!st->ok)                                         // 本次失败：打印错因（0xE2 = 无应答）
        UART0.printf("  %s\n", Modbus_ErrStr(st->err));
    else if (!s_slv[d->slave].online) UART0.println("  离线");
    else                              UART0.println("  在线");
}

void Sensor_PrintTable(void) {
    s_count = User_SensorCount();
    if (s_count > SENSOR_MAX) s_count = SENSOR_MAX;

    UART0.printf("\n======== 传感器配置表（共 %u 行；接传感器只改 User_Config.cpp 这张表）========\n",
                 s_count);
    Print_Head();
    for (uint8_t i = 0; i < s_count; i++) Print_Row(i);
    UART0.println("-------------------------------------------------------------------------------");

    uint8_t bad = User_ConfigCheck();
    if (bad == 0) UART0.println("  开机自检：表格全部合法");
    else          UART0.printf("  开机自检：第 %u 行有问题，请对照说明书检查\n", bad);

    UART0.printf("  排程参数：间隙 %lu ms ｜ 失败 %u 次判离线 ｜ 离线后每 %lu ms 探一次\n",
                 (unsigned long)SENSOR_BUS_GAP_MS, SENSOR_OFFLINE_ERR,
                 (unsigned long)SENSOR_PROBE_MS);
    UART0.println("  提示：sensor 看全表 ｜ sensor <行号> 看某一行 ｜ cal <行号> <标准值> 标定");
}

void Sensor_PrintOne(uint8_t idx) {
    if (idx == 0 || idx > s_count) {
        UART0.println("用法：sensor <行号>，行号从 1 开始（先敲 sensor 看全表）");
        return;
    }
    const SensorDesc* d = Row((uint8_t)(idx - 1));

    UART0.println();
    Print_Head();
    Print_Row((uint8_t)(idx - 1));
    UART0.printf("  说明书核对：功能码 0x0%X ｜ 寄存器 0x%04X ｜ 格式 %s ｜ 换算 = 原始值 × %.2f + %.2f\n",
                 d->reg_type, d->reg_addr, User_DataTypeName(d->data_type), d->scale, d->offset);
    UART0.printf("  周期 %lu ms ｜ 上云主题 %s\n",
                 (unsigned long)Period((uint8_t)(idx - 1)), d->topic);
}

// ---- time：排程体检（一轮几次事务、预期多久、现在在线几个量）----
void Sensor_PrintTiming(void) {
    uint8_t txn = 0;
    for (uint8_t i = 0; i < s_count; ) { txn++; i = (uint8_t)(i + (CanPair(i) ? 2 : 1)); }

    uint8_t on = OnlineCount();
    UART0.println("\n======== 排程体检 ========");
    UART0.printf("  表内 %u 个量 ｜ 合并后一轮 %u 次事务（同从站相邻寄存器合并成一帧）\n",
                 s_count, txn);
    UART0.printf("  一轮预期：正常约 %lu ms ｜ 全掉线最坏约 %lu ms（每次等满库的 2000 ms）\n",
                 (unsigned long)(txn * 50UL + (txn - 1) * SENSOR_BUS_GAP_MS),
                 (unsigned long)(txn * 2000UL + (txn - 1) * SENSOR_BUS_GAP_MS));
    UART0.printf("  实测：累计事务 %lu 次 ｜ 在线 %u 个量 ｜ 离线 %u 个量\n",
                 (unsigned long)s_txn, on, (uint8_t)(s_count - on));
    UART0.println("==========================\n");
}

// ============================================================
//  ⑥ 标定：把"标准值 − 实测值"补进 offset（★ 掉电回到表里的值，要抄回去）
// ============================================================
void Sensor_Calibrate(uint8_t idx, float std_val) {
    if (idx == 0 || idx > s_count) return;
    SensorDesc*  d  = (SensorDesc*)Row((uint8_t)(idx - 1));
    SensorState* st = &s_val[idx - 1];

    Sensor_Read((uint8_t)(idx - 1));        // 先现场读一次：读不到就没有"实测值"可标
    if (!st->ok) {
        UART0.println("标定失败：这一路现在读不到值，先解决通信（看状态列的错因）");
        return;
    }

    float before = st->value;
    d->offset += std_val - before;          // 偏差补进偏置
    st->value  = Apply(d, Decode(d, &st->raw));

    UART0.printf("[标定] %s：标准 %.2f ｜ 修正前 %.2f ｜ 修正后 %.2f %s ｜ 现 offset = %.2f\n",
                 d->name, std_val, before, st->value, d->unit, d->offset);
    UART0.println("       ★ 把这个 offset 抄回 User_Config.cpp 对应行，掉电才不会丢");
}

void Sensor_SetZero(uint8_t idx) {
    if (idx == 0 || idx > s_count) return;
    SensorDesc* d = (SensorDesc*)Row((uint8_t)(idx - 1));
    d->offset = 0.0f;
    UART0.printf("[标定] %s 的 offset 已清 0（回到说明书上的换算）\n", d->name);
}

// ============================================================
//  ⑦ 采集总开关（巴法云"传感器开关"卡片用）
//  ------------------------------------------------------------
//  关掉：Sensor_Loop 直接返回 → 不读总线、不上报；云端各主题保留最后一次的值。
//  打开：立刻恢复轮询，旧值马上会被新值覆盖。
//  ★ 为什么做成"总开关"而不是"每路一个"：巴法云每个开关卡片对应一个主题，
//    7 路就 7 个主题，免费账号很可能不够；要单独控某一路，见 README 的扩展说明。
// ============================================================
void Sensor_SetAllEnabled(uint8_t on) { s_enabled = on ? 1 : 0; }
uint8_t Sensor_IsEnabled(void)        { return s_enabled; }


