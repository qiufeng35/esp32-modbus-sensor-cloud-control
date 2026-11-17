// ============================================================
//  secret_store.h —— 加密密钥存储（运行时解密）
//
//  构建期：tools/gen_secrets.py 把 secrets.ini 用 AES-128-CBC 加密成
//          src/05_secrets/gen/secret_blob.h（生成物，已在 .gitignore 中）
//  运行期：本模块用 mbedTLS 解密，明文只在内存里使用；串口/日志永不打印
//
//  ★ 仓库里不保存任何明文凭据：没有 secrets.ini 时按「演示模式」运行，
//    WiFi 与巴法云私钥均为空，不会连网（工程仍可正常编译烧录）。
// ============================================================
#ifndef SECRET_STORE_H
#define SECRET_STORE_H

#include <Arduino.h>

// 返回值：0 = 已解密加载；1 = 解密失败（口令/密文不匹配）；
//         2 = 数据格式错误；3 = 演示模式（未配置 secrets.ini）
uint8_t     Secret_Init(void);          // 开机调一次（可重复调用，幂等）
bool        Secret_IsConfigured(void);  // WiFi 与巴法云私钥是否都配置了
const char* Secret_WifiSsid(void);      // 解密后的 WiFi 名字（未配置返回 ""）
const char* Secret_WifiPassword(void);  // 解密后的 WiFi 密码（未配置返回 ""）
const char* Secret_BemfaUid(void);      // 解密后的 32 位私钥（未配置返回 ""）
const char* Secret_ErrorName(void);     // Secret_Init 返回值的文字说明

#endif
