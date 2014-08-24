/* uart.h -*-c-*-
 * A minimalistic UART interface.
 * Copyright (c) 2014 Michael Fitzmayer.  All rights reserved.
 *
 * This program has has been released under the terms of a BSD-like
 * license.  See the file LICENSE for details. */


#ifndef UART_h
#define UART_h


#ifndef BAUD
#define BAUD     57600UL
#endif
#ifndef F_CPU
#define F_CPU    16000000UL
#endif
#define BAUDRATE ((F_CPU) / (BAUD * 16UL) - 1)


#include <stdlib.h>
#include <avr/interrupt.h>
#include <util/setbaud.h>


void initUART(void);
void uartPrintArray(uint8_t array[], int size, int base, char delimiter);
void uartPutc(uint8_t c);
void uartPuts(uint8_t *s);


#endif