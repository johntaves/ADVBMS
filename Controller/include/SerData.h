
#include <Arduino.h>

#define MAX_CELLS 16
#define MAX_BANKS 4

struct CellChunk {
  uint16_t used:1,dump:1,v:10;
  uint16_t t;
} __attribute__((packed));

struct DataChunk {
  uint8_t crc;
  uint8_t bank:2,id:6;
  CellChunk cells[MAX_CELLS];
} __attribute__((packed));

extern DataChunk dataBuff;
