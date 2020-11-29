#include <Arduino.h>
#include <INA.h>
#include "SerData.h"

#ifndef JTBMS_DEFINES_H_
#define JTBMS_DEFINES_H_

//Maximum of 16 cell modules (dont change this!)
#define maximum_cell_modules 16
#define maximum_bank_of_modules 4

#define COMMS_BAUD_RATE 2400
#define RELAY_ON 0xFF
#define RELAY_OFF 0x99
#define RELAY_X 0x00

#define RELAY_TOTAL 6

struct WiFiSettings {
  char ssid[33];
  char password[64];
  char apName[32];
  char apPW[32];
};

enum {
  Relay_Load,Relay_Charge
};

struct limits {
  static const int N=4;
  enum { Volt, Temp, Max0 };
  enum { Cell, Pack, Max1 };
  enum { Max, Min, Max2 };
  enum { Trip, Rec, Max3 };
};

struct RelaySettings {
  char name[16];
  uint8_t type,doSOC,trip,rec,off;
};

struct EmailSettings {
  char email[64];
  char senderEmail[64];
  char senderPW[64];
  char senderServer[64];
  char senderSubject[64];
  char logEmail[64];
  char logPW[32];
  uint16_t senderPort;
  bool doLogging;
};

enum {
  Temp1,Temp2,TempC,MAX_TEMPS
};

struct ADCTSet {
  uint16_t bCoef,addr,mul,div,range;
};

struct SensSettings {
  ADCTSet temps[MAX_TEMPS];
};

struct BattSettings {
  uint16_t limits[2][2][2][2];
  uint16_t BattAH,MaxAmps,PVMaxAmps;
  uint32_t ShuntUOhms,PVShuntUOhms;
  RelaySettings relays[RELAY_TOTAL];
  char errorEmail[128];
  bool doCelsius,useCellC,useTempC;
  uint16_t Avg,ConvTime;
  uint32_t PollFreq;
  uint8_t nBanks,nCells;
};

#define EEPROM_WIFI 0
#define EEPROM_EMAIL (EEPROM_WIFI+sizeof(WiFiSettings)+2)
#define EEPROM_BATT (EEPROM_EMAIL+sizeof(EmailSettings)+2)
#define EEPROM_SENS (EEPROM_BATT+sizeof(BattSettings)+2)
#define EEPROMSize (EEPROM_SENS+sizeof(SensSettings)+2)

struct CellInfo {
  uint16_t rawV,rawT,v,t;
};
#endif
