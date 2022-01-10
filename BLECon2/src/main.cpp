#include <Arduino.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <INA.h>

#include <time.h>
#include <Ticker.h>
#include <BMSADC.h>
#include <BMSCommArd.h>
#include <CellData.h>
#include "defines.h"

// improve server
// adjustable cell history
// cell dump safety, stop if cell gets low, show dump status
// charge control feature to reduce amps
// improve the SoC allow to run logic. Make the code bad, and see if the charge/loads go off

#define AVEADJAVGS 16

int8_t analCnt=0,curAnal=0;
bool cellsOverDue = true,loadsOff = true,chgOff = true
  ,updateINAs=false,writeStatSets=false,writeDynSets=false,writeCellSet=false,missingCell;
uint32_t statusCnt=0,lastHitCnt=0,scanStart=0;
Ticker watchDog;
bool OTAInProg = false;

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

const int relayPins[C_RELAY_TOTAL] = { GPIO_NUM_19,GPIO_NUM_18,GPIO_NUM_2,GPIO_NUM_15,GPIO_NUM_13,GPIO_NUM_14,GPIO_NUM_27,GPIO_NUM_26 };

NimBLEAddress emptyAddress;
DynSetts dynSets;
StatSetts statSets;
BLESettings cellBLE;
BMSStatus st;
char spb[1024];

uint32_t statusMS=0,connectMS=0,shuntPollMS=0,pvPollMS=0;
bool inAlertState = true;

#define MAX_CELLV (statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Trip])
NimBLEScan* pBLEScan;

INA_Class         INA(2);
uint32_t lastShuntMillis;
int64_t milliAmpMillis,battMilliAmpMillis,accumMilliAmpMillis;
int curAdj;
int64_t aveAdjMilliAmpMillis,curAdjMilliAmpMillis[AVEADJAVGS];
int lastTrip;

//char (*__kaboom)[sizeof( BMSStatus )] = 1;

void initState() {
  st.cmd = Status;
  st.lastMillis = 0;
  st.milliRolls = 0;
  st.watchDogHits = 0;
  st.lastPackMilliVolts = 0xffff;
  st.maxDiffMilliVolts = 0;
  st.lastPVMicroAmps=0;
  st.doFullChg = true;
  st.maxCellVState=false;st.minCellVState=false;
  st.maxPackVState=false;st.minPackVState=false;
  st.maxCellCState=false;st.minCellCState=false;
  st.maxPackCState=false;st.minPackCState=false;
  st.maxChargePctState=false;
  st.lastAdjMillAmpHrs = 0;
  st.BatAHMeasured = 0;
}

void getAmps() {
  st.lastMicroAmps = -INA.getBusMicroAmps(0);

  uint32_t thisMillis = millis();
  int64_t deltaMilliAmpMillis = (int64_t)st.lastMicroAmps * (thisMillis - lastShuntMillis) / 1000;
  milliAmpMillis += deltaMilliAmpMillis;
  accumMilliAmpMillis += abs(deltaMilliAmpMillis);

  lastShuntMillis = thisMillis;
  if (milliAmpMillis < 0) {
    curAdjMilliAmpMillis[curAdj] -= milliAmpMillis;
    milliAmpMillis = 0;
  } else if (milliAmpMillis > battMilliAmpMillis) {
    curAdjMilliAmpMillis[curAdj] += battMilliAmpMillis - milliAmpMillis;
    milliAmpMillis = battMilliAmpMillis;
  }
}

bool doShutOffNoStatus(uint32_t t) {
  return ((uint32_t)statSets.CellsOutTime < ((millis() - t)*1000) || !st.stateOfChargeValid || st.stateOfCharge < statSets.CellsOutMin || st.stateOfCharge > statSets.CellsOutMax);
}

void clearRelays() {
  for (int i=0;i<C_RELAY_TOTAL;i++) {
    digitalWrite(relayPins[i], LOW);
    st.previousRelayState[i] = LOW;
  }
}

void doAHCalcs() {
  if (st.adjCnt < AVEADJAVGS)
    st.adjCnt++;
  for (int i=0;i<st.adjCnt;i++)
    aveAdjMilliAmpMillis += curAdjMilliAmpMillis[i];
  aveAdjMilliAmpMillis = st.adjCnt;
  st.lastAdjMillAmpHrs = (int32_t)(curAdjMilliAmpMillis[curAdj] / ((int64_t)1000 * 60 * 60));
  curAdj++;
  if (curAdj == AVEADJAVGS)
    curAdj = 0;
  curAdjMilliAmpMillis[curAdj] = 0;
}

void SendEvent(uint8_t cmd,uint32_t amps=0, uint16_t val=0,int cell=0) {
  EventMsg evt;
  evt.cmd = cmd;
  evt.cell = cell;
  evt.val = val;
  evt.amps = (uint16_t)(amps/1000000);
  BMSSend(&evt);
}

void doWatchDog() {
  if (doShutOffNoStatus(statusMS)) {
    clearRelays();
    SendEvent(WatchDog);
  }
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
    cells[cell].cellSentSet = false;
    SettingMsg ms;
    ms.cmd = ConnCell;
    ms.val = cell;
    BMSSend(&ms);
  }

  void onDisconnect(NimBLEClient* pclient) {
    st.cells[cell].conn = false;
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
    if (doShutOffNoStatus(3000))
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

void checkStatus()
{
  statusCnt++;
  statusMS = millis();

  digitalWrite(RESISTOR_PWR,HIGH);
  if (dynSets.cellSets.delay)
    delay(dynSets.cellSets.delay);
  uint16_t vp;
  st.curBoardTemp = BMSReadTemp(TEMP1,statSets.bdVolts,BCOEF,47000,47000,dynSets.cellSets.cnt,&vp);
  if (!dynSets.cellSets.resPwrOn)
    digitalWrite(RESISTOR_PWR,LOW);
  if ((st.lastMicroAmps > 0 && chgOff) || (st.lastMicroAmps < 0 && loadsOff)) {
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
          ,st.lastMicroAmps,chgOff,loadsOff,(int)st.lastPackMilliVolts,(int)maxCellV,(int)minCellV
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
  if (battMilliAmpMillis != 0)
    st.stateOfCharge = milliAmpMillis * 100 / battMilliAmpMillis;
  st.lastPackMilliVolts = INA.getBusMilliVolts(0);

  bool allovervoltrec = true,allundervoltrec = true,hitTop=false,hitUnder=false;
  bool allovertemprec = true,allundertemprec = true;
  //Loop through cells
  cellsOverDue = false;
  uint16_t totalVolts=0;
  for (int8_t i = 0; i < dynSets.nCells; i++)
  {
    if (!st.cells[i].conn && doShutOffNoStatus(cells[i].cellLast)) {
      clearRelays();
      if (!cellsOverDue)
        SendEvent(CellsDisc,st.lastMicroAmps,0,i);
      cellsOverDue = true;
      break;
    }
    if ((millis() - cells[i].cellLast) > (5*dynSets.cellSets.time)) {
      char buf[256];
      buf[0] = 0;
      uint32_t ct = millis();
      for (int j=0;j<dynSets.nCells;j++)
        snprintf(buf,sizeof(buf),"%s #%d %u\n",buf,j,ct - cells[j].cellLast);
      BMSSend(buf);
    }
      
    uint16_t cellV = st.cells[i].volts;
    totalVolts += cellV;

    if (cellV > statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Trip]) {
      if (!st.maxCellVState) {
        if (!hitTop)
          SendEvent(CellTopV,st.lastMicroAmps,cellV,i);
        hitTop = true;
      }
      st.maxCellVState = true;
    }
    if (cellV > statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Rec])
      allovervoltrec = false;

    if (cellV < statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Min][LimitConsts::Trip]) {
      if (!st.minCellVState) {
        if (!hitUnder)
          SendEvent(CellBotV,st.lastMicroAmps,cellV,i);
        hitUnder = true;
      }
      st.minCellVState = true;
    }
    if (cellV < statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Min][LimitConsts::Rec])
      allundervoltrec = false;

    int8_t cellT = st.cells[i].exTemp;
    if (statSets.useCellC && cellT != -40) {
      if (cellT > statSets.limits[LimitConsts::Temp][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Trip]) {
        if (!st.maxCellCState)
          SendEvent(CellTopT,st.lastMicroAmps,cellT,i);
        st.maxCellCState = true;
      }

      if (cellT > statSets.limits[LimitConsts::Temp][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Rec])
        allovertemprec = false;

      if (cellT < statSets.limits[LimitConsts::Temp][LimitConsts::Cell][LimitConsts::Min][LimitConsts::Trip]) {
        if (!st.minCellCState)
          SendEvent(CellBotT,st.lastMicroAmps,cellT,i);
        st.minCellCState = true;
      }
      if (cellT < statSets.limits[LimitConsts::Temp][LimitConsts::Cell][LimitConsts::Min][LimitConsts::Rec])
        allundertemprec = false;
    }
  }
  uint16_t diffVolts = totalVolts > st.lastPackMilliVolts? totalVolts - st.lastPackMilliVolts: st.lastPackMilliVolts - totalVolts;
  if (diffVolts > st.maxDiffMilliVolts)
    st.maxDiffMilliVolts = diffVolts;
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
  if (st.lastPackMilliVolts > statSets.limits[LimitConsts::Volt][LimitConsts::Pack][LimitConsts::Max][LimitConsts::Trip]) {
    if (!st.maxPackVState) {
      if (!hitTop)
        SendEvent(PackTopV,st.lastMicroAmps,st.lastPackMilliVolts);
      hitTop = true;
    }
    st.maxPackVState = true;
  } else if (st.lastPackMilliVolts < statSets.limits[LimitConsts::Volt][LimitConsts::Pack][LimitConsts::Max][LimitConsts::Rec])
    st.maxPackVState = false;

  if (st.lastPackMilliVolts < statSets.limits[LimitConsts::Volt][LimitConsts::Pack][LimitConsts::Min][LimitConsts::Trip]) {
    if (!st.minPackVState) {
      if (!hitUnder)
        SendEvent(PackBotV,st.lastMicroAmps,st.lastPackMilliVolts);
      hitUnder = true;
    }
    st.minPackVState = true;
  } else if (st.lastPackMilliVolts > statSets.limits[LimitConsts::Volt][LimitConsts::Pack][LimitConsts::Min][LimitConsts::Rec])
    st.minPackVState = false;

  if (hitTop || hitUnder) {
    if (!lastHitCnt)
      lastHitCnt=statusCnt;
    else lastHitCnt++;
  } else
    lastHitCnt = 0;
  hitTop = hitTop && (int32_t)dynSets.TopAmps > (st.lastMicroAmps/1000000) && (statusCnt == lastHitCnt); // We don't want to trigger a hittop 
  hitUnder = hitUnder && (int32_t)-dynSets.TopAmps < (st.lastMicroAmps/1000000) && (statusCnt == lastHitCnt);
  if (hitTop) {
    if (lastTrip != 0) {
      if (milliAmpMillis < battMilliAmpMillis)
        curAdjMilliAmpMillis[curAdj] += battMilliAmpMillis - milliAmpMillis;
      if (lastTrip < 0)
        st.BatAHMeasured = (milliAmpMillis + curAdjMilliAmpMillis[curAdj]) / ((uint64_t)1000 * 1000 * 60 * 60);
      doAHCalcs();
    }
    lastTrip = 1;
    milliAmpMillis = battMilliAmpMillis;
    st.stateOfChargeValid = true;
    st.doFullChg = false;
  }
  if (hitUnder) {
    if (lastTrip != 0) {
      if (milliAmpMillis > 0)
        curAdjMilliAmpMillis[curAdj] -= milliAmpMillis;
      if (lastTrip > 0)
        st.BatAHMeasured = (battMilliAmpMillis - milliAmpMillis + curAdjMilliAmpMillis[curAdj]) / ((uint64_t)1000 * 1000 * 60 * 60);
      doAHCalcs();
    }
    lastTrip = -1;
    milliAmpMillis = 0;
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
  loadsOff = st.minCellVState || st.minPackVState || st.maxCellCState || st.maxPackCState;
  chgOff = st.maxChargePctState || st.maxCellVState || st.maxPackVState || st.minCellCState || st.maxCellCState || st.minPackCState || st.maxPackCState;
  for (int8_t y = 0; y < C_RELAY_TOTAL; y++)
  {
    RelaySettings *rp = &statSets.relays[y];
    if (rp->off)
      relay[y] = LOW;
    else {
      relay[y] = st.previousRelayState[y]; // don't change it because we might be in the SOC trip/rec area
      switch (rp->type) {
        default: case Relay_Connect: relay[y] = cellsOverDue || (wasLoadsOff && loadsOff && st.lastMicroAmps < 0) || (wasChgOff && chgOff && st.lastMicroAmps > 0)?LOW:HIGH; break;
        case Relay_Load:
          if (isFromOff(rp))
            relay[y] = HIGH;
          else if (loadsOff || cellsOverDue || (rp->doSoC && (!st.stateOfChargeValid || st.stateOfCharge < rp->trip)))
            relay[y] = LOW; // turn if off
          else if (!rp->doSoC || (rp->doSoC && st.stateOfChargeValid && st.stateOfCharge > rp->rec))
            relay[y] = HIGH; // turn it on
          // else leave it as-is
          break;
        case Relay_Charge:
          if (chgOff || cellsOverDue || (rp->doSoC && (!st.stateOfChargeValid || st.stateOfCharge > rp->trip)))
            relay[y] = LOW; // off
          else if (!rp->doSoC || (rp->doSoC && st.stateOfChargeValid && st.stateOfCharge < rp->rec))
            relay[y] = HIGH; // on
          // else leave it as-is
          break;
        case Relay_Therm:
          uint8_t val=255;
          switch (rp->therm) {
            case 'b': val = st.curBoardTemp;
              break;
            case 'c':
              for (int i=0;i<dynSets.nCells;i++)
                if (st.cells[i].exTemp < val && st.cells[i].conn)
                  val = st.cells[i].exTemp;
              break;
          }
          if (val < rp->trip)
            relay[y] = HIGH;
          else if (val > rp->rec)
            relay[y] = LOW;
          break;
      }
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
  if (!cellsOverDue) {
    st.watchDogHits = 0;
    watchDog.once_ms(CHECKSTATUS+WATCHDOGSLOP,doWatchDog);
  }
  BMSSend(&st);
}

void setINAs() {
  INA.begin(dynSets.MaxAmps, dynSets.ShuntUOhms,0);
  INA.begin(dynSets.PVMaxAmps,dynSets.PVShuntUOhms,1);
  INA.setShuntConversion(dynSets.ConvTime);
  INA.setBusConversion(dynSets.ConvTime);
  INA.setAveraging(dynSets.Avg);
//  INA.setShuntConversion(dynSets.PVConvTime,1);
//  INA.setBusConversion(dynSets.PVConvTime,1);
//  INA.setAveraging(dynSets.PVAvg,1);
Serial.printf("%d %d %d %d\n",dynSets.ConvTime,dynSets.Avg,dynSets.PVConvTime,dynSets.PVAvg);
}

void setStateOfCharge(int64_t val,bool valid) {
  milliAmpMillis = val;
  st.stateOfChargeValid = valid;
  curAdj = 0;
  st.adjCnt = 0;
  curAdjMilliAmpMillis[0] = 0;
  lastTrip = 0;
  aveAdjMilliAmpMillis = 0;
  st.doFullChg = true;
}

void setBattAH() {
  battMilliAmpMillis = (uint64_t)dynSets.BattAH * (1000 * 60 * 60) * 1000; // convert to milliampmilliseconds
}

void initdynSets() {
  dynSets.ConvTime = 1000;
  dynSets.PVMaxAmps = 100;
  dynSets.PVShuntUOhms = 500;
  dynSets.ShuntUOhms = 167;
  dynSets.MaxAmps = 300;
  dynSets.nCells=0;
  dynSets.PollFreq = 500;
  dynSets.BattAH = 1;
  dynSets.ConvTime = 1000;
  dynSets.Avg = 1000;
  dynSets.PVConvTime = 1000;
  dynSets.PVAvg = 1000;
  dynSets.savedTime = 0;
  dynSets.milliAmpMillis = 0;
  dynSets.cellSets.cnt = 4;
  dynSets.cellSets.delay = 0;
  dynSets.cellSets.resPwrOn = false;
  dynSets.cellSets.time = 1000; // this will be like 2 secs, because cell goes to sleep and slows CPU by 2x
  dynSets.TopAmps = 6;
}
void initstatSets() {
  statSets.useBoardTemp = true;
  statSets.useCellC = true;
  statSets.bdVolts = 3300;
  statSets.ChargePct = 100;
  statSets.ChargePctRec = 0;
  statSets.ChargeRate = 0;
  statSets.CellsOutMax = 80;
  statSets.CellsOutMin = 30;
  statSets.CellsOutTime = 120;
  statSets.FloatV = 3400;
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
      setStateOfCharge((battMilliAmpMillis * val)/100,true);
      break;
    case SetPollFreq:
      dynSets.PollFreq = val;
      if (dynSets.PollFreq < 500)
        dynSets.PollFreq = 500;
      break;
    case SetTopAmps:
      dynSets.TopAmps = val;
      break;
    case SetBattAH:
      dynSets.BattAH = val;
      setBattAH();
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
    case SetMaxAmps: dynSets.MaxAmps = val; updateINAs = true; break;
    case SetShuntUOhms: dynSets.ShuntUOhms = val; updateINAs = true; break;
    case SetPVMaxAmps: dynSets.PVMaxAmps = val; updateINAs = true; break;
    case SetPVShuntUOhms: dynSets.PVShuntUOhms = val; updateINAs = true; break;
    case SetAvg: dynSets.Avg = val; updateINAs = true; break;
    case SetConvTime: dynSets.ConvTime = val; updateINAs = true; break;
    case SetPVAvg: dynSets.PVAvg = val; updateINAs = true; break;
    case SetPVConvTime: dynSets.PVConvTime = val; updateINAs = true; break;
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

void JTPrint(const std::string &ad) {
  Serial.printf("FFF: %s %d\n",ad.c_str(),ad.length());
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
    case ClrMaxDiff: st.maxDiffMilliVolts = 0; break;
    case StatQuery: BMSSend(&statSets); break;
    case DynQuery: BMSSend(&dynSets); break;
    case StatSets: statSets = *(StatSetts*)mp; writeStatSets=true; break;
  }
}

void BLETask(void *arg) {
  for (;;) {
    ConnectCells();
    CheckBLEScan();
    delay(CHECKCONNECT);
  }
}

void setup() {
  Serial.begin(9600);
  BMSADCInit();
  adc1_config_channel_atten(TEMP1, ADC_ATTEN_DB_11);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RESISTOR_PWR, OUTPUT);
  digitalWrite(GREEN_LED,1);
  BMSInitCom(ConSerData);
  Wire.begin();
  for (int i=0;i<MAX_CELLS;i++) cells[i].pClient = NULL;

  NimBLEDevice::init("");
  emptyAddress = NimBLEAddress("00:00:00:00:00:00");
  Serial.printf("EA: %s\n",emptyAddress.toString().c_str());
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new adCB(), false);
  pBLEScan->setMaxResults(0); // do not store the scan results, use callback only.

  if (!readEE("battS",(uint8_t*)&statSets,sizeof(statSets)))
    initstatSets();

  if (!readEE("battD",(uint8_t*)&dynSets,sizeof(dynSets)))
    initdynSets();

  if (!readEE("ble",(uint8_t*)&cellBLE,sizeof(cellBLE)))
    cellBLE.numCells = 0;

  Serial.printf("ncells %d\n",cellBLE.numCells);
  checkMissingCell();
  dynSets.cmd = DynSets;
  statSets.cmd = StatSets;

  for (int i=0;i<C_RELAY_TOTAL;i++)
    pinMode(relayPins[i],OUTPUT);
  clearRelays();
  initState();
  INA.begin(dynSets.MaxAmps, dynSets.ShuntUOhms);
  Serial.printf("INA D: %s %s\n",INA.getDeviceName(0),
            INA.getDeviceName(1));
  lastShuntMillis = millis();
  setBattAH();
  setINAs();

  configTime(0,0,"pool.ntp.org");
  setStateOfCharge((battMilliAmpMillis * 4)/5,false);
  InitCells();
  BMSSend(&statSets); 
  BMSSend(&dynSets); 
  digitalWrite(GREEN_LED,0);
  xMut = xSemaphoreCreateMutex();
  xTaskCreate(BLETask, "BLE task", 4096, NULL, 10, NULL); // priority value?
}

time_t saveTimeDiff = 0;
void loop() {
  if (st.lastMillis > millis()) st.milliRolls++;
  st.lastMillis = millis(); // for uptime to continue after 50 days

  if (dynSets.savedTime) {
    time_t now = time(nullptr);
    if ((now - dynSets.savedTime) < 60) {
      setStateOfCharge(dynSets.milliAmpMillis,true);
      dynSets.savedTime = 0;
      dynSets.milliAmpMillis = 0;
      writeDynSets = true;
    } else if (saveTimeDiff && saveTimeDiff < (now - dynSets.savedTime)) {
      // difference is growing so give up
      dynSets.savedTime = 0;
      dynSets.milliAmpMillis = 0;
      writeDynSets = true;
    } else saveTimeDiff = now - dynSets.savedTime;
  }
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
  if (updateINAs)
    setINAs();
  updateINAs = false;
  if ((millis() - shuntPollMS) > dynSets.PollFreq) {
    getAmps();
    shuntPollMS = millis();
  }
  if ((millis() - pvPollMS) > POLLPV) {
    st.lastPVMicroAmps = INA.getBusMicroAmps(1);
    pvPollMS = millis();
  }

  if ((millis() - statusMS) > CHECKSTATUS)
    checkStatus();

  BMSGetSerial();
}
