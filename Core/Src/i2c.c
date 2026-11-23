/* USER CODE BEGIN Header */
/* Шина I2C1 стенда SDK-1.1M. Транзакции запускаются из основного кода. */
/* USER CODE END Header */

#include "i2c.h"

/* USER CODE BEGIN 0 */

#define PCA9538_RESET_ADDRESS_7BIT 0x70u
#define MCP79411_EEPROM_ADDRESS_7BIT 0x57u
#define SSD1306_ADDRESS_7BIT 0x3Cu
#define PROBE_TRIALS 2u
#define PROBE_TIMEOUT_MS 10u

/* Посылает адрес устройства и ждёт подтверждение ACK. Содержимое устройства
 * при проверке не читается и не меняется. HAL принимает адрес со сдвигом
 * на один бит, так как младший бит адресного байта зарезервирован под R/W. */
static HAL_StatusTypeDef probe_device(uint8_t address_7bit)
{
  return HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)address_7bit << 1,
                               PROBE_TRIALS, PROBE_TIMEOUT_MS);
}

/* USER CODE END 0 */

I2C_HandleTypeDef hi2c1;

/* I2C1 init function */
void MX_I2C1_Init(void)
{
  /* USER CODE BEGIN I2C1_Init 0 */
  /* Настраивает I2C1 на Standard-mode 100 кГц и 7-битную адресацию.
   * STM32 начинает транзакции как ведущий, поэтому собственный адрес
   * и широковещательный режим здесь не используются. */
  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */
  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */
  /* USER CODE END I2C1_Init 2 */
}

void HAL_I2C_MspInit(I2C_HandleTypeDef *i2cHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if (i2cHandle->Instance == I2C1)
  {
    /* USER CODE BEGIN I2C1_MspInit 0 */
    /* HAL вызывает эту функцию из HAL_I2C_Init. Она включает тактирование
     * и подключает PB8/SCL и PB9/SDA к I2C1 через AF4. Режим open-drain
     * позволяет устройствам тянуть линию к нулю, а высокий уровень
     * обеспечивается подтяжкой. */
    /* USER CODE END I2C1_MspInit 0 */

    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    __HAL_RCC_I2C1_CLK_ENABLE();

    /* USER CODE BEGIN I2C1_MspInit 1 */
    /* USER CODE END I2C1_MspInit 1 */
  }
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef *i2cHandle)
{
  if (i2cHandle->Instance == I2C1)
  {
    /* USER CODE BEGIN I2C1_MspDeInit 0 */
    /* HAL вызывает эту функцию из HAL_I2C_DeInit. Она отключает
     * тактирование I2C1 и освобождает PB8/PB9. */
    /* USER CODE END I2C1_MspDeInit 0 */

    __HAL_RCC_I2C1_CLK_DISABLE();
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_8 | GPIO_PIN_9);

    /* USER CODE BEGIN I2C1_MspDeInit 1 */
    /* USER CODE END I2C1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* Проверяет три адреса на шине по очереди. Результаты независимы.
 * OLED может не отвечать, пока не выполнен его аппаратный сброс через PCA9538. */
I2C1_ProbeResult I2C1_ProbeBoard(void)
{
  I2C1_ProbeResult result;
  result.pca9538 = probe_device(PCA9538_RESET_ADDRESS_7BIT);
  result.eeprom = probe_device(MCP79411_EEPROM_ADDRESS_7BIT);
  result.oled = probe_device(SSD1306_ADDRESS_7BIT);
  return result;
}

/* USER CODE END 1 */
