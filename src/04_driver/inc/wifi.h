// ============================================================
//  wifi.h —— WiFi 驱动（只干"连上路由器"这一件事）
//
//  ★ 连接做成【非阻塞状态机】：
//      WIFI_Init() 只提交请求，WIFI_Loop() 每次 loop 推一小步。
//    这样它才能和 Modbus 轮询并排住在同一个 loop 里，谁都不按住谁。
// ============================================================
#ifndef WIFI_H
#define WIFI_H

#include <Arduino.h>

// 连接等 10 秒还没拿到 IP，就断开重来（程序照常跑，不卡）
#define WIFI_CONNECT_TIMEOUT_MS  10000

// 连接状态：只有三种，够用了
typedef enum {
    WIFI_ST_IDLE = 0,     // 还没开始
    WIFI_ST_CONNECTING,   // 正在连（认证 / 关联 / 等 IP）
    WIFI_ST_CONNECTED     // ★ 已拿到 IP（真正连上了）
} WifiState;

void        WIFI_Init(const char* ssid, const char* password);  // 提交连接请求，立即返回
void        WIFI_Loop(void);                                    // 每次 loop 推进一步

bool        WIFI_IsConnected(void);                             // 现在连上了吗（拿到 IP 才算）
const char* WIFI_StateName(void);                               // 状态的中文名
String      WIFI_IP(void);                                      // 本机 IP（没连上返回 "--"）
int32_t     WIFI_Rssi(void);                                    // 信号强度 dBm

void        WIFI_PrintStatus(void);                             // wifi 命令：一屏状态

#endif
