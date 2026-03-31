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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "dwt_timer.h"
#include "heatshrink_encoder.h"
#include "heatshrink_decoder.h"
#include "stm32f1xx_hal_flash_ex.h"
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

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void        SystemClock_Config(void);
static void MX_GPIO_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// 压缩地址

#define ORIGIN_DATA_PAGE   (134)
#define CODE_PAGE          (5)
#define ORIGIN_DATA_ADDR   (0x8000000 + CODE_PAGE * PAGESIZE)
#define COMPRESS_DATA_ADDR (0x8000000 + (CODE_PAGE + ORIGIN_DATA_PAGE) * PAGESIZE)
#define COMPRESS_MAX_SIZE  (FLASH_BANK1_END + 1U - COMPRESS_DATA_ADDR)
volatile uint32_t outputBytes = 0;
#define ORIGIN_DATA_SIZE (272972)
#define COMPRESSED_BYTES (162103)

// 实现使用heatshrink算法将FLASH中以ORIGIN_DATA_ADDR地址开始的ORIGIN_DATA_SIZE字节数据压缩后存储到COMPRESS_DATA_ADDR地址,
// 并将压缩后的数据大小存储到outputBytes变量中
void compress_flash_data()
{
    // 原始输入数据位于 Flash 固定地址，按块送入编码器。
    const uint8_t *originData      = (const uint8_t *)ORIGIN_DATA_ADDR;
    size_t         inputOffset     = 0;
    size_t         compressedCount = 0;

    // 输出写入目标 Flash 区域：按页擦除、按半字(16-bit)编程。
    uint32_t flashWriteAddr = COMPRESS_DATA_ADDR;
    uint32_t nextEraseAddr  = COMPRESS_DATA_ADDR;
    uint32_t pageEndAddr    = COMPRESS_DATA_ADDR;
    uint32_t flashLimitAddr = COMPRESS_DATA_ADDR + COMPRESS_MAX_SIZE;

    // 编码器输出暂存缓冲；Flash 写半字时用 pendingByte 处理奇数字节对齐。
    uint8_t outputBuffer[64];
    uint8_t pendingByte    = 0;
    uint8_t hasPendingByte = 0;

    FLASH_EraseInitTypeDef EraseInit;
    EraseInit.TypeErase   = FLASH_TYPEERASE_PAGES;
    EraseInit.PageAddress = COMPRESS_DATA_ADDR;
    EraseInit.Banks       = 1;
    EraseInit.NbPages     = 1;
    uint32_t PageError    = 0;

    heatshrink_encoder encoder;
    heatshrink_encoder_reset(&encoder);

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        outputBytes = 0;
        return;
    }

    // 第一阶段：持续 sink 原始数据并 poll 编码输出。
    while (inputOffset < ORIGIN_DATA_SIZE)
    {
        size_t sinkSize   = 0;
        size_t inputChunk = ORIGIN_DATA_SIZE - inputOffset;
        if (inputChunk > HEATSHRINK_STATIC_INPUT_BUFFER_SIZE)
        {
            inputChunk = HEATSHRINK_STATIC_INPUT_BUFFER_SIZE;
        }

        if (heatshrink_encoder_sink(&encoder, (uint8_t *)&originData[inputOffset], inputChunk, &sinkSize) != HSER_SINK_OK || sinkSize == 0)
        {
            HAL_FLASH_Lock();
            outputBytes = 0;
            return;
        }
        inputOffset += sinkSize;

        while (1)
        {
            size_t       outputSize = 0;
            HSE_poll_res pollRes    = heatshrink_encoder_poll(&encoder, outputBuffer, sizeof(outputBuffer), &outputSize);
            if (pollRes < 0)
            {
                HAL_FLASH_Lock();
                outputBytes = 0;
                return;
            }

            for (size_t i = 0; i < outputSize; i++)
            {
                // 全局边界保护，防止压缩结果超出预留 Flash 区域。
                if (compressedCount >= COMPRESS_MAX_SIZE)
                {
                    HAL_FLASH_Lock();
                    outputBytes = 0;
                    return;
                }

                // 当前页写满时，先擦下一页，再继续写入。
                if (flashWriteAddr >= pageEndAddr)
                {
                    if (nextEraseAddr >= flashLimitAddr)
                    {
                        HAL_FLASH_Lock();
                        outputBytes = 0;
                        return;
                    }
                    EraseInit.PageAddress = nextEraseAddr;
                    if (HAL_FLASHEx_Erase(&EraseInit, &PageError) != HAL_OK)
                    {
                        HAL_FLASH_Lock();
                        outputBytes = 0;
                        return;
                    }
                    pageEndAddr = nextEraseAddr + PAGESIZE;
                    nextEraseAddr += PAGESIZE;
                }

                // 先缓存一个字节，等待凑成半字再调用 HAL_FLASH_Program。
                if (hasPendingByte == 0U)
                {
                    pendingByte    = outputBuffer[i];
                    hasPendingByte = 1U;
                    compressedCount++;
                    continue;
                }

                // STM32F1 Flash 按 halfword 编程：低字节在前，高字节在后。
                uint16_t halfWord = (uint16_t)pendingByte | ((uint16_t)outputBuffer[i] << 8);
                if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flashWriteAddr, halfWord) != HAL_OK)
                {
                    HAL_FLASH_Lock();
                    outputBytes = 0;
                    return;
                }

                flashWriteAddr += 2U;
                compressedCount++;
                hasPendingByte = 0U;
            }

            if (pollRes != HSER_POLL_MORE)
            {
                break;
            }
        }
    }

    // 第二阶段：通知编码器 finish，并将剩余压缩输出全部刷出。
    while (1)
    {
        HSE_finish_res finishRes = heatshrink_encoder_finish(&encoder);
        if (finishRes < 0)
        {
            HAL_FLASH_Lock();
            outputBytes = 0;
            return;
        }

        while (1)
        {
            size_t       outputSize = 0;
            HSE_poll_res pollRes    = heatshrink_encoder_poll(&encoder, outputBuffer, sizeof(outputBuffer), &outputSize);
            if (pollRes < 0)
            {
                HAL_FLASH_Lock();
                outputBytes = 0;
                return;
            }

            for (size_t i = 0; i < outputSize; i++)
            {
                if (compressedCount >= COMPRESS_MAX_SIZE)
                {
                    HAL_FLASH_Lock();
                    outputBytes = 0;
                    return;
                }

                if (flashWriteAddr >= pageEndAddr)
                {
                    if (nextEraseAddr >= flashLimitAddr)
                    {
                        HAL_FLASH_Lock();
                        outputBytes = 0;
                        return;
                    }
                    EraseInit.PageAddress = nextEraseAddr;
                    if (HAL_FLASHEx_Erase(&EraseInit, &PageError) != HAL_OK)
                    {
                        HAL_FLASH_Lock();
                        outputBytes = 0;
                        return;
                    }
                    pageEndAddr = nextEraseAddr + PAGESIZE;
                    nextEraseAddr += PAGESIZE;
                }

                if (hasPendingByte == 0U)
                {
                    pendingByte    = outputBuffer[i];
                    hasPendingByte = 1U;
                    compressedCount++;
                    continue;
                }

                uint16_t halfWord = (uint16_t)pendingByte | ((uint16_t)outputBuffer[i] << 8);
                if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flashWriteAddr, halfWord) != HAL_OK)
                {
                    HAL_FLASH_Lock();
                    outputBytes = 0;
                    return;
                }

                flashWriteAddr += 2U;
                compressedCount++;
                hasPendingByte = 0U;
            }

            if (pollRes != HSER_POLL_MORE)
            {
                break;
            }
        }

        if (finishRes == HSER_FINISH_DONE)
        {
            break;
        }
    }

    // 收尾：如果还有单字节未配对，用 0xFF 填充高字节后写入最后一个半字。
    if (hasPendingByte != 0U)
    {
        if (compressedCount >= COMPRESS_MAX_SIZE)
        {
            HAL_FLASH_Lock();
            outputBytes = 0;
            return;
        }

        if (flashWriteAddr >= pageEndAddr)
        {
            if (nextEraseAddr >= flashLimitAddr)
            {
                HAL_FLASH_Lock();
                outputBytes = 0;
                return;
            }
            EraseInit.PageAddress = nextEraseAddr;
            if (HAL_FLASHEx_Erase(&EraseInit, &PageError) != HAL_OK)
            {
                HAL_FLASH_Lock();
                outputBytes = 0;
                return;
            }
            pageEndAddr = nextEraseAddr + PAGESIZE;
            nextEraseAddr += PAGESIZE;
        }

        uint16_t halfWord = (uint16_t)pendingByte | 0xFF00U;
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flashWriteAddr, halfWord) != HAL_OK)
        {
            HAL_FLASH_Lock();
            outputBytes = 0;
            return;
        }
        flashWriteAddr += 2U;
    }

    // 正常结束：上锁 Flash，并记录压缩输出的实际字节数。
    HAL_FLASH_Lock();
    outputBytes = compressedCount;
}

// 实现使用heatshrink算法将FLASH中以COMPRESS_DATA_ADDR地址开始的COMPRESSED_BYTES字节数据解压后存储到ORIGIN_DATA_ADDR地址,
// 并将解压后的数据大小存储到outputBytes变量中
void decompress_flash_data()
{
    const uint8_t *compressedData    = (const uint8_t *)COMPRESS_DATA_ADDR;
    size_t         inputOffset       = 0;
    size_t         decompressedCount = 0;

    uint32_t flashWriteAddr = ORIGIN_DATA_ADDR;
    uint32_t nextEraseAddr  = ORIGIN_DATA_ADDR;
    uint32_t pageEndAddr    = ORIGIN_DATA_ADDR;
    uint32_t flashLimitAddr = ORIGIN_DATA_ADDR + ORIGIN_DATA_PAGE * PAGESIZE;

    uint8_t outputBuffer[64];
    uint8_t pendingByte    = 0;
    uint8_t hasPendingByte = 0;

    FLASH_EraseInitTypeDef EraseInit;
    EraseInit.TypeErase   = FLASH_TYPEERASE_PAGES;
    EraseInit.PageAddress = ORIGIN_DATA_ADDR;
    EraseInit.Banks       = 1;
    EraseInit.NbPages     = 1;
    uint32_t PageError    = 0;

    heatshrink_decoder decoder;
    heatshrink_decoder_reset(&decoder);

    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        outputBytes = 0;
        return;
    }

    while (inputOffset < COMPRESSED_BYTES)
    {
        size_t sinkSize   = 0;
        size_t inputChunk = COMPRESSED_BYTES - inputOffset;
        if (inputChunk > HEATSHRINK_STATIC_INPUT_BUFFER_SIZE)
        {
            inputChunk = HEATSHRINK_STATIC_INPUT_BUFFER_SIZE;
        }

        if (heatshrink_decoder_sink(&decoder, (uint8_t *)&compressedData[inputOffset], inputChunk, &sinkSize) != HSER_SINK_OK || sinkSize == 0)
        {
            HAL_FLASH_Lock();
            outputBytes = 0;
            return;
        }
        inputOffset += sinkSize;

        while (1)
        {
            size_t       outputSize = 0;
            HSD_poll_res pollRes    = heatshrink_decoder_poll(&decoder, outputBuffer, sizeof(outputBuffer), &outputSize);
            if (pollRes < 0)
            {
                HAL_FLASH_Lock();
                outputBytes = 0;
                return;
            }

            for (size_t i = 0; i < outputSize; i++)
            {
                if (decompressedCount >= ORIGIN_DATA_SIZE)
                {
                    HAL_FLASH_Lock();
                    outputBytes = 0;
                    return;
                }

                if (flashWriteAddr >= pageEndAddr)
                {
                    if (nextEraseAddr >= flashLimitAddr)
                    {
                        HAL_FLASH_Lock();
                        outputBytes = 0;
                        return;
                    }
                    EraseInit.PageAddress = nextEraseAddr;
                    if (HAL_FLASHEx_Erase(&EraseInit, &PageError) != HAL_OK)
                    {
                        HAL_FLASH_Lock();
                        outputBytes = 0;
                        return;
                    }
                    pageEndAddr = nextEraseAddr + PAGESIZE;
                    nextEraseAddr += PAGESIZE;
                }

                if (hasPendingByte == 0U)
                {
                    pendingByte    = outputBuffer[i];
                    hasPendingByte = 1U;
                    decompressedCount++;
                    continue;
                }

                uint16_t halfWord = (uint16_t)pendingByte | ((uint16_t)outputBuffer[i] << 8);
                if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flashWriteAddr, halfWord) != HAL_OK)
                {
                    HAL_FLASH_Lock();
                    outputBytes = 0;
                    return;
                }

                flashWriteAddr += 2U;
                decompressedCount++;
                hasPendingByte = 0U;
            }

            if (pollRes != HSER_POLL_MORE)
            {
                break;
            }
        }
    }

    if (hasPendingByte != 0U)
    {
        if (decompressedCount >= ORIGIN_DATA_SIZE)
        {
            HAL_FLASH_Lock();
            outputBytes = 0;
            return;
        }

        if (flashWriteAddr >= pageEndAddr)
        {
            if (nextEraseAddr >= flashLimitAddr)
            {
                HAL_FLASH_Lock();
                outputBytes = 0;
                return;
            }
            EraseInit.PageAddress = nextEraseAddr;
            if (HAL_FLASHEx_Erase(&EraseInit, &PageError) != HAL_OK)
            {
                HAL_FLASH_Lock();
                outputBytes = 0;
                return;
            }
            pageEndAddr = nextEraseAddr + PAGESIZE;
            nextEraseAddr += PAGESIZE;
        }

        uint16_t halfWord = (uint16_t)pendingByte | 0xFF00U;
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flashWriteAddr, halfWord) != HAL_OK)
        {
            HAL_FLASH_Lock();
            outputBytes = 0;
            return;
        }
        decompressedCount++;
    }

    HAL_FLASH_Lock();
    outputBytes = decompressedCount;
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
    /* USER CODE BEGIN 2 */
    /* Initialize DWT timer module */
    dwt_init();
    static volatile uint32_t elapsed_10ns;
    volatile uint32_t        scale     = 100000;
    volatile uint8_t         startFlag = 0;
    /* USER CODE END 2 */

    /* Infinite loop */
    /* USER CODE BEGIN WHILE */
    while (1)
    {
        /* USER CODE END WHILE */

        if (startFlag == 1)
        {
            startFlag = 0;
            /* Test DWT timer: measure LED toggle time */
            uint8_t timer = dwt_start();

            compress_flash_data();
            elapsed_10ns = dwt_get_elapsed(timer, scale);
            dwt_stop(timer);
        }
        else if (startFlag == 2)
        {
            startFlag = 0;
            /* Test DWT timer: measure LED toggle time */
            uint8_t timer = dwt_start();

            decompress_flash_data();
            elapsed_10ns = dwt_get_elapsed(timer, scale);
            dwt_stop(timer);
        }

        uint8_t timer = dwt_start();
        HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);

        /* Get elapsed time in 10ns units */
        elapsed_10ns = dwt_get_elapsed(timer, scale);

        HAL_Delay(500);

        elapsed_10ns = dwt_get_elapsed(timer, scale);
        /* Stop timer for next measurement */
        dwt_stop(timer);

        /* USER CODE BEGIN 3 */
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

    /** Initializes the RCC Oscillators according to the specified parameters
     * in the RCC_OscInitTypeDef structure.
     */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState       = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL     = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    /** Initializes the CPU, AHB and APB buses clocks
     */
    RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
    {
        Error_Handler();
    }
}

/**
 * @brief GPIO Initialization Function
 * @param None
 * @retval None
 */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    /* USER CODE BEGIN MX_GPIO_Init_1 */

    /* USER CODE END MX_GPIO_Init_1 */

    /* GPIO Ports Clock Enable */
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /*Configure GPIO pin Output Level */
    HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);

    /*Configure GPIO pins : PE2 PE3 PE4 PE5
                             PE6 PE7 PE8 PE9
                             PE10 PE11 PE12 PE13
                             PE14 PE15 PE0 PE1 */
    GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10
                          | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15 | GPIO_PIN_0 | GPIO_PIN_1;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

    /*Configure GPIO pins : PC13 PC0 PC1 PC2
                             PC3 PC4 PC5 PC6
                             PC7 PC8 PC9 PC10
                             PC11 PC12 */
    GPIO_InitStruct.Pin = GPIO_PIN_13 | GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7
                          | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /*Configure GPIO pins : PF0 PF1 PF2 PF3
                             PF4 PF5 PF6 PF7
                             PF8 PF9 PF10 PF11
                             PF12 PF13 PF14 PF15 */
    GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8
                          | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

    /*Configure GPIO pins : PA0 PA1 PA2 PA3
                             PA4 PA5 PA6 PA7
                             PA8 PA9 PA10 PA11
                             PA12 */
    GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8
                          | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /*Configure GPIO pins : PB0 PB1 PB2 PB10
                             PB11 PB12 PB13 PB14
                             PB15 PB4 PB6 PB7
                             PB8 PB9 */
    GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15
                          | GPIO_PIN_4 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /*Configure GPIO pins : PG0 PG1 PG2 PG3
                             PG4 PG5 PG6 PG7
                             PG8 PG9 PG10 PG11
                             PG12 PG13 PG14 PG15 */
    GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8
                          | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

    /*Configure GPIO pins : PD8 PD9 PD10 PD11
                             PD12 PD13 PD14 PD15
                             PD0 PD1 PD2 PD3
                             PD4 PD5 PD6 PD7 */
    GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15 | GPIO_PIN_0
                          | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

    /*Configure GPIO pin : LED0_Pin */
    GPIO_InitStruct.Pin   = LED0_Pin;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED0_GPIO_Port, &GPIO_InitStruct);

    /* USER CODE BEGIN MX_GPIO_Init_2 */

    /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

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
