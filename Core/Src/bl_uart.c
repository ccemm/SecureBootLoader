/**
  * @file    bl_uart.c
  * @brief   Interrupt driven UART receive ring buffer and blocking transmit.
  *
  * The RX interrupt only pushes bytes into a ring buffer; frames are parsed
  * in the main loop (spec section 10). USART1_IRQHandler lives here, so do
  * not enable the USART1 global interrupt in CubeMX (it would generate a
  * second handler in stm32l1xx_it.c).
  */
#include "bl_uart.h"

#define RX_BUF_SIZE 512U   /* power of two */

static volatile uint8_t  rx_buf[RX_BUF_SIZE];
static volatile uint16_t rx_head;   /* written by the ISR */
static volatile uint16_t rx_tail;   /* written by the main loop */
static UART_HandleTypeDef *bl_huart;

void bl_uart_init(UART_HandleTypeDef *huart)
{
    bl_huart = huart;
    rx_head = 0;
    rx_tail = 0;

    // Drop anything received before now (also clears ORE)
    (void)huart->Instance->SR;
    (void)huart->Instance->DR;

    __HAL_UART_ENABLE_IT(huart, UART_IT_RXNE);
    HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

bool bl_uart_getc(uint8_t *b)
{
    uint16_t tail = rx_tail;

    if (tail == rx_head)
        return false;

    *b = rx_buf[tail];
    rx_tail = (tail + 1U) & (RX_BUF_SIZE - 1U);
    return true;
}

void bl_uart_send(const uint8_t *d, uint16_t n)
{
    // 271 bytes take ~24 ms at 115200 baud; HAL_UART_Transmit waits for TC at the end
    HAL_UART_Transmit(bl_huart, (uint8_t *)d, n, 100);
}

void USART1_IRQHandler(void)
{
    uint32_t sr = bl_huart->Instance->SR;

    if (sr & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE)) {
        // Reading SR then DR clears RXNE and the error flags
        uint8_t b = (uint8_t)bl_huart->Instance->DR;

        if (sr & USART_SR_RXNE) {
            uint16_t next = (rx_head + 1U) & (RX_BUF_SIZE - 1U);
            if (next != rx_tail) {          // drop the byte if the buffer is full
                rx_buf[rx_head] = b;
                rx_head = next;
            }
        }
    }
}
