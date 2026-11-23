#include "uart_driver.h"
#include "stm32f4xx_hal.h"
#include <string.h>

static uint8_t receive_buffer[UART_RECEIVE_BUFFER_SIZE];
static uint8_t send_buffer[UART_SEND_BUFFER_SIZE];
static volatile uint16_t receive_head, receive_tail;
static volatile uint16_t send_head, send_tail;
static volatile uint32_t receive_errors;

static bool interrupts_enabled;

static uint32_t enter_critical(void)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    return mask;
}

static void receive_byte(void)
{
    uint32_t errors = USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE;
    uint32_t status = USART6->SR;
    if (!(status & (USART_SR_RXNE | errors))) return;
    uint8_t byte = USART6->DR;
    uint16_t next = receive_head + 1;
    if (next == UART_RECEIVE_BUFFER_SIZE){
    	next = 0;
    }
    if ((status & errors) || next == receive_tail) {
        receive_errors++;
    } else {
        receive_buffer[receive_head] = byte;
        receive_head = next;
    }
}

static void send_byte(void)
{
    if (!(USART6->SR & USART_SR_TXE)) return;

    if (send_head != send_tail) {
        USART6->DR = send_buffer[send_tail];
        send_tail++;
        if (send_tail == UART_SEND_BUFFER_SIZE){
        	send_tail = 0;
        }
    } else {
        USART6->CR1 &= ~USART_CR1_TXEIE;
    }
}

void UART_DriverInit(void)
{
    /* Скорость, формат кадра и пины уже настроены CubeMX. */
    HAL_NVIC_ClearPendingIRQ(USART6_IRQn);
    HAL_NVIC_SetPriority(USART6_IRQn, 5, 0);
    interrupts_enabled = true;
    USART6->CR1 |= USART_CR1_RXNEIE;
    HAL_NVIC_EnableIRQ(USART6_IRQn);
}

bool UART_InterruptsEnabled(void)
{
    return interrupts_enabled;
}

bool UART_SetInterrupts(bool enabled)
{
    if (enabled == interrupts_enabled)
    	return true;

    uint32_t mask = enter_critical();
    if (send_head != send_tail || !(USART6->SR & USART_SR_TC)) {
        __set_PRIMASK(mask);
        return false;
    }
    USART6->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_TXEIE | USART_CR1_TCIE | USART_CR1_PEIE);
    USART6->CR3 &= ~USART_CR3_EIE;
    HAL_NVIC_DisableIRQ(USART6_IRQn);
    HAL_NVIC_ClearPendingIRQ(USART6_IRQn);
    interrupts_enabled = enabled;
    if (enabled) {
        USART6->CR1 |= USART_CR1_RXNEIE;
        HAL_NVIC_EnableIRQ(USART6_IRQn);
    }
    __set_PRIMASK(mask);
    return true;
}

int UART_ReadByte(void)
{
    uint32_t mask = enter_critical();
    if (receive_head == receive_tail) {
        __set_PRIMASK(mask);
        return -1;
    }
    uint8_t byte = receive_buffer[receive_tail];
    receive_tail++;
    if (receive_tail == UART_RECEIVE_BUFFER_SIZE) {
        receive_tail = 0;
    }
    __set_PRIMASK(mask);
    return byte;
}


bool UART_Write(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t mask = enter_critical();
    /* Один элемент остаётся свободным, чтобы отличить полный буфер от пустого. */
    size_t free = (send_tail + UART_SEND_BUFFER_SIZE - send_head - 1u)
                  % UART_SEND_BUFFER_SIZE;
    if (length > free) {
        __set_PRIMASK(mask);
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        send_buffer[send_head] = bytes[i];
        send_head++;
        if (send_head == UART_SEND_BUFFER_SIZE)
        	send_head = 0;
    }
    if (interrupts_enabled && send_head != send_tail) {
        USART6->CR1 |= USART_CR1_TXEIE;
    }
    __set_PRIMASK(mask);
    return true;
}

bool UART_Print(const char *text)
{
    return UART_Write(text, strlen(text));
}

uint32_t UART_RxErrors(void)
{
    return receive_errors;
}

uint32_t UART_DiscardRx(void)
{
    uint32_t mask = enter_critical();
    receive_tail = receive_head;
    uint32_t errors = receive_errors;
    __set_PRIMASK(mask);
    return errors;
}

void UART_Service(void)
{
    if (!interrupts_enabled) {
        receive_byte();
        send_byte();
    }
}

/* Обработчик принадлежит этому драйверу. Генерация USART6 IRQ в .ioc
 * выключена; NVIC и источники прерываний настраиваются здесь. */
void USART6_IRQHandler(void)
{
    if (USART6->CR1 & USART_CR1_RXNEIE) receive_byte();
    if (USART6->CR1 & USART_CR1_TXEIE) send_byte();
}
