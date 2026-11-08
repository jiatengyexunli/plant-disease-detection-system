#include "bh1750.h"
#include "i2c.h"

// I2C总线恢复：手动时钟脉冲释放SDA
static void BH1750_I2C_Recovery(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    // 先反初始化I2C，释放引脚控制
    HAL_I2C_DeInit(&hi2c1);

    // SCL设为输出，手动产生时钟脉冲
    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    // SDA设为输入（浮空）
    GPIO_InitStruct.Pin = GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    // 最多发9个时钟脉冲，直到SDA被释放
    for (int i = 0; i < 9; i++)
    {
        if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_SET)
            break;
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
        for (volatile int d = 0; d < 50; d++);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
        for (volatile int d = 0; d < 50; d++);
    }

    // 产生STOP条件：SDA从低到高
    GPIO_InitStruct.Pin = GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET);
    for (volatile int d = 0; d < 50; d++);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);

    // 重新初始化I2C外设
    HAL_I2C_Init(&hi2c1);
}

HAL_StatusTypeDef BH1750_Init(void)
{
    uint8_t opecode = BH1750_POWER_ON;
    return BH1750_WriteOpecode(&opecode, 1);
}

HAL_StatusTypeDef BH1750_WriteOpecode(uint8_t* pData, uint16_t size)
{
    HAL_StatusTypeDef status = HAL_I2C_Master_Transmit(&BH1750_PORT, BH1750_ADDRESS, pData, size, 100);
    if (status != HAL_OK)
    {
        BH1750_I2C_Recovery();
    }
    return status;
}

HAL_StatusTypeDef BH1750_ReadData(uint8_t* pData, uint16_t size)
{
    HAL_StatusTypeDef status = HAL_I2C_Master_Receive(&BH1750_PORT, BH1750_ADDRESS, pData, size, 100);
    if (status != HAL_OK)
    {
        BH1750_I2C_Recovery();
    }
    return status;
}

// ===================== 纯整数获取光照 =====================
static uint16_t last_valid_lux = 0;

uint16_t BH1750_GetLux(void)
{
    uint8_t buf[2] = {0};
    uint32_t data;
    uint16_t lux;

    uint8_t cmd = BH1750_H_RES_MODE;
    BH1750_WriteOpecode(&cmd, 1);

    HAL_Delay(180);

    // 最多重试3次，失败时恢复I2C总线
    for (int retry = 0; retry < 3; retry++)
    {
        if (BH1750_ReadData(buf, 2) == HAL_OK)
        {
            data = ((uint32_t)buf[0] << 8) | buf[1];
            lux = (uint16_t)((data * 5) / 6);

            // 0 lx 是合法测量值（黑暗环境），不能把 0 当成读取失败。
            last_valid_lux = lux;
            return lux;
        }
        HAL_Delay(10);
    }

    return last_valid_lux;
}
