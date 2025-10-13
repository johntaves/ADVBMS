
#ifndef JTBMS_CELLSETTINGS_H_
#define JTBMS_CELLSETTINGS_H_

struct CellStatus {
    uint16_t volts;
    int8_t tempExt,tempBd,draining;
} __attribute__((packed));
 
struct CellSettings {
    uint16_t time,drainV;
    uint8_t cnt,delay;
    bool resPwrOn;
};

#endif
