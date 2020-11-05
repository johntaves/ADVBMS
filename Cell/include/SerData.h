
#include <Arduino.h>

struct CellChunk {
  uint16_t used:1,dump:1,v:10;
  uint16_t t;
} __attribute__((packed));

struct DataChunk {
  uint8_t crc;
  uint8_t size:4,id:4;
  CellChunk cells[0];
} __attribute__((packed));
