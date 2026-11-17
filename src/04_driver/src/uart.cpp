// ============================================================
//  uart.cpp —— 调试串口初始化（只在 setup 里调一次）
// ============================================================

#include <Arduino.h>
#include "../inc/uart.h"

void UART_Init(unsigned long baud_rate) {
    UART0.begin(baud_rate);                // UART0 = USB 串口
    delay(1000);                           // 等串口监视器打开（setup 里允许阻塞）
    UART0.println("UART0(USB) 就绪");
}

