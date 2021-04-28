#include "JTHardwareSerial.h"
#include <Arduino.h>
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

// .5 sec interrupts, take 4 averages, 4 seconds is too long for the controller
#define WD_AMT (WDP2|WDP0)
#define NAVE 4
#define WD_CNT 8
// 3 wdt hits before giving up on serial
#define WD_CNT_WAIT 3
// temp every 5 seconds
#define TEMP_CNT 10 

struct AnalogInput {
  uint16_t value;
  uint16_t vals[NAVE];
  uint32_t sumValue;
} __attribute__((packed));

CellsSerData theBuff;
uint8_t bank=0,nAve=0,wdtCnt=0,curAve=0,tempCnt=0,adcFails=0;
uint8_t wdtHits=0;
volatile bool wdt_hit,uart_hit;
volatile uint8_t wdtdoda;
volatile uint16_t adcRead;
uint16_t lastT;
AnalogInput volts;

uint16_t bauds[] = {2400,4800,9600,14400,19200,28800,38400,57600 };

ISR(WDT_vect)
{
  if (wdtdoda & 1) BLUELED_ON
  else BLUELED_OFF;
  wdt_hit = true; // this just turns off the load if we did not get woken by serial
}

ISR(USART0_START_vect)
{
  uart_hit = true;
}

ISR(ADC_vect)
{
  uint8_t low = ADCL;
  uint16_t val = (ADCH << 8) | low;
  adcRead = val;
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

void doOneLed(int led,int del) {
  switch (led) {
    case 0: BLUELED_ON; break;
    case 1: GREENLED_ON; break;
    case 2: LOAD_ON; break;
  }
  delay(del);
  switch (led) {
    case 0: BLUELED_OFF; break;
    case 1: GREENLED_OFF; break;
    case 2: LOAD_OFF; break;
  }
}

void flashLed(int blue,int n) {
  int del=50;
  for (int i=0;i<n-1;i++) {
    doOneLed(blue,del);
    delay(del);
  }
  doOneLed(blue,del);
}

bool StartADC() {
    //ADMUXB – ADC Multiplexer Selection Register
  //Select external AREF pin (internal reference turned off)
  ADMUXB = _BV(REFS2);

  //ADCSRA – ADC Control and Status Register A
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
  bool ret = bit_is_set(ADCSRA, ADSC);
  ADCSRA &= (~(1 << ADEN));
  if (ret) adcFails++;
  return ret;
}

void processPkt(uint8_t len)
{
  uint8_t nCells = (len - sizeof(CellsHeader))/sizeof(CellSerData);
  uint8_t* p = (uint8_t*)&theBuff;
  if (nCells > 16 || theBuff.hdr.crc != CRC8(p+1, len - 1) || theBuff.hdr.ver)
    return;
  uint8_t cmd = theBuff.hdr.cmd;
  uint8_t arg = theBuff.hdr.arg;
  if (!cmd && arg == bank && nCells >=0 && nCells <= MAX_CELLS) {
    int i=0;
    while (i<nCells && theBuff.cells[i].used == 1)
      i++;
    if (i<nCells) {
      theBuff.cells[i].ver = VER;
      theBuff.cells[i].used = 1;
      if (theBuff.cells[i].dump == 1)
        LOAD_ON
      else LOAD_OFF;
      if (adcFails > 15) theBuff.cells[i].fails = 15;
      else theBuff.cells[i].fails = 0;
      adcFails = 0;
      theBuff.cells[i].v = volts.value; // read voltage
      theBuff.cells[i].t = lastT; // read temp

      theBuff.hdr.crc = CRC8(p+1, len - 1);
    }
  }
  JTSerial.sendPacket(0); // wake up next dude

  JTSerial.sendPacket(len);
  delay(len); // delay to let the last byte go. At 2400 it should take 3.3ms
  if (cmd) {
    flashLed(0,5);
    JTSerial.begin((uint8_t*)&theBuff,sizeof(theBuff), bauds[arg], SERIAL_8N1);
  }
}

void getADCVolts() {
  REFV_ON;
  delay(4);

  ADMUXA = (0 << MUX5) | (0 << MUX4) | (1 << MUX3) | (0 << MUX2) | (0 << MUX1) | (0 << MUX0);
  if (StartADC())
    return;
  REFV_OFF;

  if (nAve == NAVE)
    volts.sumValue -= (uint32_t)volts.vals[curAve];
  volts.vals[curAve] = adcRead;
  volts.sumValue += (uint32_t)volts.vals[curAve];
  if (nAve < NAVE)
    nAve++;
  volts.value = volts.sumValue / nAve;
  curAve++;
  if (curAve >= NAVE)
    curAve=0;
}

void getADCTemp() {
  REFV_ON;

  lastT = 0;
  ADMUXA = (0 << MUX5) | (0 << MUX4) | (0 << MUX3) | (1 << MUX2) | (0 << MUX1) | (0 << MUX0);
  if (StartADC())
    return;
  REFV_OFF;
  lastT = adcRead;

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

void setWatch(uint8_t cy) {
  MCUSR = 0;
  //Enable watchdog (to reset)
  WDTCSR |= bit(WDE);

  CCP = 0xD8;
  //WDTCSR – Watchdog Timer Control and Status Register
  // We INTERRUPT the chip after 8 seconds of sleeping (not reboot!)
  // WDE: Watchdog Enable
  // Bits 5, 2:0 – WDP[3:0]: Watchdog Timer Prescaler 3 - 0
  WDTCSR = bit(WDIE) | bit(cy);
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

void setup() {
  wdt_disable();
  wdt_reset();
  setWatch(WD_AMT);

  setPorts();
  power_usart0_enable();
  
  ENABLE_SERRX;
  ENABLE_SERTX;
  // disable serial 1
  UCSR1B &= ~_BV(RXEN1);
  UCSR1B &= ~_BV(TXEN1);
  volts.sumValue=0;
  volts.value=0;
  JTSerial.begin((uint8_t*)&theBuff,sizeof(theBuff), 2400, SERIAL_8N1);
}

bool checkIfStuck() {
  if (!wdt_hit)
    return false;
  if (wdtHits & 1) BLUELED_OFF
  else BLUELED_ON;
  wdt_hit = false;
  return wdtHits++ >= WD_CNT_WAIT;
}

void loop() {
  wdt_reset();
  wdt_hit = false;
  uart_hit = false;
  enableStartFrameDetection();
  doSleep();
  
  if (wdt_hit)
  {
//    BLUELED_ON;
    getADCVolts();
//    BLUELED_OFF;
    if (!tempCnt) {
      tempCnt=TEMP_CNT;
      getADCTemp();
    } else tempCnt--;
    if (wdtCnt++ > WD_CNT) {
      LOAD_OFF;
      JTSerial.clear();
      flashLed(0,2);
      wdtCnt=0;
    }
  } 
  if (uart_hit) {
    wdtCnt = 0;
    GREENLED_ON;
    uint8_t len;
    wdt_hit = false;
    wdtHits = 0;
    while (!(len = JTSerial.checkForPacket()) && !JTSerial.didFault() && !checkIfStuck())
      delay(1);
    if (len)
      processPkt(len);
    
    JTSerial.clear();
    GREENLED_OFF;
    BLUELED_OFF;
  }
}
