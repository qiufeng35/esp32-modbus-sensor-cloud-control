// ============================================================
//  modbus_rtu.cpp —— Modbus RTU 主站（协议层实现）
//
//  整个文件只做三件事，读完就懂这一层为什么这么薄：
//    ① 参数把关：库不检查参数，错的参数进去会读出垃圾值
//    ② 帧间静默：协议要求两帧之间安静 3.5 个字符（9600 下约 3.6 ms），★ 库不管
//    ③ 错误码翻译：把库的返回码变成人话（0xE2 = 从站无应答，最常碰到）
// ============================================================

#include "../inc/modbus_rtu.h"
#include "../../04_driver/inc/uart.h"      // UART1：挂在 RS485 上的那个串口
#include "../../04_driver/inc/rs485.h"     // RS485_BAUD：算 3.5 字符时间要用
#include <ModbusMaster.h>                  // ★ 主角：组帧/CRC/收发/校验全在库里

static ModbusMaster s_node;          // 库对象：自己记着"当前从站地址 + 用哪个串口"
static bool         s_ready  = false;
static uint32_t     s_last_us = 0;   // 上一次事务结束的时刻（算帧间隙的起点）

// ------------------------------------------------------------
//  初始化：begin() 只是把"从站地址 + 串口"记下来，非常轻量，
//  所以"换从站"就是每帧前再调一次 begin()（见下面 ReadRegs 的第二段）。
// ------------------------------------------------------------
void Modbus_Init(void) {
    s_node.begin(1, UART1);          // 先给个默认从站 1，真正的地址每帧前按表设置
    s_ready = true;
}

// ------------------------------------------------------------
//  核心：一问一答读寄存器
// ------------------------------------------------------------
uint8_t Modbus_ReadRegs(uint8_t slave, uint8_t regType, uint16_t addr, uint16_t qty,
                        uint16_t* out) {
    // ---- ① 参数把关（slave=0、数量超限、格式不支持都会读出错误数据）----
    if (!s_ready)                                                     return MB_ERR_NOT_READY;
    if (out == NULL || qty == 0 || slave == 0 || qty > MB_FC_READ_MAX_QTY) return MB_ERR_PARAM;
    if (regType != MB_FC_READ_HOLDING && regType != MB_FC_READ_INPUT)  return MB_ERR_PARAM;

    // ---- ② 换从站：begin 只做赋值，开销极小 ----
    s_node.begin(slave, UART1);

    // ---- ③ 帧间静默：安静够 3.5 个字符再发，否则从站会把两帧接成一帧 ----
    uint32_t gap  = Modbus_FrameGapUs(RS485_BAUD);
    uint32_t used = (uint32_t)(micros() - s_last_us);
    if (used < gap) delayMicroseconds(gap - used);

    // ---- ④ 交给库：组帧 + CRC + 发送 + 等应答 + 验 CRC + 拼成 16 位 ----
    //      ★ 这一行会阻塞：最长 2000 ms（库内置超时），离线设备就是在这里耗时间
    uint8_t rc = (regType == MB_FC_READ_HOLDING)
               ? s_node.readHoldingRegisters(addr, qty)           // 功能码 0x03
               : s_node.readInputRegisters(addr, (uint8_t)qty);   // 功能码 0x04
    s_last_us = micros();        // 记下结束时刻：下一次事务的间隙从这里算起

    // ---- ⑤ 失败：把库的返回码原样上抛（上层用 Modbus_ErrStr 翻成中文）----
    if (rc != ModbusMaster::ku8MBSuccess) return rc;

    // ---- ⑥ 成功：取回每个寄存器的值。★ 下标是"第几个寄存器"，不是"第几个字节" ----
    for (uint16_t i = 0; i < qty; i++) out[i] = s_node.getResponseBuffer(i);
    return MB_OK;
}

// ------------------------------------------------------------
//  写单个线圈（功能码 0x05）：控制继电器/灯这类开关量
//  和读寄存器只差两处：① 请求里不是"字节数"，而是 2 字节的值（FF00 / 0000）
//                     ② 应答是请求原样回显，没有数据区
// ------------------------------------------------------------
uint8_t Modbus_WriteCoil(uint8_t slave, uint16_t coil, uint8_t state01) {
    if (!s_ready)   return MB_ERR_NOT_READY;
    if (slave == 0) return MB_ERR_PARAM;              // 地址 0 是广播，本工程不用

    s_node.begin(slave, UART1);                       // 换从站（只赋值，开销极小）

    // 帧间静默 3.5 个字符 —— 与读那一套完全一样
    uint32_t gap  = Modbus_FrameGapUs(RS485_BAUD);
    uint32_t used = (uint32_t)(micros() - s_last_us);
    if (used < gap) delayMicroseconds(gap - used);

    // state01 = 1 → 库发 FF00（吸合）；0 → 发 0000（释放）
    uint8_t rc = s_node.writeSingleCoil(coil, state01 ? 1 : 0);
    s_last_us = micros();

    return (rc == ModbusMaster::ku8MBSuccess) ? MB_OK : rc;   // 失败就把库的返回码上抛
}

// ------------------------------------------------------------
//  错误码 → 中文。排查时看的就是这句话，所以写得直白一点。
// ------------------------------------------------------------
const char* Modbus_ErrStr(uint8_t err) {
    switch (err) {
        case MB_OK:            return "成功";
        // 从站回的 Modbus 标准异常码
        case 0x01:             return "从站异常：非法功能码";
        case 0x02:             return "从站异常：非法数据地址";
        case 0x03:             return "从站异常：非法数据值";
        case 0x04:             return "从站异常：从站设备故障";
        // 库内部错误
        case MB_ERR_SLAVE:     return "响应异常：从站地址不匹配";
        case MB_ERR_FUNC:      return "响应异常：功能码不匹配";
        case MB_ERR_NOT_FOUND: return "从站无应答（超时）";     // ★ 接线没接好/地址不对/没供电
        case MB_ERR_CRC:       return "CRC 校验错误";           // 干扰、A-B 反、波特率不一致
        default:               return "参数非法或未初始化";
    }
}

// ------------------------------------------------------------
//  3.5 字符时间：1 个字符 = 起始位 + 8 数据位 + 停止位 = 10 位
//  9600 bps → 35 × 10⁶ ÷ 9600 ≈ 3646 µs
// ------------------------------------------------------------
uint32_t Modbus_FrameGapUs(uint32_t baud) {
    if (baud == 0) return 0;
    return (uint32_t)((35UL * 1000000UL) / baud);
}
