// ============================================================
//  User_Console.cpp —— 串口命令台（实现）
//
//  就三件事：① 收字节拼成一行 ② 按命令名分发 ③ 把结果打到 UART0。
//  ★ 全程不阻塞：没收到整行就立刻返回，绝不 while 等输入。
//
//  命令一共 11 条：
//    help / sensor [行号] / cal <行号> <标准值> / period <行号> <毫秒> / time
//    mqtt / pub / relay on|off / enable on|off / wifi
// ============================================================

#include "../inc/User_Console.h"
#include "../inc/User_Config.h"
#include "../../04_driver/inc/uart.h"
#include "../../04_driver/inc/wifi.h"
#include "../../02_business/inc/task_sensor.h"
#include <string.h>
#include <stdlib.h>

// 实现在 src/main.cpp 的两个上云入口（本文件只声明，避免互相包含）
void Bemfa_PrintStatus(void);      // mqtt：云端状态 + 7 个主题当前值
void Bemfa_PublishNow(void);       // pub ：立刻发一遍
void Relay_Command(const char* arg);   // relay：本地直接开关继电器（不经过云）

static char    s_line[CONSOLE_LINE_MAX];
static uint8_t s_len = 0;

// ---------------- 命令表 ----------------
static void Console_Help(void) {
    UART0.println("\n======== 命令表 ========");
    UART0.println("sensor                看整张配置表（配置 + 本次值 + 状态）");
    UART0.println("sensor <行号>         看某一行：含原始值十六进制，用来核说明书");
    UART0.println("cal <行号> <标准值>   标定；写成 cal <行号> 0 就是把 offset 清 0");
    UART0.println("period <行号> <毫秒>  改这一路的读取周期（掉电丢失）");
    UART0.println("time                  排程体检：一轮几次事务、预期多久、在线几个量");
    UART0.println("mqtt                  看云端状态：连接 / 在线数 / 7 个主题与当前值");
    UART0.println("pub                   立刻把整张表发一遍（不用等 10 秒）");
    UART0.println("relay on|off          直接开关继电器（本地验总线，不经过云）");
    UART0.println("enable on|off         暂停/恢复采集（相当于 APP 上的传感器开关）");
    UART0.println("wifi                  看 WiFi：状态 / IP / 网关 / DNS / 信号 / MAC");
    UART0.println("help                  显示本帮助");
    UART0.println("★ 数据流：配置表 → 02_business 轮询 → main.cpp 发 MQTT → 巴法云");
    UART0.println("========================\n");
}

// ---------------- 三个带参数的命令 ----------------
static void Console_Sensor(char* arg) {
    if (arg == NULL || arg[0] == '\0') Sensor_PrintTable();      // 只敲 sensor：整张表
    else                              Sensor_PrintOne((uint8_t)atoi(arg));
}

static void Console_Cal(char* arg) {
    char* a = strtok(arg, " ");
    char* b = strtok(NULL, " ");
    if (a == NULL || b == NULL) {
        UART0.println("用法：cal <行号> <标准值>，例：cal 6 52.0；清零用 cal 6 0");
        return;
    }
    uint8_t idx = (uint8_t)atoi(a);
    float   std = (float)atof(b);
    if (std == 0.0f) Sensor_SetZero(idx);          // cal x 0 = 清 offset
    else             Sensor_Calibrate(idx, std);
}

static void Console_Period(char* arg) {
    char* a = strtok(arg, " ");
    char* b = strtok(NULL, " ");
    if (a == NULL || b == NULL) {
        UART0.println("用法：period <行号> <毫秒>，例：period 1 500");
        return;
    }
    uint8_t  idx = (uint8_t)atoi(a);
    uint32_t ms  = (uint32_t)strtoul(b, NULL, 10);

    if (idx == 0 || idx > Sensor_Count()) { UART0.println("行号不对：先敲 sensor 看一共几行"); return; }
    if (ms < 100)                         { UART0.println("周期太短：至少 100 ms（先敲 time 看一轮要多久）"); return; }

    User_SetPeriod((uint8_t)(idx - 1), ms);
    UART0.printf("[周期] %s 改为 %lu ms\n", Sensor_Desc((uint8_t)(idx - 1))->name,
                 (unsigned long)ms);
    UART0.println("       ★ 抄回 User_Config.cpp 对应行的 period_ms，掉电才不会丢");
}

// ---------------- 采集总开关（云端那张 006 卡片的本地版）----------------
static void Console_Enable(char* arg) {
    if (arg != NULL && strcmp(arg, "on") == 0) {
        Sensor_SetAllEnabled(1);
        UART0.println("[采集] 已打开（继续读总线、继续上报）");
    } else if (arg != NULL && strcmp(arg, "off") == 0) {
        Sensor_SetAllEnabled(0);
        UART0.println("[采集] 已暂停（不再读总线，云端保留最后一次的值）");
    } else {
        UART0.printf("[采集] 当前 %s ｜ 用法：enable on ｜ enable off\n",
                     Sensor_IsEnabled() ? "开" : "关");
    }
}

// ---------------- 分发：命令名 → 谁去干 ----------------
static void Console_Exec(char* line) {
    if      (strcmp(line, "help")   == 0) Console_Help();
    else if (strcmp(line, "sensor") == 0) Sensor_PrintTable();
    else if (strncmp(line, "sensor ", 7) == 0) Console_Sensor(line + 7);
    else if (strncmp(line, "cal ",    4) == 0) Console_Cal(line + 4);
    else if (strncmp(line, "period ", 7) == 0) Console_Period(line + 7);
    else if (strcmp(line, "time")   == 0) Sensor_PrintTiming();
    else if (strcmp(line, "mqtt")   == 0) Bemfa_PrintStatus();
    else if (strcmp(line, "pub")    == 0) Bemfa_PublishNow();
    else if (strcmp(line, "relay")  == 0) Relay_Command(NULL);          // 只看状态
    else if (strncmp(line, "relay ", 6) == 0) Relay_Command(line + 6);  // relay on / off
    else if (strcmp(line, "enable")  == 0) Console_Enable(NULL);        // 只看采集状态
    else if (strncmp(line, "enable ", 7) == 0) Console_Enable(line + 7); // enable on / off
    else if (strcmp(line, "wifi")   == 0) WIFI_PrintStatus();
    else if (line[0] != '\0') {
        UART0.printf("未知命令：%s\n输入 help 查看全部命令\n", line);
    }
}

// ---------------- 收字节 → 拼一行 → 执行 ----------------
void Console_Poll(void) {
    while (UART0.available() > 0) {
        char c = (char)UART0.read();

        if (c == '\r') continue;                          // 串口工具会发 \r\n，只认 \n
        if (c == '\n') {
            s_line[s_len] = '\0';
            Console_Exec(s_line);                         // 收满一行才执行
            s_len = 0;
        } else if (s_len < CONSOLE_LINE_MAX - 1) {
            s_line[s_len++] = c;
        } else {
            s_len = 0;                                    // 太长就丢弃，防止溢出
            UART0.println("命令太长，已丢弃");
        }
    }
}

void Console_Init(void) {
    s_len = 0;
}
