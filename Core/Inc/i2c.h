#ifndef __I2C_H__
#define __I2C_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

extern I2C_HandleTypeDef hi2c1;

/* USER CODE BEGIN Private defines */

/* Отдельный результат HAL для каждого устройства. HAL_OK означает ACK. */
typedef struct {
    HAL_StatusTypeDef pca9538;
    HAL_StatusTypeDef eeprom;
    HAL_StatusTypeDef oled;
} I2C1_ProbeResult;

/* USER CODE END Private defines */

void MX_I2C1_Init(void);

/* USER CODE BEGIN Prototypes */
I2C1_ProbeResult I2C1_ProbeBoard(void);
/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif

#endif /* __I2C_H__ */
