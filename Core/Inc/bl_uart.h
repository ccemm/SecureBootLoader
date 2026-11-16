/**
  * @file    bl_uart.h
  * @brief   Interrupt driven UART receive ring buffer and blocking transmit.
  */
#ifndef BL_UART_H
#define BL_UART_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32l1xx_hal.h"

/* Start interrupt reception on USART1 (handle must already be initialised) */
void bl_uart_init(UART_HandleTypeDef *huart);

/* Fetch one received byte. Returns false when the buffer is empty. */
bool bl_uart_getc(uint8_t *b);

/* Blocking send, returns once the last bit has left the shifter (TC set) */
void bl_uart_send(const uint8_t *d, uint16_t n);

#endif /* BL_UART_H */
