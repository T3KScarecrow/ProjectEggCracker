/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"

#include "stm32l4xx_nucleo.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define Buttons_and_LEDs_Pin GPIO_PIN_1
#define Buttons_and_LEDs_GPIO_Port GPIOA
#define USART_TX_Pin GPIO_PIN_2
#define USART_TX_GPIO_Port GPIOA
#define USART_RX_Pin GPIO_PIN_3
#define USART_RX_GPIO_Port GPIOA
#define Buttons_and_LEDsA4_Pin GPIO_PIN_4
#define Buttons_and_LEDsA4_GPIO_Port GPIOA
#define Buttons_and_LEDsA5_Pin GPIO_PIN_5
#define Buttons_and_LEDsA5_GPIO_Port GPIOA
#define Buttons_Pin GPIO_PIN_6
#define Buttons_GPIO_Port GPIOA
#define ButtonsA7_Pin GPIO_PIN_7
#define ButtonsA7_GPIO_Port GPIOA
#define Buttons_and_LEDsB0_Pin GPIO_PIN_0
#define Buttons_and_LEDsB0_GPIO_Port GPIOB
#define Keypad_Pin GPIO_PIN_10
#define Keypad_GPIO_Port GPIOB
#define KeypadC7_Pin GPIO_PIN_7
#define KeypadC7_GPIO_Port GPIOC
#define KeypadA8_Pin GPIO_PIN_8
#define KeypadA8_GPIO_Port GPIOA
#define KeypadA9_Pin GPIO_PIN_9
#define KeypadA9_GPIO_Port GPIOA
#define KeypadA10_Pin GPIO_PIN_10
#define KeypadA10_GPIO_Port GPIOA
#define TMS_Pin GPIO_PIN_13
#define TMS_GPIO_Port GPIOA
#define TCK_Pin GPIO_PIN_14
#define TCK_GPIO_Port GPIOA
#define ButtonsB3_Pin GPIO_PIN_3
#define ButtonsB3_GPIO_Port GPIOB
#define KeypadB4_Pin GPIO_PIN_4
#define KeypadB4_GPIO_Port GPIOB
#define KeypadB5_Pin GPIO_PIN_5
#define KeypadB5_GPIO_Port GPIOB
#define ButtonsB6_Pin GPIO_PIN_6
#define ButtonsB6_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
