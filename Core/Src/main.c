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

#include "arm_math.h"
#include "st7789.h"
#include "bitmap.h"
#include "stdio.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

//Change these values to however you like. Make sure they are a factor of AUDIO_BUFFER_SIZE
#define AUDIO_BUFFER_SIZE 1024
#define PROCESSED_AUDIO_SIZE 512
#define AUDIO_CHANNEL_BUFFER_SIZE 256
#define FFT_CHANNEL_BUFFER_SIZE 256
#define FFT_MAGNITUDE_BUFFER_SIZE 128+1 //Includes DC (0Hz) and Nyquist (Sampling Frequency)

#define SAMPLING_FREQUENCY 48000

#define DISPLAY_WIDTH 320	// Important!: Assuming at ST7789_rotation(2); (landscape)
#define DISPLAY_HEIGHT 240	// If rotation is at 1 or 3, it will be portrait (I think)

#define STUCK_PIXEL_CLEAR_MS 60000 // Clears top of display after 60 seconds. Some pixels get stuck
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

SAI_HandleTypeDef hsai_BlockA1;
DMA_HandleTypeDef hdma_sai1_a;

SPI_HandleTypeDef hspi1;

TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart3;

PCD_HandleTypeDef hpcd_USB_OTG_FS;

/* USER CODE BEGIN PV */

int32_t audioRawBuffer[AUDIO_BUFFER_SIZE];
volatile int32_t *audioBufferPtr;
volatile uint8_t audioBufferReadyFlag;

// L/R -> L = GND | R = 3.3V
static float processedLeftAudio[AUDIO_CHANNEL_BUFFER_SIZE];
static float processedRightAudio[AUDIO_CHANNEL_BUFFER_SIZE]; // for 2 microphones

float fftLeftAudio[FFT_CHANNEL_BUFFER_SIZE];
float fftLeftMagnitude[FFT_MAGNITUDE_BUFFER_SIZE];
float fftRightAudio[FFT_CHANNEL_BUFFER_SIZE];
float fftRightMagnitude[FFT_MAGNITUDE_BUFFER_SIZE];

arm_rfft_fast_instance_f32 fftAudioInstance;

int previousHeight[DISPLAY_WIDTH] = {0};

static uint32_t lastClearTime     = 0;
static int16_t  spectrumMaxHeight = 240;
static int      spectrumTopY      = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_USB_OTG_FS_PCD_Init(void);
static void MX_TIM2_Init(void);
static void MX_SPI1_Init(void);
static void MX_SAI1_Init(void);
/* USER CODE BEGIN PFP */


/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)	// DMA Half Buffer complete
{
	audioBufferPtr = &audioRawBuffer[0];

	audioBufferReadyFlag = 1;
}

void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)		// DMA Full Buffer complete
{
	audioBufferPtr = &audioRawBuffer[AUDIO_BUFFER_SIZE/2];

	audioBufferReadyFlag = 1;
}

// @brief:			Converts 24 bit audio data to 32 bit format and stores as float
// @param rawAudio:	Pointer to the address of the audio buffer array (audioRawBuffer) set by the DMA
// @param Size:		Size of the buffer (Half of the total buffer because double buffering is being used)
void processAudio(volatile int32_t *rawAudio, int32_t Size)
{
	uint16_t j = 0;

	for(uint16_t i = 0; i < Size; i+=2)
	{
		int32_t left  = rawAudio[i];
		if(left & 0x800000)
		{
			left |= 0xFF000000;
		}
		else
		{
			left &= 0x00FFFFFF;
		}

		int32_t right = rawAudio[i + 1];
		if(right & 0x800000)
		{
			right |= 0xFF000000;
		}
		else
		{
			right &= 0x00FFFFFF;
		}

		processedLeftAudio[j] = (float) left;
		processedRightAudio[j] = (float) right;

		j++;
	}
}

// @brief:			Transforms processed audio data into the frequency domain
// @param input:	Pointer to the address of the processed audio buffer array (processedLeftAudio/processedRightAudio)
// @param fftOut:	Pointer to the address of where the processed data will be stored (fftLeftAudio/fftRightAudio)
void computeFFT(float *input, float *fftOut)
{
    arm_rfft_fast_f32(&fftAudioInstance, input, fftOut, 0);
}

// @brief:			Takes the magnitude of the transformed data and places it into an array
// @param fftOut:	Pointer to the address of the transformed data (fftLeftAudio/fftRightAudio)
// @param mag:		Pointer to the address of the magnitude of the transformed data (fftLeftMagnitude/fftRightMagnitude)
// @Note:			With the way arm_rfft_fast_f32 is coded, the first and last elements of mag is reserved for DC and Nyquist respectively (I think)
void computeMagnitude(float *fftOut, float *mag)
{
    // Bin 0 (DC)
    mag[0] = fabsf(fftOut[0]);

    // Bins 1 → N/2 - 1 // The data we care about
    for (uint16_t i = 1; i < AUDIO_CHANNEL_BUFFER_SIZE/2; i++)
    {
        float real = fftOut[2*i];
        float imag = fftOut[2*i + 1];
        mag[i] = sqrtf(real * real + imag * imag);
    }

    // Bin N/2 (Nyquist)
    mag[AUDIO_CHANNEL_BUFFER_SIZE/2] = fabsf(fftOut[1]);
}

// @brief: Draws the magnitude of the transformed data to the display. Contains configurations to alter how the FFT is displayed.
// @Note:  The FFT will be displayed throughout the entire horizontal part of the screen.
// @Note:  The display has fade functionality meaning the pixels used to draw the FFT data will slowly go down
void drawSpectrum()
{
	//-------------Configurations--------------
    const int16_t speedOfFade = 44;		// How 'fast' the data goes down (i.e. How many pixels the display will turn black for every loop)
    int16_t maxHeight         = 240;	// Maximum height (pixels) the FFT can be drawn onto the display
    const int xOffset         = 0;		// How far from the left (how many pixels starting from the left) the display will start showing data
    const int yOffset         = 9;		// How far from the bottom (how many pixels starting from the bottom) the display will start showing data
    //-----------------------------------------
    if (yOffset + maxHeight > DISPLAY_HEIGHT)
    {
        maxHeight = DISPLAY_HEIGHT - yOffset;
    }

    spectrumMaxHeight = maxHeight; 								// Used by clearStuckPixels function to align with max height
    spectrumTopY = DISPLAY_HEIGHT - 1 - yOffset - maxHeight;									// Used by clearStuckPixels function to align with yOffset

    // Find the peak bin (skip DC bin)
    static uint32_t lastPeakUpdateTime = 0;
    static float    peakFrequency      = 0.0f;

    uint32_t now = HAL_GetTick(); // Peak frequency drawing part
    if ((now - lastPeakUpdateTime) >= 1000)
    {
        int   peakBin = 1;
        float peakMag = fftLeftMagnitude[1] + fftRightMagnitude[1];
        for (int i = 2; i < FFT_MAGNITUDE_BUFFER_SIZE; i++)
        {
            float mag = fftLeftMagnitude[i] + fftRightMagnitude[i];
            if (mag > peakMag)
            {
                peakMag = mag;
                peakBin = i;
            }
        }
        peakFrequency      = (float)peakBin * ((float)SAMPLING_FREQUENCY / FFT_CHANNEL_BUFFER_SIZE);
        lastPeakUpdateTime = now;

        char msg[64];
        sprintf(msg, "Peak Frequency: %.1f", peakFrequency);
        ST7789_FillRect(320-56, 240-9, 56, 9, ST7789_BLACK); //Just guessing
        ST7789_print(154, 240-9, ST7789_WHITE, ST7789_BLUE, 0, &Font_7x9, 1, msg);
    }

    //Utilizing how many bins (size of the number of FFT data) in FFT data to spread out to the display
    int16_t frequencyToPixel;
    float pixelsPerBin = (float)DISPLAY_WIDTH / (FFT_MAGNITUDE_BUFFER_SIZE - 1);

    for (int i = 1; i < FFT_MAGNITUDE_BUFFER_SIZE; i++)
    {
        float bothMag = fftLeftMagnitude[i] + fftRightMagnitude[i];

        frequencyToPixel = (int16_t)((bothMag / 8388608.0f) * maxHeight); 	// Microphone used (INMP441) utilizes 24 bit data. Divide by 2^23
        if (frequencyToPixel < 0)         frequencyToPixel = 0; 			// Clamping: Pixels cannot go out of bounds of display
        if (frequencyToPixel > maxHeight) frequencyToPixel = maxHeight;		// Clamping

        for (int px = 0; px < (int)pixelsPerBin; px++) 						// Display drawing part
        {
            int x = xOffset + (int)(i * pixelsPerBin + px);
            if (x < 0 || x >= DISPLAY_WIDTH) continue;

            int barBottom = DISPLAY_HEIGHT - 1 - yOffset;
            int prev      = previousHeight[x];
            int barTop    = barBottom - prev;
            int newBarTop = barBottom - frequencyToPixel;

            if (frequencyToPixel >= prev) 									// Part where pixels are drawn
            {
                ST7789_DrawLine(x, newBarTop, x, barTop, ST7789_WHITE);
                previousHeight[x] = frequencyToPixel;
            }
            else 															// Part where pixels are 'erased' (drawn black)
            {
                int newHeight = prev - speedOfFade;
                if (newHeight < 0) newHeight = 0;

                int fadeTop = barBottom - newHeight;

                ST7789_DrawLine(x, barTop, x, fadeTop - 1, ST7789_BLACK);
                previousHeight[x] = newHeight;
            }
        }
    }
}

// @brief: Fills the FFT area with a color (black) after 60 seconds to combat pixels getting stuck
void clearStuckPixels()
{
    uint32_t now = HAL_GetTick();
    if ((now - lastClearTime) > STUCK_PIXEL_CLEAR_MS)
    {
    	ST7789_FillRect(0, 0, DISPLAY_WIDTH, spectrumMaxHeight / 3, ST7789_BLACK);
    	memset(previousHeight, 0, sizeof(previousHeight));
        lastClearTime = now;
    }
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

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

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
  MX_USART3_UART_Init();
  MX_USB_OTG_FS_PCD_Init();
  MX_TIM2_Init();
  MX_SPI1_Init();
  MX_SAI1_Init();
  /* USER CODE BEGIN 2 */

  HAL_SAI_Receive_DMA(&hsai_BlockA1, (uint8_t *)audioRawBuffer, AUDIO_BUFFER_SIZE);

  arm_rfft_fast_init_f32(&fftAudioInstance, FFT_CHANNEL_BUFFER_SIZE);

  ST7789_Init();
  ST7789_rotation(2);
  ST7789_print(0, 240-9, ST7789_WHITE, ST7789_BLUE, 0, &Font_7x9, 1, "0-24KHz");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
	while (1)
	{
		if(audioBufferReadyFlag)
		{
			audioBufferReadyFlag = 0;
			processAudio(audioBufferPtr, PROCESSED_AUDIO_SIZE);

			computeFFT(processedLeftAudio, fftLeftAudio);
			computeMagnitude(fftLeftAudio, fftLeftMagnitude);

			computeFFT(processedRightAudio, fftRightAudio);
			computeMagnitude(fftRightAudio, fftRightMagnitude);

			clearStuckPixels();
			drawSpectrum();

			// Show raw data through UART
			/*
			for (int i = 0; i < 16; i++)
			{
			    char msg[128];

			    int32_t left  = (int32_t)processedLeftAudio[i];
			    int32_t right = (int32_t)processedRightAudio[i];

			    int len = sprintf(msg, "L:%ld R:%10ld\r\n", left, right);
			    HAL_UART_Transmit(&huart3, (uint8_t*)msg, len, HAL_MAX_DELAY);
			}
			*/

			// Show FFT data through UART
			/*
			for (int i = 1; i < FFT_MAGNITUDE_BUFFER_SIZE; i++)
			{
				char msg[128];

			    float leftMag  = fftLeftMagnitude[i];
			    float rightMag = fftRightMagnitude[i];

			    float freq = (float)i * SAMPLING_FREQUENCY / AUDIO_CHANNEL_BUFFER_SIZE;

			    int len = sprintf(msg, "%.2f \t %.2f \t %.2f\r\n", freq, leftMag, rightMag);

			    HAL_UART_Transmit(&huart3, (uint8_t*)msg, len, HAL_MAX_DELAY);
			}
			*/
		}

    /* USER CODE END WHILE */

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

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 96;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  RCC_OscInitStruct.PLL.PLLR = 2;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SAI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SAI1_Init(void)
{

  /* USER CODE BEGIN SAI1_Init 0 */

  /* USER CODE END SAI1_Init 0 */

  /* USER CODE BEGIN SAI1_Init 1 */

  /* USER CODE END SAI1_Init 1 */
  hsai_BlockA1.Instance = SAI1_Block_A;
  hsai_BlockA1.Init.AudioMode = SAI_MODEMASTER_RX;
  hsai_BlockA1.Init.Synchro = SAI_ASYNCHRONOUS;
  hsai_BlockA1.Init.OutputDrive = SAI_OUTPUTDRIVE_DISABLE;
  hsai_BlockA1.Init.NoDivider = SAI_MASTERDIVIDER_ENABLE;
  hsai_BlockA1.Init.FIFOThreshold = SAI_FIFOTHRESHOLD_EMPTY;
  hsai_BlockA1.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_48K;
  hsai_BlockA1.Init.SynchroExt = SAI_SYNCEXT_DISABLE;
  hsai_BlockA1.Init.MonoStereoMode = SAI_STEREOMODE;
  hsai_BlockA1.Init.CompandingMode = SAI_NOCOMPANDING;
  if (HAL_SAI_InitProtocol(&hsai_BlockA1, SAI_I2S_STANDARD, SAI_PROTOCOL_DATASIZE_24BIT, 2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SAI1_Init 2 */

  /* USER CODE END SAI1_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_HIGH;
  hspi1.Init.CLKPhase = SPI_PHASE_2EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 96-1;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 100-1;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief USB_OTG_FS Initialization Function
  * @param None
  * @retval None
  */
static void MX_USB_OTG_FS_PCD_Init(void)
{

  /* USER CODE BEGIN USB_OTG_FS_Init 0 */

  /* USER CODE END USB_OTG_FS_Init 0 */

  /* USER CODE BEGIN USB_OTG_FS_Init 1 */

  /* USER CODE END USB_OTG_FS_Init 1 */
  hpcd_USB_OTG_FS.Instance = USB_OTG_FS;
  hpcd_USB_OTG_FS.Init.dev_endpoints = 6;
  hpcd_USB_OTG_FS.Init.speed = PCD_SPEED_FULL;
  hpcd_USB_OTG_FS.Init.dma_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_OTG_FS.Init.Sof_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.vbus_sensing_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.use_dedicated_ep1 = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_OTG_FS) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USB_OTG_FS_Init 2 */

  /* USER CODE END USB_OTG_FS_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);

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
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD1_GPIO_Port, LD1_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(RST_GPIO_Port, RST_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOG, CS_Pin|DC_Pin|USB_PowerSwitchOn_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : PC3 */
  GPIO_InitStruct.Pin = GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : LD1_Pin */
  GPIO_InitStruct.Pin = LD1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : RST_Pin */
  GPIO_InitStruct.Pin = RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(RST_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : CS_Pin DC_Pin USB_PowerSwitchOn_Pin */
  GPIO_InitStruct.Pin = CS_Pin|DC_Pin|USB_PowerSwitchOn_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /*Configure GPIO pin : USB_OverCurrent_Pin */
  GPIO_InitStruct.Pin = USB_OverCurrent_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(USB_OverCurrent_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */



/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

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
