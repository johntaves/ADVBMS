
#include <Arduino.h>
#ifndef SerData_H_
#define SerData_H_

#define MAX_CELLS 16
#define MAX_BANKS 4
#define VER 0

struct CellSerData {
  uint16_t used:1,dump:1,un1:4,t:10;
  uint16_t ver:4,un2:2,v:10;
} __attribute__((packed));

struct CellsHeader {
  uint8_t crc;
  uint8_t bank:2,id:2,ver:4;
} __attribute__((packed));

struct CellsSerData {
  CellsHeader hdr;
  CellSerData cells[MAX_CELLS];
} __attribute__((packed));

#endif
