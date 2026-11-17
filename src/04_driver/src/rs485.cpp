// ============================================================
//  rs485.cpp —— RS485 物理层实现：把 UART1 打开就够了
//
//  ★ 为什么只有这么几行：半双工收发切换由模块自动完成，
//    组帧和收发时序由 ModbusMaster 库完成 —— 物理层真的只剩"开串口"。
// ============================================================

#include "../inc/rs485.h"
#include "../inc/uart.h"

void RS485_Init(void) {
    // 9600 8N1：8 位数据、无校验、1 位停止 —— 与从站出厂值一致
    UART1.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    delay(50);                                   // 等 UART 稳定（setup 里允许阻塞）

    while (UART1.available()) (void)UART1.read(); // 清掉上电瞬间的杂字节，免得污染第一帧

    UART0.printf("RS485 就绪 : %lu 8N1 | RX=IO%d TX=IO%d\n",
                 (unsigned long)RS485_BAUD, RS485_RX_PIN, RS485_TX_PIN);
}
