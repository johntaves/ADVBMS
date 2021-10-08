
#ifndef JTBMS_CELLSETTINGS_H_
#define JTBMS_CELLSETTINGS_H_

struct CellSettings {
    uint16_t time;
    uint8_t cnt,delay;
} __attribute__((packed));

struct CellStatus {
    uint16_t volts;
    int8_t tempExt,tempBd,draining,drainSecs;
} __attribute__((packed));

#endif
