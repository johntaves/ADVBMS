/*
  JTHardwareSerial.h - Hardware serial library for Wiring
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

  Modified 28 September 2010 by Mark Sproul
*/

#ifndef JTHardwareSerial_h
#define JTHardwareSerial_h

#include <inttypes.h>

#include "core_build_options.h"
#include "Stream.h"
#include "SerData.h"

// Define config for Serial.begin(baud, config);
#define SERIAL_5N1 0x00
#define SERIAL_6N1 0x02
#define SERIAL_7N1 0x04
#define SERIAL_8N1 0x06
#define SERIAL_5N2 0x08
#define SERIAL_6N2 0x0A
#define SERIAL_7N2 0x0C
#define SERIAL_8N2 0x0E
#define SERIAL_5E1 0x20
#define SERIAL_6E1 0x22
#define SERIAL_7E1 0x24
#define SERIAL_8E1 0x26
#define SERIAL_5E2 0x28
#define SERIAL_6E2 0x2A
#define SERIAL_7E2 0x2C
#define SERIAL_8E2 0x2E
#define SERIAL_5O1 0x30
#define SERIAL_6O1 0x32
#define SERIAL_7O1 0x34
#define SERIAL_8O1 0x36
#define SERIAL_5O2 0x38
#define SERIAL_6O2 0x3A
#define SERIAL_7O2 0x3C
#define SERIAL_8O2 0x3E


struct COBBuffer;

class JTHardwareSerial
{
  private:
    COBBuffer *_rx_buffer;
    volatile uint8_t *_ubrrh;
    volatile uint8_t *_ubrrl;
    volatile uint8_t *_ucsra;
    volatile uint8_t *_ucsrb;
    volatile uint8_t *_udr;
    volatile uint8_t *_ucsrc;
    uint8_t _rxen;
    uint8_t _txen;
    uint8_t _rxcie;
    uint8_t _udre;
    uint8_t _u2x;
    void sendOne(uint8_t c);
  public:
    JTHardwareSerial(COBBuffer *rx_buffer,
      volatile uint8_t *ubrrh, volatile uint8_t *ubrrl,
      volatile uint8_t *ucsra, volatile uint8_t *ucsrb, volatile uint8_t *ucsrc,
      volatile uint8_t *udr,
      uint8_t rxen, uint8_t txen, uint8_t rxcie, uint8_t udre, uint8_t u2x);
    void begin(uint8_t* buf,uint8_t len,unsigned long baud) { begin(buf,len,baud, SERIAL_8N1); }
    void begin(uint8_t* buf,uint8_t len,unsigned long, uint8_t);
    void end();
    uint8_t checkForPacket();
    void clear();
    bool didFault();
    void sendPacket(uint8_t len);
};

#if (defined(UBRRH) || defined(UBRR0H)) && ! DEFAULT_TO_TINY_DEBUG_SERIAL
  extern JTHardwareSerial JTSerial;
#elif defined(USBCON)
  #include "usb_api.h"
#endif

#endif
