#include <Arduino.h>
#include <PacketSerial.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include "SerData.h"

#define BLUELED_ON {PORTA |= _BV(PORTA5);}
#define BLUELED_OFF {PORTA &= (~_BV(PORTA5));}
#define GREENLED_ON {PORTA |= _BV(PORTA6);}
#define GREENLED_OFF {PORTA &= (~_BV(PORTA6));}

#define ENABLE_SERRX {UCSR0B |= (1 << RXEN0);}
#define DISABLE_SERRX {UCSR0B &= ~_BV(RXEN0);}

#define ENABLE_SERTX {UCSR0B |= (1 << TXEN0);}
#define DISABLE_SERTX {UCSR0B &= ~_BV(TXEN0);}

#define LOAD_ON {PORTA |= _BV(PORTA3);}
#define LOAD_OFF {PORTA &= (~_BV(PORTA3));}

#define REFV_ON {PORTA |= _BV(PORTA7);}
#define REFV_OFF {PORTA &= (~_BV(PORTA7));}

#define frame (uint8_t)0x00

PacketSerial_<COBS, frame, sizeof(CellsSerData)> dataSer;
bool doingV,wdt_hit,doneRec;
uint16_t lastT,lastV;

ISR(WDT_vect)
{
  wdt_hit = true; // this just turns off the load if we did not get woken by serial
}

ISR(ADC_vect)
{
  uint8_t low = ADCL;
  uint16_t val = (ADCH << 8) | low;
  // when ADC completed, take an interrupt and process result
  if (doingV)
    lastV = val;
  else lastT = val;
}

uint8_t CRC8(const uint8_t *data,int length) 
{
   uint8_t crc = 0x00;
   uint8_t extract;
   uint8_t sum;
   for(int i=0;i<length;i++)
   {
      extract = *data;
      for (uint8_t tempI = 8; tempI; tempI--) 
      {
         sum = (crc ^ extract) & 0x01;
         crc >>= 1;
         if (sum)
            crc ^= 0x8C;
         extract >>= 1;
      }
      data++;
   }
   return crc;
}

void doOneLed(bool blue,int del) {
  if (blue) BLUELED_ON
  else GREENLED_ON;
  delay(del);
  if (blue) BLUELED_OFF
  else GREENLED_OFF;
}

void flashLed(bool blue,int n) {
  int del=50;
  for (int i=0;i<n-1;i++) {
    doOneLed(blue,del);
    delay(del);
  }
  doOneLed(blue,del);
}

void StartADC() {
    //ADMUXB – ADC Multiplexer Selection Register
  //Select external AREF pin (internal reference turned off)
  ADMUXB = _BV(REFS2);

  //ADCSRA – ADC Control and Status Register A
  //Consider ADC sleep conversion mode?
  //prescaler of 64 = 8MHz/64 = 125KHz.
  ADCSRA |= _BV(ADPS2) | _BV(ADPS1); // | _BV(ADPS0);

  //adc_enable();
  //Bit 4 – ADIF: ADC Interrupt Flag
  //Bit 7 – ADEN: ADC Enable
  ADCSRA |= _BV(ADEN) | _BV(ADIF); // enable ADC, turn off any pending interrupt

  // wait for ADC to settle
  // The ADC must be enabled during the settling time.
  // ADC requires a settling time of 1ms before measurements are stable
  delay(2);

  noInterrupts();
  set_sleep_mode(SLEEP_MODE_ADC); // sleep during ADC sample
  sleep_enable();

  // start the conversion
  ADCSRA |= _BV(ADSC) | _BV(ADIE);
  interrupts();
  sleep_cpu();
  sleep_disable();

  // awake again, reading should be done, better make sure maybe the timer interrupt fired
  while (bit_is_set(ADCSRA, ADSC)) {}

  //adc_disable
  ADCSRA &= (~(1 << ADEN));
}

void onSerData(const uint8_t *inBuf, size_t len)
{
  GREENLED_ON;
  CellsSerData* cc = (CellsSerData*)inBuf;
  int nCells = (len - sizeof(CellsSerData))/sizeof(CellSerData);
  uint8_t* p = (uint8_t*)cc;
  if (len > 0 && cc->crc == CRC8(p+1, len - 1)) {
    int i=0;
    while (i<nCells && cc->cells[i].used == 1)
      i++;
    if (i<nCells) {
      cc->cells[i].used = 1;
      if (cc->cells[i].dump == 1)
        // set wake up to shut this off
        LOAD_ON;
      
      cc->cells[i].v = lastV; // read voltage
      cc->cells[i].t = lastT; // read temp

      cc->crc = CRC8(p+1, len - 1);
    }
    Serial.write(frame);
    dataSer.send(inBuf, len);
  }

  GREENLED_OFF;
  doneRec = true;
}

void getADCVals() {
  REFV_ON;
  delay(4);

  doingV = true;
  ADMUXA = (0 << MUX5) | (0 << MUX4) | (1 << MUX3) | (0 << MUX2) | (0 << MUX1) | (0 << MUX0);
  StartADC(); // an interrupt will fire to catch the value;

  doingV = false;
  ADMUXA = (0 << MUX5) | (0 << MUX4) | (0 << MUX3) | (1 << MUX2) | (0 << MUX1) | (0 << MUX0);
  StartADC();

  REFV_OFF;
}

void setPorts() {
  PUEA = 0;
  PUEB = 0;
  DDRA |= _BV(DDA3) | _BV(DDA6) | _BV(DDA7) | _BV(DDA5); // PA3, PA5, and PA6 outputs
  DDRB |= _BV(DDB1);

  //Set the extra high sink capability of pin PA7 is enabled.
  PHDE |= _BV(PHDEA1);
  LOAD_OFF;
  REFV_OFF;

  GREENLED_OFF;
  BLUELED_OFF;
}

void watch5sec() {
  MCUSR = 0;
  //Enable watchdog (to reset)
  WDTCSR |= bit(WDE);

  CCP = 0xD8;
  //WDTCSR – Watchdog Timer Control and Status Register
  // We INTERRUPT the chip after 8 seconds of sleeping (not reboot!)
  // WDE: Watchdog Enable
  // Bits 5, 2:0 – WDP[3:0]: Watchdog Timer Prescaler 3 - 0
  WDTCSR = bit(WDIE) | bit(WDP3);
  //| bit(WDE)

  wdt_reset();
}

void enableStartFrameDetection() {
  noInterrupts();
  // Enable Start Frame Detection
  UCSR0D = (1 << RXSIE0) | (1 << SFDE0);

  interrupts();
}

void doSleep() {
  //ATTINY841 sleep mode
  byte old_ADCSRA = ADCSRA;
  //For low power applications, before entering sleep, remember to turn off the ADC
  //ADCSRA&=(~(1<<ADEN));
  // disable ADC
  ADCSRA = 0;
  
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
 
  power_spi_disable();
  power_timer0_disable();
  power_timer1_disable();
  power_timer2_disable();
  power_twi_disable();
  power_adc_disable();

  power_usart1_disable();

  //Keep this alive
  //power_usart0_enable();

  sei();
  interrupts();
  sleep_enable();
  sleep_cpu();

  //Snoring can be heard at this point....

  sleep_disable();

  power_adc_enable();
  power_timer0_enable();
  power_timer1_enable();
  power_timer2_enable();

  //power_all_enable();

  ADCSRA = old_ADCSRA;
}

CellsSerData buf;
void sendTest() {
  uint8_t* p = (uint8_t*)&buf;
  int len = sizeof(CellsSerData) - (sizeof(CellSerData) * (MAX_CELLS - 1));
  buf.hdr.id++;
  buf.hdr.bank=0;
  buf.cells[0].dump = 0;
  buf.cells[0].used = 1;
  buf.cells[0].v = 12;
  buf.cells[0].t = 54;
  buf.hdr.crc = CRC8(p+1, len - 1);
  dataSer.send(p,len);
}

void setup() {
  wdt_disable();
  wdt_reset();
  watch5sec();

  setPorts();
  power_usart0_enable();
  
  ENABLE_SERRX;
  ENABLE_SERTX;
  // disable serial 1
  UCSR1B &= ~_BV(RXEN1);
  UCSR1B &= ~_BV(TXEN1);

  memset(&buf,0,sizeof(buf));
  Serial.begin(2400, SERIAL_8N1);
  dataSer.setStream(&Serial);
  dataSer.setPacketHandler(&onSerData);
  flashLed(true,3);
  sendTest();
}

void loop() {
  wdt_reset();
  wdt_hit = false;
  enableStartFrameDetection();
  doSleep();
  
  if (wdt_hit)
  {
    setPorts();
    ENABLE_SERRX;
    ENABLE_SERTX;

    LOAD_OFF;

    sendTest();

    flashLed(true,2);
  } else {
    flashLed(false,1);
    getADCVals();
    flashLed(false,3);
    doneRec = false;
    while (!doneRec) {
      delay(10);
      dataSer.update();
    }
  }
}
