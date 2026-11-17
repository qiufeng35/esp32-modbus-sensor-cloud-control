# T33_Bemfa —— 项目3 任务3 巴法云：把 7 路传感器发上云

> 注：本文是项目开发过程记录。文中真实热点名、内网 IP、MAC 与私钥均已替换为占位符；
> 当前版本的密钥配置方式见根目录 README「密钥加密」。


本工程由 `T31_WiFi` 起步，上云逻辑集中在 `src/main.cpp`；WiFi 连接交给
`src/04_driver/src/wifi.cpp` 的非阻塞状态机（与 `T32_Mqtt` 同一份实现），
命令台的 `wifi / wifiscan / nettest` 三条命令同步恢复，另新增 `mqtt / pub / cloudtest`。
协议层、业务层、传感器表未改。

- 平台：PlatformIO + Arduino 框架（framework-arduinoespressif32 2.0.17）
- 板卡：esp32-s3-devkitc-1
- 云：巴法云（bemfa.com）普通端口 9501，只用私钥认证
- 编译结果：**SUCCESS**，RAM 46928 B / Flash 753613 B

## 一、main.cpp 的结构（345 行）

| 行号 | 内容 | 说明 |
|------|------|------|
| 1 ~ 18 | 文件头注释 | 6 个库函数 + ★ WiFi 交给驱动层状态机 |
| 20 ~ 29 | 7 个工程头 + 2 个库头 | ★ 新增 `04_driver/inc/wifi.h`；WiFi.h + ArduinoMqttClient.h |
| 31 ~ 37 | 6 个宏 | 私钥 / 服务器 / 端口；发布 10 s / 心跳 60 s / 重试 15 s |
| 39 ~ 47 | 2 个对象 + 6 个状态量 | `WiFiClient` → `MqttClient`；重连/发布时刻、成功/失败计数、上次错误码、是否连上过 |
| 50 ~ 54 | `Bemfa_OnlineRows()` | 表里有几路在线（汇总行与 mqtt 命令要用） |
| 57 ~ 68 | `Bemfa_ErrStr()` | 把库的返回码翻成中文（5 = 私钥不对、-1 = 本机没连上） |
| 72 ~ 93 | `Bemfa_Publish()` / `Bemfa_PublishAll()` | 发一条 / 发整张表；读不到就跳过，不发旧值 |
| 96 ~ 142 | `Bemfa_Task()` | 没连就重连；连上就维持心跳、到点发，并报「已上传 x/7　在线 y/7」 |
| 150 ~ 220 | `Bemfa_PrintStatus()` | `mqtt` 命令：服务器 / WiFi / MQTT / 在线数 / 7 个主题与当前值 |
| 223 ~ 254 | `Bemfa_PublishNow()` | `pub` 命令：立刻发一遍，逐行说清「已发」还是「跳过」 |
| 264 ~ 316 | `Bemfa_CloudTest()` | `cloudtest` 命令：DNS → TCP → 结论（明文 9501，不提证书） |
| 318 ~ 337 | `setup()` | 五个初始化 + `User_WifiCheck` + `WIFI_Init`（提交请求，不阻塞） |
| 339 ~ 345 | `loop()` | `WIFI_Loop` → `Bemfa_Task` → `Sensor_Loop` → `Console_Poll` → `RS485_Loop` |

## 二、密钥配置（当前版本）

```ini
# 复制 secrets.example.ini 为 secrets.ini，填入以下四项：
[secrets]
passphrase    = 一串足够长的加密口令
wifi_ssid     = 你的2.4G热点名
wifi_password = 你的WiFi密码
bemfa_uid     = 你的32位巴法云私钥
```

构建时 `tools/gen_secrets.py` 会把 `secrets.ini` 用 AES-128-CBC 加密成
`src/05_secrets/gen/secret_blob.h`，运行时由 `05_secrets` 解密到内存；
仓库与源码中不出现明文凭据（详见根目录 README「密钥加密」）。

服务器与端口不用改：巴法云固定 `bemfa.com:9501`（另有加密口 9503）。

## 三、7 个主题（照 User_Config.cpp 的 topic 列建，必须在控制台先建好）

| # | 主题名 | 对应 | 末尾类型码 |
|---|--------|------|-----------|
| 1 | `reg0004` | 风速（从站1 0x0000） | 004 = 传感器 |
| 2 | `dir0004` | 风向（从站2 0x0001） | 004 |
| 3 | `humi0004` | 湿度（从站3 0x0000） | 004 |
| 4 | `temp0004` | 温度（从站3 0x0001） | 004 |
| 5 | `lux0004` | 光照（从站4 0x0008） | 004 |
| 6 | `humi50004` | 湿度5（从站5 0x0000） | 004 |
| 7 | `temp50004` | 温度5（从站5 0x0001） | 004 |

★ 巴法云主题名**只允许字母和数字**：下划线、空格、中文都建不出来（写 `temp5_0004`
会被控制台直接拒绝）。同一台设备的两个量靠「名字+序号」区分，即 `humi50004` / `temp50004`；
末尾三位是设备类型码（004 = 传感器，手机 APP 自动出数值卡片）。

## 四、运行现象

开机先打底座（与任务 2.3 结束时一字不差）：串口就绪、RS485 就绪、7 行传感器表、
`开机自检：表格全部合法`。

接着是 WiFi 状态机（`WIFI_Init` 只提交请求，`WIFI_Loop` 每次 loop 推一小步）：

```
======== WiFi 连接（非阻塞状态机）========
已提交连接请求（★ 不阻塞，接着往下跑）
[WiFi] ★ 已拿到 IP（真正连上了）
[WiFi] 本机 IP：192.168.x.x
```

拿到 IP 之后才去连巴法云，然后每 10 秒（`PUB_MS`）打一行汇总：

```
[MQTT] 已连上 bemfa.com:9501
[MQTT] 已上传 7/7　在线 7/7
[MQTT] 已上传 7/7　在线 7/7
```

★ 没接传感器时是这样 —— **前半截是网络、后半截是总线，一眼分得开**：

```
[MQTT] 已上传 0/7　在线 0/7        ← 7 路全都没读到，与 WiFi / 巴法云无关
```

连不上时每 15 秒（`RETRY_MS`）重复一次，并直接给出中文原因：

```
[MQTT] 还没连上，稍后再试（库码 5　★ 未授权 —— 私钥不对（去巴法云「连接密钥」页核对））
       排错：敲 cloudtest 看网络/端口；敲 wifi 看 WiFi 状态
```

### 串口命令（`help` 全部列出）

> 下表是当时版本的命令；当前发布版已精简为 11 条，见根目录 README「串口命令」。

| 命令 | 作用 |
|------|------|
| `sensor` / `sensor <行号>` | 看整张表 / 看某一行（含原始值十六进制） |
| `cal <行号> <标准值>` / `period <行号> <毫秒>` | 标定 / 改读取周期 |
| `time` | 排程体检：一轮几帧、预期多久、实测节奏 |
| `mqtt` | 云端状态：服务器 / WiFi / MQTT / 在线数 / 7 个主题与当前值 |
| `pub` | 立刻把整张表发一遍，逐行打印「已发」或「★ 跳过（没读到）」 |
| `cloudtest` | 云端口体检：DNS → TCP → 结论（连不上云先敲它） |
| `wifi` | WiFi 状态：IP / 网关 / DNS / 信号 / MAC / 已连多久 |
| `wifiscan` | 扫周边热点（会阻塞 2~4 秒，正常） |
| `nettest` | 一键体检：配置 / 扫描 / 连接 / ★总线 四连查 |

## 五、排错要点

| 现象 | 原因 | 处理 |
|------|------|------|
| **「已上传 0/7　在线 0/7」** | **一路传感器都没读到**：没接 / 没供电 / A-B 接反 / 从站地址不是 1~5 / 波特率不是 9600 | 敲 `sensor` 看状态列与失败次数，敲 `nettest` 第④项会直接报「一路都不在线」。★ 与 WiFi、巴法云无关，别去查网络 |
| 一直「还没连上，稍后再试（库码 …）」 | ★ 私钥不对（其次网络） | 看括号里的库码：5 = 未授权（私钥错）、2 = clientId 被拒、-1 = 本机没连上；再敲 `cloudtest` 分清是网络还是私钥 |
| 连上了但云端没数 | ★ 主题没建 / 名字不一致 | 逐字对 7 个主题名（第 6、7 行是 `humi50004` / `temp50004`，★ 都没有下划线） |
| 连上约两分钟就掉 | 心跳设大了 | 本程序 60 秒，巴法云超过 65 秒即判离线 |
| 「已上传 5/7　在线 5/7」 | 有 2 路传感器离线 | 先修 RS485 总线；程序**跳过不发**，不补旧值 |
| WiFi 一直连不上 | 连的是 5G 热点 / 名字写错 | ESP32-S3 只认 2.4G（WPA2）；敲 `wifiscan` 看目标热点在不在列表里 |

## 六、目录结构

```
T33_Bemfa/
├── platformio.ini
├── src/
│   ├── main.cpp                ← 上云逻辑（345 行）
│   ├── 01_user/                配置表、命令台（★ 恢复 wifi/wifiscan/nettest，新增 mqtt/pub/cloudtest）
│   ├── 02_business/            传感器排程（2.3 原样）
│   ├── 03_protocol/            Modbus RTU（2.2 原样）
│   └── 04_driver/              UART / RS485 / ★ WiFi（wifi.cpp 状态机已在 setup/loop 里调用）
├── lib/                        随工程携带的库（含 ArduinoMqttClient）
├── README.md                   继承自 T31_WiFi（历史说明）
└── README_T33_Bemfa.md         本文件
```

串口命令台：`sensor` / `time` / `cal` / `period` / `help` 原样保留；
`wifi` / `wifiscan` / `nettest` 已恢复；另新增 `mqtt` / `pub` / `cloudtest`（均为历史版本）。

★ WiFi 的"连没连上"只有 `04_driver/src/wifi.cpp` 一份状态（setup 里 `WIFI_Init` 提交、
loop 里 `WIFI_Loop` 推进），命令台和上云层都问它（`WIFI_IsConnected`），
不会出现两套状态互相打脸 —— 之前正是两套状态，才只好把三条命令关掉，表现成
「工作台里的 WiFi 连接已关闭」。
