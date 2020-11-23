/*
  HardwareSerial.cpp - Hardware serial library for Wiring
  Copyright (c) 2006 Nicholas Zambetti.  All right reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

  Modified 23 November 2006 by David A. Mellis
  Modified 28 September 2010 by Mark Sproul
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "core_build_options.h"
#include "wiring.h"
#include "wiring_private.h"

// this next line disables the entire HardwareSerial.cpp,
// this is so I can support Attiny series and any other chip without a uart
// (If DISABLE_UART is set in core_build_options.h, HW serial is disabled completely.)
#if defined(UBRRH) || defined(UBRR0H) || defined(UBRR1H) || defined(UBRR2H) || defined(UBRR3H) && !DISABLE_UART

#include "HardwareSerial.h"

struct COBBuffer
{
  uint8_t cnt,nextCode,bufferSize;
  bool fault;
  uint8_t *buffer;
};

#if defined(UBRRH) || defined(UBRR0H)
  COBBuffer rx_buffer  =  { 0, 0, 0, false, 0 };
#endif

// do the COBs decoding in place
inline void store_char(unsigned char c, COBBuffer *rx_buffer)
{
  if (!rx_buffer->nextCode)
    rx_buffer->nextCode = c;
  else if (!c)
    rx_buffer->nextCode = 0;
  else if (rx_buffer->cnt < rx_buffer->bufferSize) {
    if (rx_buffer->cnt == (rx_buffer->nextCode-1)) {
      rx_buffer->buffer[rx_buffer->cnt++] = 0;
      rx_buffer->nextCode = c + rx_buffer->cnt;
    } else
      rx_buffer->buffer[rx_buffer->cnt++] = c;
  } else
    rx_buffer->fault = true;
}

#if defined(USART_RX_vect)
  ISR(USART_RX_vect)
  {
  #if defined(UDR0)
    unsigned char c  =  UDR0;
  #elif defined(UDR)
    unsigned char c  =  UDR;  //  atmega8535
  #else
    #error UDR not defined
  #endif
    store_char(c, &rx_buffer);
  }
#elif defined(USART0_RECV_vect) && defined(UDR0)
  ISR(USART0_RECV_vect)
  {
    unsigned char c  =  UDR0;
    store_char(c, &rx_buffer);
  }
#elif defined(UART0_RECV_vect) && defined(UDR0)
  ISR(UART0_RECV_vect)
  {
    unsigned char c  =  UDR0;
    store_char(c, &rx_buffer);
  }
//#elif defined(SIG_USART_RECV)
#elif defined(USART0_RX_vect)
  // fixed by Mark Sproul this is on the 644/644p
  //ISR(SIG_USART_RECV)
  ISR(USART0_RX_vect)
  {
  #if defined(UDR0)
    unsigned char c  =  UDR0;
  #elif defined(UDR)
    unsigned char c  =  UDR;  //  atmega8, atmega32
  #else
    #error UDR not defined
  #endif
    store_char(c, &rx_buffer);
  }
#elif defined(UART_RECV_vect)
  // this is for atmega8
  ISR(UART_RECV_vect)
  {
  #if defined(UDR0)
    unsigned char c  =  UDR0;  //  atmega645
  #elif defined(UDR)
    unsigned char c  =  UDR;  //  atmega8
  #endif
    store_char(c, &rx_buffer);
  }
#elif defined(USBCON)
  #warning No interrupt handler for usart 0
  #warning Serial(0) is on USB interface
#else
  #error No interrupt handler for usart 0
#endif


// Constructors ////////////////////////////////////////////////////////////////

HardwareSerial::HardwareSerial(COBBuffer *rx_buffer,
  volatile uint8_t *ubrrh, volatile uint8_t *ubrrl,
  volatile uint8_t *ucsra, volatile uint8_t *ucsrb,  volatile uint8_t *ucsrc,
  volatile uint8_t *udr,
  uint8_t rxen, uint8_t txen, uint8_t rxcie, uint8_t udre, uint8_t u2x)
{
  _rx_buffer = rx_buffer;
  _ubrrh = ubrrh;
  _ubrrl = ubrrl;
  _ucsra = ucsra;
  _ucsrb = ucsrb;
  _ucsrc = ucsrc;
  _udr = udr;
  _rxen = rxen;
  _txen = txen;
  _rxcie = rxcie;
  _udre = udre;
  _u2x = u2x;
}

// Public Methods //////////////////////////////////////////////////////////////

void HardwareSerial::begin(uint8_t* buf,uint8_t len,unsigned long baud, byte config)
{
  _rx_buffer->buffer = buf;
  _rx_buffer->bufferSize = len;
  _rx_buffer->cnt = 0;
  _rx_buffer->nextCode = 0;
  uint16_t baud_setting;
  bool use_u2x = true;
/*
#if F_CPU == 16000000UL
  // hardcoded exception for compatibility with the bootloader shipped
  // with the Duemilanove and previous boards and the firmware on the 8U2
  // on the Uno and Mega 2560.
  if (baud == 57600) {
    use_u2x = false;
  }
#endif
*/
  if (use_u2x) {
    *_ucsra = 1 << _u2x;
    baud_setting = (F_CPU / 4 / baud - 1) / 2;
  } else {
    *_ucsra = 0;
    baud_setting = (F_CPU / 8 / baud - 1) / 2;
  }

  // assign the baud_setting, a.k.a. ubbr (USART Baud Rate Register)
  *_ubrrh = baud_setting >> 8;
  *_ubrrl = baud_setting;

  *_ucsrc = config;
  sbi(*_ucsrb, _rxen);
  sbi(*_ucsrb, _txen);
  sbi(*_ucsrb, _rxcie);
}

void HardwareSerial::end()
{
  cbi(*_ucsrb, _rxen);
  cbi(*_ucsrb, _txen);
  cbi(*_ucsrb, _rxcie);
}

uint8_t HardwareSerial::waitForPacket(uint32_t timeout)
{
  uint32_t startMillis=0;
  do {
    if (_rx_buffer->cnt && !_rx_buffer->nextCode)
      return _rx_buffer->cnt;
    if (!startMillis)
      startMillis = millis();

  } while (millis() - startMillis < 1000);

  return 0;
}

bool HardwareSerial::didFault()
{
  return _rx_buffer->fault;
}

void HardwareSerial::clear()
{
  _rx_buffer->cnt = 0;
  _rx_buffer->nextCode = 0;
  _rx_buffer->fault = false;
}

uint8_t HardwareSerial::getCnt()
{
  return _rx_buffer->cnt;
}

void HardwareSerial::sendOne(uint8_t c)
{
  while (!((*_ucsra) & (1 << _udre)))
    ;

  *_udr = c;
}

void HardwareSerial::sendPacket(uint8_t len)
{
  if (!len) {
    sendOne(0);
    return;
  }
  _rx_buffer->nextCode = 0;
  for (uint8_t i=0;i<len;i++) {
    if (_rx_buffer->buffer[i])
      continue;
    uint8_t val = i-_rx_buffer->nextCode+1;
    if (!_rx_buffer->nextCode)
      _rx_buffer->nextCode = val;
    else _rx_buffer->buffer[i] = val;
  }
  sendOne(_rx_buffer->nextCode);
  for (uint8_t i=0;i<len;i++)
    sendOne(_rx_buffer->buffer[i]);
  sendOne(0);
  _rx_buffer->cnt = 0;
  _rx_buffer->nextCode = 0; // read for next read
}

// Preinstantiate Objects //////////////////////////////////////////////////////

#if ! DEFAULT_TO_TINY_DEBUG_SERIAL
  #if defined(UBRRH) && defined(UBRRL)
    HardwareSerial Serial(&rx_buffer, &UBRRH, &UBRRL, &UCSRA, &UCSRB, &UCSRC, &UDR, RXEN, TXEN, RXCIE, UDRE, U2X);
  #elif defined(UBRR0H) && defined(UBRR0L)
    HardwareSerial Serial(&rx_buffer, &UBRR0H, &UBRR0L, &UCSR0A, &UCSR0B, &UCSR0C, &UDR0, RXEN0, TXEN0, RXCIE0, UDRE0, U2X0);
  #elif defined(USBCON)
    #warning no serial port defined  (port 0)
  #else
    #error no serial port defined  (port 0)
  #endif
#endif

#endif // whole file
