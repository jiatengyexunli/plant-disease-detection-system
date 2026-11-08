#ifndef __BH1750_H
#define __BH1750_H

#include "stm32f1xx_hal.h"

#define BH1750_ADDRESS     0x46
#define BH1750_PORT        hi2c1

#define BH1750_POWER_ON    0x01
#define BH1750_H_RES_MODE  0x10

HAL_StatusTypeDef BH1750_Init(void);
HAL_StatusTypeDef BH1750_WriteOpecode(uint8_t* pData, uint16_t size);
HAL_StatusTypeDef BH1750_ReadData(uint8_t* pData, uint16_t size);
uint16_t BH1750_GetLux(void);  // 这里返回整数！

#endif