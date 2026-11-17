// ============================================================
//  User_Console.h —— 串口命令台（声明）
//  这个文件只回答一个问题：「命令台对外提供什么」；实现在 User_Console.cpp。
// ============================================================
#ifndef USER_CONSOLE_H
#define USER_CONSOLE_H

#include <Arduino.h>

#define CONSOLE_LINE_MAX   64      // 一行命令最多多少字节（含结尾 '\0'）

void Console_Init(void);           // 开机一次：清空接收缓冲
void Console_Poll(void);           // 每次 loop：收字节 → 拼一行 → 执行（★ 绝不阻塞）

#endif
