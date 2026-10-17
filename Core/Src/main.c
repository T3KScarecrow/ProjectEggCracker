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

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
// ENUM ZONE ENUM ZONE ENUM ZONE ENUM ZONE //
/* App display modes enum, real*/
typedef enum {
		//APP_WAIT_START,
		APP_NO_FINGER_WARNING,
		APP_MONITORING,
		APP_ALERT,
		APP_MENU,
		APP_STAT,
		APP_PATIENT_INFO,
		APP_ADDRESS,
		APP_CPR
	} AppState;

/* Event flag enum*/	
typedef enum {
	    EVENT_NONE = 0u,
	    EVENT_IRREGULAR = 1u << 0u,
		EVENT_MANUAL = 1u << 1u
	} EventFlags;

/* CPR phase enum*/
typedef enum {
	CPR_COMPRESS,
	CPR_RELAX,
	CPR_BREATHE,
} CPRPhase;


// VARIABLE ZONE VARIABLE ZONE VARIABLE ZONE VARIABLE ZONE //
static uint32_t cpr_phase_start_ms = 0;
static CPRPhase cpr_phase = CPR_COMPRESS;
static uint8_t cpr_screen_index = 0;
static uint32_t cpr_screen_switch_ms = 0;
static uint8_t cpr_breath_screen_active = 0;
static uint8_t cpr_compressions_since_breath = 0u;

static const char *cpr_screen_text[6] = {
	"COMPRESS IN TIME"
	"WITH LIGHTS     ",
	"CALL 000 IF CPR ",
	"NEEDED          ",
	"PRESS # TO GO   ",
	"BACK            "
};
static const char *cpr_breath_screen_text1 = "BREATHE 2 RESCUE";
static const char *cpr_breath_screen_text2 "BREATHS         ";
static const char monitor_line_1[16] = "All Clear       ";
static const char monitor_line_2[16] = "System Idle...  ";
static const char warning_line_1[16] = "No Finger Found ";
static const char warning_line_2[16] = "Place on Finger ";
static const char menu_line_1[16] = "1.STAT 2.P.INFO ";
static const char menu_line_2[16] = "3.ADDR 4.CPR    ";
static const char patient_name[16] = "Stanley Leichter";
static const char patient_age[7] = "         AGE: 68";
static const char address_line_1[16] = "Bldg 420, 3 Kent";
static const char address_line_2[16] = "St, Bentley     "
static uint32_t time_at_last_readms = 0;
static const uint32_t OXIMETER_POLL_DELAYms = 10;
static char prev1[16] = "                ";
static char prev2[16] = "                ";

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

/* ==========================================================================
 * Power-on PIN
 *
 * Blocking by design: nothing proceeds until the PIN is accepted, so a plain
 * loop is enough and no state machine is needed. It runs once per power-up;
 * after that the main loop never touches the keypad again.
 * ========================================================================== */

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
// THE FUNCTIONS FUNCTION THE FUNCTIONS FUNCTION //
static void wait_for_start(void) {
	for (;;) {
		char key = Keypad_Scan();
		if (key == '1') {
		return;
		}

		HAL_Delay(PIN_SCAN_MS);
	}
}

static bool time_to_read_oximeter(uint32_t now_time) {
	if (now_time - time_at_last_readms == OXIMETER_POLL_DELAYms) {
		time_at_last_readms = now_time;
		return true;
	}
	else {
		return false;
	}
}

static bool accident_detected(void) {
	if (reading.heartbeat > 100 || reading.heartbeat < 55) {
		event_flags |= EVENT_IRREGULAR;
	}
	return (event_flags != EVENT_NONE);
}

static void clear_event_codes(void) {
	event_flags = EVENT_NONE;
}

static void cpr_leds(uint8_t on) {
	Panel_SetLed(2, on);
	Panel_SetLed(3, on);
}
/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

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

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
/* Functions for menu states, each shows information relating
* to the patient to help with their care such as their address
* their current heart rate and SpO2 and so on */
// SHOW ZONE SHOW ZONE SHOW ZONE SHOW ZONE SHOW ZONE //

static void printer_l1(char[16] fresh_line) {
	if (strncmp(prev1, fresh_line, 16) != 0) {
		lcd_put_cur(0, 0);
		lcd_send_string(fresh_line);
		strncpy(prev1, fresh_line, sizeof(prev1));
	}
}

static void printer_l2(char fresh_line) {
	if (strncmp(prev2, fresh_line, 16) != 0) {
		lcd_put_cur(1, 0);
		lcd_send_string(fresh_line);
		strncpy(prev2, fresh_line, sizeof(prev2));
	}
}

static void show_monitor(void) {
	// show monitor code
	printer_l1(monitor_line_1);
	printer_l2(monitor_line_2);
	
}

static void show_no_finger_warning(void) {
	// show no finger warning code
	printer_l1(warning_line_1);
	lcd_put_cur(1,0);
	printer_l2(warning_line_2);
}

static void show_menu(void) {
	// show menu code
	lcd_put_cur(0,0);
	printer_l1(menu_line_1);
	lcd_put_cur(1,0);
	printer_l2(menu_line_2);
}

static void show_stat(void) {
	// show stat code

}

static void show_patient_info(void) {
	// show patient info code
	lcd_put_cur(0,0);
	printer_l1(patient_name);
	lcd_put_cur(1,0); //Goddamn loser
	printer_l2(patient_age);
}

static void show_address(void) {
	// show address code
	// attempting to code it better
	// for loop logic worked in an external editor
	lcd_put_cur(0,0);
	printer_l1(address_line_1);
	lcd_put_cur(1,0);
	printer_l2(address_line_2);
}

static void show_cpr(void) {
	// show cpr code
	// Needs to change screen and flash LEDs
	// jagged array containing all three screens of text for the cpr mode
	uint32_t now_time = HAL_GetTick();

	if (cpr_breath_screen_active) {
		lcd_clear();
		lcd_put_cur(0, 0);
		lcd_send_string(cpr_breath_screen_text1);
		lcd_put_cur(1, 0);
		lcd_send_string(cpr_breath_screen_text2);
		return;
	}

	if (HAL_GetTick() - cpr_screen_switch_ms >= 4000u) {
		cpr_screen_switch_ms = HAL_GetTick();
		cpr_screen_index = (cpr_screen_index + 2u) % 6u;
	}
	// char cpr_line_1[17];
	// char cpr_line_2[17];
	// //set the lines to be printed to the lcd
	// for(int j = 0; j<16; j++){
	// 	cpr_line_1[j]=cpr_screen_text[cpr_screen_index][j];
	// 	cpr_line_2[j]=cpr_screen_text[cpr_screen_index+1][j+16];
	// }
	lcd_clear();
	lcd_put_cur(0, 0)
	lcd_send_string(cpr_screen_text[cpr_screen_index]);
	lcd_put_cur(1, 0);
	lcd_send_string(cpr_screen_text[cpr_screen_index+1]);
}
/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

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
	show_monitor();
	HAL_Delay(4000);
	//   show_menu();
	//   HAL_Delay(4000);
	//   show_patient_info();
	//   HAL_Delay(4000);
	//   show_address();
	//   HAL_Delay(4000);
	//   show_cpr();
	//   HAL_Delay(4000);
	/*#####wait_for_password();*/
	
	/* --- Heart rate sensor on I2C3 --- */
	{
		uint8_t addr = Oxi_ScanBus(&hi2c3); /* break here; expect 0xAE */
		(void) addr;
	}
	Oxi_Start(&hi2c3); /* the module only measures after this command */
	hist_reset(&s_hr_hist);
	HAL_Delay(1000);
	AppState app_state = APP_MONITORING;
  /* USER CODE END 2 */

  /* Initialize leds */
  BSP_LED_Init(LED_GREEN);

  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
//   OxiReading rd;    //Don't think we need this
//   char line1[17], line2[17];
//   char prev1[17] = "", prev2[17] = "";
  uint8_t spin = 0;

 while (1)
 {

   /* USER CODE END WHILE */

   /* USER CODE BEGIN 3 */
	char key = Keypad_Scan();
	Panel_Task();

	if ((HAL_GPIO_ReadPin(BTN_PORT[0], BTN_MASK[0]) == GPIO_PIN_RESET) && app_state == APP_MONITORING) {
		event_flags |= EVENT_MANUAL;
	}

	// if (time_to_read_oximeter() && app_state == APP_MONITORING) { //I think this can just go in the case for monitoring
	// 	OxiReading reading;

	// 	if (Oxi_Read(&reading) == HAL_OK) {
	// 		update_irregularity(reading.heartbeat);

	// 		if (accident_detected()) {
	// 			enter_alert_state();
	// 		}
	// 		// Handle read error
	// 	}
	// }

// MAIN CASE MAIN CASE MAIN CASE MAIN CASE //
	switch (app_state) {
	// case APP_WAIT_START:
	// 	// Handle wait start state
	// 	break; // I don't think we need this because the function for it already happens in main, before the while.

	case APP_MONITORING:
		// Handle monitoring state
		uint32_t now_time = HAL_GetTick()
		if (accident_detected()) {
			enter_alert_state();
		}
		if (time_to_read_oximeter()) {
		OxiReading reading;

		if (Oxi_Read(&reading) == HAL_OK) {
			update_irregularity(reading.heartbeat);

			
			// Handle read error
		}
		else {
			app_state = APP_NO_FINGER_WARNING;
		}
		}
		show_monitor();
		break;

	case APP_NO_FINGER_WARNING:
		// Handle no finger warning state

		show_no_finger_warning();
		app_state = APP_MONITORING;
		break;
	
		case APP_ALERT:
		// Handle alert state
		update_alert_display();
		update_alert_leds();
		handle_alert_key(key);
		break;

	case APP_MENU:
		// Handle menu 
		if (Keypad_Scan() == "1") {
			app_state = APP_STAT;
		}
		else if (Keypad_Scan() == "2") {
			app_state = APP_PATIENT_INFO;
		}
		else if (Keypad_Scan() == "3") {
			app_state = APP_ADDRESS;;
		}
		else if (Keypad_Scan() == "4") {
			app_state = APP_CPR;
		}
		else {
			show_menu();
		}
		break;

	case APP_STAT:
		// Handle stat state
		if (Keypad_Scan() == '#') {
			app_state = APP_MENU;
		}
		else {
			show_stat();
		}
		break;

	case APP_PATIENT_INFO:
		// Handle patient info state
		if (Keypad_Scan() == '#') {
			app_state = APP_MENU;
		}
		else {
			show_patient_info();
		}
		break;

	case APP_ADDRESS:
		// Handle address state
		if (Keypad_Scan() == '#') {
			app_state = APP_MENU;
		}
		else {
			show_address();
		}
		break;

	case APP_CPR:
		// Handle CPR state
		uint32_t now_time = HAL_GetTick();

		if (Keypad_Scan() == '#') {
			app_state = APP_MENU;
			break;
		}
		switch (cpr_phase) {
			case CPR_COMPRESS:
				if (now_time - cpr_phase_start_ms >= 450u) {
					cpr_leds(0);
					cpr_phase = CPR_RELAX;
					cpr_phase_start_ms = now_time;
					cpr_compressions_since_breath++;
					if (cpr_compressions_since_breath >= 5u) {
						cpr_phase = CPR_BREATH;
						cpr_phase_start_ms = now_time;
						cpr_breath_screen_active = 1u;
						cpr_compressions_since_breath = 0u;
					}
				}
				else {
					cpr_leds(1);
				}
				break;
			
			case CPR_RELAX:
				if (now_time - cpr_phase_start_ms >= 150u) {
					cpr_phase = CPR_COMPRESS;
					cpr_phase_start_ms = now_time;
				}
				else {
					cpr_leds(0);
				}
				break;

			case CPR_BREATHE:
				if (now_time - cpr_phase_start_ms >= 2500u) {
					cpr_phase = CPR_COMPRESS;
					cpr_phase_start_ms = now_time;
					cpr_breath_screen_active = 0u;
					cpr_screen_switch_ms = now_time;
				}
				else {
					cpr_set_leds(1);
				}
				break;
		}
		show_cpr();
		break;
	}
//
//	//   uint8_t ok;
//	//   float temp;
//	//   DispState st;
//	//   ok = (Oxi_Read(&rd) == HAL_OK) ? 1u : 0u;
//	//   temp = ok ? Oxi_ReadTemperature() : -100.0f;
//	//   st = ok ? hr_step(rd.heartbeat, rd.spo2) : ST_NO_FINGER;
//	//   build_lines(st, ok, temp, spin, line1, line2);
//	//   /* Write to the LCD only when the text actually changed. This is what
//	//   * makes fast polling practical: the screen may stay still for seconds,
//	//   * so there is no flicker and almost no traffic on I2C1. */
//	//   if (strncmp(prev1, line1, 16) != 0) {
//	//   lcd_put_cur(0, 0);
//	//   lcd_send_string(line1);
//	//   strncpy(prev1, line1, sizeof(prev1));
//	//   }
//	//   if (strncmp(prev2, line2, 16) != 0) {
//	//   lcd_put_cur(1, 0);
//	//   lcd_send_string(line2);
//	//   strncpy(prev2, line2, sizeof(prev2));
//	//   }
//	//   spin++;
//	//   /* The heart rate display only needs refreshing every 250 ms, but the
//	//   * buttons must be polled far more often to feel responsive. So the wait
//	//   * is split into 10 ms slices with a button scan in each. Pressing
//	//   * BUTTONn toggles LEDn without disturbing the readings. */
//	//   {
//	//   uint16_t t;
//	//   for (t = 0; t < (POLL_MS / PANEL_TICK_MS); t++)
//	//   {
//	//   Panel_Task();
//	//   HAL_Delay(PANEL_TICK_MS);
//	//   }
//	//   }
//	//   }
//  /* USER CODE END 3 */
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
