/**
  * @file    bl_debug.c
  * @brief   Debug log on USART2. Each line: "[  tick ms] message\r\n".
  */
#include "bl_debug.h"

#if BL_DEBUG

#include <stdarg.h>
#include <stdio.h>

static UART_HandleTypeDef *dbg_huart;

void bl_debug_init(UART_HandleTypeDef *huart)
{
    dbg_huart = huart;
}

void bl_log(const char *fmt, ...)
{
    char line[128];
    va_list ap;
    int n;

    if (dbg_huart == NULL)
        return;

    n = snprintf(line, sizeof(line), "[%7lu] ", HAL_GetTick());
    va_start(ap, fmt);
    n += vsnprintf(&line[n], sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    if (n > (int)sizeof(line) - 3)          // truncated, keep room for CR LF
        n = (int)sizeof(line) - 3;
    line[n++] = '\r';
    line[n++] = '\n';

    HAL_UART_Transmit(dbg_huart, (uint8_t *)line, (uint16_t)n, 50);
}

#endif /* BL_DEBUG */
