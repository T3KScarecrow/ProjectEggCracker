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
#include "i2c.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "LCD1602_I2C.h"
#include "oximeter.h"
#include "keypad.h"
#include "panel.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* ---- Sampling and stability parameters; tune them here only ---- */
#define POLL_MS 	 250u /* Polling period. The module refreshes its
 	 	 	 	 	 	   * data every 4 s, so reading faster just
 	 	 	 	 	 	   * re-reads the same value - harmless, and it
 	 	 	 	 	 	   * notices a finger within 0.25 s */
#define HIST_N 			5 /* history length used for the stability test */
#define HR_SPREAD_MAX  12 /* spread over 5 readings above which the
 	 	 	 	 	 	   * value is considered not settled yet (bpm) */
#define LOST_GRACE 	   12 /* invalid ticks before the finger counts as
 	 	 	 	 	       * removed (250 ms x 12 = 3 s) */

/* ---- Power-on PIN ---- */
#define PASSWORD   "1234" /* change the PIN on this line only */
#define PIN_LEN 		4 /* must match the length of PASSWORD */
#define PIN_SCAN_MS 	8u /* keypad polling; debouncing lives in keypad.c */
#define PIN_WRONG_HOLD 1500u /* how long the "wrong PIN" message stays */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/

/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* Pad a line out to 16 characters.
 * The LCD never clears itself: after "HR 100" a shorter "HR 72" would leave
 * the stray 0 on screen. Padding beats calling lcd_clear() every time, which
 * produces a visible flicker. */
static void pad16(char *s) {
	size_t n = strlen(s);
	while (n < 16u) {
		s[n++] = ' ';
	}
	s[16] = '\0';
}
/* ==========================================================================
 * Stability filtering
 *
 * A single reading cannot be trusted. Just after a finger is placed, or with
 * the wrong contact pressure, or on a weak signal, the module's algorithm
 * counts the dicrotic notch of the pulse waveform as a second beat and
 * reports exactly twice the true rate - 70 shows up as 140. This is the
 * classic PPG doubling artefact.
 *
 * A range check cannot catch it: 140 is perfectly physiological. What does
 * catch it is agreement between consecutive readings - the artefact comes
 * and goes, while a real heart rate settles into a narrow band.
 * ========================================================================== */
typedef struct {
	int32_t buf[HIST_N];
	uint8_t cnt; /* how many samples are filled in */
	uint8_t idx; /* ring buffer write index */
} Hist;
static Hist s_hr_hist;
static int32_t s_hr_shown = -1; /* heart rate on display, -1 = none yet */
static int32_t s_spo2_shown = -1; /* likewise for SpO2 */
static uint16_t s_lost = 0; /* consecutive invalid readings */
static void hist_reset(Hist *h) {
	h->cnt = 0;
	h->idx = 0;
}
static void hist_push(Hist *h, int32_t v) {
	h->buf[h->idx] = v;
	h->idx = (uint8_t) ((h->idx + 1u) % HIST_N);
	if (h->cnt < HIST_N) {
		h->cnt++;
	}
}
/* Full history and spread within the limit -> return the median,
 * otherwise -1 meaning "not settled yet". */
static int32_t hist_stable(const Hist *h, int32_t spread_max) {
	int32_t t[HIST_N];
	uint8_t i, j;
	if (h->cnt < HIST_N) {
		return -1;
	}
	for (i = 0; i < HIST_N; i++) {
		t[i] = h->buf[i];
	}
	for (i = 1; i < HIST_N; i++) { /* insertion sort, N=5 */
		int32_t k = t[i];
		j = i;
		while (j > 0u && t[j - 1] > k) {
			t[j] = t[j - 1];
			j--;
		}
		t[j] = k;
	}
	if ((t[HIST_N - 1] - t[0]) > spread_max) {
		return -1;
	}
	return t[HIST_N / 2]; /* median */
}
/* Display state. Three of them, not two:
 * ST_NO_FINGER nothing readable -> ask for a finger
 * ST_MEASURING data but unsettled -> ask the user to hold still; this is
 * where a 140 artefact gets held back
 * ST_SHOW settled -> show the numbers
 */
typedef enum {
	ST_NO_FINGER, ST_MEASURING, ST_SHOW
} DispState;
static DispState hr_step(int32_t raw_hr, int32_t raw_spo2) {
	if (raw_hr <= 0) {
		s_lost++;
		if (s_lost > LOST_GRACE) { /* the finger really is gone */
			hist_reset(&s_hr_hist);
			s_hr_shown = -1;
			s_spo2_shown = -1;
			return ST_NO_FINGER;
		}
		/* Inside the grace period keep the previous reading on screen so
		 * the display does not flip back and forth. */
		if (s_hr_shown > 0) {
			return ST_SHOW;
		}
		if (s_hr_hist.cnt > 0) {
			return ST_MEASURING;
		}
		return ST_NO_FINGER; /* never read anything at all */
	}
	s_lost = 0;
	hist_push(&s_hr_hist, raw_hr);
	{
		int32_t v = hist_stable(&s_hr_hist, HR_SPREAD_MAX);
		if (v > 0) {
			s_hr_shown = v;
			if (raw_spo2 > 0) {
				s_spo2_shown = raw_spo2;
			}
			return ST_SHOW;
		}
	}
	if (s_hr_shown > 0) {
		return ST_SHOW;
	} /* settled already, one bad tick */
	return ST_MEASURING;
}
/* Lay out the two display lines, each exactly 16 characters */
static void build_lines(DispState st, uint8_t comm_ok, float temp, uint8_t spin,
		char *l1, char *l2) {
	static const char *dots[4] = { " ", ". ", ".. ", "..." };
	if (!comm_ok) {
		strcpy(l1, "Sensor offline");
		strcpy(l2, "Check wiring");
	} else if (st == ST_NO_FINGER) {
		snprintf(l1, 17, "Place finger%s", dots[spin & 3u]);
		snprintf(l2, 17, "Board %4.1f degC", (double) temp);
	} else if (st == ST_MEASURING) {
		snprintf(l1, 17, "Measuring%s", dots[spin & 3u]);
		strcpy(l2, "Hold still");
	} else {
		int sp =
				(s_spo2_shown > 0 && s_spo2_shown <= 100) ?
						(int) s_spo2_shown : 0;
		int hr = (s_hr_shown > 0 && s_hr_shown <= 999) ? (int) s_hr_shown : 0;
		snprintf(l1, 17, "SpO2%3d%% HR%3d", sp, hr);
		snprintf(l2, 17, "Temp %4.1f degC", (double) temp);
	}
	pad16(l1);
	pad16(l2);
}

/* App display modes enum, real*/
typedef enum {
		APP_WAIT_START,
		APP_MONITORING,
		APP_ALERT,
		APP_MENU,
		APP_STAT,
		APP_PATIENT_INFO,
		APP_ADDRESS,
		APP_CPR
	} AppState;
/* ==========================================================================
 * Power-on PIN
 *
 * Blocking by design: nothing proceeds until the PIN is accepted, so a plain
 * loop is enough and no state machine is needed. It runs once per power-up;
 * after that the main loop never touches the keypad again.
 * ========================================================================== */
static void wait_for_start(void) {
	for (;;) {
		char key = Keypad_Scan();
		if (key == '1') {
		return;
		}

		HAL_Delay(PIN_SCAN_MS);
	}
}

static void add_event_code(const char *code);
static uint8_t accident_detected(void) {
	return (event_flags & (EVENT_IRREGULAR | EVENT_MANUAL)) != 0u;
}
static void clear_event_codes(void);
static void Panel_GetPressed(void);

static void wait_for_password(void) {
#if KEYPAD_LEARN_MODE
/* ---- Learn mode: no PIN check, just show which pin pair was pressed.
* Press 1 2 3 4 5 6 7 8 9 * 0 # in turn, note the twelve pairs, copy them
* into the KEYMAP table in keypad.c, then set KEYPAD_LEARN_MODE back. */
char l[17];
uint8_t sa, sb;
lcd_put_cur(0, 0);
lcd_send_string("LEARN MODE ");
/* Self test: no key should be down right now. A short found here means
* two wires are permanently connected, in which case the scan would keep
* returning that pair and every key would appear dead. */
if (Keypad_SelfTest(&sa, &sb))
{
snprintf(l, 17, "STUCK %u-%u! ",
(sa <= 9u) ? (unsigned)sa : 9u,
(sb <= 9u) ? (unsigned)sb : 9u);
lcd_put_cur(1, 0);
lcd_send_string(l);
HAL_Delay(3000); /* hold it long enough to read, then carry on */
}
else
{
lcd_put_cur(1, 0);
lcd_send_string("Press any key ");
}
for (;;)
{
char k = Keypad_Scan();
if (k != 0)
{
uint8_t a, b;
Keypad_LastPair(&a, &b);
/* Show the pin pair and the character the table yields, so the
* mapping can be checked while pressing. An unmapped pair shows
* key:?. The pin numbers can only be 1..7; clamping them again
* lets the compiler prove this line fits, which silences
* -Wformat-truncation. */
unsigned ua = (a <= 9u) ? (unsigned)a : 9u;
unsigned ub = (b <= 9u) ? (unsigned)b : 9u;
snprintf(l, 17, "pins:%u-%u key:%c ", ua, ub, k);
lcd_put_cur(1, 0);
lcd_send_string(l);
}
Panel_Task(); /* buttons and LEDs work from power-up */
HAL_Delay(PIN_SCAN_MS);
}
#else
	char entered[PIN_LEN + 1];
	char masked[17];
	uint8_t n = 0;
	lcd_put_cur(0, 0);
	lcd_send_string("Enter PIN: ");
	lcd_put_cur(1, 0);
	lcd_send_string("____ ");
	for (;;) {
		char k = Keypad_Scan();
		if (k != 0) {
			if (k >= '0' && k <= '9') {
				if (n < PIN_LEN) {
					entered[n++] = k;
				}
			} else if (k == '*') /* backspace */
			{
				if (n > 0) {
					n--;
				}
			} else if (k == '#') /* clear everything, start over */
			{
				n = 0;
			}
			/* '?' means the pair is absent from KEYMAP, i.e. calibration is
			 * not finished; ignore it. */
			/* Echo: entered digits as *, the rest as _ */
			{
				uint8_t i;
				for (i = 0; i < PIN_LEN; i++) {
					masked[i] = (i < n) ? '*' : '_';
				}
				for (i = PIN_LEN; i < 16u; i++) {
					masked[i] = ' ';
				}
				masked[16] = '\0';
			}
			lcd_put_cur(1, 0);
			lcd_send_string(masked);
			/* ---- Verify automatically once PIN_LEN digits are in; no
			 * confirm key is needed. Show the last '*' first, otherwise the
			 * screen jumps away before the user sees the final digit. */
			if (n == PIN_LEN) {
				entered[n] = '\0';
				HAL_Delay(250); /* let the last digit be seen */
				if (strncmp(entered, PASSWORD, PIN_LEN) == 0) {
					lcd_put_cur(0, 0);
					lcd_send_string("PIN accepted ");
					lcd_put_cur(1, 0);
					lcd_send_string(" ");
					HAL_Delay(800);
					return; /* the only way out */
				}
				lcd_put_cur(0, 0);
				lcd_send_string("Wrong PIN ");
				lcd_put_cur(1, 0);
				lcd_send_string("Try again ");
				HAL_Delay(PIN_WRONG_HOLD);
				n = 0;
				lcd_put_cur(0, 0);
				lcd_send_string("Enter PIN: ");
				lcd_put_cur(1, 0);
				lcd_send_string("____ ");
			}
		}
		Panel_Task(); /* buttons and LEDs work from power-up */
		HAL_Delay(PIN_SCAN_MS);
	}
#endif
}

/* Functions for menu states, each shows information relating
* to the patient to help with their care such as their address
* their current heart rate and SpO2 and so on */
void show_monitor() {
	// show monitor code
}

void show_menu() {
	// show menu code
	char menu_line_1[16] = "1.STAT 2.P.INFO";
	char menu_line_2[16] = "3.ADDR 4.CPR";
	lcd_put_cur(0,0);
	lcd_send_string(menu_line_1);
	lcd_put_cur(1,0);
	lcd_send_string(menu_line_2);
}

void show_stat() {
	// show stat code
}

void show_patient_info() {
	char patient_name[16] = "Stanley Leichter";
	char patient_age[7] = "AGE: 68";
	lcd_put_cur(0,0);
	lcd_send_string(patient_name);
	lcd_put_cur(0,10);
	lcd_send_string(patient_age);
}

void show_address() {
	// show address code
	// hardcoding it until I learn how to do it better
	char address_line_1[16] = "Bldg 420, 3/Kent";
	char address_line_2[16] = "St, Bentley";
	lcd_put_cur(0, 0);
	lcd_send_string(address_line_1);
	lcd_put_cur(1, 0);
	lcd_send_string(address_line_2);
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
  MX_I2C1_Init();
  MX_I2C3_Init();
  /* USER CODE BEGIN 2 */
	/* --- LCD on I2C1 --- */
	lcd_init(&hi2c1);
	lcd_put_cur(0, 0);
	lcd_send_string("BOOT SYSTEM?");
	lcd_put_cur(1, 0);
	lcd_send_string("1. YES");
	/* --- Keypad: the PIN gate comes first ---
	 * Placed before the sensor is started, so nothing is acquired until the
	 * PIN is accepted and the sensor LEDs stay dark while unauthorised. */
	Keypad_Init();
	Panel_Init(); /* all four LEDs off; buttons live from power-up */
	/* Initial state of the four LEDs at power-up. Change a 0 to a 1 to have
	 * that LED lit from the start; Panel_SetLed(0, 1) lights LED0 so the first
	 * press of BUTTON0 turns it off.
	 * Always go through Panel_SetLed() rather than the CubeMX initial level: it
	 * updates both the pin and the internal s_leds bit, and once those two
	 * disagree the first press toggles the wrong way. */
	Panel_SetLed(0, 0);
	Panel_SetLed(1, 0);
	Panel_SetLed(2, 0);
	Panel_SetLed(3, 0);
	wait_for_start();
	lcd_put_cur(0, 0);
	lcd_send_string("BOOTING...");
	lcd_put_cur(1, 0);
	lcd_send_string("");
	HAL_Delay(1000);
	lcd_put_cur(0, 0);
	lcd_send_string("SYSTEM IDLE");
	lcd_put_cur(1, 0);
	lcd_send_string("ALL CLEAR");
	/*#####wait_for_password();*/
	
	/* --- Heart rate sensor on I2C3 --- */
	{
		uint8_t addr = Oxi_ScanBus(&hi2c3); /* break here; expect 0xAE */
		(void) addr;
	}
	Oxi_Start(&hi2c3); /* the module only measures after this command */
	hist_reset(&s_hr_hist);
	HAL_Delay(1000);
  /* USER CODE END 2 */

  /* Initialize leds */
  BSP_LED_Init(LED_GREEN);

  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  OxiReading rd;
  char line1[17], line2[17];
  char prev1[17] = "", prev2[17] = "";
  uint8_t spin = 0;
  while (1)
  {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	char key = Keypad_Scan();
	Pane_Task();
	uint8_t pressed = Panel_GetPressed();

	if (pressed != 0u && app_state == APP_MONITORING) {
		add_event_code("MANUAL");
		event_flags |= EVENT_MANUAL;
	}

	if (time_to_read_oximeter() && app_state == APP_MONITORING) {
		OxiReading reading;

		if (Oxi_Read(&reading) == HAL_OK) {
			update_irregularity(reading.heartbeat);
			
			if (accident_detected()) {
				enter_alert_state();
			}
			// Handle read error
		}
	}

	switch (app_state) {
	case APP_WAIT_START:
		// Handle wait start state
		break;

	case APP_MONITORING:
		// Handle monitoring state
		show_monitor();
		break;

	case APP_ALERT:
		// Handle alert state
		update_alert_display();
		update_alert_leds();
		handle_alert_key(key);
		break;

	case APP_MENU:
		// Handle menu state
		show_menu();
		break;

	case APP_STAT:
		// Handle stat state
		show_stat();
		break;

	case APP_PATIENT_INFO:
		// Handle patient info state
		show_patient_info();
		break;

	case APP_ADDRESS:
		// Handle address state
		show_address();
		break;
	
	case APP_CPR:
		// Handle CPR state
		show_cpr();
		break;
	}

	//   uint8_t ok;
	//   float temp;
	//   DispState st;
	//   ok = (Oxi_Read(&rd) == HAL_OK) ? 1u : 0u;
	//   temp = ok ? Oxi_ReadTemperature() : -100.0f;
	//   st = ok ? hr_step(rd.heartbeat, rd.spo2) : ST_NO_FINGER;
	//   build_lines(st, ok, temp, spin, line1, line2);
	//   /* Write to the LCD only when the text actually changed. This is what
	//   * makes fast polling practical: the screen may stay still for seconds,
	//   * so there is no flicker and almost no traffic on I2C1. */
	//   if (strncmp(prev1, line1, 16) != 0) {
	//   lcd_put_cur(0, 0);
	//   lcd_send_string(line1);
	//   strncpy(prev1, line1, sizeof(prev1));
	//   }
	//   if (strncmp(prev2, line2, 16) != 0) {
	//   lcd_put_cur(1, 0);
	//   lcd_send_string(line2);
	//   strncpy(prev2, line2, sizeof(prev2));
	//   }
	//   spin++;
	//   /* The heart rate display only needs refreshing every 250 ms, but the
	//   * buttons must be polled far more often to feel responsive. So the wait
	//   * is split into 10 ms slices with a button scan in each. Pressing
	//   * BUTTONn toggles LEDn without disturbing the readings. */
	//   {
	//   uint16_t t;
	//   for (t = 0; t < (POLL_MS / PANEL_TICK_MS); t++)
	//   {
	//   Panel_Task();
	//   HAL_Delay(PANEL_TICK_MS);
	//   }
	//   }
	//   }
  /* USER CODE END 3 */
	}
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 10;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
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
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
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
