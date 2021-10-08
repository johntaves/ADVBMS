#include <Arduino.h>
#include <INA.h>
#include "CellData.h"

#ifndef JTBMS_DEFINES_H_
#define JTBMS_DEFINES_H_

#define MAX_CELLS 16

#define RELAY_ON 0xFF
#define RELAY_OFF 0x99
#define RELAY_X 0x00
#define RELAY_TOTAL 8

#define GREEN_LED GPIO_NUM_23
#define RESISTOR_PWR GPIO_NUM_32
#define TEMP1 GPIO_NUM_35
#define SPEAKER GPIO_NUM_4

// poll the PV current every 2 secs
#define POLLPV 2000
// check everything every 2 secs
#define CHECKSTATUS 2000
// shut everything off if status has not happened within 2 secs of when it should
#define WATCHDOGSLOP 2000
// read and average the analog stuff every
#define POLLANALOGS 100
// attempt to connect to cells
#define CHECKCONNECT 1000
// average this many
#define NUMANALOGSAMPS 16

#define BLESCANTIME 10000
#define BLESCANREST 20000

#define LAST_EVT_MSG_CNT 5
enum {
  Temp1_Analog,
  Max_Analog
};

struct AnalogInput {
  uint8_t pin;
  uint16_t rawValue;
  uint16_t vals[NUMANALOGSAMPS];
  uint32_t sumValue;
};

struct WiFiSettings {
  char ssid[33];
  char password[64];
  char apName[32];
  char apPW[32];
};

enum {
  Relay_Connect,Relay_Load,Relay_Charge
};

struct limits {
  static const int N=4;
  enum { Volt, Temp, Max0 };
  enum { Cell, Pack, Max1 };
  enum { Max, Min, Max2 };
  enum { Trip, Rec, Max3 };
};

struct RelaySettings {
  char name[16],from[16];
  bool off,doSoC,fullChg;
  uint8_t type,trip,rec,rank;
};

struct CommSettings {
  char email[64];
  char senderEmail[64];
  char senderPW[64];
  char senderServer[64];
  char senderSubject[64];
  char logEmail[64];
  char logPW[32];
  uint16_t senderPort;
  bool doLogging;
  uint32_t foo;
};

struct BattSettings {
  uint16_t limits[2][2][2][2];
  uint16_t BattAH,MaxAmps,PVMaxAmps;
  uint32_t ShuntUOhms,PVShuntUOhms;
  RelaySettings relays[RELAY_TOTAL];
  char errorEmail[128];
  bool doCelsius,useCellC,useTemp1;
  uint16_t Avg,ConvTime;
  uint32_t PollFreq;
  uint8_t nCells;
  uint8_t ChargePct,ChargePctRec;
  uint16_t FloatV;
  int16_t ChargeRate,TopAmps;
  time_t savedTime;
  uint64_t milliAmpMillis;
  uint8_t CellsOutMin,CellsOutMax,CellsOutTime;
};

struct BLECells {
  int numCells;
  CellSettings sets;
  NimBLEAddress addrs[MAX_CELLS];
};

#define EEPROM_WIFI 0
#define EEPROM_COMM (EEPROM_WIFI+256)
#define EEPROM_BATT (EEPROM_COMM+512)
#define EEPROM_BLE (EEPROM_BATT+1024)
#define EEPROMSize (EEPROM_BLE+256)

static_assert(sizeof(WiFiSettings) < 256,"EEPROM Size 1");
static_assert(sizeof(CommSettings) < 512,"EEPROM Size 2");
static_assert(sizeof(BattSettings) < 1024,"EEPROM Size 3");
static_assert(sizeof(BLECells) < 256,"EEPROM Size 4");

struct CellData {
  uint16_t v;
  int16_t t;
  uint32_t dumpSecs,last;
  bool dumping,connected;
  BLEClient* pClient;
  NimBLERemoteCharacteristic* pSettings;
};

struct ChargeData {
  int64_t milliAmpMillis;
  uint16_t mV;
  uint8_t cell;
};
#endif
