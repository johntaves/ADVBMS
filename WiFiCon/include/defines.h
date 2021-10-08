#include <Arduino.h>

#ifndef JTBMS_DEFINES_H_
#define JTBMS_DEFINES_H_

#define RELAY_ON 0xFF
#define RELAY_OFF 0x99
#define RELAY_X 0x00
#define MAINRELAY_TOTAL 8
#define RELAY_TOTAL 6

#define GREEN_LED GPIO_NUM_23

#define LAST_EVT_MSG_CNT 5

struct WiFiSettings {
  char ssid[33];
  char password[64];
  char apName[32];
  char apPW[32];
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

struct DispSettings {
  bool doCelsius;
};

#define EEPROM_WIFI 0
#define EEPROM_COMM (EEPROM_WIFI+256)
#define EEPROM_DISP (EEPROM_COMM+512)
#define EEPROMSize (EEPROM_DISP+16)

static_assert(sizeof(WiFiSettings) < 256,"EEPROM Size 1");
static_assert(sizeof(CommSettings) < 512,"EEPROM Size 2");
static_assert(sizeof(DispSettings) < 16,"EEPROM Size 3");

#endif
