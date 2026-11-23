#ifndef OLED_H
#define OLED_H
#include "i2c.h"
#include <stdbool.h>

/* Сбрасывает OLED через PCA9538 и настраивает SSD1306 128x64. */
HAL_StatusTypeDef OLED_Init(void);
/* Очищает буфер в RAM; само изображение пока не передаётся. */
void OLED_Clear(void);
/* Рисует ASCII-текст шрифтом 7x10; scale = 1 или 2. Координаты в пикселях. */
void OLED_Text(unsigned x, unsigned y, const char *text, unsigned scale);
/* Начинает передачу нарисованного кадра. Буфер менять после OLED_Busy() == false. */
void OLED_Present(void);
bool OLED_Busy(void);
/* Передаёт один небольшой фрагмент. Вызывать в main, не из прерывания. */
void OLED_Update(void);
/* HAL_OK означает успешную инициализацию/последнюю передачу, не проверку картинки. */
HAL_StatusTypeDef OLED_GetStatus(void);
#endif
