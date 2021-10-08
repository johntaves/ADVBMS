#include <bits/stdc++.h>

#ifndef JTBMS_CPUComm_H_
#define JTBMS_CPUComm_H_

#define CPUBAUD 9600

enum Msg {
  Panic,
  NoPanic,
  WatchDog,
  CellsOverDue,
  CellTopV,
  CellBotV,
  CellTopT,
  CellBotT,
  PackTopV,
  PackBotV,
  PackTopT,
  PackBotT,

  UpTime,

  FirstSetting,SetCurSOC,SetPollFreq,SetAvg,SetConvTime,SetBattAH,
  SetMaxAmps,SetShuntUOhms,SetPVMaxAmps,SetPVShuntUOhms,SetNCells,
  SetRelayOff,
  LastSetting,

  FirstCellSetting,
  SetCellCnt,SetCellDelay,SetCellTime,
  LastCellSetting,
  
  DumpCell,
  FullChg,
  ClrMaxDiff,
  BattSets,
  Status,
  DebugStr

};

struct EventMsg {
  uint8_t cmd;
  uint8_t cell;
  uint16_t val;
  uint16_t amps;
} __attribute__((packed));

struct SettingMsg {
  uint8_t cmd;
  uint16_t val;
} __attribute__((packed));

struct DumpMsg {
  uint8_t cmd,cell;
  uint32_t secs;
} __attribute__((packed));

struct StrMsg {
  uint8_t cmd;
  char msg[256];
} __attribute__((packed));

#define RELAY_TOTAL 8
#define MAX_CELLS 8

struct BMSStatus {
  std::bitset<RELAY_TOTAL> previousRelayState;
  uint8_t stateOfCharge,watchDogHits;
  int8_t curTemp1;
  uint16_t lastPackMilliVolts,maxDiffMilliVolts;
  uint32_t lastMicroAmps,lastPVMicroAmps;
  uint16_t stateOfChargeValid:1,doFullChg:1,  maxCellVState:1,minCellVState:1,
    maxPackVState:1,minPackVState:1,
    maxCellCState:1,minCellCState:1,
    maxPackCState:1,minPackCState:1,
    maxChargePctState:1;
  uint16_t cellVolts[MAX_CELLS];
  uint8_t cellTemps[MAX_CELLS];
  std::bitset<MAX_CELLS> cellDumping;
  std::bitset<MAX_CELLS> cellConn;
};

struct limits {
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
} __attribute__((packed));

struct BattSettings {
  uint8_t cmd;
  uint16_t limits[2][2][2][2];
  uint16_t BattAH,MaxAmps,PVMaxAmps;
  uint32_t ShuntUOhms,PVShuntUOhms;
  RelaySettings relays[RELAY_TOTAL];
  bool useCellC,useTemp1;
  uint16_t Avg,ConvTime;
  uint32_t PollFreq;
  uint8_t nCells;
  uint8_t ChargePct,ChargePctRec;
  uint16_t FloatV;
  int16_t ChargeRate,TopAmps;
  time_t savedTime;
  uint64_t milliAmpMillis;
  uint8_t CellsOutMin,CellsOutMax,CellsOutTime;
} __attribute__((packed));

union MaxData {
  EventMsg evt;
  StrMsg msg;
  SettingMsg set;
  BattSettings batt;
} __attribute__((packed));

#endif
