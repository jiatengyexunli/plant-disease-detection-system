#include "dht11.h"

/*
 * 使用 Cortex-M3 的 DWT 周期计数器产生微秒级延时。
 * 原来的空循环在 -O0 下也许碰巧可用，但在 -Os/Release 下可能被优化，
 * 导致 DHT11 的单总线时序完全失效。
 */
static void delay_us(uint32_t us)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t ticks = us * (SystemCoreClock / 1000000U);
  while ((DWT->CYCCNT - start) < ticks) {
    __NOP();
  }
}

static void DHT11_Rst(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = DHT11_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(DHT11_PORT, &GPIO_InitStruct);

  HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_RESET);
  HAL_Delay(20);
  HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_SET);
  delay_us(30);
}

static uint8_t DHT11_Check(void)
{
  uint8_t retry = 0;
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = DHT11_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(DHT11_PORT, &GPIO_InitStruct);

  while(HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) && retry < 100)
  {
    retry++;
    delay_us(1);
  }
  if(retry >= 100) return 1;

  retry = 0;
  while(!HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) && retry < 100)
  {
    retry++;
    delay_us(1);
  }
  if(retry >= 100) return 1;
  return 0;
}

static uint8_t DHT11_Read_Bit(void)
{
  uint8_t retry = 0;
  while(HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) && retry < 100)
  {
    retry++;
    delay_us(1);
  }
  retry = 0;
  while(!HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN) && retry < 100)
  {
    retry++;
    delay_us(1);
  }
  delay_us(40);
  if(HAL_GPIO_ReadPin(DHT11_PORT, DHT11_PIN)) return 1;
  else return 0;
}

static uint8_t DHT11_Read_Byte(void)
{
  uint8_t i, dat = 0;
  for(i=0; i<8; i++)
  {
    dat <<= 1;
    dat |= DHT11_Read_Bit();
  }
  return dat;
}

void DHT11_Init(void)
{
  __HAL_RCC_GPIOA_CLK_ENABLE();
  // 使能 DWT 周期计数器，供 delay_us 使用。
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = DHT11_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(DHT11_PORT, &GPIO_InitStruct);
  HAL_GPIO_WritePin(DHT11_PORT, DHT11_PIN, GPIO_PIN_SET);
}

uint8_t DHT11_Read_Data(uint8_t *temp, uint8_t *humi)
{
  uint8_t buf[5];
  DHT11_Rst();
  if(DHT11_Check() == 0)
  {
    for(int i=0; i<5; i++) buf[i] = DHT11_Read_Byte();
    if(buf[0]+buf[1]+buf[2]+buf[3] == buf[4])
    {
      *humi = buf[0];
      *temp = buf[2];
      return 0;
    }
  }
  return 1;
}
