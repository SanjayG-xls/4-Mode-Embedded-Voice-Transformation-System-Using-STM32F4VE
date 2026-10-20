/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Final 4-Mode Walkie-Talkie (Hardware Timings + Custom Colors)
  ******************************************************************************
  */
/* USER CODE END Header */
#include "main.h"
#include <math.h>

ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

I2S_HandleTypeDef hi2s2;
DMA_HandleTypeDef hdma_spi2_tx;

TIM_HandleTypeDef htim3;

/* USER CODE BEGIN PV */
#define CHUNK_SIZE 256

uint16_t adc_buffer[CHUNK_SIZE * 2];
uint16_t i2s_buffer[CHUNK_SIZE * 4];

// RAM Buffer: 3.5 seconds of 16kHz audio
#define RECORD_SAMPLES 56000
int16_t audio_memory[RECORD_SAMPLES];

// System States (Volatile for safe interrupt handling)
typedef enum {
    STATE_IDLE,
    STATE_RECORDING,
    STATE_PLAYING
} SystemState;

volatile SystemState current_state = STATE_IDLE;
volatile uint8_t current_mode = 1; // 1: Clear, 2: Enhanced, 3: Robot, 4: Echo
volatile uint32_t memory_index = 0;
volatile uint32_t playback_index = 0;

// DSP Variables
float dc_offset = 1551.0f;
float hp_prev_in = 0.0f;
float hp_prev_out = 0.0f;
float lp_prev_out = 0.0f;

// Robot Variables
float carrier_phase = 0.0f;
float phase_step = 0.01570796f;

// Echo Variables
#define DELAY_SAMPLES 6000
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2S2_Init(void);
static void MX_TIM3_Init(void);

/* USER CODE BEGIN PFP */
void WS2812_SendColor(uint8_t r, uint8_t g, uint8_t b);
void Update_LED_Mode(void);
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */

// --- 84MHz WS2812B TIMING-COMPENSATED DRIVER (PA2) ---
void WS2812_SendColor(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t color = (g << 16) | (r << 8) | b;

    __disable_irq();
    for(int led = 0; led < 8; led++) {
        for (int i = 23; i >= 0; i--) {
            if (color & (1 << i)) {
                // Send '1'
                GPIOA->BSRR = GPIO_PIN_2;
                uint32_t t = DWT->CYCCNT;
                while((DWT->CYCCNT - t) < 50);

                GPIOA->BSRR = (uint32_t)GPIO_PIN_2 << 16;
                t = DWT->CYCCNT;
                while((DWT->CYCCNT - t) < 30);
            } else {
                // Send '0'
                GPIOA->BSRR = GPIO_PIN_2;
                uint32_t t = DWT->CYCCNT;
                while((DWT->CYCCNT - t) < 12);

                GPIOA->BSRR = (uint32_t)GPIO_PIN_2 << 16;
                t = DWT->CYCCNT;
                while((DWT->CYCCNT - t) < 60);
            }
        }
    }
    __enable_irq();
}

void Update_LED_Mode(void) {
    if (current_mode == 1) WS2812_SendColor(255, 0, 0);       // Mode 1: Red (Clear)
    else if (current_mode == 2) WS2812_SendColor(0, 255, 0);  // Mode 2: Green (BG Filter)
    else if (current_mode == 3) WS2812_SendColor(0, 0, 255);  // Mode 3: Blue (Robot)
    else if (current_mode == 4) WS2812_SendColor(128, 0, 255);// Mode 4: Purple (Echo)
}

// --- INTERRUPT AUDIO PROCESSING ---
void process_audio_state(uint16_t* in_buf, uint16_t* out_buf, uint16_t len) {
    for(int i = 0; i < len; i++) {

        if (current_state == STATE_RECORDING) {

            // Maintain Filters for all modes
            dc_offset = (0.995f * dc_offset) + (0.005f * (float)in_buf[i]);
            float input = (float)in_buf[i] - dc_offset;

            float hp_out = 0.98f * hp_prev_out + 0.98f * (input - hp_prev_in);
            hp_prev_in = input;
            hp_prev_out = hp_out;

            float lp_out = 0.85f * hp_out + 0.15f * lp_prev_out;
            lp_prev_out = lp_out;

            int32_t processed = 0;

            // Mode Switching Logic
            switch(current_mode) {
                case 1: // CLEAR MODE
                    processed = (int32_t)(lp_out * 4.0f);
                    if (processed > -150 && processed < 150) processed = 0;
                    break;
                case 2: // NOISE REMOVAL & ENHANCED
                    processed = (int32_t)(lp_out * 8.0f);
                    if (processed > -200 && processed < 200) processed = 0;
                    break;
                case 3: // ROBOT MODE
                    carrier_phase += phase_step;
                    if (carrier_phase > 6.2831853f) carrier_phase -= 6.2831853f;

                    processed = (int32_t)(lp_out * 8.0f);
                    if (processed > -400 && processed < 400) {
                        processed = 0;
                    } else {
                        processed = (int32_t)((float)processed * sinf(carrier_phase) * 1.5f);
                    }
                    break;
                case 4: // CANYON ECHO
                    processed = (int32_t)(lp_out * 4.0f);
                    if (processed > -300 && processed < 300) {
                        processed = 0;
                    }
                    if (memory_index >= DELAY_SAMPLES) {
                        processed += audio_memory[memory_index - DELAY_SAMPLES] / 2;
                    }
                    break;
            }

            // Limit and Store
            if(processed > 32767) processed = 32767;
            if(processed < -32768) processed = -32768;

            if (memory_index < RECORD_SAMPLES) {
                audio_memory[memory_index++] = (int16_t)processed;
            } else {
                current_state = STATE_PLAYING;
                playback_index = 0;
            }
            out_buf[i*2] = 0; out_buf[i*2+1] = 0;

        } else if (current_state == STATE_PLAYING) {
            int16_t sample = 0;
            if (playback_index < memory_index) {
                sample = audio_memory[playback_index++];
            } else {
                current_state = STATE_IDLE;
            }
            out_buf[i*2] = sample; out_buf[i*2+1] = sample;
        } else {
            out_buf[i*2] = 0; out_buf[i*2+1] = 0;
        }
    }
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* hadc) {
    process_audio_state(&adc_buffer[0], &i2s_buffer[0], CHUNK_SIZE);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc) {
    process_audio_state(&adc_buffer[CHUNK_SIZE], &i2s_buffer[CHUNK_SIZE * 2], CHUNK_SIZE);
}
/* USER CODE END 0 */

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_I2S2_Init();
  MX_TIM3_Init();

  /* USER CODE BEGIN 2 */
  // ENABLE HARDWARE CYCLE COUNTER FOR PERFECT LED TIMING
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  Update_LED_Mode(); // Boot-up Red LED (Mode 1)

  HAL_I2S_Transmit_DMA(&hi2s2, i2s_buffer, CHUNK_SIZE * 4);
  HAL_ADC_Start_DMA(&hadc1, (uint32_t*)adc_buffer, CHUNK_SIZE * 2);
  HAL_TIM_Base_Start(&htim3);

  uint32_t button_press_time = 0;
  uint32_t last_release_time = 0;
  uint8_t button_was_pressed = 0;
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      uint8_t button_is_pressed = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_RESET);
      uint32_t current_time = HAL_GetTick();

      // DEBOUNCE: Ignore button changes for 50ms after the last release
      if (current_time - last_release_time > 50)
      {
          if (button_is_pressed && !button_was_pressed) {
              // Button just went down
              button_was_pressed = 1;
              button_press_time = current_time;
          }
          else if (button_is_pressed && button_was_pressed) {
              // Button is being held
              if (current_time - button_press_time > 400 && current_state == STATE_IDLE) {
                  current_state = STATE_RECORDING;
                  memory_index = 0;
              }
          }
          else if (!button_is_pressed && button_was_pressed) {
              // Button was just released
              button_was_pressed = 0;
              last_release_time = current_time;
              uint32_t hold_duration = current_time - button_press_time;

              // Mode switch on quick click
              if (hold_duration > 20 && hold_duration <= 400 && current_state == STATE_IDLE) {
                  current_mode++;
                  if (current_mode > 4) current_mode = 1;
                  Update_LED_Mode(); // Update the mode indicator color
              }
              else if (current_state == STATE_RECORDING) {
                  current_state = STATE_PLAYING;
                  playback_index = 0;
              }
          }
      }

      HAL_Delay(5);
    /* USER CODE END WHILE */
  }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 84;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) { Error_Handler(); }
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) { Error_Handler(); }
}

static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T3_TRGO;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) { Error_Handler(); }
}

static void MX_I2S2_Init(void)
{
  hi2s2.Instance = SPI2;
  hi2s2.Init.Mode = I2S_MODE_MASTER_TX;
  hi2s2.Init.Standard = I2S_STANDARD_PHILIPS;
  hi2s2.Init.DataFormat = I2S_DATAFORMAT_16B;
  hi2s2.Init.MCLKOutput = I2S_MCLKOUTPUT_DISABLE;
  hi2s2.Init.AudioFreq = I2S_AUDIOFREQ_16K;
  hi2s2.Init.CPOL = I2S_CPOL_LOW;
  hi2s2.Init.ClockSource = I2S_CLOCK_PLL;
  hi2s2.Init.FullDuplexMode = I2S_FULLDUPLEXMODE_DISABLE;
  if (HAL_I2S_Init(&hi2s2) != HAL_OK) { Error_Handler(); }
}

static void MX_TIM3_Init(void)
{
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 0;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 5250;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK) { Error_Handler(); }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK) { Error_Handler(); }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK) { Error_Handler(); }
}

static void MX_DMA_Init(void)
{
  __HAL_RCC_DMA2_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();
  HAL_NVIC_SetPriority(DMA1_Stream4_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream4_IRQn);
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  // PA0 Input (Push-Button with Pull-Up)
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  // PA2 Output (WS2812B Data Line)
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}
