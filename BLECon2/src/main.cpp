#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>

#include <time.h>
#include <Ticker.h>
#include <BMSADC.h>
#include <BMSCommArd.h>
#include <CellData.h>
#include <CAN.h>
#include "defines.h"

// improve server
// adjustable cell history
// cell dump safety, stop if cell gets low, show dump status
// charge control feature to reduce amps
// improve the SoC allow to run logic. Make the code bad, and see if the charge/loads go off

#define AVEADJAVGS 16

int8_t analCnt=0,curAnal=0;
bool shuntOverDue=true,cellsOverDue = true,loadsOff = true,chgOff = true
  ,writeStatSets=false,writeDynSets=false,writeCellSet=false,missingCell;
uint32_t statusCnt=0,lastHitCnt=0,scanStart=0;
Ticker watchDog;

bool AmpinvtOn; // status goal
uint32_t AmpinvtSt = 0; // start time of toggle
uint32_t AmpinvtStWait = 0; // start time of wait after toggled
RelaySettings* AmpinvtRelayPtr;
int AmpinvtRelayPin;

class MyClientCallback;

struct Cell {
  NimBLEClient* pClient;
  NimBLERemoteCharacteristic* pChar;
  MyClientCallback* pCB;
  NimBLERemoteCharacteristic* pSettings;
  uint32_t cellDumpSecs,cellLast;
  bool cellSentSet;
};

Cell cells[MAX_CELLS];
SemaphoreHandle_t xMut;

const int relayPins[C_RELAY_TOTAL] = { GPIO_NUM_19,GPIO_NUM_18,GPIO_NUM_2,GPIO_NUM_15,GPIO_NUM_13,GPIO_NUM_14,GPIO_NUM_27,GPIO_NUM_26,GPIO_NUM_25,GPIO_NUM_33 };

NimBLEAddress emptyAddress;
DynSetts dynSets;
StatSetts statSets;
BLESettings cellBLE;
BMSStatus st;
char spb[1024];

uint32_t ShuntMS[3];
enum { Main, PV, Inv };
uint32_t statusMS=0,connectMS=0,pvPollMS=0;
bool inAlertState = true;

#define MAX_CELLV (statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Trip])
NimBLEScan* pBLEScan;

uint32_t lastShuntMillis;
int32_t battCoulombs;
int64_t coulombs,adjCoulombs=0;
int lastTrip;

//char (*__kaboom)[sizeof( BMSStatus )] = 1;

bool doShutOffNoStatus() {
  return !st.stateOfChargeValid || st.stateOfCharge < statSets.CellsOutMin || st.stateOfCharge > statSets.CellsOutMax;
}

void clearRelays() {
  for (int i=0;i<C_RELAY_TOTAL;i++) {
    digitalWrite(relayPins[i], LOW);
    st.previousRelayState[i] = LOW;
  }
}

void SendEvent(uint8_t cmd,uint16_t volts=0,int8_t temp=0,int cell=0,uint16_t ms=0,int relay=0,int xtra=0) {
  EventMsg evt;
  evt.cmd = cmd;
  evt.cell = cell;
  evt.mV = volts;
  evt.tC = temp;
  evt.relay = relay;
  evt.xtra = xtra;
  evt.ms = ms;
  evt.amps = (int16_t)(st.lastMilliAmps/1000);
  BMSSend(&evt);
}

void doWatchDog() {
  clearRelays();
  SendEvent(WatchDog);
  st.watchDogHits++;
}

bool isFromOff(RelaySettings* rs) {
  if (!strlen(rs->from))
    return false;
  for (int8_t y = 0; y < C_RELAY_TOTAL; y++)
  {
    RelaySettings *rp = &statSets.relays[y];
    if (rp == rs) continue;
    if (!strcmp(rp->name,rs->from))
      return st.previousRelayState[y] == LOW;
  }
  return false;
}

void sendCellSet(int i) {
  NimBLEClient*  pC = cells[i].pClient;
  if (!pC || !pC->isConnected())
    return;
  if (cells[i].pSettings) {
    cells[i].pSettings->writeValue<CellSettings>(dynSets.cellSets,false);
    cells[i].cellSentSet = true;
  }
}

class MyClientCallback : public NimBLEClientCallbacks {
  public:
  int cell;
  MyClientCallback(int cell) {
    this->cell = cell;
  }
  void onConnect(NimBLEClient* pclient) {
    st.cells[cell].conn = true;
    st.cells[cell].volts = 0;
    cells[cell].cellSentSet = false;
    SettingMsg ms;
    ms.cmd = ConnCell;
    ms.val = cell;
    BMSSend(&ms);
  }

  void onDisconnect(NimBLEClient* pclient) {
    st.cells[cell].conn = false;
    st.cells[cell].volts = 0;
    SettingMsg ms;
    ms.cmd = DiscCell;
    ms.val = cell;
    BMSSend(&ms);
  }
};

void ConnectCell(int i) {
  NimBLEClient*  pC = cells[i].pClient;
  if (!pC) return;
  if (pC->isConnected()) {
    if (!cells[i].cellSentSet)
      sendCellSet(i);
    return;
  }
  // Connect to the remove BLE Server.
  if (cellBLE.addrs[i] == emptyAddress)
    return;
Serial.printf("Connecting: %s\n",cellBLE.addrs[i].toString().c_str());
  if (!pC->connect(BLEAddress(cellBLE.addrs[i]),true)) {
    Serial.printf("Failed to connect: %d, %s\n",i,((std::string)cellBLE.addrs[i]).c_str());
    return;
  }

  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService* pServ = pC->getService(NimBLEUUID((uint16_t)0x180F));
  if (pServ == nullptr) {
    Serial.printf("Failed to find our service UUID: %d\n",i);
    pC->disconnect();
    return;
  }
  NimBLERemoteCharacteristic* pChar = pServ->getCharacteristic(NimBLEUUID((uint16_t)0x2B18)); // status
  if (pChar == nullptr) {
    pC->disconnect();
    return;
  }
  cells[i].pChar = pChar;
  pChar->subscribe(true,[](NimBLERemoteCharacteristic* pBLERemoteCharacteristic,
                              uint8_t* pData, size_t length, bool isNotify) {
          int i=0;
          for (;i<cellBLE.numCells && cells[i].pChar != pBLERemoteCharacteristic;i++) ;
          if (i == cellBLE.numCells) return;
          CellStatus* cd = (CellStatus*)pData;
          st.cells[i].volts = cd->volts;
          st.cells[i].exTemp = cd->tempExt;
          st.cells[i].bdTemp = cd->tempBd;
          st.cells[i].dumping = cd->drainSecs > 0;
          st.cells[i].conn = true;
          cells[i].cellLast = millis();
        },false);
  cells[i].pSettings = pServ->getCharacteristic(NimBLEUUID((uint16_t)0x2B15)); // settings
  sendCellSet(i);
}

void sendCellSets() {
  for (int i=0;i<cellBLE.numCells;i++)
    sendCellSet(i);
}

#define IntervalToms(x) (x * 1000 / BLE_HCI_CONN_ITVL)

void InitCell(int i) {
  NimBLEClient*  pC  = NimBLEDevice::createClient();

  cells[i].pCB = new MyClientCallback(i);
  pC->setClientCallbacks(cells[i].pCB,true);
  pC->setConnectionParams(IntervalToms(100),IntervalToms(100),25,600);
  cells[i].pClient = pC;
  st.cells[i].dumping = false;
  cells[i].cellDumpSecs = 0;
}
void InitCells() {
  for (int i=0;i<cellBLE.numCells;i++)
    InitCell(i);
}

void ConnectCells() {
  for (int i=0;i<cellBLE.numCells;i++) {
    xSemaphoreTake( xMut, portMAX_DELAY );
    ConnectCell(i);
    xSemaphoreGive( xMut );
  }
}

void checkMissingCell() {
  int i=0;
  for (;i<cellBLE.numCells && cellBLE.addrs[i] != emptyAddress;i++) ;
  missingCell = i < cellBLE.numCells;
  if (missingCell)
    Serial.printf("Miss: %s\n",cellBLE.addrs[i].toString().c_str());
  else Serial.println("All Present");
}

void CheckBLEScan() {
  if ((dynSets.nCells > cellBLE.numCells || missingCell) && !scanStart && dynSets.nCells) {
    Serial.println("Starting scan");
    if (doShutOffNoStatus())
      clearRelays();
    pBLEScan->start(0,NULL,false);
    scanStart = millis();
  } else if (cellBLE.numCells == dynSets.nCells && !missingCell && scanStart) {
    pBLEScan->stop(); // need to stop scanning to connect
    scanStart = 0;
    Serial.println("Stop scan");
  }
}

class adCB: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* ad) {
      if (!ad->getName().compare("LiFePo4 Cell")
              && ad->haveServiceUUID()
              && ad->isAdvertisingService(NimBLEUUID((uint16_t)0x180F))) {

        int i=0;
        for (;i<cellBLE.numCells && cellBLE.addrs[i] != ad->getAddress()
           && cellBLE.addrs[i] != emptyAddress;i++) ;
        Serial.printf("Found %d %d\n",cellBLE.numCells,i);
        if (i < dynSets.nCells) {
          if (i==cellBLE.numCells)
            cellBLE.numCells++;
          cellBLE.addrs[i] = ad->getAddress();
          Serial.printf("Add/Upd %d: %s\n",i,cellBLE.addrs[i].toString().c_str());
          InitCell(i);
          writeCellSet = true;
        }
        checkMissingCell();
      }
    }
};

void setOffset(int16_t val) {
  dynSets.coulombOffset = ((battCoulombs*val)/100) - coulombs;
}

void setBattAH() {
  battCoulombs = (uint64_t)dynSets.BattAH * (60 * 60); // convert to coulombs
}

void checkStatus()
{
  statusCnt++;
  statusMS = millis();
  digitalWrite(RESISTOR_PWR,HIGH);
  if (dynSets.cellSets.delay)
    delay(dynSets.cellSets.delay);
  st.curBoardTemp = BMSReadTemp(TEMP1,false,statSets.bdVolts,BCOEF,47000,47000,dynSets.cellSets.cnt);
  if (!dynSets.cellSets.resPwrOn)
    digitalWrite(RESISTOR_PWR,LOW);
  if ((st.lastMilliAmps > 0 && chgOff) || (st.lastMilliAmps < 0 && loadsOff)) {
    if (!inAlertState) {
      uint16_t maxCellV = 0;
      uint16_t minCellV = 0xffff;
      for (int j=0;j<dynSets.nCells;j++) {
        uint16_t cellV = st.cells[j].volts;
        if (cellV > maxCellV)
          maxCellV = cellV;
        if (cellV < minCellV)
          minCellV = cellV;
      }
      StrMsg msg;
      msg.cmd = Panic;
      snprintf(msg.msg,sizeof(msg.msg),"uA=%d, chg: %d, Lds: %d, pack: %dmV, max cell: %dmV, min cell: %dmV, MxPV: %d, MxCV: %d, MnPV: %d, MnCV %d, MxCC: %d, MxPC: %d"
          ,st.lastMilliAmps,chgOff,loadsOff,(int)st.lastPackMilliVolts,(int)maxCellV,(int)minCellV
          ,st.maxPackVState,st.maxCellVState,st.minPackVState,st.minCellVState,st.maxCellCState,st.maxPackCState);
      BMSSend(&msg);
      clearRelays();
      inAlertState = true;
    }
  } else if (inAlertState) {
    inAlertState = false;
    AMsg msg;
    msg.cmd = NoPanic;
    BMSSend(&msg);
  }
  if (lastTrip > 0 && (coulombs + dynSets.coulombOffset) > battCoulombs)
  {
    adjCoulombs = battCoulombs - (coulombs + dynSets.coulombOffset);
    setOffset(100);
  }
  st.stateOfCharge = ((coulombs + dynSets.coulombOffset) * 100) / battCoulombs;

  bool allovervoltrec = true,allundervoltrec = true,hitTop=false,hitUnder=false;
  bool allovertemprec = true,allundertemprec = true;
  //Loop through cells
  uint16_t totalVolts=0;
  int nCellsAlive = 0;
  if ((millis() - ShuntMS[PV]) > statSets.ShuntErrTime)
    st.lastPVMilliAmps = 0;
  if ((millis() - ShuntMS[Inv]) > statSets.ShuntErrTime)
    st.lastInvMilliAmps = 0;
  if ((millis() - ShuntMS[Main]) > statSets.ShuntErrTime) {
    if (!shuntOverDue)
      SendEvent(ShuntOverDue);
    st.lastPackMilliVolts = 0;
    st.lastMilliAmps = 0;
    shuntOverDue = true;
  } else
    shuntOverDue = false;
  for (int8_t i = 0; i < dynSets.nCells; i++)
  {
    if (!st.cells[i].conn || !st.cells[i].volts || (millis() - cells[i].cellLast) > (1000*(uint32_t)statSets.CellsOutTime)) {
      if (!cellsOverDue)
        SendEvent(CellsOverDue,st.cells[i].volts,st.cells[i].bdTemp,i,millis() - cells[i].cellLast);
      cellsOverDue = true;
      st.cells[i].conn = false;
      continue;
    }
    nCellsAlive++;
      
    uint16_t cellV = st.cells[i].volts;
    int8_t cellT = st.cells[i].exTemp;
    totalVolts += cellV;

    if (cellV > statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Trip]) {
      if (!st.maxCellVState) {
        if (!hitTop)
          SendEvent(CellTopV,cellV,cellT,i);
        hitTop = true;
      }
      st.maxCellVState = true;
    }
    if (cellV > statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Rec])
      allovervoltrec = false;

    if (cellV < statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Min][LimitConsts::Trip]) {
      if (!st.minCellVState) {
        if (!hitUnder)
          SendEvent(CellBotV,cellV,cellT,i);
        hitUnder = true;
      }
      st.minCellVState = true;
    }
    if (cellV < statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Min][LimitConsts::Rec])
      allundervoltrec = false;

    if (statSets.useCellC && cellT != -40) {
      if (cellT > statSets.limits[LimitConsts::Temp][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Trip]) {
        if (!st.maxCellCState)
          SendEvent(CellTopT,cellV,cellT,i);
        st.maxCellCState = true;
      }

      if (cellT > statSets.limits[LimitConsts::Temp][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Rec])
        allovertemprec = false;

      if (cellT < statSets.limits[LimitConsts::Temp][LimitConsts::Cell][LimitConsts::Min][LimitConsts::Trip]) {
        if (!st.minCellCState)
          SendEvent(CellBotT,cellV,cellT,i);
        st.minCellCState = true;
      }
      if (cellT < statSets.limits[LimitConsts::Temp][LimitConsts::Cell][LimitConsts::Min][LimitConsts::Rec])
        allundertemprec = false;
    }
  }
  if (nCellsAlive == dynSets.nCells)
    cellsOverDue = false;
  if (st.maxCellVState && allovervoltrec)
    st.maxCellVState = false;
  if (st.minCellVState && allundervoltrec)
    st.minCellVState = false;
  if (!statSets.useCellC || (st.maxCellCState && allovertemprec))
    st.maxCellCState = false;
  if (!statSets.useCellC || (st.minCellCState && allundertemprec))
    st.minCellCState = false;

  if (statSets.useBoardTemp) {
    if (st.curBoardTemp > statSets.limits[LimitConsts::Temp][LimitConsts::Pack][LimitConsts::Max][LimitConsts::Trip])
      st.maxPackCState = true;
    if (st.curBoardTemp < statSets.limits[LimitConsts::Temp][LimitConsts::Pack][LimitConsts::Max][LimitConsts::Rec])
      st.maxPackCState = false;
    if (st.curBoardTemp < statSets.limits[LimitConsts::Temp][LimitConsts::Pack][LimitConsts::Min][LimitConsts::Trip])
      st.minPackCState = true;
    if (st.curBoardTemp > statSets.limits[LimitConsts::Temp][LimitConsts::Pack][LimitConsts::Min][LimitConsts::Rec])
      st.minPackCState = false;
  }
  uint16_t packV = (st.lastPackMilliVolts > totalVolts ? st.lastPackMilliVolts : totalVolts);
  if (packV > statSets.limits[LimitConsts::Volt][LimitConsts::Pack][LimitConsts::Max][LimitConsts::Trip]) {
    if (!st.maxPackVState) {
      if (!hitTop)
        SendEvent(PackTopV,st.lastPackMilliVolts);
      hitTop = true;
    }
    st.maxPackVState = true;
  } else if (packV < statSets.limits[LimitConsts::Volt][LimitConsts::Pack][LimitConsts::Max][LimitConsts::Rec])
    st.maxPackVState = false;

  if (packV < statSets.limits[LimitConsts::Volt][LimitConsts::Pack][LimitConsts::Min][LimitConsts::Trip]) {
    if (!st.minPackVState) {
      if (!hitUnder)
        SendEvent(PackBotV,st.lastPackMilliVolts);
      hitUnder = true;
    }
    st.minPackVState = true;
  } else if (packV > statSets.limits[LimitConsts::Volt][LimitConsts::Pack][LimitConsts::Min][LimitConsts::Rec])
    st.minPackVState = false;

  if (hitTop || hitUnder) {
    if (!lastHitCnt)
      lastHitCnt=statusCnt;
    else lastHitCnt++;
  } else
    lastHitCnt = 0;
  hitTop = hitTop && (int32_t)dynSets.TopAmps > (st.lastMilliAmps/1000) && (statusCnt == lastHitCnt); // We don't want to trigger a hittop 
  hitUnder = hitUnder && (int32_t)-dynSets.TopAmps < (st.lastMilliAmps/1000) && (statusCnt == lastHitCnt);
  if (hitTop) {
    if (lastTrip > 0)
      st.lastAdjCoulomb = battCoulombs - (coulombs + dynSets.coulombOffset) + adjCoulombs;
    lastTrip = 1;
    setOffset(100);
    writeDynSets = true;
    st.stateOfChargeValid = true;
    st.doFullChg = false;
  }
  if (hitUnder) {
    if (lastTrip > 0)
        st.BatAHMeasured = (battCoulombs - (coulombs + dynSets.coulombOffset)) / ((uint64_t)60 * 60);
    lastTrip = -1;
    st.stateOfChargeValid = true;
  }
  if (st.stateOfChargeValid && !st.doFullChg) {
    if (st.stateOfCharge > statSets.ChargePct)
      st.maxChargePctState = true;
    else if (st.stateOfCharge < statSets.ChargePctRec)
      st.maxChargePctState = false;
  } else
    st.maxChargePctState = false;

  uint8_t relay[C_RELAY_TOTAL];
  bool wasLoadsOff = loadsOff;
  bool wasChgOff = chgOff;
  loadsOff = st.minCellVState || st.minPackVState || st.maxCellCState || st.maxPackCState
      || (cellsOverDue && (!st.stateOfChargeValid || st.stateOfCharge < statSets.CellsOutMin));
  chgOff = st.maxChargePctState || st.maxCellVState || st.maxPackVState || st.minCellCState
           || st.maxCellCState || st.minPackCState || st.maxPackCState
           || (cellsOverDue && (!st.stateOfChargeValid || st.stateOfCharge > statSets.CellsOutMax));
  for (int8_t y = 0; y < C_RELAY_TOTAL; y++)
  {
    RelaySettings *rp = &statSets.relays[y];
    relay[y] = st.previousRelayState[y]; // don't change it because we might be in the SOC trip/rec area
    if (rp->off)
      relay[y] = LOW;
    else
      switch (rp->type) {
        default:
          break;
        case Relay_Connect: relay[y] = (wasLoadsOff && loadsOff && st.lastMilliAmps < 0)
         || (wasChgOff && chgOff && st.lastMilliAmps > 0)?LOW:HIGH; break;
        case Relay_Load:
          if (isFromOff(rp))
            relay[y] = HIGH;
          else if (loadsOff || (rp->doSoC && (!st.stateOfChargeValid || st.stateOfCharge < rp->trip)))
            relay[y] = LOW; // turn if off
          else if (!rp->doSoC || (rp->doSoC && st.stateOfChargeValid && st.stateOfCharge > rp->rec))
            relay[y] = HIGH; // turn it on
          // else leave it as-is
          break;
        case Relay_Charge:
          if (chgOff || (rp->doSoC && (!st.stateOfChargeValid || st.stateOfCharge > rp->trip)))
            relay[y] = LOW; // off
          else if (!rp->doSoC || (rp->doSoC && st.stateOfChargeValid && st.stateOfCharge < rp->rec))
            relay[y] = HIGH; // on
          // else leave it as-is
          break;
        case Relay_Heat: {
            int16_t val=255;
            for (int i=0;i<dynSets.nCells;i++)
              if (st.cells[i].exTemp < val && st.cells[i].conn && st.cells[i].volts)
                val = st.cells[i].exTemp;
            if (val < rp->trip) relay[y] = HIGH;
            else if (val > rp->rec) relay[y] = LOW;
          }
          break;
        case Relay_Ampinvt:
          if (isFromOff(rp))
            AmpinvtOn = false;
          else if (rp->doSoC && (!st.stateOfChargeValid || st.stateOfCharge < rp->trip))
            AmpinvtOn = false; // turn it off
          else if (rp->doSoC && st.stateOfChargeValid && st.stateOfCharge > rp->rec)
            AmpinvtOn = true; // turn it on
          break;
      }
  }
  for (int8_t n = 0; n < C_RELAY_TOTAL; n++)
  {
    if (st.previousRelayState[n] != relay[n])
    {
      digitalWrite(relayPins[n], relay[n]);
      st.previousRelayState[n] = relay[n];
    }
  }
  // balance calcs
  if (hitTop) {
    
  }
  watchDog.once_ms(CHECKSTATUS+WATCHDOGSLOP,doWatchDog);
  BMSSend(&st);
}

void initdynSets() {
  dynSets.nCells=0;
  dynSets.BattAH = 1;
  dynSets.coulombOffset = 0;
  dynSets.cellSets.cnt = 4;
  dynSets.cellSets.delay = 1;
  dynSets.cellSets.resPwrOn = false;
  dynSets.cellSets.time = 1000; // this will be like 2 secs, because cell goes to sleep and slows CPU by 2x
  dynSets.TopAmps = 6;
}
void initstatSets() {
  statSets.useBoardTemp = true;
  statSets.useCellC = true;
  statSets.ShuntErrTime = 750;
  statSets.MainID = 3;
  statSets.PVID = 4;
  statSets.InvID = 6;
  statSets.bdVolts = 3300;
  statSets.ChargePct = 100;
  statSets.ChargePctRec = 0;
  statSets.ChargeRate = 0;
  statSets.CellsOutMax = 80;
  statSets.CellsOutMin = 30;
  statSets.CellsOutTime = 12;
  statSets.FloatV = 3400;
  statSets.slideMS = 10000;
  InitRelays(&statSets.relays[0],C_RELAY_TOTAL);
  for (int i=0;i<C_RELAY_TOTAL;i++) {
    RelaySettings* r = &statSets.relays[i];
    r->name[0] = 0;
    r->from[0] = 0;
    r->doSoC = false;
    r->off = true;
    r->fullChg = false;
    r->rec = 0;
    r->trip = 0;
    r->type = 0;
  }
  for (int i=LimitConsts::Cell;i<LimitConsts::Max1;i++) {
    int mul = !i?1:8;
    statSets.limits[LimitConsts::Volt][i][LimitConsts::Max][LimitConsts::Trip] = 3500 * mul;
    statSets.limits[LimitConsts::Volt][i][LimitConsts::Max][LimitConsts::Rec] = 3400 * mul;
    statSets.limits[LimitConsts::Volt][i][LimitConsts::Min][LimitConsts::Trip] = 3100 * mul;
    statSets.limits[LimitConsts::Volt][i][LimitConsts::Min][LimitConsts::Rec] = 3200 * mul;
    statSets.limits[LimitConsts::Temp][i][LimitConsts::Max][LimitConsts::Trip] = 50;
    statSets.limits[LimitConsts::Temp][i][LimitConsts::Max][LimitConsts::Rec] = 45;
    statSets.limits[LimitConsts::Temp][i][LimitConsts::Min][LimitConsts::Trip] = 4;
    statSets.limits[LimitConsts::Temp][i][LimitConsts::Min][LimitConsts::Rec] = 7;
  }
}

void DoSetting(uint8_t cmd,uint16_t val) {
  switch (cmd) {
    case SetCurSOC:
      setOffset(val);
      st.stateOfChargeValid = true;
      lastTrip = 0;
      st.doFullChg = true;
      break;
    case SetTopAmps:
      dynSets.TopAmps = val;
      break;
    case SetBattAH:
      dynSets.BattAH = val;
      setBattAH();
      setOffset(st.stateOfCharge);
      break;
    case SetNCells:
      dynSets.nCells = val;
      Serial.printf("NCells: %d\n",dynSets.nCells);
      if (dynSets.nCells < cellBLE.numCells) {
        for (int i=dynSets.nCells;i<cellBLE.numCells;i++) {
          Serial.printf("Deleting %d\n",i);
          NimBLEDevice::deleteClient(cells[i].pClient);
          cells[i].pClient = NULL;
          cells[i].pSettings = NULL;
          cellBLE.addrs[i] = emptyAddress;
        }
        cellBLE.numCells = dynSets.nCells;
        writeCellSet = true;
      } else if (dynSets.nCells > cellBLE.numCells) {
        for (int i=cellBLE.numCells;i<dynSets.nCells;i++)
          cellBLE.addrs[i] = emptyAddress;
      }
      break;
    case SetRelayOff: {
      RelaySettings *rp = &statSets.relays[val];
      rp->off = !rp->off;
      }
      break;
  }
  writeDynSets = true;
}

void DoDump(DumpMsg *dm) {
  Serial.println("Doing dump\n");
    cells[dm->cell].cellDumpSecs = dm->secs;
    BLERemoteService* pServ = cells[dm->cell].pClient->getService(NimBLEUUID((uint16_t)0x180F));
    if (pServ) {
      NimBLERemoteCharacteristic* pChar = pServ->getCharacteristic(NimBLEUUID((uint16_t)0X2AE2));
      if (pChar) {
        pChar->writeValue<uint32_t>(cells[dm->cell].cellDumpSecs);
        Serial.printf("Write %d %d\n",dm->cell,cells[dm->cell].cellDumpSecs);
      }
    }
}

void DoMove(SettingMsg *mp) {
  if (!mp->val)
    return;
  xSemaphoreTake( xMut, portMAX_DELAY );
  Cell x = cells[mp->val];
  NimBLEAddress a = cellBLE.addrs[mp->val];
  cells[mp->val] = cells[mp->val-1];
  cells[mp->val].pCB->cell = mp->val;
  cellBLE.addrs[mp->val] = cellBLE.addrs[mp->val-1];
  cells[mp->val-1] = x;
  cells[mp->val-1].pCB->cell = mp->val-1;
  cellBLE.addrs[mp->val-1] = a;
  writeCellSet = true;
  xSemaphoreGive( xMut );
}

void DoForget(SettingMsg *mp) {
  if (mp->val >= MAX_CELLS)
    return;
  missingCell = true;
  Serial.printf("forgetting: %d %s\n",mp->val,cellBLE.addrs[mp->val].toString().c_str());
  cellBLE.addrs[mp->val] = emptyAddress;
  if (!cells[mp->val].pClient) return;
  NimBLEDevice::deleteClient(cells[mp->val].pClient);
  cells[mp->val].pClient = NULL;
  st.cells[mp->val].volts = 0;
  st.cells[mp->val].conn = 0;
  writeCellSet = true;
}

void SetAmpinvtVals() {
  for (int i=0;i<C_RELAY_TOTAL;i++) {
    if (statSets.relays[i].type == Relay_Ampinvt) {
      AmpinvtRelayPtr = &statSets.relays[i];
      AmpinvtRelayPin = relayPins[i];
      break;
    }
  }
}

void ConSerData(const AMsg *mp)
{
  if (mp->cmd > FirstSetting && mp->cmd < LastSetting)
    DoSetting(mp->cmd,((SettingMsg*)mp)->val);
  else switch (mp->cmd) {
    case SetCellSetts: {
        dynSets.cellSets = ((CellSetts*)mp)->cellSets;
        sendCellSets();
        writeDynSets = true;
      }
      break;
    case MoveCell: DoMove((SettingMsg*)mp); break;
    case ForgetCell: DoForget((SettingMsg*)mp); break;
    case DumpCell: DoDump((DumpMsg*)mp); break;
    case FullChg: st.doFullChg = !st.doFullChg; break;
    case StatQuery: BMSSend(&statSets); break;
    case DynQuery: BMSSend(&dynSets); break;
    case StatSets: 
      statSets = *(StatSetts*)mp;
      SetAmpinvtVals();
      writeStatSets=true;
      break;
  }
}

void BLETask(void *arg) {
  for (;;) {
    ConnectCells();
    CheckBLEScan();
    delay(CHECKCONNECT);
  }
}

void canSender(uint16_t id,byte n,byte p[],int len) {
//    Serial.printf("Asking 0x%x 0x%x, ",id | 0xFB,n);
    byte mod = 0xfb;
    if (len) mod = 0xfa;
    if (!CAN.beginPacket (id | mod))
      Serial.println("Cannot begin packet"); 
    CAN.write (n);
    for (int i=0;i<len;i++) {
//      Serial.printf("%x",p[i]);
      CAN.write(p[i]);
    }
    CAN.endPacket();
//    Serial.println ("done");
}

int64_t ReadIt(int len, int inc) {
  int st=0;
  if (len == 8) {
    ArrTo8 val8;
    if (inc < 0)
      st=7;
    else st=0;
    for (int x=0;x<8;x++) {
      val8.array[st] = CAN.read();
      st += inc;
    }
    return val8.val;
  } else if (len == 4) {
    ArrTo4 val4;
    if (inc < 0)
      st=3;
    else st=0;
    for (int x=0;x<8;x++) {
      val4.array[st] = CAN.read();
      st += inc;
    }
    return val4.val;
  } else if (len == 2) {
    if (inc < 0)
      st=1;
    ArrTo2 val2;
    for (int x=0;x<2;x++) {
      val2.array[st] = CAN.read();
      st += inc;
    }
    return val2.val;
  }
  while (len--)
    CAN.read();
  return 0;
}
void RecCAN(int packetSize) {
  long id = CAN.packetId();
  int64_t val;
  uint8_t dev = id >> 8;
  uint8_t msg = id & 0xff;
  if ((dev != statSets.PVID && dev != statSets.MainID && dev != statSets.InvID) ||
      (msg != 0xF1 && msg != 0xF3 && msg != 0xF4)) {
    Serial.printf("Bad id: 0x%lx len: %d\n",id,packetSize);
      return;
  }
  if (msg < 0xfa) val = ReadIt(packetSize,1);
  else val = ReadIt(packetSize,-1);
//  Serial.printf("l: %d, d: %d, m: %d, v: %d\n",packetSize,dev,msg,(int)val);
  if (dev == statSets.MainID) {
    switch (msg) {
      case 0xF1: // current
        st.lastMilliAmps = val;
        break;
      case 0xF3: // voltage
        st.lastPackMilliVolts = val;
        break;
      case 0xF4:
        coulombs = val;
        break;
    }
    ShuntMS[Main] = millis();
  } else if (dev == statSets.PVID){
    if (msg == 0xF1) // current
      st.lastPVMilliAmps = val;
    ShuntMS[PV] = millis();
  } else if (dev == statSets.InvID) {
    if (msg == 0xF1) // current
      st.lastInvMilliAmps = val;
    ShuntMS[Inv] = millis();
  }
}

void setup() {
  Serial.begin(9600);
  for (int i=0;i<MAX_CELLS;i++) cells[i].pClient = NULL;

  BMSADCInit();
  adc1_config_channel_atten(TEMP1, ADC_ATTEN_DB_11);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RESISTOR_PWR, OUTPUT);
  pinMode(INV, INPUT);
  digitalWrite(GREEN_LED,1);

  if (!readEE("battS",(uint8_t*)&statSets,sizeof(statSets)))
    initstatSets();
  if (!readEE("battD",(uint8_t*)&dynSets,sizeof(dynSets)))
    initdynSets();
  if (!readEE("ble",(uint8_t*)&cellBLE,sizeof(cellBLE)))
    cellBLE.numCells = 0;
  BMSInitCom(ConSerData);
  Wire.begin();

  SetAmpinvtVals();

  NimBLEDevice::init("");
  emptyAddress = NimBLEAddress("00:00:00:00:00:00");
Serial.printf("EA: %s\n",emptyAddress.toString().c_str());
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new adCB(), false);
  pBLEScan->setMaxResults(0); // do not store the scan results, use callback only.

  Serial.printf("ncells %d\n",cellBLE.numCells);
  checkMissingCell();
  dynSets.cmd = DynSets;
  statSets.cmd = StatSets;

  for (int i=0;i<C_RELAY_TOTAL;i++)
    pinMode(relayPins[i],OUTPUT);
  clearRelays();
  BMSInitStatus(&st);
  lastShuntMillis = millis();
  setBattAH();

  configTime(0,0,"pool.ntp.org");
  st.stateOfChargeValid = false;
  lastTrip = 0;
  st.doFullChg = true;
  InitCells();
  BMSSend(&statSets); 
  BMSSend(&dynSets); 
  digitalWrite(GREEN_LED,0);
  xMut = xSemaphoreCreateMutex();
  xTaskCreate(BLETask, "BLE task", 4096, NULL, 10, NULL); // priority value?
  CAN.setPins(GPIO_NUM_21, GPIO_NUM_22);
  if (!CAN.begin(1E6)) {
    Serial.println("Starting CAN failed!");
    while (1);
  } else
    Serial.println("Started CAN");
  CAN.onReceive(RecCAN);
}

void loop() {
  if (st.lastMillis > millis()) st.milliRolls++;
  st.lastMillis = millis(); // for uptime to continue after 50 days

  if (writeDynSets) {
    writeEE("battD",(uint8_t*)&dynSets, sizeof(dynSets));
    Serial.println("Write Dyn");
    writeDynSets = false;
  } else if (writeStatSets) {
    writeEE("battS",(uint8_t*)&statSets, sizeof(statSets));
    Serial.println("Write Stat");
    writeStatSets = false;
  } else if (writeCellSet) {
    writeEE("ble",(uint8_t*)&cellBLE,sizeof(cellBLE));
    writeCellSet = false;
  }
  if ((millis() - pvPollMS) > POLLPV) {
    pvPollMS = millis();
  }

  if ((millis() - statusMS) > CHECKSTATUS)
    checkStatus();
  bool AState = digitalRead(INV);
  if (!AmpinvtSt && !AmpinvtStWait && (AmpinvtOn != AState)) {
    AmpinvtSt = millis();
    if (!AmpinvtSt) AmpinvtSt = 1; // 1 in 4billion chance of this happening;
    digitalWrite(AmpinvtRelayPin,HIGH);
  } else if (AmpinvtSt) {
    uint32_t diff = millis() - AmpinvtSt;
    if ((AmpinvtOn && (diff > 100*(uint32_t)AmpinvtRelayPtr->trip)) ||
      (!AmpinvtOn && (diff > 100*(uint32_t)AmpinvtRelayPtr->rec))) {
        digitalWrite(AmpinvtRelayPin,LOW);
        AmpinvtSt = 0;
        AmpinvtStWait = millis();
        if (!AmpinvtStWait) AmpinvtStWait = 1;
      }
  } else if (AmpinvtStWait && (millis() - AmpinvtStWait) > 3000)
    AmpinvtStWait = 0;

  BMSGetSerial();
}
