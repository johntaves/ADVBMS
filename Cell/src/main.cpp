#include <Arduino.h>
#include <PacketSerial.h>
#include "CRC8.h"

#define BLUELED_ON {PORTA |= _BV(PORTA5);}
#define BLUELED_OFF {PORTA &= (~_BV(PORTA5));}
#define GREENLED_ON {PORTA |= _BV(PORTA6);}
#define GREENLED_OFF {PORTA &= (~_BV(PORTA6));}

#define ENABLE_SER {UCSR0B |= (1 << TXEN0);}
#define DISABLE_SER {UCSR0B &= ~_BV(TXEN0);}

#define LOAD_ON {PORTA |= _BV(PORTA3);}
#define LOAD_OFF {PORTA &= (~_BV(PORTA3));}

#define REFV_ON {PORTA |= _BV(PORTA7);}
#define REFV_OFF {PORTA &= (~_BV(PORTA7));}

#define framingmarker (uint8_t)0x00

CRC8 crc8;


PacketSerial_<COBS, framingmarker, 64> packSer;

void doPacket(const uint8_t *inBuf, size_t len)
{
  struct DataChunk* cc;
  if (len > 0)
  {

    //A data packet has just arrived, process it and forward the results to the next module
    if (cc->crc == crc8.get_crc8(&cc->crc, len - 1)) {
      GREENLED_ON;
      int i=0;
      while (i<cc->size && cc->cells[i].used == 1)
        i++;
      cc->cells[i].used = 1;
      if (cc->cells[i].dump == 1)
        // set wake up to shut this off
        LOAD_ON;
      
      cc->cells[i].v = 1; // read voltage
      cc->cells[i].t = 1; // read temp

      cc->crc = crc8.get_crc8(&cc->size, len - 1);
    }

    ENABLE_SER;

    Serial.write(framingmarker);
    hardware.FlushSerial0();

    packSer.send(inBuf, len);

    hardware.FlushSerial0();

    DISABLE_SER;
  }

  GREENLED_OFF;
}

void setup() {
  DDRA |= _BV(DDA3) | _BV(DDA6) | _BV(DDA5); // PA3, PA5, and PA6 outputs
  crc8.begin();
  Serial.begin(2400, SERIAL_8N1);
  packSer.setStream(&Serial);
  packSer.setPacketHandler(&doPacket);
}

void loop() {
  packSer.update();
}
