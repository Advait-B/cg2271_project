#include <stdbool.h>
#include <stdint.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

//#if (configUSE_PREEMPTION != 1) || (configUSE_TIME_SLICING != 1)
//#error "Enable configUSE_PREEMPTION and configUSE_TIME_SLICING in FreeRTOSConfig.h"
//#endif

#define DAILY_BUDGET     10
#define UNLOCK_BONUS     10
#define AMBER_LEFT       3
#define LONG_PRESS_MS    1000
#define DEBOUNCE_MS      30
#define CLAP_GAP_MS      1000
#define DENY_SHOW_MS     3000

#define SOUND_PIN        12  // PTA12 (D3)
#define BUTTON_PIN       13  // PTA13 (D8)
#define JOY_PIN          0   // PTB0 (A0), ADC0_SE8
#define JOY_CHANNEL      8
#define LED_B_PIN        1   // PTB1 (A1)
#define BUZZER_PIN       2   // PTB2 (A2), TPM2_CH0
#define LED_R_PIN        5   // PTA5 (D5), TPM0_CH2
#define LED_G_PIN        30  // PTE30 (D7), TPM0_CH3
#define UART_TX_PIN      22  // PTE22
#define UART_RX_PIN      23  // PTE23

#define LED_R_CH         2
#define LED_G_CH         3
#define BUZZER_CH        0
#define LED_PWM_MOD      250
#define BUZZER_CLOCK_HZ  1000000

#define JOY_LOW          800
#define JOY_HIGH         3300
#define JOY_CENTRE_LOW   1500
#define JOY_CENTRE_HIGH  2600

#define BAUD_RATE        9600
#define UART2_INT_PRIO   128
#define PORTA_INT_PRIO   64

#define PACKET_START     0xAA
#define PACKET_SIZE      5

#define PKT_HELLO        0x01
#define PKT_OPEN         0x02
#define PKT_CLOSE        0x03
#define PKT_NEXT         0x04
#define PKT_PREV         0x05
#define PKT_LIKE         0x06
#define PKT_STATE        0x07
#define PKT_ASK          0x08
#define PKT_SYNC         0x10
#define PKT_APPROVED     0x11
#define PKT_DENIED       0x12
#define PKT_BREAK_START  0x13
#define PKT_BREAK_TICK   0x14
#define PKT_BREAK_CANCEL 0x15
#define PKT_BREAK_DONE   0x16

#define SEG_A            0x01
#define SEG_B            0x02
#define SEG_C            0x04
#define SEG_D            0x08
#define SEG_E            0x10
#define SEG_F            0x20
#define SEG_G            0x40

#define LEN(a)           (sizeof(a) / sizeof((a)[0]))

typedef enum {
	ST_IDLE,
	ST_OPEN,
	ST_LOCKED,
	ST_WAITING,
	ST_BREAK
} State;

typedef enum {
	EV_NEXT,
	EV_PREV,
	EV_CLAP,
	EV_SHORT_PRESS,
	EV_LONG_PRESS,
	EV_SYNC,
	EV_APPROVED,
	EV_DENIED,
	EV_BREAK_START,
	EV_BREAK_TICK,
	EV_BREAK_CANCEL,
	EV_BREAK_DONE
} EventType;

typedef enum {
	SND_BEEP,
	SND_WARN,
	SND_LOCK,
	SND_TUNE,
	SND_ERROR
} Sound;

typedef struct {
	EventType type;
	int value;
} Event;

typedef struct {
	uint8_t type;
	uint8_t a;
	uint8_t b;
} Packet;

typedef struct {
	State state;
	int left;
	int pos;
	int maxPos;
	int breakSecs;
} Session;

typedef struct {
	uint16_t freq;
	uint16_t ms;
} Note;

typedef struct {
	const Note *notes;
	uint8_t count;
} Melody;

static const Note BEEP_NOTES[] = {{2000, 40}};
static const Note WARN_NOTES[] = {{1000, 80}, {0, 40}, {1000, 80}};
static const Note LOCK_NOTES[] = {{400, 1200}};
static const Note TUNE_NOTES[] = {{523, 120}, {659, 120}, {784, 120}, {1047, 250}};
static const Note ERROR_NOTES[] = {{200, 150}, {0, 50}, {200, 150}};

static const Melody MELODIES[] = {
	[SND_BEEP] = {BEEP_NOTES, LEN(BEEP_NOTES)},
	[SND_WARN] = {WARN_NOTES, LEN(WARN_NOTES)},
	[SND_LOCK] = {LOCK_NOTES, LEN(LOCK_NOTES)},
	[SND_TUNE] = {TUNE_NOTES, LEN(TUNE_NOTES)},
	[SND_ERROR] = {ERROR_NOTES, LEN(ERROR_NOTES)}
};

static const uint8_t DIGIT_GLYPHS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
static const uint8_t TEXT_OFF[4] = {0x3F, 0x71, 0x71, 0x00};
static const uint8_t TEXT_LOC[4] = {0x38, 0x3F, 0x39, 0x00};
static const uint8_t TEXT_SENT[4] = {0x6D, 0x79, 0x54, 0x78};
static const uint8_t TEXT_DENY[4] = {0x5E, 0x79, 0x54, 0x6E};  // "dEny": the friend said no

static volatile TickType_t denyShownAt = 0;  // when "dEny" started showing (0 = not showing)
static const uint8_t LCD_DEFG_PINS[4] = {20, 26, 40, 43};
static const uint8_t LCD_CBA_PINS[4] = {24, 27, 42, 44};

static QueueHandle_t eventQueue;
static QueueHandle_t soundQueue;
static QueueHandle_t txQueue;
static SemaphoreHandle_t sessionMutex;
static SemaphoreHandle_t uartRxSem;
static SemaphoreHandle_t txDoneSem;

static Session session = {ST_IDLE, DAILY_BUDGET, 0, 0, 0};
static volatile uint8_t rxFrame[PACKET_SIZE];
static volatile uint8_t txFrame[PACKET_SIZE];
static volatile int txIdx = PACKET_SIZE;

static void setMux(PORT_Type *port, int pin, int alt) {
	port->PCR[pin] &= ~PORT_PCR_MUX_MASK;
	port->PCR[pin] |= PORT_PCR_MUX(alt);
}

static void initClocks(void) {
	SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK | SIM_SCGC5_PORTB_MASK | SIM_SCGC5_PORTC_MASK |
			SIM_SCGC5_PORTD_MASK | SIM_SCGC5_PORTE_MASK | SIM_SCGC5_SLCD_MASK;
	SIM->SCGC6 |= SIM_SCGC6_TPM0_MASK | SIM_SCGC6_TPM2_MASK | SIM_SCGC6_ADC0_MASK;
	SIM->SCGC4 |= SIM_SCGC4_UART2_MASK;

	MCG->C1 |= MCG_C1_IRCLKEN_MASK;
	MCG->C2 |= MCG_C2_IRCS_MASK;
	MCG->SC &= ~MCG_SC_FCRDIV_MASK;
	MCG->MC &= ~MCG_MC_LIRC_DIV2_MASK;

	SIM->SOPT2 &= ~SIM_SOPT2_TPMSRC_MASK;
	SIM->SOPT2 |= SIM_SOPT2_TPMSRC(3);
}

static void initLED(void) {
	setMux(PORTA, LED_R_PIN, 3);
	setMux(PORTE, LED_G_PIN, 3);
	setMux(PORTB, LED_B_PIN, 1);
	GPIOB->PDDR |= (1 << LED_B_PIN);

	TPM0->SC = 0;
	TPM0->CNT = 0;
	TPM0->MOD = LED_PWM_MOD;
	TPM0->SC = TPM_SC_PS(7);
	TPM0->CONTROLS[LED_R_CH].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
	TPM0->CONTROLS[LED_G_CH].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
	TPM0->CONTROLS[LED_R_CH].CnV = 0;
	TPM0->CONTROLS[LED_G_CH].CnV = 0;
	TPM0->SC |= TPM_SC_CMOD(1);
}

static void setLED(int redPct, int greenPct, bool blue) {
	TPM0->CONTROLS[LED_R_CH].CnV = LED_PWM_MOD * redPct / 100;
	TPM0->CONTROLS[LED_G_CH].CnV = LED_PWM_MOD * greenPct / 100;
	if (blue) {
		GPIOB->PSOR = (1 << LED_B_PIN);
	} else {
		GPIOB->PCOR = (1 << LED_B_PIN);
	}
}

static void initBuzzer(void) {
	setMux(PORTB, BUZZER_PIN, 3);

	TPM2->SC = 0;
	TPM2->CNT = 0;
	TPM2->MOD = 999;
	TPM2->SC = TPM_SC_PS(3);
	TPM2->CONTROLS[BUZZER_CH].CnSC = TPM_CnSC_MSB_MASK | TPM_CnSC_ELSB_MASK;
	TPM2->CONTROLS[BUZZER_CH].CnV = 0;
	TPM2->SC |= TPM_SC_CMOD(1);
}

static void buzzerTone(uint16_t freq) {
	if (freq == 0) {
		TPM2->CONTROLS[BUZZER_CH].CnV = 0;
		return;
	}
	TPM2->SC &= ~TPM_SC_CMOD_MASK;
	TPM2->CNT = 0;
	TPM2->MOD = BUZZER_CLOCK_HZ / freq - 1;
	TPM2->CONTROLS[BUZZER_CH].CnV = BUZZER_CLOCK_HZ / freq / 2;
	TPM2->SC |= TPM_SC_CMOD(1);
}

static void initLCD(void) {
	setMux(PORTE, 20, 0);
	setMux(PORTE, 21, 0);
	setMux(PORTB, 18, 0);
	setMux(PORTB, 19, 0);
	setMux(PORTC, 0, 0);
	setMux(PORTC, 4, 0);
	setMux(PORTC, 6, 0);
	setMux(PORTC, 7, 0);
	setMux(PORTD, 0, 0);
	setMux(PORTD, 2, 0);
	setMux(PORTD, 3, 0);
	setMux(PORTD, 4, 0);

	LCD->GCR = LCD_GCR_VSUPPLY_MASK | LCD_GCR_LADJ(3) | LCD_GCR_SOURCE_MASK |
			LCD_GCR_ALTDIV(2) | LCD_GCR_LCLK(1) | LCD_GCR_DUTY(3);
	LCD->AR = 0;
	LCD->PEN[0] = (1U << 14) | (1U << 15) | (1U << 20) | (1U << 24) | (1U << 26) | (1U << 27);
	LCD->PEN[1] = (1U << (40 - 32)) | (1U << (42 - 32)) | (1U << (43 - 32)) |
			(1U << (44 - 32)) | (1U << (59 - 32)) | (1U << (60 - 32));
	LCD->BPEN[0] = (1U << 14) | (1U << 15);
	LCD->BPEN[1] = (1U << (59 - 32)) | (1U << (60 - 32));
	LCD->WF8B[59] = 0x11;
	LCD->WF8B[60] = 0x22;
	LCD->WF8B[14] = 0x44;
	LCD->WF8B[15] = 0x88;
	for (int i = 0; i < 4; i++) {
		LCD->WF8B[LCD_DEFG_PINS[i]] = 0;
		LCD->WF8B[LCD_CBA_PINS[i]] = 0;
	}
	LCD->GCR |= LCD_GCR_LCDEN_MASK;
}

static void lcdShow(const uint8_t glyphs[4], bool colon) {
	for (int i = 0; i < 4; i++) {
		uint8_t g = glyphs[i];
		LCD->WF8B[LCD_DEFG_PINS[i]] = ((g & SEG_D) ? 0x11 : 0) | ((g & SEG_E) ? 0x22 : 0) |
				((g & SEG_G) ? 0x44 : 0) | ((g & SEG_F) ? 0x88 : 0);
		LCD->WF8B[LCD_CBA_PINS[i]] = ((i == 3 && colon) ? 0x11 : 0) | ((g & SEG_C) ? 0x22 : 0) |
				((g & SEG_B) ? 0x44 : 0) | ((g & SEG_A) ? 0x88 : 0);
	}
}

static void lcdNumber(int n) {
	uint8_t glyphs[4] = {0, 0, 0, DIGIT_GLYPHS[n % 10]};
	if (n >= 10) {
		glyphs[2] = DIGIT_GLYPHS[(n / 10) % 10];
	}
	if (n >= 100) {
		glyphs[1] = DIGIT_GLYPHS[(n / 100) % 10];
	}
	lcdShow(glyphs, false);
}

static void lcdTime(int secs) {
	uint8_t glyphs[4] = {0, DIGIT_GLYPHS[(secs / 60) % 10], DIGIT_GLYPHS[(secs % 60) / 10], DIGIT_GLYPHS[secs % 10]};
	lcdShow(glyphs, true);
}

static void initJoystick(void) {
	setMux(PORTB, JOY_PIN, 0);
	ADC0->CFG1 = ADC_CFG1_MODE(1);
	ADC0->SC2 = ADC_SC2_REFSEL(1);
	ADC0->SC3 = 0;
}

static int readJoystick(void) {
	ADC0->SC1[0] = ADC_SC1_ADCH(JOY_CHANNEL);
	while (!(ADC0->SC1[0] & ADC_SC1_COCO_MASK)) {
	}
	return ADC0->R[0];
}

static void initInputs(void) {
	NVIC_DisableIRQ(PORTA_IRQn);
	setMux(PORTA, BUTTON_PIN, 1);
	setMux(PORTA, SOUND_PIN, 1);
	PORTA->PCR[BUTTON_PIN] |= PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;
	PORTA->PCR[SOUND_PIN] |= PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;
	GPIOA->PDDR &= ~((1 << BUTTON_PIN) | (1 << SOUND_PIN));
	PORTA->PCR[BUTTON_PIN] &= ~PORT_PCR_IRQC_MASK;
	PORTA->PCR[BUTTON_PIN] |= PORT_PCR_IRQC(0b1011);
	PORTA->PCR[SOUND_PIN] &= ~PORT_PCR_IRQC_MASK;
	PORTA->PCR[SOUND_PIN] |= PORT_PCR_IRQC(0b1011);
	PORTA->ISFR = (1 << BUTTON_PIN) | (1 << SOUND_PIN);
	NVIC_SetPriority(PORTA_IRQn, PORTA_INT_PRIO);
	NVIC_ClearPendingIRQ(PORTA_IRQn);
	NVIC_EnableIRQ(PORTA_IRQn);
}

void PORTA_IRQHandler(void) {
	static TickType_t pressTick = 0;
	static TickType_t lastClapTick = 0;
	BaseType_t hpw = pdFALSE;
	TickType_t now = xTaskGetTickCountFromISR();
	Event ev = {EV_CLAP, 0};

	NVIC_ClearPendingIRQ(PORTA_IRQn);
	if (PORTA->ISFR & (1 << BUTTON_PIN)) {
		if (!(GPIOA->PDIR & (1 << BUTTON_PIN))) {
			pressTick = now;
		} else if (now - pressTick >= pdMS_TO_TICKS(DEBOUNCE_MS)) {
			ev.type = (now - pressTick >= pdMS_TO_TICKS(LONG_PRESS_MS)) ? EV_LONG_PRESS : EV_SHORT_PRESS;
			xQueueSendFromISR(eventQueue, &ev, &hpw);
		}
		PORTA->ISFR = (1 << BUTTON_PIN);
	}
	if (PORTA->ISFR & (1 << SOUND_PIN)) {
		if (now - lastClapTick >= pdMS_TO_TICKS(CLAP_GAP_MS)) {
			lastClapTick = now;
			ev.type = EV_CLAP;
			xQueueSendFromISR(eventQueue, &ev, &hpw);
		}
		PORTA->ISFR = (1 << SOUND_PIN);
	}
	portYIELD_FROM_ISR(hpw);
}

static void initUART2(uint32_t baudRate) {
	NVIC_DisableIRQ(UART2_FLEXIO_IRQn);
	UART2->C2 &= ~(UART_C2_TE_MASK | UART_C2_RE_MASK);
	setMux(PORTE, UART_TX_PIN, 4);
	setMux(PORTE, UART_RX_PIN, 4);

	uint32_t busClk = CLOCK_GetBusClkFreq();
	uint32_t sbr = (busClk + (baudRate * 8)) / (baudRate * 16);
	UART2->BDH &= ~UART_BDH_SBR_MASK;
	UART2->BDH |= ((sbr >> 8) & UART_BDH_SBR_MASK);
	UART2->BDL = (uint8_t)(sbr & 0xFF);

	UART2->C1 &= ~(UART_C1_LOOPS_MASK | UART_C1_RSRC_MASK | UART_C1_PE_MASK | UART_C1_M_MASK);
	UART2->C2 |= UART_C2_RIE_MASK | UART_C2_TE_MASK | UART_C2_RE_MASK;

	NVIC_SetPriority(UART2_FLEXIO_IRQn, UART2_INT_PRIO);
	NVIC_ClearPendingIRQ(UART2_FLEXIO_IRQn);
	NVIC_EnableIRQ(UART2_FLEXIO_IRQn);
}

void UART2_FLEXIO_IRQHandler(void) {
	static uint8_t frame[PACKET_SIZE];
	static int rxIdx = 0;
	BaseType_t hpw = pdFALSE;

	if ((UART2->C2 & UART_C2_TIE_MASK) && (UART2->S1 & UART_S1_TDRE_MASK)) {
		if (txIdx < PACKET_SIZE) {
			UART2->D = txFrame[txIdx++];
		} else {
			UART2->C2 &= ~UART_C2_TIE_MASK;
			xSemaphoreGiveFromISR(txDoneSem, &hpw);
		}
	}
	if (UART2->S1 & UART_S1_RDRF_MASK) {
		uint8_t byte = UART2->D;
		if (rxIdx > 0 || byte == PACKET_START) {
			frame[rxIdx++] = byte;
			if (rxIdx == PACKET_SIZE) {
				for (int i = 0; i < PACKET_SIZE; i++) {
					rxFrame[i] = frame[i];
				}
				rxIdx = 0;
				xSemaphoreGiveFromISR(uartRxSem, &hpw);
			}
		}
	}
	portYIELD_FROM_ISR(hpw);
}

static void sendPacket(uint8_t type, uint8_t a, uint8_t b) {
	Packet p = {type, a, b};
	xQueueSend(txQueue, &p, 0);
}

static void playSound(Sound s) {
	xQueueSend(soundQueue, &s, 0);
}

static void lockSession(void) {
	session.state = ST_LOCKED;
	playSound(SND_LOCK);
}

static void grantVideos(void) {
	session.left += UNLOCK_BONUS;
	session.state = ST_OPEN;
	playSound(SND_TUNE);
}

static void onShortPress(void) {
	switch (session.state) {
	case ST_IDLE:
		session.pos = 0;
		session.maxPos = 0;
		sendPacket(PKT_OPEN, 0, 0);
		if (session.left > 0) {
			session.state = ST_OPEN;
			playSound(SND_BEEP);
		} else {
			session.state = ST_LOCKED;
			playSound(SND_ERROR);
		}
		break;
	case ST_OPEN:
	case ST_LOCKED:
		session.state = ST_IDLE;
		sendPacket(PKT_CLOSE, 0, 0);
		playSound(SND_BEEP);
		break;
	default:
		playSound(SND_ERROR);
		break;
	}
}

static void onLongPress(void) {
	if (session.state != ST_LOCKED) {
		playSound(SND_ERROR);
		return;
	}
	denyShownAt = 0;
	session.state = ST_WAITING;
	sendPacket(PKT_ASK, 0, 0);
	playSound(SND_BEEP);
}

static void onNext(void) {
	if (session.state == ST_LOCKED) {
		playSound(SND_ERROR);
		return;
	}
	if (session.state != ST_OPEN) {
		return;
	}
	session.pos++;
	bool isNew = session.pos > session.maxPos;
	if (isNew) {
		session.maxPos = session.pos;
		session.left--;
	}
	sendPacket(PKT_NEXT, isNew, session.left);
	if (session.left == 0) {
		lockSession();
	} else {
		playSound(session.left <= AMBER_LEFT ? SND_WARN : SND_BEEP);
	}
}

static void onPrev(void) {
	if (session.state != ST_OPEN || session.pos == 0) {
		return;
	}
	session.pos--;
	sendPacket(PKT_PREV, 0, session.left);
	playSound(session.left <= AMBER_LEFT ? SND_WARN : SND_BEEP);
}

static void onClap(void) {
	if (session.state != ST_OPEN) {
		return;
	}
	sendPacket(PKT_LIKE, 0, 0);
	playSound(SND_BEEP);
}

static void onSync(int left) {
	session.left = left;
	if (session.state == ST_OPEN && left == 0) {
		lockSession();
	} else if (session.state == ST_LOCKED && left > 0) {
		session.state = ST_IDLE;
	}
}

static void onFriendReply(bool approved) {
	if (session.state != ST_WAITING) {
		return;
	}
	if (approved) {
		grantVideos();
	} else {
		session.state = ST_LOCKED;
		denyShownAt = xTaskGetTickCount();
		if (denyShownAt == 0) {
			denyShownAt = 1;
		}
		playSound(SND_ERROR);
	}
}

static void onBreak(EventType type, int secs) {
	if (type == EV_BREAK_START && session.state == ST_LOCKED) {
		session.state = ST_BREAK;
		session.breakSecs = secs;
		playSound(SND_BEEP);
		return;
	}
	if (session.state != ST_BREAK) {
		return;
	}
	if (type == EV_BREAK_TICK) {
		session.breakSecs = secs;
	} else if (type == EV_BREAK_CANCEL) {
		session.state = ST_LOCKED;
		playSound(SND_ERROR);
	} else if (type == EV_BREAK_DONE) {
		grantVideos();
	}
}

static void controlTask(void *p) {
	(void)p;
	Event ev;

	while (1) {
		xQueueReceive(eventQueue, &ev, portMAX_DELAY);
		xSemaphoreTake(sessionMutex, portMAX_DELAY);
		State before = session.state;
		switch (ev.type) {
		case EV_SHORT_PRESS:
			onShortPress();
			break;
		case EV_LONG_PRESS:
			onLongPress();
			break;
		case EV_NEXT:
			onNext();
			break;
		case EV_PREV:
			onPrev();
			break;
		case EV_CLAP:
			onClap();
			break;
		case EV_SYNC:
			onSync(ev.value);
			break;
		case EV_APPROVED:
		case EV_DENIED:
			onFriendReply(ev.type == EV_APPROVED);
			break;
		default:
			onBreak(ev.type, ev.value);
			break;
		}
		if (session.state != before) {
			sendPacket(PKT_STATE, session.state, session.left);
		}
		xSemaphoreGive(sessionMutex);
	}
}

static void joystickTask(void *p) {
	(void)p;
	bool centred = true;

	while (1) {
		int value = readJoystick();
		if (centred && (value < JOY_LOW || value > JOY_HIGH)) {
			Event ev = {(value > JOY_HIGH) ? EV_NEXT : EV_PREV, 0};
			xQueueSend(eventQueue, &ev, 0);
			centred = false;
		} else if (value > JOY_CENTRE_LOW && value < JOY_CENTRE_HIGH) {
			centred = true;
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}

static void buzzerTask(void *p) {
	(void)p;
	Sound s;

	while (1) {
		xQueueReceive(soundQueue, &s, portMAX_DELAY);
		for (int i = 0; i < MELODIES[s].count; i++) {
			buzzerTone(MELODIES[s].notes[i].freq);
			vTaskDelay(pdMS_TO_TICKS(MELODIES[s].notes[i].ms));
		}
		buzzerTone(0);
	}
}

static void displayTask(void *p) {
	(void)p;

	while (1) {
		xSemaphoreTake(sessionMutex, portMAX_DELAY);
		Session s = session;
		xSemaphoreGive(sessionMutex);

		bool blinkOn = (xTaskGetTickCount() / pdMS_TO_TICKS(500)) % 2 == 0;
		switch (s.state) {
		case ST_IDLE:
			setLED(0, 0, false);
			lcdShow(TEXT_OFF, false);
			break;
		case ST_OPEN:
			if (s.left <= AMBER_LEFT) {
				setLED(100, 25, false);
			} else {
				setLED(0, 100, false);
			}
			lcdNumber(s.left);
			break;
		case ST_LOCKED:
			setLED(100, 0, false);
			if (denyShownAt != 0 && xTaskGetTickCount() - denyShownAt < pdMS_TO_TICKS(DENY_SHOW_MS)) {
				lcdShow(TEXT_DENY, false);  // friend denied: "dEny" for 3 s, then back to "LOC"
			} else {
				lcdShow(TEXT_LOC, false);
			}
			break;
		case ST_WAITING:
			setLED(blinkOn ? 100 : 0, 0, false);
			lcdShow(TEXT_SENT, false);
			break;
		case ST_BREAK:
			setLED(0, 0, true);
			lcdTime(s.breakSecs);
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

static void uartRxTask(void *p) {
	(void)p;
	uint8_t f[PACKET_SIZE];

	while (1) {
		xSemaphoreTake(uartRxSem, portMAX_DELAY);
		taskENTER_CRITICAL();
		for (int i = 0; i < PACKET_SIZE; i++) {
			f[i] = rxFrame[i];
		}
		taskEXIT_CRITICAL();
		if ((f[1] ^ f[2] ^ f[3]) != f[4]) {
			continue;
		}

		Event ev = {EV_SYNC, (f[2] << 8) | f[3]};
		switch (f[1]) {
		case PKT_SYNC:
			ev.type = EV_SYNC;
			break;
		case PKT_APPROVED:
			ev.type = EV_APPROVED;
			break;
		case PKT_DENIED:
			ev.type = EV_DENIED;
			break;
		case PKT_BREAK_START:
			ev.type = EV_BREAK_START;
			break;
		case PKT_BREAK_TICK:
			ev.type = EV_BREAK_TICK;
			break;
		case PKT_BREAK_CANCEL:
			ev.type = EV_BREAK_CANCEL;
			break;
		case PKT_BREAK_DONE:
			ev.type = EV_BREAK_DONE;
			break;
		default:
			continue;
		}
		xQueueSend(eventQueue, &ev, portMAX_DELAY);
	}
}

static void uartTxTask(void *p) {
	(void)p;
	Packet pkt;

	while (1) {
		xQueueReceive(txQueue, &pkt, portMAX_DELAY);
		txFrame[0] = PACKET_START;
		txFrame[1] = pkt.type;
		txFrame[2] = pkt.a;
		txFrame[3] = pkt.b;
		txFrame[4] = pkt.type ^ pkt.a ^ pkt.b;
		txIdx = 0;
		UART2->C2 |= UART_C2_TIE_MASK;
		xSemaphoreTake(txDoneSem, portMAX_DELAY);
	}
}

int main(void) {
	BOARD_InitBootPins();
	BOARD_InitBootClocks();
	BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
	BOARD_InitDebugConsole();
#endif

	eventQueue = xQueueCreate(16, sizeof(Event));
	soundQueue = xQueueCreate(8, sizeof(Sound));
	txQueue = xQueueCreate(16, sizeof(Packet));
	sessionMutex = xSemaphoreCreateMutex();
	uartRxSem = xSemaphoreCreateBinary();
	txDoneSem = xSemaphoreCreateBinary();

	initClocks();
	initLED();
	initBuzzer();
	initLCD();
	initJoystick();
	initInputs();
	initUART2(BAUD_RATE);
	PRINTF("Doomscroll Breaker\r\n");

	sendPacket(PKT_HELLO, 0, 0);

	xTaskCreate(uartRxTask, "uart_rx", configMINIMAL_STACK_SIZE + 100, NULL, 4, NULL);
	xTaskCreate(controlTask, "control", configMINIMAL_STACK_SIZE + 100, NULL, 3, NULL);
	xTaskCreate(joystickTask, "joystick", configMINIMAL_STACK_SIZE + 100, NULL, 2, NULL);
	xTaskCreate(buzzerTask, "buzzer", configMINIMAL_STACK_SIZE + 100, NULL, 2, NULL);
	xTaskCreate(uartTxTask, "uart_tx", configMINIMAL_STACK_SIZE + 100, NULL, 2, NULL);
	xTaskCreate(displayTask, "display", configMINIMAL_STACK_SIZE + 100, NULL, 1, NULL);
	vTaskStartScheduler();

	while (1) {
	}
	return 0;
}
