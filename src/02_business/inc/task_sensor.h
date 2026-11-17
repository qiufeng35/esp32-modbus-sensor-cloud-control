// ============================================================
//  task_sensor.h —— 多传感器轮询（业务层）
//
//  这一层只做四件事，全部围绕配置表：① 按表读 ② 按表算 ③ 按表排 ④ 按表给。
//  ★ 它属于"业务"，不属于"协议"：协议层只管把一帧读回来，怎么用表、多久读一次，
//    判离线怎么判，都是这一层的事。上层（命令台/上云）只跟下面这些函数打交道。
// ============================================================
#ifndef TASK_SENSOR_H
#define TASK_SENSOR_H

#include <Arduino.h>
#include "../../01_user/inc/User_Config.h"

// ---------------- 参数（要改就改这几行）----------------
#define SENSOR_MAX             16       // 表最多多少行
#define SENSOR_DEFAULT_PERIOD  5000UL   // 表里 period_ms 填 0 时用这个周期
#define SENSOR_BUS_GAP_MS      200UL    // ★ 两次事务之间至少隔 200 ms（给从站反应时间）
#define SENSOR_OFFLINE_ERR     3        // ★ 连续失败几次判离线
#define SENSOR_PROBE_MS        2000UL   // ★ 已离线设备多久再探一次（探到就恢复）

// ---------------- 生命周期 ----------------
void Sensor_Init(void);          // 开机一次：载入表、清状态、打印整张表
void Sensor_Loop(void);          // 每次 loop：推进轮询（★ 不阻塞，一次最多一个事务）

// ---------------- 取数据（命令台和上云都用这几个）----------------
const SensorDesc* Sensor_Desc(uint8_t idx);      // 第 idx 行的配置（越界返回 NULL）
uint8_t Sensor_Count(void);                      // 表里几行
uint8_t Sensor_Value(uint8_t idx, float* out);   // 最新物理量；返回 1 表示有效
uint8_t Sensor_IsOnline(uint8_t idx);            // 1 = 在线，0 = 已判离线

// ---------------- 命令用 ----------------
uint8_t Sensor_Read(uint8_t idx);                 // 立刻读一行（从 0 起），返回 0 成功

// ★ 写线圈（继电器这类开关量）：和读共用同一个"总线间隙"守卫
//   ★ 为什么要放这一层：协议层只懂"发一帧"，事务之间要隔 200 ms 是业务规矩
uint8_t Sensor_BusWriteCoil(uint8_t slave, uint16_t coil, uint8_t on);   // 返回 0 成功

// ---------------- ★ 采集总开关（云端"传感器开关"用）----------------
//  关掉后：不读总线、不上报；云端各主题保留最后一次的值。打开后立刻恢复轮询。
void    Sensor_SetAllEnabled(uint8_t on);   // 1 = 采集，0 = 暂停
uint8_t Sensor_IsEnabled(void);             // 现在开着吗

void    Sensor_Calibrate(uint8_t idx, float std_val);  // 标定：offset += 标准值 − 实测值
void    Sensor_SetZero(uint8_t idx);              // 把该行 offset 清 0
void    Sensor_PrintTable(void);                  // sensor：整张表
void    Sensor_PrintOne(uint8_t idx);             // sensor <行号>：只看一行
void    Sensor_PrintTiming(void);                 // time：排程体检

#endif
