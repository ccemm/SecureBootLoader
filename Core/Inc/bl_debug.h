/**
  * @file    bl_debug.h
  * @brief   Debug log on USART2 (115200 8N1). The protocol runs on USART1 only.
  *
  * Output is blocking, so call it from the main loop only, never from an ISR.
  * Set BL_DEBUG to 0 in bl_config.h to compile all logging out.
  */
#ifndef BL_DEBUG_H
#define BL_DEBUG_H

#include "bl_config.h"
#include "stm32l1xx_hal.h"

#if BL_DEBUG
void bl_debug_init(UART_HandleTypeDef *huart);
void bl_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
#define bl_debug_init(huart)  ((void)(huart))
#define bl_log(...)           ((void)0)
#endif

#endif /* BL_DEBUG_H */
