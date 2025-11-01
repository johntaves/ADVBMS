
#include <CellData.h>

#ifndef JTBMS_BMSComm_H_
#define JTBMS_BMSComm_H_

enum Msg {
  Nada,
  FirstMsg,
  NoPanic,
  FullChg,StatQuery,DynQuery,
  LastMsg,

  FirstEvent,
  WatchDog,
  CellsOverDue,
  ShuntOverDue,
  CellTopV,
  CellBotV,
  CellTopT,
  CellBotT,
  PackTopV,
  PackBotV,
  PackTopT,
  PackBotT,
  HeaterOn,
  HeaterOff,LastEvent,

  FirstStr,Panic,DebugStr,LastStr,

  FirstSetting,SetCurSOC,SetBattAH,
  SetNCells,
  SetRelayOff,SetTopAmps,
  SetDelay,SetCnt,SetResPwrOn,
  LastSetting,

  SetCellSetts,
  StatSets,DynSets,Status,
  DumpCell,
  ForgetCell,
  ConnCell,
  DiscCell,
  MoveCell

};

struct AMsg {
  uint8_t crc,cmd;
} __attribute__((packed));

struct EventMsg {
  uint8_t crc,cmd,relay,xtra;
  uint8_t cell;
  int8_t tC;
  uint16_t mV;
  int16_t amps;
  uint16_t ms;
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

#define C_RELAY_TOTAL 10
#define W_RELAY_TOTAL 6
#define RELAY_TOTAL (C_RELAY_TOTAL + W_RELAY_TOTAL)
#define MAX_CELLS 8
#define CHECKSTATUS 2000
// shut everything off if status has not happened within 2 secs of when it should
#define WATCHDOGSLOP 2000

enum {
  Relay_Connect,Relay_Load,Relay_Charge,Relay_Heat,Relay_Direction,Relay_Slide,Relay_Therm,Relay_Ampinvt,Relay_Unused
};

struct Cells {
  uint16_t volts:14,conn:1,draining:1;
  int8_t exTemp,bdTemp;
} __attribute__((packed));

struct BMSStatus {
  uint8_t crc,cmd;
  uint8_t stateOfCharge,watchDogHits;
  int8_t curBoardTemp,milliRolls;
  uint16_t lastPackMilliVolts,lastPVMilliVolts;
  uint32_t BatAHMeasured,lastMillis;
  int32_t lastPVMilliAmps,lastInvMilliAmps,lastMilliAmps,lastAdjCoulomb;
  Cells cells[MAX_CELLS];
  uint8_t previousRelayState[C_RELAY_TOTAL];
  uint16_t stateOfChargeValid:1,doFullChg:1,  maxCellVState:1,minCellVState:1,
    maxPackVState:1,minPackVState:1,
    maxCellCState:1,minCellCState:1,
    maxPackCState:1,minPackCState:1,
    maxChargePctState:1,
    ampInvtGoal:1;
  int16_t ampTemp;
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
  uint8_t type,trip,rec;
};

struct StatSetts {
  uint8_t crc,cmd;
  bool useCellC,useBoardTemp;
  uint16_t limits[2][2][2][2],unused,ChargeRate,bdVolts;
  RelaySettings relays[C_RELAY_TOTAL];
  uint8_t ChargePct,ChargePctRec,CellsOutMin,CellsOutMax,CellsOutTime,MainID,PVID,InvID;
  uint32_t slideMS,ShuntErrTime;
};

struct DynSetts {
  uint8_t crc,cmd,cnt,delay;
  uint16_t BattAH,TopAmps;
  uint8_t nCells;
  bool resPwrOn;
  int64_t coulombOffset;
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

extern void InitRelays(RelaySettings* r,int num);
extern uint8_t CRC8(const uint8_t *data,uint16_t length);
extern void BMSInitCom(void (*func)(const AMsg* mp));
extern void BMSSendRaw(uint8_t *d,uint16_t len);
template <typename T>
void BMSSend(T* m) {
  BMSSendRaw((uint8_t*)m,sizeof(T));
}

extern void BMSInitStatus(BMSStatus *sp);
extern bool readEE(const char* name,uint8_t *p,size_t s);
extern void writeEE(const char* name,uint8_t *p,size_t s);
extern void InitRelays(RelaySettings* rp,int num);
extern void BMSSend(StrMsg* m);
extern bool BMSWaitFor(AMsg*d,uint8_t cmd);
extern void BMSGetSerial();
#endif