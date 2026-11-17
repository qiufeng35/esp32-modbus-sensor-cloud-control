#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_secrets.py —— 把本地 secrets.ini 加密成固件可解密的 C 头文件。

流程：
    secrets.ini（本地，已 gitignore）
        │  AES-128-CBC + PBKDF2-HMAC-SHA256
        ▼
    src/05_secrets/gen/secret_blob.h（生成物，已 gitignore）
        │  编译进固件
        ▼
    ESP32 启动时用 mbedTLS 解密，只在内存里还原 WiFi 账号密码与巴法云私钥

找不到 secrets.ini 时生成「演示模式」头文件：不含任何真实凭据，
工程照样能编译、能烧录，只是不会去连 WiFi / 连巴法云。

注意：解密所需的密钥材料（KEY/IV/密文）最终都在固件里，这层加密用于
避免凭据以明文出现在仓库、截图和源码里；它不能替代 ESP32 的 Flash 加密。
"""

import hashlib
import os
import sys
from configparser import ConfigParser
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from aes128 import BLOCK, cbc_encrypt  # noqa: E402

GEN_DIR = ROOT / "src" / "05_secrets" / "gen"
HEADER = GEN_DIR / "secret_blob.h"
STATE = GEN_DIR / ".secrets.state"
INI = ROOT / "secrets.ini"

FORMAT_VERSION = 1
ITERATIONS = 20000
DEMO_MARK = "demo-mode: no secrets.ini"

REQUIRED_KEYS = ("wifi_ssid", "wifi_password", "bemfa_uid")
PLACEHOLDER_MARKS = ("你的", "请改", "改成", "填写", "example")


def fail(msg):
    print("\n[secrets] ✗ %s\n" % msg, file=sys.stderr)
    sys.exit(1)


def read_ini():
    """读取 secrets.ini；不存在返回 None（演示模式）。"""
    if not INI.exists():
        return None

    cfg = ConfigParser(interpolation=None)
    cfg.optionxform = str
    try:
        if not cfg.read(INI, encoding="utf-8-sig"):
            fail("无法读取 secrets.ini（检查文件编码，建议 UTF-8）")
    except Exception as exc:  # 解析失败时给出可操作提示
        fail("secrets.ini 格式错误：%s" % exc)

    if not cfg.has_section("secrets"):
        fail("secrets.ini 缺少 [secrets] 段落，请对照 secrets.example.ini")

    section = cfg["secrets"]
    values = {}
    for key in REQUIRED_KEYS:
        values[key] = section.get(key, "").strip()

    passphrase = section.get("passphrase", "").strip()
    if not passphrase:
        fail("secrets.ini 里的 passphrase 不能为空（它是加密口令，建议 16 位以上）")
    if len(passphrase) < 8:
        fail("passphrase 太短（至少 8 位，建议 16 位以上随机字符串）")

    for key in REQUIRED_KEYS:
        if not values[key]:
            fail("secrets.ini 里的 %s 不能为空" % key)
        for mark in PLACEHOLDER_MARKS:
            if mark in values[key]:
                fail("secrets.ini 里的 %s 还是示例占位内容，请填入真实值" % key)

    # ---- 长度检查（按 UTF-8 字节数，跟 WiFi / 巴法云的真实限制一致）----
    ssid = values["wifi_ssid"].encode("utf-8")
    password = values["wifi_password"].encode("utf-8")
    uid = values["bemfa_uid"].encode("utf-8")

    if len(ssid) > 32:
        fail("WiFi 名字超过 32 字节（UTF-8）")
    if len(password) > 63:
        fail("WiFi 密码超过 63 字节")
    if len(uid) != 32 or any(c not in b"0123456789abcdefABCDEF" for c in uid):
        fail("bemfa_uid 必须是 32 位十六进制字符串（巴法云「连接密钥」页）")
    for name, raw in (("wifi_ssid", ssid), ("wifi_password", password),
                      ("bemfa_uid", uid)):
        if len(raw) > 255:
            fail("%s 太长" % name)

    return passphrase, ssid, password, uid


def build_payload(ssid, password, uid):
    """二进制载荷：'MBES' + 版本号 + 3 个「长度 + 内容」。"""
    payload = bytearray(b"MBES")
    payload.append(FORMAT_VERSION)
    for field in (ssid, password, uid):
        payload.append(len(field))
        payload += field
    return bytes(payload)


def emit_header(lines):
    GEN_DIR.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines) + "\n"
    tmp = HEADER.with_suffix(".tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, HEADER)


def bytes_literal(name, data):
    body = ", ".join("0x%02X" % b for b in data)
    return [
        "static const uint8_t %s[%d] = { %s };" % (name, len(data), body),
    ]


def gen_demo():
    emit_header([
        "// ============================================================",
        "//  secret_blob.h —— 演示模式（由 tools/gen_secrets.py 生成）",
        "//  未找到 secrets.ini，因此不包含任何真实凭据。",
        "//  按 README「密钥加密」创建 secrets.ini 后重新编译即可替换本文件。",
        "// ============================================================",
        "#pragma once",
        "#define SECRET_DEMO 1",
        "#define SECRET_FORMAT_VERSION %d" % FORMAT_VERSION,
    ])
    STATE.write_text(DEMO_MARK, encoding="utf-8")
    print("[secrets] 未找到 secrets.ini → 生成演示模式头文件：%s" %
          HEADER.relative_to(ROOT))
    print("[secrets] 演示模式不含真实凭据：烧录后不会连 WiFi / 巴法云")


def gen_real(passphrase, ssid, password, uid):
    payload = build_payload(ssid, password, uid)
    salt = os.urandom(16)
    key = hashlib.pbkdf2_hmac("sha256", passphrase.encode("utf-8"), salt,
                              ITERATIONS, 16)
    iv = os.urandom(BLOCK)
    cipher = cbc_encrypt(key, iv, payload)

    lines = [
        "// ============================================================",
        "//  secret_blob.h —— 由 tools/gen_secrets.py 自动生成，请勿提交到 Git！",
        "//  内容：WiFi 账号密码 + 巴法云私钥（AES-128-CBC 加密后的密文与密钥材料）。",
        "//  修改 secrets.ini 后重新编译，本文件会自动重新生成。",
        "// ============================================================",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "#define SECRET_FORMAT_VERSION %d" % FORMAT_VERSION,
        "#define SECRET_PBKDF2_ITER %d" % ITERATIONS,
        "",
    ]
    lines += bytes_literal("SECRET_SALT", salt)
    lines += bytes_literal("SECRET_IV", iv)
    lines += bytes_literal("SECRET_KEY", key)
    lines += bytes_literal("SECRET_CIPHER", cipher)
    emit_header(lines)

    digest = hashlib.sha256(passphrase.encode("utf-8") + b"\x00" +
                            payload).hexdigest()
    STATE.write_text(digest, encoding="utf-8")

    print("[secrets] 已生成 %s" % HEADER.relative_to(ROOT))
    print("[secrets]   WiFi SSID：已配置（%d 字节，不在日志中显示）" % len(ssid))
    print("[secrets]   WiFi 密码：已配置（%d 字节）" % len(password))
    print("[secrets]   巴法云私钥：已配置（32 位）")
    print("[secrets]   加密：AES-128-CBC + PBKDF2-HMAC-SHA256(%d 次)" % ITERATIONS)
    print("[secrets]   明文只存在于内存与 secret_blob.h 的加密结果中")


def changed_inputs(passphrase, ssid, password, uid):
    """输入没变就跳过重写，避免每次编译都因随机盐/IV 触发重新编译。"""
    if not HEADER.exists() or not STATE.exists():
        return True
    digest = hashlib.sha256(passphrase.encode("utf-8") + b"\x00" +
                            build_payload(ssid, password, uid)).hexdigest()
    try:
        return STATE.read_text(encoding="utf-8").strip() != digest
    except OSError:
        return True


def main():
    parsed = read_ini()
    if parsed is None:
        gen_demo()
        return
    passphrase, ssid, password, uid = parsed
    if not changed_inputs(passphrase, ssid, password, uid):
        print("[secrets] secrets.ini 未变化，沿用已有加密头文件")
        return
    gen_real(passphrase, ssid, password, uid)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        raise
