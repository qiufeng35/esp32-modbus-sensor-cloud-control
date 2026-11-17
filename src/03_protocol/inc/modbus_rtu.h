// ============================================================
//  modbus_rtu.h —— Modbus RTU 主站（协议层）
//
//  这一层只回答一个问题：「怎么跟总线上的某个从站要几个寄存器」。
//  组帧 / 算 CRC / 发送 / 收应答 / 校验 / 解析 —— 全部由 ModbusMaster 库完成；
//  我们只补库不管的三件事：参数把关、帧间静默、错误码翻译。
//  上层（02_business）只认 Modbus_ReadRegs() 与 Modbus_ErrStr()。
// ============================================================
#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <Arduino.h>

// ---------------- 返回码（0 = 成功）----------------
// ★ 0xE0~0xE3 的含义以【库】为准，别照手搓版记忆：
//   0xE0 地址不匹配 / 0xE1 功能码不匹配 / 0xE2 无应答(超时) / 0xE3 CRC 错
//   最容易记反的是 0xE0 和 0xE2：照手搓版记会把"超时"看成"地址不匹配"，排查方向跑偏。
#define MB_OK                 0x00      // 成功
#define MB_ERR_SLAVE          0xE0      // 应答里的从站地址对不上
#define MB_ERR_FUNC           0xE1      // 应答里的功能码对不上
#define MB_ERR_NOT_FOUND      0xE2      // ★ 从站无应答（超时）—— 最常见，先查线/地址/波特率
#define MB_ERR_CRC            0xE3      // CRC 校验错误
#define MB_ERR_PARAM          0xFE      // 本层参数把关没过
#define MB_ERR_NOT_READY      0xFD      // 没初始化就调用
// 从站回了"异常帧"时，库直接把异常码 0x01~0x04 当返回码透传出来

// ---------------- 参数 ----------------
#define MB_FC_READ_HOLDING    0x03      // 读保持寄存器（可读可写）
#define MB_FC_READ_INPUT      0x04      // 读输入寄存器（只读）
#define MB_FC_WRITE_COIL      0x05      // 写单个线圈：值只能是 FF00(吸合) / 0000(释放)
#define MB_FC_READ_MAX_QTY    125       // 协议规定：一次最多读 125 个寄存器

// ---------------- 对外就 5 个函数 ----------------
void        Modbus_Init(void);      // 开机一次：把"串口 + 默认从站"交给库（很轻，不断言什么）

// 核心：读 qty 个寄存器到 out[]，返回 0 成功
//   slave 1~247 ｜ regType 0x03/0x04 ｜ addr 起始地址 ｜ qty 1~125
//   ★ 内部会阻塞等应答，最长 2000 ms（库写死的超时）；读不到的从站就是这么耗时的
uint8_t     Modbus_ReadRegs(uint8_t slave, uint8_t regType, uint16_t addr, uint16_t qty,
                            uint16_t* out);

// 写单个线圈（功能码 0x05）—— 控制继电器、灯这类"开关量"
//   state01 = 1 → 线圈吸合；0 → 释放
//   ★ 应答是"请求原样回显"的 8 个字节，没有数据区；失败与否只看返回码
uint8_t     Modbus_WriteCoil(uint8_t slave, uint16_t coil, uint8_t state01);

const char* Modbus_ErrStr(uint8_t err);           // 返回码 → 中文（串口排查用）
uint32_t    Modbus_FrameGapUs(uint32_t baud);     // 3.5 字符时间（微秒），帧间静默用

#endif
