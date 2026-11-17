// ============================================================
//  uart.h —— 两个串口的分工
//
//    UART0 = Serial  → USB 接电脑：打印日志、收命令
//    UART1 = Serial1 → 接 RS485 模块：跟总线上的从站说话
//  ★ 写成宏是为了防止"把调试日志发到 485 总线上"这种事故。
// ============================================================
#ifndef UART_H
#define UART_H

#include <Arduino.h>

#define UART0 Serial          // 调试串口（USB），串口监视器用的就是它
#define UART1 Serial1         // 接 RS485 模块的硬件串口

void UART_Init(unsigned long baud_rate);   // 开机一次：打开 UART0 并打印"就绪"

#endif

