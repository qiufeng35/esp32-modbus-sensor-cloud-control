// ============================================================
//  secret_store.cpp —— 加密密钥存储（实现）
//
//  密文由 tools/gen_secrets.py 在编译前生成：AES-128-CBC + PKCS#7；
//  AES 密钥（KEY）由 secrets.ini 的 passphrase 经 PBKDF2-HMAC-SHA256 派生，
//  IV 在每次生成时随机（与密文一起存放在生成的头文件里）。
//  这里只负责「解出来、按格式拆开」，解密失败一律当作未配置处理，
//  绝不把密文或明文打到串口。
// ============================================================

#include "../inc/secret_store.h"

#include <string.h>
#include <mbedtls/aes.h>

// 构建脚本会在编译前生成这个头文件；没有它（例如直接用 Arduino IDE 打开）
// 就按演示模式编译，保证工程本身能编译通过。
#if __has_include("../gen/secret_blob.h")
#include "../gen/secret_blob.h"
#else
#define SECRET_DEMO 1
#endif

#ifndef SECRET_FORMAT_VERSION
#define SECRET_FORMAT_VERSION 1
#endif

#define SECRET_SSID_MAX  33   // SSID 上限 32 字节 + '\0'
#define SECRET_PASS_MAX  64   // WPA 密码上限 63 字节 + '\0'
#define SECRET_UID_MAX   33   // 32 位十六进制 + '\0'
#define SECRET_PLAIN_MAX 256  // 明文缓冲（本工程 3 个字段远小于此值）

static char    s_ssid[SECRET_SSID_MAX];
static char    s_pass[SECRET_PASS_MAX];
static char    s_uid[SECRET_UID_MAX];
static bool    s_configured = false;
static bool    s_inited     = false;
static uint8_t s_state      = 3;   // 默认：演示模式

// ---------------- 拆载荷：'MBES' + 版本 + 3 个「长度 + 内容」 ----------------
static uint8_t ParsePayload(const uint8_t* p, size_t len) {
    if (len < 5) return 2;
    if (memcmp(p, "MBES", 4) != 0) return 2;
    if (p[4] != (uint8_t)SECRET_FORMAT_VERSION) return 2;

    size_t off = 5;
    char*  dst[3] = { s_ssid, s_pass, s_uid };
    size_t cap[3] = { sizeof(s_ssid), sizeof(s_pass), sizeof(s_uid) };

    for (uint8_t i = 0; i < 3; i++) {
        if (off >= len) return 2;
        size_t n = p[off++];
        if (off + n > len) return 2;
        if (n + 1 > cap[i]) return 2;
        memcpy(dst[i], p + off, n);
        dst[i][n] = '\0';
        off += n;
    }
    if (off != len) return 2;

    s_configured = (s_ssid[0] != '\0' && s_pass[0] != '\0' &&
                    strlen(s_uid) == 32);
    return 0;
}

// ---------------- 开机解密：只做一次 ----------------
uint8_t Secret_Init(void) {
    if (s_inited) return s_state;
    s_inited = true;

#ifdef SECRET_DEMO
    s_state = 3;                                   // 演示模式：没有真实凭据
    return s_state;
#else
    const size_t clen = sizeof(SECRET_CIPHER);
    if (clen == 0 || (clen % 16) != 0 || clen > SECRET_PLAIN_MAX) {
        s_state = 2;
        return s_state;
    }

    uint8_t plain[SECRET_PLAIN_MAX];
    uint8_t iv[16];
    memcpy(iv, SECRET_IV, sizeof(iv));

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);

    int rc = mbedtls_aes_setkey_dec(&ctx, SECRET_KEY, 128);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, clen, iv,
                                   SECRET_CIPHER, plain);
    }
    mbedtls_aes_free(&ctx);

    if (rc != 0) {
        s_state = 1;                               // 密钥不对 / 密文损坏
        return s_state;
    }

    // PKCS#7 去填充：填充字节必须 1~16 且全部相同
    uint8_t pad = plain[clen - 1];
    if (pad == 0 || pad > 16 || (size_t)pad > clen) {
        s_state = 1;
        return s_state;
    }
    for (size_t i = 0; i < pad; i++) {
        if (plain[clen - 1 - i] != pad) {
            s_state = 1;
            return s_state;
        }
    }

    s_state = ParsePayload(plain, clen - pad);
    return s_state;
#endif
}

bool Secret_IsConfigured(void) {
    return s_configured;
}

const char* Secret_WifiSsid(void) {
    return s_ssid;
}

const char* Secret_WifiPassword(void) {
    return s_pass;
}

const char* Secret_BemfaUid(void) {
    return s_uid;
}

const char* Secret_ErrorName(void) {
    switch (s_state) {
        case 0:  return "已解密加载";
        case 1:  return "解密失败（检查 secrets.ini 的 passphrase 或重新编译）";
        case 2:  return "密钥数据格式错误";
        default: return "演示模式（未配置 secrets.ini）";
    }
}
