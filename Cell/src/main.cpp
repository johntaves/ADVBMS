#include <Arduino.h>
#include <PacketSerial.h>
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/wdt.h>
#include "SerData.h"
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

PacketSerial_<COBS, framingmarker, 64> dataSer;

bool doingV;
uint16_t lastT,lastV;

ISR(WDT_vect)
{
  //This is the watchdog timer - something went wrong and no activity recieved in a while
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
  if (len > 0 && cc->crc == crc8.get_crc8(p+1, len - 1)) {
    int i=0;
    while (i<nCells && cc->cells[i].used == 1)
      i++;
    if (i<nCells) {
      cc->cells[i].used = 1;
      if (cc->cells[i].dump == 1)
        // set wake up to shut this off
        LOAD_ON;
      
      REFV_ON;
      delay(4);

      doingV = true;
      ADMUXA = (0 << MUX5) | (0 << MUX4) | (1 << MUX3) | (0 << MUX2) | (0 << MUX1) | (0 << MUX0);
      StartADC(); // an interrupt will fire to catch the value;

      doingV = false;
      ADMUXA = (0 << MUX5) | (0 << MUX4) | (0 << MUX3) | (1 << MUX2) | (0 << MUX1) | (0 << MUX0);
      StartADC();

      REFV_OFF;
      cc->cells[i].v = lastV; // read voltage
      cc->cells[i].t = lastT; // read temp

      cc->crc = crc8.get_crc8(p+1, len - 1);
    }
    ENABLE_SER;

    Serial.write(framingmarker);
    Serial.flush();

    dataSer.send(inBuf, len);

    Serial.flush();
    DISABLE_SER;
  }

  GREENLED_OFF;
}

void setup() {
  wdt_disable();
  wdt_reset();

  MCUSR = 0;
  //Enable watchdog (to reset)
  WDTCSR |= bit(WDE);

  CCP = 0xD8;
  //WDTCSR – Watchdog Timer Control and Status Register
  // We INTERRUPT the chip after 8 seconds of sleeping (not reboot!)
  // WDE: Watchdog Enable
  // Bits 5, 2:0 – WDP[3:0]: Watchdog Timer Prescaler 3 - 0
  WDTCSR = bit(WDIE) | bit(WDP3) | bit(WDP0);
  //| bit(WDE)

  wdt_reset();

  DDRA |= _BV(DDA3) | _BV(DDA6) | _BV(DDA5); // PA3, PA5, and PA6 outputs
  crc8.begin();
  Serial.begin(2400, SERIAL_8N1);
  dataSer.setStream(&Serial);
  dataSer.setPacketHandler(&onSerData);
}

void doSleep() {
  //ATTINY841 sleep mode
  byte old_ADCSRA = ADCSRA;
  //For low power applications, before entering sleep, remember to turn off the ADC
  //ADCSRA&=(~(1<<ADEN));
  // disable ADC
  ADCSRA = 0;
  
#if defined(DIYBMSMODULEVERSION) && DIYBMSMODULEVERSION < 430
set_sleep_mode(SLEEP_MODE_PWR_DOWN);
#else
//Using an external crystal so keep it awake - consumes more power (about 0.97mA vs 0.78mA) but module wakes quicker (6 clock cycles)
set_sleep_mode(SLEEP_MODE_STANDBY);
#endif
  
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

void loop() {
  wdt_reset();

  noInterrupts();
  // Enable Start Frame Detection
  UCSR0D = (1 << RXSIE0) | (1 << SFDE0);

  interrupts();


  dataSer.update();
}
