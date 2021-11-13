
#include <CellData.h>
#include "nvs_handle.hpp"
extern void WriteRelays(std::shared_ptr<nvs::NVSHandle> h, RelaySettings* r,int num);
extern void InitRelays(std::shared_ptr<nvs::NVSHandle> h, RelaySettings* r,int num);

#ifndef JTBMS_BMSComm_H_
#define JTBMS_BMSComm_H_

enum Msg {
  Nada,
  FirstMsg,
  NoPanic,
  FullChg,StatQuery,DynQuery,ClrMaxDiff,
  LastMsg,

  FirstEvent,
  WatchDog,
  CellsOverDue,
  CellTopV,
  CellBotV,
  CellTopT,
  CellBotT,
  PackTopV,
  PackBotV,
  PackTopT,
  PackBotT,LastEvent,

  FirstStr,Panic,DebugStr,LastStr,

  FirstSetting,SetCurSOC,SetPollFreq,SetAvg,SetConvTime,SetBattAH,
  SetMaxAmps,SetShuntUOhms,SetPVMaxAmps,SetPVShuntUOhms,SetNCells,
  SetRelayOff,SetTopAmps,
  LastSetting,

  SetCellSetts,
  StatSets,DynSets,Status,
  DumpCell,

};

struct AMsg {
  uint8_t crc,cmd;
} __attribute__((packed));

struct EventMsg {
  uint8_t crc,cmd;
  uint8_t cell;
  uint16_t val;
  uint16_t amps;
} __attribute__((packed));

struct SettingMsg {
  uint8_t crc,cmd;
  uint16_t val;
} __attribute__((packed));

struct DumpMsg {
  uint8_t crc,cmd,cell;
  uint32_t secs;
} __attribute__((packed));

struct StrMsg {
  uint8_t crc,cmd;
  char msg[256];
} __attribute__((packed));

#define C_RELAY_TOTAL 8
#define W_RELAY_TOTAL 6
#define RELAY_TOTAL (C_RELAY_TOTAL + W_RELAY_TOTAL)
#define MAX_CELLS 8

enum {
  Relay_Connect,Relay_Load,Relay_Charge
};

struct Cells {
  uint16_t volts:14,conn:1,dumping:1;
  uint8_t exTemp,bdTemp;
} __attribute__((packed));

struct BMSStatus {
  uint8_t crc,cmd;
  uint8_t stateOfCharge,watchDogHits;
  int8_t curTemp1,milliRolls;
  uint16_t lastPackMilliVolts,maxDiffMilliVolts;
  uint32_t lastPVMicroAmps,BatAHMeasured,lastMillis;
  int32_t lastMicroAmps,lastAdjMillAmpHrs;
  int64_t aveAdjMilliAmpMillis;
  Cells cells[MAX_CELLS];
  uint8_t previousRelayState[C_RELAY_TOTAL];
  uint16_t stateOfChargeValid:1,doFullChg:1,  maxCellVState:1,minCellVState:1,
    maxPackVState:1,minPackVState:1,
    maxCellCState:1,minCellCState:1,
    maxPackCState:1,minPackCState:1,
    maxChargePctState:1;
  uint8_t adjCnt;
} __attribute__((packed));

struct LimitConsts {
  static const int N=4;
  enum { Volt, Temp, Max0 };
  enum { Cell, Pack, Max1 };
  enum { Max, Min, Max2 };
  enum { Trip, Rec, Max3 };
} __attribute__((packed));

struct RelaySettings {
  char name[16],from[16];
  bool off,doSoC,fullChg;
  uint8_t type,trip,rec,rank;
};

struct StatSetts {
  uint8_t crc,cmd;
  bool useCellC,useTemp1;
  uint16_t limits[2][2][2][2],FloatV,ChargeRate,bdVolts;
  RelaySettings relays[C_RELAY_TOTAL];
  uint8_t ChargePct,ChargePctRec,CellsOutMin,CellsOutMax,CellsOutTime;
};

struct DynSetts {
  uint8_t crc,cmd;
  uint16_t BattAH,MaxAmps,PVMaxAmps,Avg,ConvTime,TopAmps;
  uint32_t ShuntUOhms,PVShuntUOhms,PollFreq;
  uint8_t nCells;
  uint64_t milliAmpMillis;
  time_t savedTime;
  CellSettings cellSets;
};

struct CellSetts {
  uint8_t crc,cmd;
  CellSettings cellSets;
};

union MaxData {
    AMsg amsg;
    EventMsg evt;
    StrMsg msg;
    SettingMsg set;
    DynSetts dynSets;
    StatSetts statSets;
    StatSetts battS;
    BMSStatus bms;
} __attribute__((packed));

extern uint8_t CRC8(const uint8_t *data,uint16_t length);
extern void BMSInitCom(void (*func)(const AMsg* mp));
extern void BMSSendRaw(uint8_t *d,uint16_t len);
template <typename T>
void BMSSend(T* m) {
  BMSSendRaw((uint8_t*)m,sizeof(T));
}

extern void BMSSend(StrMsg* m);
extern bool BMSWaitFor(AMsg*d,uint8_t cmd);
#endif