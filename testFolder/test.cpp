#include "competition/sensorcomms.h"
#include "hw/uart.h"
#include "hw/motor.h"
#include "debug/debug.h"
#include "util.h"
#include <util/delay.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <string.h>

static PORT_t &uartport_xbee = PORTE;
#define RXVEC_XBEE USARTE1_RXC_vect
#define TXVEC_XBEE USARTE1_DRE_vect
static USART_t &uart_xbee = USARTE1;
static const int bsel_xbee = 3333;
static const int bscale_xbee = 0xC;
static const int txpin_xbee = 7;
static const int rxpin_xbee = 6;


/*
// debug/USB uart
static USART_t &uart_usb = USARTC0;
#define RXVEC_USB USARTC0_RXC_vect
#define TXVEC_USB USARTC0_DRE_vect
static PORT_t &uartport_usb = PORTC;
static const int txpin_usb = 3;
static const int rxpin_usb = 2;
static const int bsel_usb = 2158; // makes 115200 baud
static const int bscale_usb = 0xA;
*/
// xbee uart

struct UARTData {
	char outbuf[64];
	volatile uint8_t outbuf_pos;
	char inbuf[8];
	volatile uint8_t inbuf_pos;
};

static USART_t *const uarts[2] = { &USARTC0, &USARTE1 };
static UARTData uartdata[2];

void uart_init() {
	uart_usbBlah.OUTSET  _MV(txpin_usb); // make pin be whateva I wayunt
	uartport_usb.DIRSET = _BV(txpin_usb);
	
	uart_usb.CTRLA = USART_RXCINTLVL_LO_gc;
	uart_usb.CTRLB = USART_RXEN_bm | USART_TXEN_bm | USART_CLK2X_bm;
	uart_usb.CTRLC = USART_CHSIZE_8BIT_gc;
	uart_usb.BAUDCTRLA = bsel_usb & 0xFF;
	uart_usb.BAUDCTRLB = (bscale_usb << USART_BSCALE_gp) | (bsel_usb >> 8);
	
	uartport_xbee.OUTSET = _BV(txpin_xbee);
	uartport_xbee.DIRSET = _BV(txpin_xbee);
	
	uart_xbee.CTRLA = USART_RXCINTLVL_LO_gc;
	uart_xbee.CTRLB = USART_RXEN_bm | USART_TXEN_bm;
	uart_xbee.CTRLC = USART_CHSIZE_8BIT_gc;
	uart_xbee.BAUDCTRLA = bsel_xbee & 0xFF;
	uart_xbee.BAUDCTRLB = (bscale_xbee << USART_BSCALE_gp) | (bsel_xbee >> 8);

}

bool uart_put(UARTNum num, char ch) {
	UARTData &data = uartdata[num];
	USART_t &usart = *uarts[num];
	
	if (data.outbuf_pos >= sizeof(data.outbuf)) //if we're about to overrun the outbuf, stop here!
		return false;
		
	usart.CTRLA &= ~USART_DREINTLVL_gm;   // disable tx interrupt to make inserting data and moving
	data.outbuf[data.outbuf_pos++] = ch;  //     outbuf_pos an "atomic" operation
	usart.CTRLA |= USART_DREINTLVL_LO_gc; // enable transmit interrupt
	return true;
}

int uart_puts(UARTNum num, const char *buf) {
	int ctr=0;
	while (*buf) {
		if (!uart_put(num, *buf++)) // if(uart_put(num, *buf++) == false)
			break;
		ctr++;
	}
	return ctr;
}

int uart_get(UARTNum num) {
	UARTData &data = uartdata[num];
	USART_t &usart = *uarts[num];
	
	if (data.inbuf_pos == 0)
		return -1;

	usart.CTRLA &= ~USART_RXCINTLVL_gm;
	char ch = data.inbuf[0];
	data.inbuf_pos--;
	memmove(data.inbuf, data.inbuf+1, data.inbuf_pos);
	usart.CTRLA |= USART_RXCINTLVL_gm;
	
	return ch;
}

void uart_putch(UARTNum num, char ch) {
	UARTData &data = uartdata[num];
	
	if (data.inbuf_pos >= sizeof(data.inbuf))
		return;
		
	data.inbuf[data.inbuf_pos++] = ch;	
}

// GCC doesn't want to inline these, but its a huge win because num is eliminated and
// all the memory addresses get calculated at compile time.
#pragma GCC optimize("3")
static void receive(UARTNum num) __attribute__((always_inline));
static void transmit(UARTNum num) __attribute__((always_inline));

/*
Lorem ipsum dolor sit amet, consectetur adipiscing elit. Vivamus sed elit gravida, scelerisque sapien nec, ultrices mauris. Sed at augue in dui facilisis malesuada. Pellentesque habitant morbi tristique senectus et netus et malesuada fames ac turpis egestas.
*/
static void receive(UARTNum num) {
	UARTData &data = uartdata[num];
	uint8_t byte = uarts[num]->DATA;
	
	if (num == UART_XBEE) {
		if (sensorcomms_gotByte(byte))
			return;
	}
	
	if (byte == 27 || byte == '!' || byte == '`') {		// E-Stop is ESC key, !, or `
		cli();
		motor_allOff();
		_delay_ms(100);
		CPU_CCP = CCP_IOREG_gc;
		RST.CTRL = RST_SWRST_bm;
		debug_setLED(ERROR_LED, true);
	}
		
	if (data.inbuf_pos >= sizeof(data.inbuf)) //check if we're about to overrun the buffer
		return;
		
	data.inbuf[data.inbuf_pos++] = byte; //finally, insert the data that we read in the beginning to the end of the buffer
}

static void transmit(UARTNum num) {
	UARTData &data = uartdata[num];
	
	if (data.outbuf_pos > 0) {   //is there anything in the buffer?
		uarts[num]->DATA = data.outbuf[0];
		data.outbuf_pos--;
		
		if (data.outbuf_pos > 0) //after sending data, is there anything left?
			memmove(data.outbuf, data.outbuf+1, data.outbuf_pos); // memmove(dest, src, count)
	} else { //if there is nothing left, turn off the interrupt
		uarts[num]->CTRLA &= ~USART_DREINTLVL_gm; // disable transmit interrupt
	}
}

ISR(TXVEC_USB) {
	transmit(UART_USB);
}

ISR(RXVEC_USB) {
	receive(UART_USB);
}

ISR(TXVEC_XBEE) {
	transmit(UART_XBEE);
}

ISR(RXVEC_XBEE) {
	receive(UART_XBEE);
}

