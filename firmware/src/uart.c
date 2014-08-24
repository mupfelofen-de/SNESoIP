/* uart.c -*-c-*-
 * A minimalistic UART interface.
 * Copyright (c) 2014 Michael Fitzmayer.  All rights reserved.
 *
 * This program has has been released under the terms of a BSD-like
 * license.  See the file LICENSE for details. */


#include "uart.h"


void initUART(void) {
	UBRR0H  = (BAUDRATE >> 8);
	UBRR0L  = BAUDRATE;

	UCSR0B |= (1 << RXEN0);
	UCSR0B |= (1 << TXEN0);
	UCSR0C  = (1 << UCSZ01) | (1 << UCSZ00);
}


void uartPrintArray(uint8_t array[], int size, int base, char delimiter) {
	char tmp[7];

	for (int i = 0; i < size; i++) {
		uartPuts((char *)itoa(array[i], tmp, base));
		if (i < size - 1) uartPutc(delimiter);
	}
}


void uartPutc(uint8_t c) {
	while (!(UCSR0A & (1 << UDRE0))) {}
	UDR0 = c;
}


void uartPuts(uint8_t *s) {
	while (*s) {
		uartPutc(*s);
		s++;
	}
}