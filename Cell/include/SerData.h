
#include <Arduino.h>
#ifndef SerData_H_
#define SerData_H_

#define MAX_CELLS 16
#define MAX_BANKS 4

struct CellSerData {
  uint16_t used:1,dump:1,v:10;
  uint16_t t;
} __attribute__((packed));

struct CellsSerData {
  uint8_t crc;
  uint8_t bank:2,id:6;
  CellSerData cells[MAX_CELLS];
} __attribute__((packed));

extern char CRC8(const char *data,int length);
#endif
