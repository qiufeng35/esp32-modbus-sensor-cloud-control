// ============================================================
//  User_Config.h —— 用户配置（传感器表 + 运行参数）
//
//    ① 传感器表      → User_Config.cpp 里那张表（接传感器就改那里）
//    ② 继电器/开关   → 本文件的几个宏（云端控制用）
//    ③ WiFi 与私钥   → ★ 不在这里！见项目根目录 secrets.example.ini
//                       （构建期 AES-128 加密进固件，仓库里没有明文）
//  改完重新上传即可，其他代码一行都不用动。
// ============================================================
#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#include <Arduino.h>

// ---------------- ★ WiFi 凭据不在源码里 ----------------
//  复制 secrets.example.ini 为 secrets.ini，填好 WiFi 与巴法云私钥后编译；
//  tools/gen_secrets.py 会把它加密成 src/05_secrets/gen/secret_blob.h。
//  详见 README「密钥加密」。

#define USER_CONSOLE_BAUD    115200          // 调试串口波特率（串口监视器要设成一样）

// ---------------- 继电器（云端控制用）----------------
//  ★ 末端三位 006 = 开关设备，手机 APP 才会出现"开关按钮"；004 只有数值、点不了。
//  ★ 继电器和传感器在同一条 485 总线上，地址别跟传感器重复（本工程传感器用 1~5）。
#define USER_RELAY_SLAVE     7               // 继电器的 Modbus 从站地址
#define USER_RELAY_COIL      1               // 线圈编号（总线上是 0x0001）
#define USER_RELAY_INVERT    0               // 0 = 正相（on 就亮）；若 on 反而灭，改成 1
#define USER_RELAY_TOPIC     "switch0006"    // ★ 必须和控制台里建的主题逐字一致

// ---------------- 传感器采集总开关（云端控制用）----------------
//  在巴法云控制台**再建一个 006 主题**（如 sensor0006），APP 会多出一张开关键卡片；
//  发 on  = 恢复采集（7 路照常读、照常上报）
//  发 off = 暂停采集（不再读总线，云端各主题保留最后一次的值）
#define USER_SENSOR_TOPIC    "sensor0006"    // ★ 开关卡片主题；不想用就建好后不管它也行

// ---------------- 数据格式（照说明书选，别猜）----------------
typedef enum {
    DT_U16 = 0,          // 无符号 16 位（1 个寄存器）：湿度、风速、光照、风向
    DT_S16,              // 有符号 16 位（1 个寄存器）：温度（可能是负的）
    DT_U32_ABCD,         // 无符号 32 位，高字在前（2 个寄存器）
    DT_F32_ABCD,         // 单精度浮点，高字在前（2 个寄存器）
    DT_MAX
} DataType;

// ---------------- 一行配置：表里一行 = 一个量 ----------------
typedef struct {
    const char* name;        // 显示名称
    uint8_t     slave;       // 从站地址 1~247
    uint8_t     reg_type;    // 功能码 0x03 / 0x04
    uint16_t    reg_addr;    // 起始寄存器地址
    DataType    data_type;   // 数据格式
    float       scale;       // 物理量 = 原始值 × scale + offset
    float       offset;      // 零点修正（cal 标定后填）
    const char* unit;        // 单位
    const char* topic;       // ★ 上云主题：只用字母数字（巴法云不许下划线），末尾 004 = 传感器
    uint32_t    period_ms;   // 读取周期（填 0 = 用默认 5000 ms）
} SensorDesc;

// ---------------- 对外接口 ----------------
const SensorDesc* User_Sensors(void);      // 表首地址（业务层从这里读配置）
uint8_t           User_SensorCount(void);  // 表里几行（编译器自己数，加行不用改）
uint8_t           User_ConfigCheck(void);  // 开机自检：0 = 合法；否则第几行有问题（从 1 起）

const char*       User_DataTypeName(DataType t);  // 格式名（打印用）
uint8_t           User_RegCount(DataType t);      // 这个格式占几个寄存器

uint32_t          User_SetPeriod(uint8_t idx, uint32_t ms);   // period 命令：改周期（掉电丢失）

const char*       User_WifiSsid(void);
const char*       User_WifiPassword(void);
uint8_t           User_WifiCheck(void);    // 0 = 已填；1 名字空；2 名字太长；3 密码空

#endif
