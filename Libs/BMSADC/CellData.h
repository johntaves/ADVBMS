
#ifndef JTBMS_CELLSETTINGS_H_
#define JTBMS_CELLSETTINGS_H_

#define MUL_ONE 10000l
#define MUL_DEV 600l

struct CellStatus {
    uint16_t volts:15,draining:1;
    int8_t tempExt,tempBd;
} __attribute__((packed));
 
struct CellSettings {
    uint16_t time,drainV;
    uint8_t cnt,delay;
    bool resPwrOn;
};

#endif
