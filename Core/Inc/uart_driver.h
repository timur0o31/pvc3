#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define UART_RECEIVE_BUFFER_SIZE 256
#define UART_SEND_BUFFER_SIZE 512

/* Вызывать после MX_USART6_UART_Init(). */
void UART_DriverInit(void);

void UART_Service(void);

int UART_ReadByte(void);

bool UART_Write(const void *data, size_t length);

bool UART_Print(const char *text);

bool UART_InterruptsEnabled(void);

bool UART_SetInterrupts(bool enabled);

uint32_t UART_RxErrors(void);
uint32_t UART_DiscardRx(void);

#endif
