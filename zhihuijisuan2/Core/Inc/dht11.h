#ifndef __DHT11_H
#define __DHT11_H
#include "stm32f1xx_hal.h"

#define DHT11_PIN GPIO_PIN_8
#define DHT11_PORT GPIOA

void DHT11_Init(void);
uint8_t DHT11_Read_Data(uint8_t *temp, uint8_t *humi);

#endif