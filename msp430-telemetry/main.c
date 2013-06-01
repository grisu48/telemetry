/* Author: Domen Puncer <domen@cba.si>.  License: WTFPL */
/* Modified for CCS: Luka Mustafa <musti@wlan-si.net>*/

/* just some jump optimizations for CCS/IAR, disable */
#define __even_in_range(x, y) x

//#include <isr_compat.h>
#include <stdlib.h>
#include "msp430.h"
#include "stdio.h"
#include "string.h"
#include "ctype.h"
#include "circ_buf.h"
#include "1w.h"
#include "bitbang_1w.h"
#include "msp430_gpio.h"
#include "board.h"
#include "types.h"
#include "msp430_i2c.h"


#define ALEN(x) (sizeof(x)/sizeof(x[0]))

#define FAMILY_DS18B20 0x28

//if connected to the router
#define BAUDRATE 115200
//if connected to the launchpad
//#define BAUDRATE 9600

//define pin on which 1W bus is
#define GPIO_1W GPIO_P1_5

#define HELLO "wlan-si telemetry 0.4"
#define WATCHDOG_TIMEOUT 660 /* seconds */
#define UNRESET_TIMEOUT  10 /* seconds */
#define SCANREAD_TIMEOUT 60 /* seconds */

/*
 * vnq05xsp16 wiring:
 * SELB     P2.6
 * SELA     P2.7
 * SENSEN   P2.5  active high
 * CSENSE   P1.4  470R to gnd U=R/I
 */
#define SELA     GPIO_P2_7
#define SELB     GPIO_P2_6
#define SENSEN   GPIO_P1_4
#define CSENSE   GPIO_P2_5

/* list of channels and default values (0 = off, 1 = on) */
static const struct {
	gpio_t gpio;
	int value;
} channels[] = {
	{ GPIO_P2_0, 1 },
	{ GPIO_P2_1, 1 },
	{ GPIO_P2_2, 1 },
	{ GPIO_P2_4, 1 },
	{ GPIO_P2_3, 1 },
	{ GPIO_P1_3, 1 },
};

enum gpio_trigger { GPIO_RISING, GPIO_FALLING };
/* list of gpio interrupts */
static const struct {
	gpio_t gpio;
	enum gpio_trigger trigger;
} gpio_interrupts[] = {
	{ GPIO_P1_0, GPIO_RISING },
};

static int watchdog_channel; /* 0 disabled, 1-6 channel */
static int watchdog_timeout; /* transition to zero triggers the reset procedure */
static int unreset_timeout; /* transition to zero finishes the reset line */
static int scanread_timeout; /* transition to zero triggers scan_read command */

volatile static u16 gpio_interrupt;

static int reset_pending;
static int unreset_pending;
static int scanread_pending;

static int initialized;

/*
 * Commands (all commands and arguments are case insensitive):
 * ATZ                   - initialize module
 *   reply: OK:wlan-si telemetry 0.1
 * channel <channel> on  - set output channel to 1
 * channel <channel> off - set output channel to 0
 * csense                - display current consumption for channels 1-4
 * watchdog on <channel> - start 300s watchdog, if no ping received, reboot is triggered
 * watchdog off          - disable watchdog
 * watchdog ping         - reset watchdog timer to 300s
 * 1w scan               - scan 1w bus for devices
 *   example reply:
 *     OK:1 1-wire devices:
 *     1WIRE DEV 2c1647070000003d
 * 1w scan_read          - scan 1w bus for devices, also read ds18b20 temperatures
 *   example reply:
 *     OK:2 1-wire devices:
 *     1WIRE DS18B20 288055bb03000052 18.43
 *     1WIRE DEV 236e5b8200000043
 * 1w match_rom <id64>   - 1w primitive MATCH_ROM, used to select device on bus
 * 1w read_rom           - 1w primitive READ_ROM, use to read device id for only device
 * 1w skip_rom           - 1w primitive SKIP_ROM, use if only 1 device on bus
 * 1w read [bytes_dec]   - read a byte or bytes, ie. "1w read 11" will read 11 bytes
 *   example reply:
 *   1WIRE DATA 27 01 4b 46 7f ff 09 10 72
 *   OK
 * 1w write <hex8>...    - write bytes, ie. "1w write 12 3c 8a"
 *   reply: OK:<number of bytes written>
 *
 * i2c <hexaddr> write <hex8>...  - write bytes, ie. "i2c 5a write 21 3a"
 * i2c <hexaddr> read <bytes_dec> - read bytes, ie. "i2c 5a read 2" will read 2 bytes from address 0x5a (so, 0x5a<<1|1, basically). 0-byte read is valid, and can be use to detect device presence.
 *   example reply:
 *   I2C 5a DATA 3e 51
 *   OK
 * i2c <hexaddr> write <hex8>... read <bytes_dec>  - a merge of write and read commands, commonly used for register writes for I2C devices.
 *
 *
 * Replies and prompts:
 * OK                    - command succeeded, possible extra arguments
 * ERROR                 - command failed, possible extra arguments
 * WATCHDOG reset        - watchdog reset triggered
 * WATCHDOG unreset      - watchdog, reset line released
 * INTERRUPT <hex16>     - there was an interrupt on pin(s), hex mask, bit 0 is P1.0, bit 15 P2.7
 *
 * 
 * Uninitialized (before ATZ) device.
 * Uninitialized device will perform as if somebody was sending
 * "1w scan_read" command at 60 seconds interval.
 * After initialization, this automatic reporting stops, and has to be
 * triggered manually.
 *
 *
 * Example use of 1w primitives to read temperature from ds18b20:
 * 1w match_rom 288055bb03000052
 * OK
 * 1w write 44
 * OK:1
 * 1w match_rom 288055bb03000052
 * OK
 * 1w write be
 * OK:1
 * 1w read 9
 * 1WIRE DATA 27 01 4b 46 7f ff 09 10 72
 * OK
 * (note: 0x127/16.0 = 18.4375)
 */

static struct circ_buf uart_rx;
static u8 uart_rx_buf[64];

static struct w1_master _w1, *w1 = &_w1;


int unhex4(const char c)
{
	int x;
	if (c < 'A')
		x = c - '0';
	else
		x = (c&~0x20) - 'A' + 10;

	if (x < 0 || x >= 16)
		return -1;
	return x;
}

int unhex8(const char *str)
{
	int x = unhex4(str[0]);
	if (x < 0)
		return -1;
	x = x<<4 | unhex4(str[1]);
	if (x < 0)
		return -1;
	return x;
}

int undec(const char *str)
{
	int x = 0;

	while (isdigit(*str)) {
		x *= 10;
		x += *str - '0';
		str++;
	}
	return x;
}

int w1_unpack(w1_addr_t *addr, const char *str)
{
	int i;
	for (i=0; i<8; i++) {
		int tmp = unhex8(&str[i*2]);
		if (tmp < 0)
			return -1;
		addr->bytes[i] = tmp;
	}
	return 0;
}

static int handle_1w(const char *line)
{
	if (strncmp(line, "read", 4) == 0 && line[4] != '_') {
		int n = 1;
		if (line[4] == ' ')
			n = undec(line+5);

		if (n < 1) {
			printf("\nERROR:arguments\n");
			return 0;
		}

		printf("1WIRE DATA");
		while (n--) {
			int r = w1_read(w1);
			if (r < 0) {
				printf("\nERROR:%i\n", r);
				return 0;
			}
			printf(" %02x", r);
		}
		printf("\nOK\n");
	} else
	if (strncmp(line, "write ", 6) == 0) {
		line += 5;
		int written = 0;
		while (line[0] == ' ') {
			int data = unhex8(line+1);
			if (data < 0)
				break;
			int r = w1_write(w1, data);
			if (r < 0) {
				printf("ERROR:%i\n", r);
				return 0;
			}
			line += 3;
			written++;
		}
		printf("OK:%i\n", written);
	} else
	if (strncmp(line, "read_rom", 8) == 0) {
		w1_addr_t addr;
		int r = w1_read_rom(w1, &addr);
		if (r < 0) {
			printf("ERROR:%i\n", r);
			return 0;
		}
		u8 *b = addr.bytes;
		printf("OK:%02x%02x%02x%02x%02x%02x%02x%02x\n",
				b[0], b[1], b[2], b[3],
				b[4], b[5], b[6], b[7]);
	} else
	if (strncmp(line, "match_rom ", 10) == 0) {
		w1_addr_t addr;
		w1_unpack(&addr, line+10);
		int r = w1_match_rom(w1, addr);
		if (r < 0) {
			printf("ERROR:%i\n", r);
			return 0;
		}
		printf("OK\n");
	} else
	if (strncmp(line, "skip_rom", 8) == 0) {
		int r = w1_skip_rom(w1);
		if (r < 0) {
			printf("ERROR:%i\n", r);
			return 0;
		}
		printf("OK\n");
	} else
	if (strcmp(line, "scan") == 0) {
		w1_addr_t addrs[5];
		int n = w1_scan(w1, addrs, 5);
		if (n <= 0) {
			printf("ERROR:%i\n", n);
			return 0;
		}
		printf("OK:%i 1-wire devices:\n", n);
		int i;
		for (i=0; i<n; i++) {
			u8 *b = addrs[i].bytes;
			printf("1WIRE DEV %02x%02x%02x%02x%02x%02x%02x%02x\n",
					b[0], b[1], b[2], b[3],
					b[4], b[5], b[6], b[7]);
		}
	} else
	if (strcmp(line, "scan_read") == 0) {
		w1_addr_t addrs[5];
		int n = w1_scan(w1, addrs, 5);
		if (n <= 0) {
			printf("ERROR:%i\n", n);
			return 0;
		}
//		printf("OK:%i 1-wire devices:\n", n);
		int i;
		for (i=0; i<n; i++) {
			//u8 *b = addrs[i].bytes;
			if (addrs[i].bytes[0] == FAMILY_DS18B20) {
				s16 temp;
				u8 tmp[9];
				w1_match_rom(w1, addrs[i]);
				w1_write(w1, 0x44); /* conversion */
				mdelay(750); /* 750ms conversion time */
				w1_match_rom(w1, addrs[i]);
				w1_write(w1, 0xbe); /* read */

				int j;
				for (j=0; j<9; j++)
					tmp[j] = w1_read(w1);

				if (crc8r(tmp, 9) != 0)
					continue;

				temp = tmp[1]<<8 | tmp[0];
/*
				printf("1WIRE DS18B20 %02x%02x%02x%02x%02x%02x%02x%02x ",
						b[0], b[1], b[2], b[3],
						b[4], b[5], b[6], b[7]);
*/				if (temp < 0) {
					printf("-");
					temp = -temp;
				}
				printf("%i.%02i\n", temp>>4, 100*(temp&0xf)/16);
/*			} else {
				printf("1WIRE DEV %02x%02x%02x%02x%02x%02x%02x%02x\n",
						b[0], b[1], b[2], b[3],
						b[4], b[5], b[6], b[7]);
*/			}
		}
	} else {
		printf("ERROR:unknown 1w command '%s'\n", line);
	}
	return 0;
}

static void i2c_power_cycle(void)
{
#if 0
	gpio_set(I2C_POWER, 0);
	mdelay(10);
	gpio_set(I2C_POWER, 1);
#endif
}

static int handle_i2c(const char *line)
{
	/* i2c slaves power cycle */
	if (strcmp(line, "reset") == 0) {
		i2c_power_cycle();
		printf("OK\n");
		return 0;
	}

	int addr = unhex8(line);
	u8 buf[32];
	u8 *rxbuf = NULL;
	u8 *txbuf = NULL;
	int rxlen, txlen;

	if (addr < 0 || line[2] != ' ')
		goto error;
	line += 3;

	if (strncmp(line, "write ", 6) == 0) {
		line += 5;
		txlen = 0;
		txbuf = buf;
		while (line[0] == ' ') {
			int data = unhex8(line+1);
			if (data < 0) {
				line++;
				break;
			}
			txbuf[txlen++] = data;
			line += 3;
		}
	}

	/* read by itself or after write */
	if (strncmp(line, "read", 4) == 0 && line[4] != '_') {
		int n = 0;
		if (line[4] == ' ')
			n = undec(line+5);

		if (n > 0) {
			rxlen = n;
			rxbuf = buf;
		}

	} else if (line[0]) {
		printf("ERROR:unknown i2c command '%s'\n", line);
		return -1;
	}

	int r = i2c_write_read(addr, txbuf, txlen, rxbuf, rxlen);
	if (r < 0) {
		printf("ERROR:i2c transfer\n");
		return -1;
	}

	if (rxbuf) {
		printf("I2C DATA");
		while (rxlen--) {
			printf(" %02x", *rxbuf++);
		}
		printf("\n");
	}
	printf("OK\n");

	return 0;

 error:
	printf("ERROR:invalid format\n");
	return -1;
}

static int handle_channel(const char *line)
{
	int chn;

	chn = undec(line);
	if (chn < 1 || chn > ALEN(channels) || line[1] != ' ') {
		printf("ERROR:channel arguments\n");
		return -1;
	}
	if (chn == watchdog_channel) {
		printf("ERROR:channel forbidden to control watchdog\n");
		return -1;
	}
	line += 2;

	if (strcmp(line, "on") == 0) {
//		printf("OK\n");
		gpio_set(channels[chn-1].gpio, 1);
	} else
	if (strcmp(line, "off") == 0) {
//		printf("OK\n");
		gpio_set(channels[chn-1].gpio, 0);
	} else {
		printf("ERROR:channel arguments\n");
	}

	return 0;
}

static int adc_read(void)
{
	const int maxreads = 1000;
	int i;
	u32 average = 0;

	for (i=0; i<maxreads; i++) {
		ADC10CTL0 |= ENC + ADC10SC;
		while (ADC10CTL1 & ADC10BUSY)
			;
		average += ADC10MEM;
	}

	average = (average+maxreads/2) / maxreads;
	return average;
}

static int handle_csense(void)
{
	int i;

	for (i=0; i<4; i++) {
		gpio_set(SELA, (i&1) == 1);
		gpio_set(SELB, (i&2) == 2);
		gpio_set(SENSEN, 1);
		mdelay(1); /* csense delay is max 500us */
//		printf("CSENSE %i %i\n", i+1, adc_read());
		if (i)
			printf(",");
		printf("%i", adc_read());

		gpio_set(SENSEN, 0);
	}
	printf("\n");
//	printf("OK\n");

	return 0;
}

static int handle_watchdog(const char *line)
{
	if (strncmp(line, "on ", 3) == 0) {
		watchdog_channel = undec(line+3);
		if (watchdog_channel < 1 || watchdog_channel > 6) {
			printf("ERROR:watchdog channel\n");
			return -1;
		}
//		printf("OK\n");
		watchdog_timeout = WATCHDOG_TIMEOUT;
	} else
	if (strcmp(line, "off") == 0) {
		watchdog_channel = 0;
		watchdog_timeout = 0;
//		printf("OK\n");
	} else
	if (strcmp(line, "ping") == 0) {
		__disable_interrupt();
		watchdog_timeout = WATCHDOG_TIMEOUT;
		__enable_interrupt();
//		printf("OK\n");
	} else {
		printf("ERROR:watchdog arguments\n");
	}
	return 0;
}

static int handle(const char *line)
{
	if (strncmp(line, "1w ", 3) == 0) {
		return handle_1w(line+3);
	} else if (strncmp(line, "i2c ", 4) == 0) {
		return handle_i2c(line+4);
	} else if (strncmp(line, "watchdog ", 9) == 0) {
		return handle_watchdog(line+9);
	} else if (strncmp(line, "channel ", 8) == 0) {
		return handle_channel(line+8);
	} else if (strcmp(line, "csense") == 0) {
		return handle_csense();
	} else if (strcmp(line, "atz") == 0) {
		printf("OK:%s\n", HELLO);
		initialized = 1;
		return 0;
	} else if (strcmp(line, "hang") == 0) {
		/* test watchdog works */
		while (1)
			;
	}

	printf("ERROR:unknown command '%s'\n", line);
	return 0;
}


/*
 * ACOM /0 - hello
 * ACOM /1 - temperature
 * ACOM /2 - watchdog channel
 * ACOM /9 - csense
 */
static int handle_acom_read(int reg)
{
	if (reg == 0)
		printf("%s\n", HELLO);
	else if (reg == 1)
		handle("1w scan_read");
	else if (reg == 2)
		printf("%i\n", watchdog_channel);
	else if (reg >= 3 && reg <=8) {
		reg -= 3;
		printf("%i\n", channels[reg].value);
	}
	else if (reg == 9)
		handle("csense");
	else
		return 1;
	return 0;
}

/*
 * ACOM /0 - write test
 * ACOM /1 - nothing
 * ACOM /2 - watchdog channel
 * ACOM /3 - channel 1 state
 * ACOM /4 - channel 2 state
 * ACOM /5 - channel 3 state
 * ACOM /6 - channel 4 state
 * ACOM /7 - channel 5 state
 * ACOM /8 - channel 6 state
 */
static int handle_acom_write(int reg, int value)
{
	char buf[32];

	if (reg == 0)
		printf("%i\n", value);
	else if (reg == 2) {
		if (value == 0)
			handle("watchdog off");
		else {
			sprintf(buf, "watchdog on %i", value);
			handle(buf);
		}
	} else if (reg >= 3 && reg <=8) {
		reg -= 3;
		sprintf(buf, "channel %i %s", reg+1, value?"on":"off");
		handle(buf);
	}
	else
		return 1;
	return 0;
}


static int handle_acom(const char *line)
{
	int reg = -1;
	int value = -1;

	handle("watchdog ping");

	line += 6;
	if (isdigit(line[0]))
		reg = atoi(line);
	line = strchr(line, ' ');
	if (line) {
		line++;
		if (isdigit(line[0]))
			value = atoi(line);
	}

	if (value != -1)
		return handle_acom_write(reg, value);
	else if (reg != -1)
		return handle_acom_read(reg);

	return 1;
}

static void msp430_gpio_interrupt(gpio_t gpio, enum gpio_trigger trigger)
{
	int port = (gpio & 0xf0) >> 4;
	int pin = gpio & 0xf;

	if (trigger == GPIO_RISING) {
		gpio_init(gpio, GPIO_INPUT_PD, 0);
		if (port == 1)
			P1IES &= ~(1<<pin);
		else if (port == 2)
			P2IES &= ~(1<<pin);
	} else {
		gpio_init(gpio, GPIO_INPUT_PU, 0);
		if (port == 1)
			P1IES |= 1<<pin;
		else if (port == 2)
			P2IES |= 1<<pin;
	}

	if (port == 1) {
		P1IFG &= ~(1<<pin);
		P1IE |= 1<<pin;
	} else if (port == 2) {
		P2IFG &= ~(1<<pin);
		P2IE |= 1<<pin;
	}
}


int main(void)
{
	WDTCTL = WDTPW + WDTHOLD; /* stop watchdog */

	/* set clock to 16 MHz */
	DCOCTL = 0x00;
#if CONFIG_HZ == 16000000
	BCSCTL1 = CALBC1_16MHZ;
	DCOCTL = CALDCO_16MHZ;
#else
#error "invalid CONFIG_HZ"
#endif

	/* set up watchdog to run off ACLK */
	/* internal VLO clock runs at 4-20MHz */
	BCSCTL1 |= DIVA_3; /* XTS=0, DIVAx=/8 */
	BCSCTL3 = LFXT1S_2; /* VLO */
	WDTCTL = WDTPW | WDTSSEL /* ACLK */ | WDTCNTCL /* counter clear */;

	/* initialize usci in uart mode */
	circ_buf_init(&uart_rx, uart_rx_buf, sizeof(uart_rx_buf));

	P1SEL |= 0x06;            /* usci rxd, txd - P1.1,P1.2 */
	P1SEL2 |= 0x06;           /* usci rxd, txd - P1.1,P1.2 */

	int i;
	/* setup gpio interrupts */
	for (i=0; i<ALEN(gpio_interrupts); i++)
		msp430_gpio_interrupt(gpio_interrupts[i].gpio, gpio_interrupts[i].trigger);

	UCA0CTL1 = UCSWRST;
	UCA0CTL0 = 0;
	u16 br = (CONFIG_HZ+BAUDRATE/2)/BAUDRATE;
	UCA0BR0 = br & 0xff;
	UCA0BR1 = br>>8 & 0xff;
	UCA0CTL1 |= 2<<6; /* smclk clock source */
	UCA0CTL1 &= ~UCSWRST;
	IE2 |= UCA0RXIE;

	/* initialize 1s timer tick */
	TACCTL0 = CCIE;
	TACCR0 = 50000; /* *8, every 25ms */
	TACTL = TASSEL_2 | ID_3 | MC_2; /* SMCLK, /8, continuous mode */

	/* 1-wire init */
	int pin = GPIO_1W;
	struct bitbang_1w_data bitbang_1w_data = {
		.pin = pin,
	};
	w1->priv = &bitbang_1w_data;
	bitbang_1w_register(w1);

	/* initialize vnq05xsp16 */
	/* use xtal pins as gpio */
	P2SEL &= ~(1<<6);
	P2SEL2 &= ~(1<<6);
	P2SEL &= ~(1<<7);
	P2SEL2 &= ~(1<<7);
	gpio_init(SELA, GPIO_OUTPUT, 0);
	gpio_init(SELB, GPIO_OUTPUT, 0);
	gpio_init(SENSEN, GPIO_OUTPUT, 0);
	gpio_init(CSENSE, GPIO_INPUT, 0);
	ADC10CTL1 = INCH_4; /* set input channel, P1.0 */
	ADC10CTL0 = SREF_1 /* Vref */ + ADC10SHT_2 + REFON + ADC10ON;

	/* initialize channels */
	for (i=0; i<ALEN(channels); i++)
		gpio_init(channels[i].gpio, GPIO_OUTPUT, channels[i].value);

	char line[64];
	int line_pos = 0;
	//gpio_init(GPIO_P1_6, GPIO_OUTPUT, 1);
	//gpio_init(GPIO_P1_3, GPIO_INPUT_PU, 0);

	i2c_power_cycle();

	i2c_init(100000);

	__enable_interrupt();

	//printf("%s, running at %i MHz\n", __func__, (int)(CONFIG_HZ/1000/1000));

	scanread_timeout = SCANREAD_TIMEOUT;

	for (;;) {
		WDTCTL = WDTPW | WDTSSEL /* ACLK */ | WDTCNTCL /* counter clear */;

		if (!initialized && scanread_pending) {
			scanread_pending = 0;
// don't do auto scan_read			handle("1w scan_read");
		}
		if (reset_pending) {
			printf("WATCHDOG reset\n");
			reset_pending = 0;
			gpio_set(channels[watchdog_channel-1].gpio, 0);
		}
		if (unreset_pending) {
			printf("WATCHDOG unreset\n");
			unreset_pending = 0;
			gpio_set(channels[watchdog_channel-1].gpio, 1);
		}
		if (gpio_interrupt) {
			printf("INTERRUPT %04x\n", gpio_interrupt);
			gpio_interrupt = 0;
		}

		//gpio_set(GPIO_P1_6, gpio_get(GPIO_P1_3));
		int tmp = circ_buf_get_one(&uart_rx);
		if (tmp < 0)
			continue;

		//putchar(tmp);

		if (tmp == '\r' || tmp == '\n' || line_pos == sizeof(line)-1) {
			line[line_pos] = '\0';

			/* lowercase */
			int i;
			for (i=0; i<line_pos; i++)
				line[i] = tolower(line[i]);

			if (strncmp(line, "acom /", 6) == 0)
				handle_acom(line);
			else
				handle(line);
			line_pos = 0;
			continue;
		}

		line[line_pos++] = tmp;

	}

	return 0;
}

int putchar(int c)
{
	if (c == '\n')
		putchar('\r');

	while (!(IFG2 & UCA0TXIFG))
		;
	UCA0TXBUF = c;
	return 0;
}

static void tick_1s_handler(void)
{
	if (watchdog_channel && watchdog_timeout) {
		if (--watchdog_timeout == 0) {
			reset_pending = 1;
			unreset_timeout = UNRESET_TIMEOUT;
		}
	}

	if (unreset_timeout) {
		if (--unreset_timeout == 0) {
			unreset_pending = 1;
		}
	}

	if (scanread_timeout) {
		if (--scanread_timeout == 0) {
			scanread_timeout = SCANREAD_TIMEOUT;
			scanread_pending = 1;
		}
	}
}

// Timer A0 interrupt service routine
//#ifdef __GNUC__
//__attribute__((interrupt(TIMER0_A0_VECTOR)))
//void Timer_A_ISR(void)
//#else
#pragma vector=TIMER0_A0_VECTOR
__interrupt void Timer_A (void)
//#endif
{
	static int second_counter;
	TACCR0 += 50000; /* 25ms later */

	if (++second_counter == 40) {
		second_counter = 0;
		tick_1s_handler();
	}
}

//RX interrupt
//#ifdef __GNUC__
//__attribute__((interrupt(USCIAB0RX_VECTOR)))
//void USCI0RX_ISR(void)
//#else
#pragma vector=USCIAB0RX_VECTOR
__interrupt void USCI0RX_ISR(void)
//#endif
{
	circ_buf_put_one(&uart_rx, UCA0RXBUF);
}

// GPIO interrupts P1
//#ifdef __GNUC__
//__attribute__((interrupt(PORT1_VECTOR)))
//void P1_ISR(void)
//#else
#pragma vector=PORT1_VECTOR
__interrupt void P1_ISR(void)
//#endif
{
	gpio_interrupt |= P1IFG;
	P1IFG = 0;
}
// GPIO interrupts P2
//#ifdef __GNUC__
//__attribute__((interrupt(PORT2_VECTOR)))
//void P2_ISR(void)
//#else
#pragma vector=PORT2_VECTOR
__interrupt void P2_ISR(void)
//#endif
{
	gpio_interrupt |= P2IFG << 8;
	P2IFG = 0;
}
