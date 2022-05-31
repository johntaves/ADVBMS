#include <Arduino.h>

#ifndef JTBMS_DEFINES_H_
#define JTBMS_DEFINES_H_

#define BLUE_LED GPIO_NUM_23
#define RESISTOR_PWR GPIO_NUM_26
#define IGN GPIO_NUM_27
#define TEMP1 ADC1_CHANNEL_0
#define TEMP2 ADC1_CHANNEL_3

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

struct WRelaySettings {
  RelaySettings relays[W_RELAY_TOTAL];
};

struct DispSettings {
  bool doCelsius;
  uint32_t t1B,t1R,t2B,t2R;
};

#define MAX_EVENTS 20
struct Event {
  EventMsg msg;
  time_t when;
};

#endif
