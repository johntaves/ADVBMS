#include <Arduino.h>
#include <INA.h>

#ifndef JTBMS_DEFINES_H_
#define JTBMS_DEFINES_H_

#define GREEN_LED GPIO_NUM_23
#define RESISTOR_PWR GPIO_NUM_32
#define TEMP1 ADC1_CHANNEL_7
#define BCOEF 4050

// poll the PV current every 2 secs
#define POLLPV 2000
// shut everything off if status has not happened within 2 secs of when it should
#define WATCHDOGSLOP 2000
// attempt to connect to cells
#define CHECKCONNECT 1000

struct BLESettings {
  int numCells;
  NimBLEAddress addrs[MAX_CELLS];
};

#define EEPROM_BATTS 0
#define EEPROM_BATTD (EEPROM_BATTS+512)
#define EEPROM_BLE (EEPROM_BATTD+512)
#define EEPROMSize (EEPROM_BLE+256)

static_assert(sizeof(StatSetts) < 512,"EEPROM Size 0");
static_assert(sizeof(DynSetts) < 512,"EEPROM Size 0");
static_assert(sizeof(BLESettings) < 256,"EEPROM Size 1");

#endif
