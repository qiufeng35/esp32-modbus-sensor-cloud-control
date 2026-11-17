// ============================================================
//  main.cpp —— 巴法云 MQTT：连上服务器，把传感器当前值发出去
//
//  ★ 本文件只干"上云"这一件事：
//     · WiFi 连没连上 → 问 04_driver/inc/wifi.h（状态机在那里）
//     · 传感器值     → 问 02_business/inc/task_sensor.h
//     · 这里只负责"每 10 秒把整张表发一遍、发完打一行汇总"
//
//  ★ 密钥不写在本文件：由 05_secrets 从加密存储解密得到（见 README「密钥加密」）
//  ★ 用到的库函数（ArduinoMqttClient）：
//     setId 身份 / setKeepAliveInterval 心跳 / connect 连接 / connected 是否连着
//     poll 维持心跳（每次 loop 都要调）/ beginMessage+print+endMessage 发一条
// ============================================================

#include "src/01_user/inc/User_Console.h"
#include "src/01_user/inc/User_Config.h"
#include "src/05_secrets/inc/secret_store.h"
#include "src/02_business/inc/task_sensor.h"
#include "src/03_protocol/inc/modbus_rtu.h"
#include "src/04_driver/inc/uart.h"
#include "src/04_driver/inc/rs485.h"
#include "src/04_driver/inc/wifi.h"

#include <WiFi.h>
#include <ArduinoMqttClient.h>
#include <string.h>

// ---------------- 云平台参数 ----------------
// ★ 32 位私钥不在这里：启动时由 05_secrets 用 AES-128-CBC 解密到内存
#define BEMFA_HOST   "bemfa.com"
#define BEMFA_PORT   9501                                  // 普通端口（明文，不加密）

#define PUB_MS       10000       // 每 10 秒发一遍
#define KEEPALIVE_S  60          // 心跳 60 秒（巴法云超过 65 秒判离线）
#define RETRY_MS     15000       // 没连上时，隔 15 秒再试

// ---------------- 运行状态 ----------------
static WiFiClient s_net;                 // TCP 通道
static MqttClient s_mqtt(s_net);         // MQTT 客户端，绑在这条通道上

static uint32_t s_nextTry = 0;           // 下次最早什么时候可以重连
static uint32_t s_lastPub = 0;           // 上次发布的时刻
static uint32_t s_pubOk   = 0;           // 累计发成功几条（mqtt 命令显示）
static uint32_t s_pubFail = 0;           // 累计失败几条（失败多 = 传感器没读到）
static int      s_lastErr = 0;           // 上次连接失败的原因（库的返回码）
static bool     s_everUp  = false;       // 这次开机有没有连上过
static uint8_t  s_relayOn = 0;           // 设备认为的继电器状态：1 = 开，0 = 关
static bool     s_noSecret = false;       // 演示模式提示只打一次

// 表里有几路在线（汇总行与 mqtt 命令都用它）
static uint8_t OnlineRows(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < Sensor_Count(); i++) if (Sensor_IsOnline(i)) n++;
    return n;
}

// 库的返回码 → 中文（正数 = 服务器不让连；负数 = 本机没连上）
static const char* ErrStr(int rc) {
    switch (rc) {
        case MQTT_SUCCESS:                 return "成功";
        case MQTT_CONNECTION_TIMEOUT:      return "连接超时（网络不通 / 端口被挡）";
        case MQTT_CONNECTION_REFUSED:      return "本机 socket 建不起来（不是服务器拒绝）";
        case MQTT_IDENTIFIER_REJECTED:     return "clientId 被拒（私钥重复或格式不对）";
        case MQTT_SERVER_UNAVAILABLE:      return "服务器不可用 / 维护中";
        case MQTT_NOT_AUTHORIZED:          return "★ 未授权（私钥不对）";
        default:                           return "未知错误";
    }
}

// ---------------- 发一条：第 idx 路的当前值 → 它在表里的主题 ----------------
// ★ 读不到就跳过，不发旧值 —— 云端宁可没数据，也不要假数据
static bool Publish(uint8_t idx) {
    const SensorDesc* d = Sensor_Desc(idx);
    float value = 0.0f;

    if (d == NULL || !Sensor_Value(idx, &value)) { s_pubFail++; return false; }

    s_mqtt.beginMessage(d->topic);        // 开始一条，主题如 temp0004
    s_mqtt.print(value, 2);               // 写数值，保留两位小数
    bool ok = (s_mqtt.endMessage() == 1); // 封口发出；1 = 成功
    if (ok) s_pubOk++; else s_pubFail++;
    return ok;
}

// 把整张表发一遍，返回发成功几条
static uint8_t PublishAll(void) {
    if (!Sensor_IsEnabled()) return 0;         // ★ 采集关了就不发（云端保留最后一次的值）
    uint8_t ok = 0;
    for (uint8_t i = 0; i < Sensor_Count(); i++) if (Publish(i)) ok++;
    return ok;
}

// 发一条纯文本（控制主题用：正文就是 on / off）
static bool PublishText(const char* topic, const char* text) {
    s_mqtt.beginMessage(topic);
    s_mqtt.print(text);
    return s_mqtt.endMessage() == 1;
}

// ============================================================
//  ★ 云端控制继电器（下行链路）
//  ------------------------------------------------------------
//  APP 点按钮 → 巴法云推 switch006 → poll() 收下 → OnCtrl()
//            → 写从站 7 的线圈 1 → 回发 on/off（APP 按钮才不弹回）
//  ★ 继电器是"反相"的（实测写 0 才亮）；反相在 USER_RELAY_INVERT 里处理，
//    换模块时只改配置，不动这里的逻辑。
// ============================================================
static uint8_t Relay_Set(uint8_t on) {
    uint8_t coil = USER_RELAY_INVERT ? (on ? 0 : 1) : (on ? 1 : 0);   // 要亮/要灭 → 线圈值
    uint8_t rc   = Sensor_BusWriteCoil(USER_RELAY_SLAVE, USER_RELAY_COIL, coil);

    if (rc == MB_OK) {
        s_relayOn = on;                                               // ★ 只有成功才更新状态
        UART0.printf("[继电器] %s\n", on ? "开" : "关");
    } else {
        UART0.printf("[继电器] %s失败：%s\n", on ? "开" : "关", Modbus_ErrStr(rc));  // 别假成功
    }
    return rc;
}

// 云端下发指令：正文是 on / off（也兼容 1 / 0）
//  ★ 一个回调管两张开关键卡片：继电器（switch0006）与采集总开关（sensor0006）
static void OnCtrl(int len) {
    String topic = s_mqtt.messageTopic();      // 先看是哪个主题发来的

    char cmd[8] = {0};
    int  n = 0;
    while (n < len && n < 7) {                 // 一个字节一个字节读完（"on" 2 个、"off" 3 个）
        int c = s_mqtt.read();
        if (c < 0) break;
        cmd[n++] = (char)c;
    }

    bool on  = (cmd[0] == 'o' && cmd[1] == 'n') || cmd[0] == '1';
    bool off = (cmd[0] == 'o' && cmd[1] == 'f') || cmd[0] == '0';
    if (!on && !off) return;                   // ★ 白名单：别的内容一律不理

    // ---- ① 继电器卡片：on = 亮，off = 灭 ----
    if (strcmp(topic.c_str(), USER_RELAY_TOPIC) == 0) {
        if ((uint8_t)on == s_relayOn) return;                  // 状态没变就不动（也防回环）
        if (Relay_Set(on ? 1 : 0) == MB_OK)
            PublishText(USER_RELAY_TOPIC, on ? "on" : "off");   // 执行成功才回发，按钮才不弹回
        return;
    }

    // ---- ② 采集总开关卡片：on = 恢复采集，off = 暂停采集 ----
    if (strcmp(topic.c_str(), USER_SENSOR_TOPIC) == 0) {
        if ((uint8_t)on == Sensor_IsEnabled()) return;
        Sensor_SetAllEnabled(on ? 1 : 0);
        UART0.printf("[采集] %s（%s）\n", on ? "已打开" : "已暂停",
                     on ? "7 路照常读、照常上报" : "不再读总线，云端保留最后一次的值");
        PublishText(USER_SENSOR_TOPIC, on ? "on" : "off");      // 回发，APP 按钮才不弹回
    }
}

// relay 命令：本地直接开关（不经过云，用来验总线接线与反相）
void Relay_Command(const char* arg) {
    if (arg == NULL || arg[0] == '\0') {
        UART0.printf("[继电器] 主题 %s ｜ 从站 %u 线圈 %u ｜ 当前 %s\n",
                     USER_RELAY_TOPIC, USER_RELAY_SLAVE, USER_RELAY_COIL,
                     s_relayOn ? "开" : "关");
        return;
    }
    if      (strcmp(arg, "on")  == 0) Relay_Set(1);
    else if (strcmp(arg, "off") == 0) Relay_Set(0);
    else UART0.println("用法：relay on ｜ relay off ｜ relay（只看状态）");
}

// ---------------- 每次 loop 走一遍：没连就重连；连上就维持心跳、到点发 ----------------
static void Bemfa_Task(void) {
    if (!WIFI_IsConnected()) return;                  // WiFi 还没拿到 IP，先不连 MQTT

    const char* uid = Secret_BemfaUid();              // ★ 私钥来自加密存储，不在源码里
    if (uid[0] == '\0') {                             // 演示模式 / secrets.ini 未配置
        if (!s_noSecret) {
            UART0.println("[MQTT] ★ 未配置巴法云私钥：按 README 创建 secrets.ini 后重新编译");
            s_noSecret = true;
        }
        return;
    }

    if (!s_mqtt.connected()) {
        // 没到重试时间就先返回（用有符号差比较，millis 回绕也不会算错）
        if ((int32_t)(millis() - s_nextTry) < 0) return;
        s_nextTry = millis() + RETRY_MS;

        s_mqtt.setId(uid);                            // 身份 = 私钥，用户名密码留空
        s_mqtt.setKeepAliveInterval((unsigned long)KEEPALIVE_S * 1000UL);

        if (s_mqtt.connect(BEMFA_HOST, BEMFA_PORT) == 1) {
            s_lastErr = 0;
            s_everUp  = true;
            UART0.println("[MQTT] 已连上 bemfa.com:9501");
            s_lastPub = 0;                            // 让下面尽快发第一轮

            // ★ 订阅必须在"连接成功"里面：这样每次掉线重连都会自动重订，
            //   否则连上一次以后只要断一次线，云端就再也控不动了（极难查）。
            s_mqtt.subscribe(USER_RELAY_TOPIC);          // 继电器卡片
            s_mqtt.subscribe(USER_SENSOR_TOPIC);         // 采集总开关卡片
            PublishText(USER_RELAY_TOPIC,  s_relayOn ? "on" : "off");            // 同步初始状态
            PublishText(USER_SENSOR_TOPIC, Sensor_IsEnabled() ? "on" : "off");
            UART0.printf("[MQTT] 已订阅 %s + %s（云端控制）\n",
                         USER_RELAY_TOPIC, USER_SENSOR_TOPIC);
        } else {
            s_lastErr = s_mqtt.connectError();
            UART0.printf("[MQTT] 还没连上，稍后再试（库码 %d　%s）\n", s_lastErr, ErrStr(s_lastErr));
            UART0.println("       排错：敲 wifi 看网络状态；库码 5 = 私钥不对");
        }
        return;                                       // 这一轮只负责连接，先不发数据
    }

    s_mqtt.poll();                                    // ★ 每次 loop 都要调，库才会按时发心跳

    if (millis() - s_lastPub < PUB_MS) return;        // 距上次发送不足 10 秒
    s_lastPub = millis();

    uint8_t ok = PublishAll();
    if (!Sensor_IsEnabled()) {                        // 采集被云端关掉：这一轮只说一句
        UART0.println("[MQTT] 采集已关闭，本轮不发送（云端保留最后一次的值）");
        return;
    }
    // ★ 汇总行带上"在线几路"：0/7 + 在线 0/7 = 传感器没读到，与网络无关
    UART0.printf("[MQTT] 已上传 %u/%u　在线 %u/%u\n",
                 ok, Sensor_Count(), OnlineRows(), Sensor_Count());
}

// ============================================================
//  mqtt 命令：云端状态 + 7 个主题的当前值
// ============================================================
void Bemfa_PrintStatus(void) {
    UART0.println("\n======== 巴法云 MQTT 状态 ========");
    UART0.printf("  服务器　%s:%d\n", BEMFA_HOST, BEMFA_PORT);
    uint8_t sec = Secret_Init();                      // 幂等：只取状态
    if (Secret_IsConfigured())
        UART0.println("  私钥　　★ 已从加密存储解密加载（AES-128-CBC，不显示内容）");
    else if (sec == 3)
        UART0.println("  私钥　　★ 未配置（演示模式，见 README「密钥加密」）");
    else
        UART0.printf("  私钥　　★ 不可用：%s\n", Secret_ErrorName());

    UART0.printf("  WiFi　　%s", WIFI_StateName());
    if (WIFI_IsConnected())
        UART0.printf("　IP %s　信号 %d dBm", WIFI_IP().c_str(), (int)WIFI_Rssi());
    UART0.println();

    UART0.print("  MQTT　　");
    if (s_mqtt.connected())               UART0.println("★ 已连上");
    else if (s_lastErr == 0 && !s_everUp) UART0.println("还没连（等 WiFi 拿到 IP 后会自动连）");
    else if (s_lastErr == 0)              UART0.println("★ 刚从已连状态掉下来（会自动重连）");
    else UART0.printf("未连上　上次原因：%s（库码 %d）\n", ErrStr(s_lastErr), s_lastErr);

    UART0.printf("  在线　　%u / %u 路（★ 读不到的那几路会跳过不发）\n",
                 OnlineRows(), Sensor_Count());
    UART0.printf("  发布　　成功 %lu 条 ｜ 失败 %lu 条（失败多 = 传感器没读到，先敲 sensor）\n",
                 (unsigned long)s_pubOk, (unsigned long)s_pubFail);
    UART0.printf("  继电器　从站 %u 线圈 %u（反相 %s）｜ 主题 %s ｜ 当前 %s\n",
                 USER_RELAY_SLAVE, USER_RELAY_COIL, USER_RELAY_INVERT ? "是" : "否",
                 USER_RELAY_TOPIC, s_relayOn ? "开" : "关");
    UART0.printf("  采集　　%s ｜ 主题 %s（on = 恢复，off = 暂停）\n",
                 Sensor_IsEnabled() ? "开" : "关", USER_SENSOR_TOPIC);

    UART0.println("  主题（必须和巴法云控制台里建的一字不差）：");
    for (uint8_t i = 0; i < Sensor_Count(); i++) {
        const SensorDesc* d = Sensor_Desc(i);
        float v = 0.0f;
        if (Sensor_IsOnline(i) && Sensor_Value(i, &v))
            UART0.printf("    %u. %s → %s　当前 %.2f %s\n", i + 1, d->name, d->topic, v, d->unit);
        else
            UART0.printf("    %u. %s → %s　★ 离线（不发）\n", i + 1, d->name, d->topic);
    }
    UART0.println("=================================\n");
}

// ============================================================
//  pub 命令：立刻发一遍，并逐行说清"发了"还是"跳过"
// ============================================================
void Bemfa_PublishNow(void) {
    if (!WIFI_IsConnected()) { UART0.println("[MQTT] WiFi 还没连上 —— 先敲 wifi 看状态"); return; }
    if (!s_mqtt.connected()) { UART0.println("[MQTT] 还没连上 —— 先敲 mqtt 看是什么原因"); return; }

    UART0.println("[MQTT] 立刻发一遍：");
    uint8_t ok = 0;
    for (uint8_t i = 0; i < Sensor_Count(); i++) {
        const SensorDesc* d = Sensor_Desc(i);
        if (Publish(i)) {
            ok++;
            UART0.printf("  %u. %s　已发\n", i + 1, d->name);
        } else {
            UART0.printf("  %u. %s　★ 跳过（这一路没读到：查 485 接线 / 从站地址 / 供电）\n",
                         i + 1, d ? d->name : "?");
        }
    }
    UART0.printf("[MQTT] 成功 %u 条 / 共 %u 条（主题见 mqtt）\n", ok, Sensor_Count());
}

// ============================================================
//  开机与主循环
// ============================================================
void setup() {
    UART_Init(USER_CONSOLE_BAUD);     // 调试串口

    // ★ 先解密凭据：密钥头文件由 tools/gen_secrets.py 在编译前生成
    uint8_t sec = Secret_Init();
    if (sec == 0)      UART0.println("[密钥] 已解密加载（AES-128-CBC / PBKDF2-SHA256），内容不打印");
    else if (sec == 3) UART0.println("[密钥] ★ 演示模式：未配置 secrets.ini，WiFi 与巴法云私钥为空");
    else               UART0.printf("[密钥] ★ 解密失败：%s\n", Secret_ErrorName());

    RS485_Init();                     // 总线串口（9600 8N1）
    Modbus_Init();                    // 协议层
    Console_Init();                   // 命令台
    Sensor_Init();                    // 业务层：载入表并打印整张表

    // ★ 云端下发的 on/off 由库在 poll() 里回调 OnCtrl()，只用设置一次
    s_mqtt.onMessage(OnCtrl);
    UART0.printf("[继电器] 从站 %u 线圈 %u 反相=%s ｜ 控制主题 %s\n",
                 USER_RELAY_SLAVE, USER_RELAY_COIL,
                 USER_RELAY_INVERT ? "是" : "否", USER_RELAY_TOPIC);
    UART0.printf("[采集] 总开关主题 %s（on = 恢复，off = 暂停）\n", USER_SENSOR_TOPIC);

    // 先自检配置：名字空 / 太长 / 密码空，就别白等 10 秒超时
    uint8_t wcfg = User_WifiCheck();
    if (wcfg != 0) {
        UART0.printf("[WiFi] ★ 配置不合格：%s\n",
                     wcfg == 1 ? "加密存储里的 WiFi 名字是空的（见 secrets.ini）"
                   : wcfg == 2 ? "名字超过 32 字节"
                               : "密码为空（开放热点请先确认安全）");
        UART0.println("[WiFi] ★ 已跳过连接：按 README 创建 secrets.ini 后重新编译");
    } else {
        WIFI_Init(User_WifiSsid(), User_WifiPassword());   // 只提交连接请求，立即返回
    }
}

void loop() {
    WIFI_Loop();        // ① 网络：推进 WiFi 状态机（掉线自动重连，不阻塞）
    Bemfa_Task();       // ② 上云：连 MQTT 并上传当前值
    Sensor_Loop();      // ③ 业务：非阻塞，一次最多读一路
    Console_Poll();     // ④ 命令台：收命令
}

