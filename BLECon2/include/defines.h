#include <Arduino.h>

#ifndef JTBMS_DEFINES_H_
#define JTBMS_DEFINES_H_

#define GREEN_LED GPIO_NUM_23
#define RESISTOR_PWR GPIO_NUM_32
#define TEMP1 ADC1_CHANNEL_7
#define BCOEF 4050

// poll the PV current every 2 secs
#define POLLPV 2000
// attempt to connect to cells
#define CHECKCONNECT 1000

struct BLESettings {
  int numCells;
  NimBLEAddress addrs[MAX_CELLS];
};

union ArrTo8 {
  byte array[8];
  int64_t val;
} __attribute__((packed));
union ArrTo4 {
  byte array[4];
  int32_t val;
} __attribute__((packed));
union ArrTo2 {
  byte array[2];
  int16_t val;
} __attribute__((packed));

#endif
