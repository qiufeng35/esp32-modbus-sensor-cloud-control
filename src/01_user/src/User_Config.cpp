// ============================================================
//  User_Config.cpp —— 用户配置（实现）
//
//  ★ 接传感器只改这里：s_table 加一行就行，行数由编译器自己数。
//  ★ 填表依据是说明书，不靠试：
//      · 出厂默认：地址 1、9600、N.8.1；温湿度变送器 0x0000 = 湿度、0x0001 = 温度
//      · "16bits 数据高字节在前" → U16；温度可能为负 → S16；示例 658 → 65.8 → scale 0.1
//  ★ 上云主题（巴法云）只允许字母和数字，末尾三位是设备类型码（004 = 传感器）。
//    同一台设备的两个量用「名字 + 序号」区分：humi50004 / temp50004（★ 不能带下划线）。
// ============================================================

#include "../inc/User_Config.h"
#include "../../05_secrets/inc/secret_store.h"
#include <string.h>

// ---------------- WiFi 配置的取值（来自加密存储）----------------
const char* User_WifiSsid(void)     { return Secret_WifiSsid(); }
const char* User_WifiPassword(void) { return Secret_WifiPassword(); }

// 返回 0 = 已填；1 = 名字空；2 = 名字太长；3 = 密码空
uint8_t User_WifiCheck(void) {
    const char* ssid = Secret_WifiSsid();
    const char* pass = Secret_WifiPassword();
    if (ssid[0] == '\0')     return 1;
    if (strlen(ssid) > 32)   return 2;               // SSID 上限 32 字节
    if (pass[0] == '\0')     return 3;
    return 0;
}

// ============================================================
//  ★★ 传感器配置表：接一个新设备就加一行 ★★
//   顺序不要随便动：相邻两行如果"同从站 + 地址挨着 + 都是 16 位"，
//   程序会自动合并成一帧读（省一次事务、省一个 200 ms 间隙）。
// ============================================================
static SensorDesc s_table[] = {
    // name    slave 功能码 寄存器   格式       系数   偏置   单位   上云主题      周期ms
    { "风速",   1,  0x03,  0x0000,  DT_U16,    0.1f,  0.0f,  "m/s", "reg0004",   1000 },
    { "风向",   2,  0x03,  0x0001,  DT_U16,    1.0f,  0.0f,  "deg", "dir0004",   2000 },
    { "湿度",   3,  0x03,  0x0000,  DT_U16,    0.1f,  0.0f,  "%RH", "humi0004",  2000 },
    { "温度",   3,  0x03,  0x0001,  DT_S16,    0.1f,  0.0f,  "℃",  "temp0004",  2000 },
    { "光照",   4,  0x03,  0x0008,  DT_U16,    1.0f,  0.0f,  "lux", "lux0004",   1000 },
    // ★ 下面两行是探头式温湿度变送器（从站 5）：湿度 0x0000、温度 0x0001
    { "湿度5",  5,  0x03,  0x0000,  DT_U16,    0.1f,  0.0f,  "%RH", "humi50004", 2000 },
    { "温度5",  5,  0x03,  0x0001,  DT_S16,    0.1f,  0.0f,  "℃",  "temp50004", 2000 },
};

#define SENSOR_ROWS  (sizeof(s_table) / sizeof(s_table[0]))   // 行数自动跟着表走

// ---------------- 对外接口 ----------------
const SensorDesc* User_Sensors(void)     { return s_table; }
uint8_t           User_SensorCount(void) { return (uint8_t)SENSOR_ROWS; }

const char* User_DataTypeName(DataType t) {
    switch (t) {
        case DT_U16:      return "U16";
        case DT_S16:      return "S16";
        case DT_U32_ABCD: return "U32";
        case DT_F32_ABCD: return "F32";
        default:          return "?";
    }
}

// 这个格式占几个寄存器：16 位占 1 个，32 位占 2 个
uint8_t User_RegCount(DataType t) {
    return (t == DT_U32_ABCD || t == DT_F32_ABCD) ? 2 : 1;
}

// period 命令用：改运行时的周期（掉电回到表里的值）
uint32_t User_SetPeriod(uint8_t idx, uint32_t ms) {
    if (idx >= SENSOR_ROWS) return 0;
    s_table[idx].period_ms = ms;
    return ms;
}

// ============================================================
//  开机自检：只看"填得合不合法"，不看"测得准不准"（那是 cal 的事）
//  返回 0 = 全部合法；否则第一个有问题行的行号（从 1 起）
// ============================================================
uint8_t User_ConfigCheck(void) {
    for (uint8_t i = 0; i < (uint8_t)SENSOR_ROWS; i++) {
        const SensorDesc* d = &s_table[i];

        if (d->name == NULL || d->name[0] == '\0')      return (uint8_t)(i + 1);
        if (d->slave == 0 || d->slave > 247)            return (uint8_t)(i + 1);   // 地址范围
        if (d->reg_type != 0x03 && d->reg_type != 0x04) return (uint8_t)(i + 1);
        if (d->data_type >= DT_MAX)                     return (uint8_t)(i + 1);
        if (d->scale == 0.0f)                           return (uint8_t)(i + 1);   // 系数不能为 0
        if (d->unit == NULL || d->unit[0] == '\0')      return (uint8_t)(i + 1);

        // ---- 主题名：只允许字母/数字，且末尾三位必须是数字（设备类型码）----
        if (d->topic == NULL)                           return (uint8_t)(i + 1);
        size_t len = strlen(d->topic);
        if (len < 4)                                    return (uint8_t)(i + 1);
        for (size_t k = 0; k < len; k++) {
            char c = d->topic[k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '_')) return (uint8_t)(i + 1);
        }
        for (uint8_t k = 0; k < 3; k++) {
            char c = d->topic[len - 3 + k];
            if (c < '0' || c > '9')                     return (uint8_t)(i + 1);
        }

        // ---- 32 位格式要连读两个寄存器，不能是最后一行（否则读越界）----
        if (User_RegCount(d->data_type) == 2 && (uint8_t)(i + 2) > (uint8_t)SENSOR_ROWS)
            return (uint8_t)(i + 1);
    }
    return 0;
}
