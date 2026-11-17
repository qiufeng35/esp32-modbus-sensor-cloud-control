# T34_Bemfa（JDQ）—— 项目3 任务4 巴法云控制 Modbus 继电器

> 注：本文是项目开发过程记录。文中真实热点名、内网 IP、MAC 与私钥均已替换为占位符；
> 当前版本的密钥配置方式见根目录 README「密钥加密」。


本工程由 `T33_Bemfa`（巴法云上行）长出来，**只改两个地方**：

| 文件 | 改动 |
|------|------|
| `src/main.cpp` | 103 行 → 149 行：加「订阅 + 回调 + 写线圈」四段 |
| `src/03_protocol/src/modbus_rtu.cpp` / `.h` | 加一个函数 `Modbus_WriteCoil()`（功能码 0x05） |

- 平台：PlatformIO + Arduino 框架（framework-arduinoespressif32 2.0.17）
- 板卡：esp32-s3-devkitc-1
- 云：巴法云 `bemfa.com:9501`（只用私钥认证）
- 编译结果：**SUCCESS**，RAM 46880 B / Flash 740881 B（2026-10-23 本机实测）

## 一、这条线只有三步

1. 连上巴法云后**订阅** `switch006`；
2. 小程序 / APP 发来 `on` 就开灯、`off` 就关灯；
3. 开 / 关，就是给**从站 7 的线圈 1** 写一个数：**写 0 灯亮、写 1 灯灭**（这台继电器反接）。

## 二、main.cpp 新增的四段

| 行号 | 内容 | 作用 |
|------|------|------|
| 22 ~ 24 | `SWITCH_TOPIC` / `RELAY_SLAVE 7` / `RELAY_COIL 1` | 主题名、从站地址、线圈地址 |
| 51 ~ 58 | `Relay_Set(on)` | 把开 / 关换成线圈值，调 `Modbus_WriteCoil()` |
| 62 ~ 78 | `onSwitchMsg(messageSize)` | 逐字节读消息，按前两个字母判 on / off |
| 106 ~ 107 | `s_mqtt.subscribe(SWITCH_TOPIC)` | 连上之后订阅（每次重连都重订） |
| 137 | `s_mqtt.onMessage(onSwitchMsg)` | 注册回调（setup 里只写一次） |
| 115 | `s_mqtt.poll()` | 既发心跳，又把消息交给回调 |

## 三、总线上是什么

```
写 1（值 FF 00）→ 灯灭     07 05 00 01 FF 00 DD 9C
写 0（值 00 00）→ 灯亮     07 05 00 01 00 00 9C 6C
```

- 功能码 `0x05` = 写单个线圈；值只能是 `FF 00` 或 `00 00`；
- 应答是**请求原样回显** 8 字节（没有数据区）；
- CRC 低字节在前（`DD 9C` 里 `DD` 是低字节）。

## 四、运行现象

```
[MQTT] 已连上 bemfa.com:9501
[MQTT] 已订阅 switch006
[MQTT] 已上传 7/7
[灯] 收到 on
[灯] 已开
[灯] 收到 off
[灯] 已关
```

## 五、测试口径（重要）

- 编译与尺寸：本机 PlatformIO 实跑；
- 云端侧：PC 侧 MQTT 客户端（clientId = 私钥）实测 `CONNACK Success`、`SUBACK Granted QoS 0`，
  发 `switch006` 的 `on` / `off` 被原样回推 —— **证明私钥、主题、转发都正常**；
- 串口那几行：取自 `src/main.cpp` 里 `UART0.println` 的原文（写材料时板子未插，未重抓）。

## 六、排错三条

| 现象 | 原因 | 处理 |
|------|------|------|
| 没有「已订阅 switch006」 | `subscribe` 没写在 connect 成功分支里 | 挪进分支，每次连上都订 |
| 有「已订阅」但收不到 | 小程序发的主题不是 `switch006` | 控制台看卡片最新消息；或 MQTTX 直接发 |
| 收到 on 但灯不动 | A/B 反、没 24 V、从站不是 7 | 先敲 `sensor` 看总线上别的从站通不通 |
| 方向反了（点开灭） | 继电器是高电平触发 | 把 `coil = on ? 0 : 1` 改成 `on ? 1 : 0` |

> 当前版本私钥已移出源码：写在本地 `secrets.ini`，构建时 AES-128-CBC 加密后编译进固件。
