// ============================================================
//  wifi.cpp —— WiFi 驱动实现（非阻塞状态机）
//
//  为什么不用 while (WiFi.status() != WL_CONNECTED) delay();
//    那样会卡住整个 loop：传感器不读、命令不响应、MQTT 不心跳。
//  这里的做法：Init 只发请求，Loop 每 500 ms 看一眼结果，连上就报一次 IP。
// ============================================================

#include "../inc/wifi.h"
#include "../inc/uart.h"
#include <WiFi.h>

// ---------------- 内部状态 ----------------
static const char* s_ssid     = NULL;      // 只存地址；账号密码在 User_Config.h 的宏里
static const char* s_pass     = NULL;
static WifiState   s_state    = WIFI_ST_IDLE;
static uint32_t    s_start_ms = 0;         // 本次连接开始时刻（算 10 秒超时）
static uint32_t    s_check_ms = 0;         // 上次检查的时刻（500 ms 一次）
static uint32_t    s_conn_ms  = 0;         // 连上的时刻（算"已连多少秒"）
static uint8_t     s_try      = 0;         // 试了几次
static int         s_last_st  = 0xFF;      // 上次打印过的库状态码（变了才打印，防刷屏）

// 库的状态码 → 一句人话（只挑上课最常碰到的几种）
static const char* Why(int st) {
    switch (st) {
        case WL_NO_SSID_AVAIL:   return "搜不到这个热点：名字写错，或它不是 2.4G";
        case WL_CONNECT_FAILED:  return "连不上：多半密码错，或路由器不让新设备接入";
        case WL_CONNECTION_LOST: return "连接中断：路由器重启，或信号太弱";
        default:                 return "还没连上，正在重试";
    }
}

// ============================================================
//  ① 状态机
// ============================================================
void WIFI_Init(const char* ssid, const char* password) {
    s_ssid = ssid;
    s_pass = password;

    WiFi.mode(WIFI_STA);          // 客户端模式（不是热点）
    WiFi.setSleep(false);         // 关省电：上课环境连接更稳
    delay(100);                   // 等模式切换生效（setup 里允许）

    UART0.printf("\n======== WiFi 连接（非阻塞状态机）========\n要连的热点：%s\n", ssid);

    WiFi.begin(ssid, password);   // ★ 发出去就返回，不等连上
    s_try++;
    s_state    = WIFI_ST_CONNECTING;
    s_start_ms = millis();
    s_last_st  = 0xFF;
    UART0.println("已提交连接请求（★ 不阻塞，接着往下跑）");
}

void WIFI_Loop(void) {
    uint32_t now = millis();

    // ---- 已连上：只盯"掉线"这一件事 ----
    if (s_state == WIFI_ST_CONNECTED) {
        if (!WiFi.isConnected()) {
            UART0.println("[WiFi] ★ 掉线了（自动重连，★ Modbus 不受影响）");
            s_state    = WIFI_ST_CONNECTING;
            s_start_ms = now;
            s_last_st  = 0xFF;
            WiFi.begin(s_ssid, s_pass);
        }
        return;
    }
    if (s_state != WIFI_ST_CONNECTING) return;

    // ---- 连接中：500 ms 看一次，绝不忙等 ----
    if ((uint32_t)(now - s_check_ms) < 500) return;
    s_check_ms = now;

    // ---- 拿到 IP 才算真的连上 ----
    if (WiFi.isConnected()) {
        s_state   = WIFI_ST_CONNECTED;
        s_conn_ms = now;
        UART0.printf("\n[WiFi] ★ 已拿到 IP：%s\n", WiFi.localIP().toString().c_str());
        UART0.printf("[WiFi] 网关 %s　DNS %s　信号 %d dBm\n",
                     WiFi.gatewayIP().toString().c_str(),
                     WiFi.dnsIP().toString().c_str(), (int)WiFi.RSSI());
        return;
    }

    // 库状态变了才打印一行原因
    int st = WiFi.status();
    if (st != s_last_st) {
        s_last_st = st;
        UART0.printf("[WiFi] 状态 %d → %s\n", st, Why(st));
    }

    // ---- 10 秒还没连上：断开重来（不傻等，其他任务照常跑）----
    if ((uint32_t)(now - s_start_ms) >= WIFI_CONNECT_TIMEOUT_MS) {
        UART0.printf("[WiFi] 连接超时 → 断开重来（第 %u 次）\n", (unsigned)(s_try + 1));
        WiFi.disconnect();
        delay(10);
        WiFi.begin(s_ssid, s_pass);
        s_try++;
        s_start_ms = now;
        s_last_st  = 0xFF;
    }
}

// ============================================================
//  ② 查状态（心跳行和 MQTT 层都问这两句）
// ============================================================
bool WIFI_IsConnected(void) {
    return (s_state == WIFI_ST_CONNECTED) && WiFi.isConnected();
}

const char* WIFI_StateName(void) {
    switch (s_state) {
        case WIFI_ST_CONNECTING: return "正在连接";
        case WIFI_ST_CONNECTED:  return "★ 已连上";
        default:                 return "未开始";
    }
}

String WIFI_IP(void) {
    return WIFI_IsConnected() ? WiFi.localIP().toString() : String("--");
}

int32_t WIFI_Rssi(void) {
    return WIFI_IsConnected() ? (int32_t)WiFi.RSSI() : 0;
}

// ============================================================
//  ③ wifi 命令：一屏状态（连不上时给出"试了几次、等了多久"）
// ============================================================
void WIFI_PrintStatus(void) {
    UART0.printf("\n======== WiFi 状态 ========\n  状态　%s\n", WIFI_StateName());
    UART0.printf("  热点　%s\n", s_ssid ? s_ssid : "(未配置)");

    if (WIFI_IsConnected()) {
        UART0.printf("  IP　　%s　网关 %s\n",
                     WiFi.localIP().toString().c_str(),
                     WiFi.gatewayIP().toString().c_str());
        UART0.printf("  DNS　 %s　信号 %d dBm\n",
                     WiFi.dnsIP().toString().c_str(), (int)WiFi.RSSI());
        UART0.printf("  MAC　 %s\n", WiFi.macAddress().c_str());
        UART0.printf("  已连　%lu 秒\n", (unsigned long)((millis() - s_conn_ms) / 1000));
    } else {
        UART0.printf("  已试　%u 次　本次已等 %lu 秒\n", (unsigned)s_try,
                     (unsigned long)((millis() - s_start_ms) / 1000));
    }
    UART0.println("===========================\n");
}
