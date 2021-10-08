#include <Arduino.h>
#include <INA.h>
#include <CellData.h>
#include <CPUComm.h>

#ifndef JTBMS_DEFINES_H_
#define JTBMS_DEFINES_H_

#define RELAY_ON 0xFF
#define RELAY_OFF 0x99
#define RELAY_X 0x00

#define GREEN_LED GPIO_NUM_23
#define RESISTOR_PWR GPIO_NUM_32
#define TEMP1 GPIO_NUM_35
#define BCOEF 4050

// poll the PV current every 2 secs
#define POLLPV 2000
// check everything every 2 secs
#define CHECKSTATUS 2000
// shut everything off if status has not happened within 2 secs of when it should
#define WATCHDOGSLOP 2000
// attempt to connect to cells
#define CHECKCONNECT 1000

#define BLESCANTIME 10000
#define BLESCANREST 20000

enum {
  Relay_Connect,Relay_Load,Relay_Charge
};

struct BLESettings {
  int numCells;
  CellSettings sets;
  NimBLEAddress addrs[MAX_CELLS];
};

#define EEPROM_BATT 0
#define EEPROM_BLE (EEPROM_BATT+1024)
#define EEPROMSize (EEPROM_BLE+256)

static_assert(sizeof(BattSettings) < 1024,"EEPROM Size 0");
static_assert(sizeof(BLESettings) < 256,"EEPROM Size 1");

#endif
