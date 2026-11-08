/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include "dht11.h"
#include "bh1750.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
// DMA句柄外部声明
extern DMA_HandleTypeDef hdma_usart1_rx;
extern DMA_HandleTypeDef hdma_usart1_tx;
extern DMA_HandleTypeDef hdma_usart2_rx;
extern DMA_HandleTypeDef hdma_usart2_tx;

// ==================== 串口中断透传配置 ====================
#define RX_BUFFER_SIZE    4096       // 接收环形缓冲区大小
#define TX_BUFFER_SIZE    1024        // 发送缓冲区大小

// 接收环形缓冲区（来自K210的数据）
uint8_t rx_buffer[RX_BUFFER_SIZE];
// USART2 中断发送必须使用独立副本：发送期间仍会有新图片字节写入接收环形区。
static uint8_t uart2_tx_buffer[TX_BUFFER_SIZE];
volatile uint16_t rx_head = 0;      // 缓冲区头部（写入位置）
volatile uint16_t rx_tail = 0;      // 缓冲区尾部（读取位置）
volatile uint8_t tx_busy = 0;       // USART2发送忙标志
volatile uint32_t tx_start_time = 0; // 发送开始时间（用于超时检测）
volatile uint32_t rx_overflow_count = 0; // 接收溢出计数
volatile uint32_t uart2_tc_count = 0; // USART2发送完成计数

// 图片传输检测
volatile uint8_t image_transmitting = 0; // 是否正在传输图片
volatile uint8_t jpeg_start_detected = 0; // 是否检测到JPEG起始标记
volatile uint32_t image_data_received = 0; // 已接收的图片数据长度
uint8_t uart1_rx_byte = 0; // USART1接收字节缓冲区

// 状态变化标志（在主循环中检查并输出）
volatile uint8_t image_start_flag = 0; // 图片开始标志
volatile uint8_t image_end_flag = 0;   // 图片结束标志
volatile uint32_t image_end_size = 0;  // 图片结束时的数据大小
volatile uint32_t image_start_tick = 0; // JPEG 开始时的 HAL tick，用于超时恢复
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void forward_data_from_rx_buffer(void);
void debug_printf(const char* fmt, ...);
void sendDataToK210(uint8_t temp, uint8_t humi, uint16_t light, int soil);
void sendDataToESP(uint8_t temp, uint8_t humi, uint16_t light, int soil);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// 调试函数
void debug_printf(const char* fmt, ...) {
    char buffer[64];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    // vsnprintf 返回“本应写入”的长度，可能大于真实缓冲区，必须截断。
    if (len >= (int)sizeof(buffer)) {
        len = (int)sizeof(buffer) - 1;
    }
    if (len > 0) {
        HAL_UART_Transmit(&huart1, (uint8_t*)buffer, len, 100);
    }
}

// 发送传感器数据给 K210
void sendDataToK210(uint8_t temp, uint8_t humi, uint16_t light, int soil)
{
    char data[64];
    sprintf(data, "DATA:%d,%d,%d,%d\n", temp, humi, soil, light);
    HAL_UART_Transmit(&huart1, (uint8_t*)data, (uint16_t)strlen(data), 100);
}

// 发送传感器数据给 ESP（带 SENSOR: 前缀）
void sendDataToESP(uint8_t temp, uint8_t humi, uint16_t light, int soil)
{
    char json[200];
    /*
     * 字段名必须与接口文档一致。ESP8266 会把 createTime 的占位值替换成
     * NTP 时间，然后去掉 SENSOR: 前缀再 POST 到 /machine/machineRecord。
     */
    snprintf(json, sizeof(json),
             "SENSOR:{\"id\":1,\"airTemp\":%u,\"airHumidity\":%u,"
             "\"soilMoisture\":%d,\"lightIntensity\":%u,"
             "\"createTime\":\"1970-01-01T00:00:00.000Z\"}\n",
             (unsigned int)temp, (unsigned int)humi, soil,
             (unsigned int)light);
    HAL_UART_Transmit(&huart2, (uint8_t*)json, (uint16_t)strlen(json), 100);
}

// 从接收环形缓冲区转发数据到USART2
void forward_data_from_rx_buffer(void)
{
    // 如果USART2忙或缓冲区为空，则返回
    if (tx_busy || rx_tail == rx_head)
        return;

    // 计算可读取的数据量
    uint16_t bytes_to_send;
    if (rx_head > rx_tail)
    {
        // 简单情况：数据在连续区域
        bytes_to_send = rx_head - rx_tail;
    }
    else
    {
        // 数据被回绕：从rx_tail到缓冲区末尾
        bytes_to_send = RX_BUFFER_SIZE - rx_tail;
    }

    // 限制每次发送的数据量，避免阻塞太久
    if (bytes_to_send > TX_BUFFER_SIZE)
        bytes_to_send = TX_BUFFER_SIZE;

    // 如果bytes_to_send为0（不应该发生，但安全起见）
    if (bytes_to_send == 0)
        return;

    /*
     * 先把本次要发送的数据复制到独立缓冲区，再推进 rx_tail。
     * 如果直接把 &rx_buffer[rx_tail] 交给 HAL，在约 90 ms 的发送期间，
     * K210 新数据可能回绕并覆盖尚未发送的图片字节。
     */
    if (rx_head >= rx_tail) {
        memcpy(uart2_tx_buffer, &rx_buffer[rx_tail], bytes_to_send);
    } else {
        uint16_t first_part = RX_BUFFER_SIZE - rx_tail;
        if (first_part > bytes_to_send) {
            first_part = bytes_to_send;
        }
        memcpy(uart2_tx_buffer, &rx_buffer[rx_tail], first_part);
        if (bytes_to_send > first_part) {
            memcpy(&uart2_tx_buffer[first_part], &rx_buffer[0],
                   bytes_to_send - first_part);
        }
    }

    tx_busy = 1;
    tx_start_time = HAL_GetTick();
    HAL_StatusTypeDef status = HAL_UART_Transmit_IT(&huart2, uart2_tx_buffer, bytes_to_send);

    if (status != HAL_OK)
    {
        debug_printf("[ERROR] UART2 transmit failed: %d\n", status);
        tx_busy = 0;
        return;
    }

    // 更新尾部指针
    rx_tail = (rx_tail + bytes_to_send) % RX_BUFFER_SIZE;
    // 移除调试输出以提高性能
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_I2C1_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  uint8_t opeCode;

  DHT11_Init();
  BH1750_Init();

  opeCode = 0x10;
  BH1750_WriteOpecode(&opeCode, 1);

  // 发送启动消息
  debug_printf("[STM32] Startup - Interrupt mode, RX buffer size=%d\n", RX_BUFFER_SIZE);

  // 确保DMA被禁用（防止DMA干扰中断模式）
  HAL_DMA_DeInit(&hdma_usart1_rx);
  HAL_DMA_DeInit(&hdma_usart1_tx);
  HAL_DMA_DeInit(&hdma_usart2_rx);
  HAL_DMA_DeInit(&hdma_usart2_tx);

  // 确保USART1的DMA接收被禁用
  CLEAR_BIT(huart1.Instance->CR3, USART_CR3_DMAR);
  CLEAR_BIT(huart1.Instance->CR3, USART_CR3_DMAT);

  // 确保USART2的DMA发送被禁用
  CLEAR_BIT(huart2.Instance->CR3, USART_CR3_DMAT);

  debug_printf("[STM32] DMA disabled, starting interrupt mode\n");

  // 启动 USART1 中断接收（每个字节触发）
  HAL_UART_Receive_IT(&huart1, &uart1_rx_byte, 1);
  // 启用空闲中断（可选）
  __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    static uint32_t last_sensor_time = 0;
    static uint32_t last_status_time = 0;
    static uint32_t last_debug_time = 0;
    static uint32_t last_buffer_size = 0;
    uint8_t temp = 0, humi = 0;
    uint16_t light = 0;
    uint16_t soil_adc = 0;
    int soil = 0;

    // 检查发送超时（500ms）
    if (tx_busy && (HAL_GetTick() - tx_start_time > 500))
    {
        // 发送超时，终止底层发送并恢复状态机，避免永久卡在 tx_busy。
        debug_printf("[TIMEOUT] UART2 send timeout, resetting\n");
        HAL_UART_AbortTransmit(&huart2);
        tx_busy = 0;
        // DMA循环模式一直在运行，不需要重新启动
    }

    // 转发接收缓冲区中的数据到USART2
    forward_data_from_rx_buffer();

    // 检查图片开始/结束标志
    if (image_start_flag)
    {
        debug_printf("[IMG] Start detected\n");
        image_start_tick = HAL_GetTick();
        image_start_flag = 0;
    }

    if (image_end_flag)
    {
        debug_printf("[IMG] End detected, total %lu bytes\n", image_end_size);
        image_end_flag = 0;
        image_end_size = 0;
    }

    // 如果 JPEG 中途断开，image_transmitting 不能永久保持为 1。
    if (image_transmitting && (HAL_GetTick() - image_start_tick > 10000))
    {
        debug_printf("[IMG] Transfer stalled, state reset\n");
        image_transmitting = 0;
        jpeg_start_detected = 0;
    }

    // 每30秒报告状态
    if (HAL_GetTick() - last_status_time > 30000)
    {
        debug_printf("[STATUS] RXovfl=%lu, U2TC=%lu, busy=%d, img=%d, jpeg=%d\n",
                    rx_overflow_count, uart2_tc_count,
                    tx_busy, image_transmitting, jpeg_start_detected);
        last_status_time = HAL_GetTick();
    }

    // 每10秒快速调试输出（如果接收缓冲区有数据）
    if (HAL_GetTick() - last_debug_time > 10000)
    {
        uint16_t buffer_size = (rx_head - rx_tail) % RX_BUFFER_SIZE;

        // 只在缓冲区大小变化超过100字节或从0变为有数据时输出
        if (buffer_size > 0 && ((buffer_size > last_buffer_size ? buffer_size - last_buffer_size : last_buffer_size - buffer_size) > 100 || last_buffer_size == 0))
        {
            debug_printf("[DEBUG] RX buff: %u bytes, busy=%d, img=%d\n",
                        buffer_size, tx_busy, image_transmitting);
            last_buffer_size = buffer_size;
        }

        // 检查是否有数据卡住（缓冲区很大但不在发送）
        if (buffer_size > 2000 && !tx_busy)
        {
            debug_printf("[WARN] Large buffer (%u bytes) but not busy\n", buffer_size);
        }

        last_debug_time = HAL_GetTick();
    }

    if (HAL_GetTick() - last_sensor_time >= 5000)
    {
        // 传感器读取失败时保留上一次有效值，避免把 0 当成真实温湿度上传。
        static uint8_t last_temp = 0;
        static uint8_t last_humi = 0;
        static int last_soil = 0;

        if (DHT11_Read_Data(&temp, &humi) == 0)
        {
            last_temp = temp;
            last_humi = humi;
        }
        else
        {
            temp = last_temp;
            humi = last_humi;
        }

        light = BH1750_GetLux();

        if (HAL_ADC_Start(&hadc1) == HAL_OK &&
            HAL_ADC_PollForConversion(&hadc1, 100) == HAL_OK)
        {
            soil_adc = HAL_ADC_GetValue(&hadc1);
            soil = ((4095 - soil_adc) * 100) / 4095;
            if (soil < 0) {
                soil = 0;
            } else if (soil > 100) {
                soil = 100;
            }
            last_soil = soil;
        }
        else
        {
            soil = last_soil;
        }

        // 发送给 K210
        sendDataToK210(temp, humi, light, soil);

        // 发送给 ESP（仅当没有图片转发时）
        if (!tx_busy && !image_transmitting)
        {
            sendDataToESP(temp, humi, light, soil);
        }

        last_sensor_time = HAL_GetTick();
    }

    HAL_Delay(1);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
// USART1 接收完成回调（每个字节触发）
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        // 接收到的字节存储在uart1_rx_byte中
        uint8_t data = uart1_rx_byte;

        // 字节计数（用于调试，不在中断中输出）
        static uint32_t rx_count = 0;
        rx_count++;

        // 计算下一个写入位置
        uint16_t next_head = (rx_head + 1) % RX_BUFFER_SIZE;

        // 检查缓冲区是否已满
        if (next_head != rx_tail)
        {
            // 缓冲区未满，存入数据
            rx_buffer[rx_head] = data;
            rx_head = next_head;

            // 检测JPEG起始标记 (0xFF, 0xD8)
            static uint8_t last_byte = 0;
            if (last_byte == 0xFF && data == 0xD8)
            {
                // 检测到JPEG起始标记
                jpeg_start_detected = 1;
                image_transmitting = 1;
                image_data_received = 2; // 当前已包含 0xFF 和 0xD8 两个字节
                image_start_flag = 1; // 设置开始标志
            }
            else if (jpeg_start_detected)
            {
                image_data_received++;
            }

            // 检测JPEG结束标记 (0xFF, 0xD9)
            if (last_byte == 0xFF && data == 0xD9)
            {
                // 检测到JPEG结束标记
                if (jpeg_start_detected)
                {
                    jpeg_start_detected = 0;
                    image_transmitting = 0;
                    image_end_flag = 1; // 设置结束标志
                    image_end_size = image_data_received; // 保存图片大小
                }
            }
            last_byte = data;
        }
        else
        {
            // 缓冲区溢出
            rx_overflow_count++;
            // 不在中断中输出错误信息，只增加计数
        }

        // 重新启动接收，等待下一个字节（使用同一个缓冲区）
        HAL_UART_Receive_IT(&huart1, &uart1_rx_byte, 1);
    }
}

// 串口溢出导致 HAL 接收停摆时，主动重新启动单字节接收。
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        HAL_UART_AbortReceive(huart);
        HAL_UART_Receive_IT(huart, &uart1_rx_byte, 1);
    }
}

// USART2 发送完成回调
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        uart2_tc_count++;  // 增加发送完成计数
        tx_busy = 0;  // 允许下一次发送
        // 不在中断中输出调试信息
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
