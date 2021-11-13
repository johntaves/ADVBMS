#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include <NimBLEDevice.h>
#include "INA.h"
#include "BMSADC.h"
#include "BMSComm.h"
#include "BMSAll.h"

// improve server
// adjustable cell history
// cell dump safety, stop if cell gets low, show dump status
// charge control feature to reduce amps
// improve the SoC allow to run logic. Make the code bad, and see if the charge/loads go off

#define AVEADJAVGS 16
#define GREEN_LED GPIO_NUM_23
#define RESISTOR_PWR GPIO_NUM_32
#define TEMP1 ADC1_CHANNEL_7
#define BCOEF 4050
// shut everything off if status has not happened within 2 secs of when it should
#define WATCHDOGSLOP 2000
// attempt to connect to cells
#define CHECKCONNECT 2000

struct BLESettings {
  int numCells;
  NimBLEAddress addrs[MAX_CELLS];
};

std::shared_ptr<nvs::NVSHandle> nvsHandle;
int8_t analCnt=0,curAnal=0;
bool cellsOverDue = true,loadsOff = true,chgOff = true;
uint32_t lastSentMillis=0,statusCnt=0,lastHitCnt=0,scanStart=0;
time_t saveTimeDiff = 0;
uint16_t resPwrMS=0;
bool OTAInProg = false;

BLEClient* pClients[MAX_CELLS];
NimBLERemoteCharacteristic* pSettings[MAX_CELLS];
uint32_t cellDumpSecs[MAX_CELLS];
uint32_t cellLasts[MAX_CELLS];
bool cellSentSet[MAX_CELLS];

const gpio_num_t relayPins[C_RELAY_TOTAL] = { GPIO_NUM_19,GPIO_NUM_18,GPIO_NUM_2,GPIO_NUM_15,GPIO_NUM_13,GPIO_NUM_14,GPIO_NUM_27,GPIO_NUM_26 };

DynSetts dynSets;
StatSetts statSets;
BLESettings cellBLE;
BMSStatus st;
char spb[1024];

uint32_t statusMS=0,connectMS=0,pvPollMS=0;
bool inAlertState = true;

#define MAX_CELLV (statSets.limits[LimitConsts::Volt][LimitConsts::Cell][LimitConsts::Max][LimitConsts::Trip])
NimBLEScan* pBLEScan;

INA_Class         INA;
int INADevs;
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

bool doShutOffNoStatus(uint32_t t) {
  return ((uint32_t)statSets.CellsOutTime < ((millis() - t)*1000) || !st.stateOfChargeValid || st.stateOfCharge < statSets.CellsOutMin || st.stateOfCharge > statSets.CellsOutMax);
}

void clearRelays() {
  for (int i=0;i<C_RELAY_TOTAL;i++) {
    gpio_set_level(relayPins[i], LOW);
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
  NimBLEClient*  pC = pClients[i];
  if (!pC || !pC->isConnected())
    return;
  if (!pSettings[i]) {
    printf("Badness here\n");
  }
  pSettings[i]->writeValue<CellSettings>(dynSets.cellSets,false);
  cellSentSet[i] = true;
}

class MyClientCallback : public NimBLEClientCallbacks {
  int cell;
  public:
  MyClientCallback(int cell) {
    this->cell = cell;
  }
  void onConnect(NimBLEClient* pclient) {
    st.cells[cell].conn = true;
    cellSentSet[cell] = false;
    printf("Con\n");
  }

  void onDisconnect(NimBLEClient* pclient) {
    st.cells[cell].conn = false;
    printf("Disc\n");
  }
};

void ConnectCell(int i) {
  NimBLEClient*  pC = pClients[i];
  if (pC->isConnected()) {
    if (!cellSentSet[i])
      sendCellSet(i);
    return;
  }
  // Connect to the remove BLE Server.
  pC->setConnectTimeout(2);
  if (!pC->connect(BLEAddress(cellBLE.addrs[i]),true)) {
    printf("Failed to connect: %d, %s\n",i,((std::string)cellBLE.addrs[i]).c_str());
    return;
  }

  // Obtain a reference to the service we are after in the remote BLE server.
  BLERemoteService* pServ = pC->getService(NimBLEUUID((uint16_t)0x180F));
  if (pServ == nullptr) {
    printf("Failed to find our service UUID: %d\n",i);
    pC->disconnect();
    return;
  }
  NimBLERemoteCharacteristic* pChar = pServ->getCharacteristic(NimBLEUUID((uint16_t)0x2B18)); // status
  if (pChar == nullptr) {
    pC->disconnect();
    return;
  }
  pChar->subscribe(true,[i](NimBLERemoteCharacteristic* pBLERemoteCharacteristic,
                              uint8_t* pData, size_t length, bool isNotify) {
          CellStatus* cd = (CellStatus*)pData;
          st.cells[i].volts = cd->volts;
          st.cells[i].exTemp = cd->tempExt;
          st.cells[i].bdTemp = cd->tempBd;
          st.cells[i].dumping = cd->drainSecs > 0;
          cellLasts[i] = millis();
        },false);
  pSettings[i] = pServ->getCharacteristic(NimBLEUUID((uint16_t)0x2B15)); // settings
  sendCellSet(i);
}

void sendCellSets() {
  for (int i=0;i<cellBLE.numCells;i++)
    sendCellSet(i);
}

#define IntervalToms(x) (x * 1000 / BLE_HCI_CONN_ITVL)

void InitCell(int i) {
  NimBLEClient*  pC  = NimBLEDevice::createClient();

  pC->setClientCallbacks(new MyClientCallback(i),true);
  pC->setConnectionParams(IntervalToms(100),IntervalToms(100),25,600);
  pClients[i] = pC;
  st.cells[i].dumping = false;
  cellDumpSecs[i] = 0;
}
void InitCells() {
  for (int i=0;i<cellBLE.numCells;i++) {
    InitCell(i);
    ConnectCell(i);
  }
}

void ScanCB(NimBLEScanResults res) {

}
void CheckBLEScan() {
  if (dynSets.nCells > cellBLE.numCells && !scanStart) {
    printf("Starting scan");
    if (doShutOffNoStatus(3000))
      clearRelays();
    pBLEScan->start(0,ScanCB,false);
    scanStart = millis();
  } else if (cellBLE.numCells == dynSets.nCells && scanStart) {
    pBLEScan->stop(); // need to stop scanning to connect
    scanStart = 0;
    printf("Stop scan\n");
  }
}

void writeCellBLE() {
  assert(nvsHandle->set_item("numCells",cellBLE.numCells) == ESP_OK);
  for (int i=0;i<cellBLE.numCells;i++) {
    char name[10];
    snprintf(name,sizeof(name),"MAC:%d",i);
    assert(nvsHandle->set_string(name,cellBLE.addrs->toString().c_str()) == ESP_OK);
  }
}

void initCellBLE() {
  if (nvsHandle->get_item("numCells",cellBLE.numCells) == ESP_ERR_NVS_NOT_FOUND)
    cellBLE.numCells = 0;
  for (int i=0;i<cellBLE.numCells;i++) {
    char name[10],val[20];
    snprintf(name,sizeof(name),"MAC:%d",i);
    assert(nvsHandle->get_string(name,val,sizeof(val)) == ESP_OK);
    cellBLE.addrs[i] = NimBLEAddress(val);
  }
  writeCellBLE();
}

class adCB: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* ad) {
      if (!ad->getName().compare("LiFePo4 Cell")
              && ad->haveServiceUUID()
              && ad->isAdvertisingService(NimBLEUUID((uint16_t)0x180F))) {

        int i=0;
        for (;i<cellBLE.numCells && cellBLE.addrs[i] != ad->getAddress();i++) ;
        printf("Found %d %d\n",cellBLE.numCells,i);
        if (i==cellBLE.numCells && cellBLE.numCells < dynSets.nCells) {
          cellBLE.numCells++;
          cellBLE.addrs[i] = ad->getAddress();
          printf("Add %d: %s\n",i,cellBLE.addrs[i].toString().c_str());
          InitCell(i);
          writeCellBLE();
        }
        if (cellBLE.numCells == dynSets.nCells) {
          pBLEScan->stop(); // need to stop scanning to connect
          scanStart = 0;
          printf("Stop scan\n");
        }
      }
    }
};

void checkStatus()
{
  statusCnt++;
  st.lastPVMicroAmps = INA.getBusMicroAmps(1);
  CheckBLEScan();
  if (cellBLE.numCells != dynSets.nCells)
    return;
  gpio_set_level(RESISTOR_PWR,HIGH);
  if (dynSets.cellSets.delay)
    vTaskDelay(pdMS_TO_TICKS(dynSets.cellSets.delay));
  st.curTemp1 = BMSReadTemp(TEMP1,statSets.bdVolts,BCOEF,47000,47000,dynSets.cellSets.cnt);
  if (!dynSets.cellSets.resPwrOn)
    gpio_set_level(RESISTOR_PWR,LOW);

  if (INADevs > 0) {
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
  }

  if (battMilliAmpMillis != 0)
    st.stateOfCharge = milliAmpMillis * 100 / battMilliAmpMillis;
  st.lastPackMilliVolts = INA.getBusMilliVolts(0);
  if (st.lastPackMilliVolts < 1000) { // low side shunt
    st.lastPackMilliVolts = 0;
    for (int8_t i = 0; i < dynSets.nCells; i++)
      st.lastPackMilliVolts += st.cells[i].volts;
  }

  bool allovervoltrec = true,allundervoltrec = true,hitTop=false,hitUnder=false;
  bool allovertemprec = true,allundertemprec = true;
  //Loop through cells
  cellsOverDue = false;
  for (int8_t i = 0; i < dynSets.nCells; i++)
  {
    if (!st.cells[i].conn && doShutOffNoStatus(lastSentMillis)) {
      clearRelays();
      if (!cellsOverDue)
        SendEvent(CellsOverDue);
      cellsOverDue = true;
      break;
    }
    uint16_t cellV = st.cells[i].volts;

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
  if (st.maxCellVState && allovervoltrec)
    st.maxCellVState = false;
  if (st.minCellVState && allundervoltrec)
    st.minCellVState = false;
  if (!statSets.useCellC || (st.maxCellCState && allovertemprec))
    st.maxCellCState = false;
  if (!statSets.useCellC || (st.minCellCState && allundertemprec))
    st.minCellCState = false;

  if (statSets.useTemp1) {
    if (st.curTemp1 > statSets.limits[LimitConsts::Temp][LimitConsts::Pack][LimitConsts::Max][LimitConsts::Trip])
      st.maxPackCState = true;
    if (st.curTemp1 < statSets.limits[LimitConsts::Temp][LimitConsts::Pack][LimitConsts::Max][LimitConsts::Rec])
      st.maxPackCState = false;
    if (st.curTemp1 < statSets.limits[LimitConsts::Temp][LimitConsts::Pack][LimitConsts::Min][LimitConsts::Trip])
      st.minPackCState = true;
    if (st.curTemp1 > statSets.limits[LimitConsts::Temp][LimitConsts::Pack][LimitConsts::Min][LimitConsts::Rec])
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
      }
    }
  }
  for (int8_t n = 0; n < C_RELAY_TOTAL; n++)
  {
    if (st.previousRelayState[n] != relay[n])
    {
      gpio_set_level(relayPins[n], relay[n]);
      st.previousRelayState[n] = relay[n];
    }
  }
  // balance calcs
  if (hitTop) {
    
  }
  if (!cellsOverDue) {
    st.watchDogHits = 0;
//    watchDog.once_ms(CHECKSTATUS+WATCHDOGSLOP,doWatchDog);
  }
  BMSSend(&st);
}

void setINAs() {
  INA.begin(dynSets.MaxAmps, dynSets.ShuntUOhms,0);
  INA.begin(dynSets.PVMaxAmps,dynSets.PVShuntUOhms,1);
  INA.setShuntConversion(300,0);
  INA.setBusConversion(300,0);
  INA.setAveraging(10000,0);
  INA.setShuntConversion(dynSets.ConvTime,1);
  INA.setBusConversion(dynSets.ConvTime,1);
  INA.setAveraging(dynSets.Avg,1);
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

void writeDynSets() {
  assert(nvsHandle->set_item("ConvTime",dynSets.ConvTime) == ESP_OK);
  assert(nvsHandle->set_item("PVMaxAmps",dynSets.PVMaxAmps) == ESP_OK);
  assert(nvsHandle->set_item("PVShuntUOhms",dynSets.PVShuntUOhms) == ESP_OK);
  assert(nvsHandle->set_item("ShuntUOhms",dynSets.ShuntUOhms) == ESP_OK);
  assert(nvsHandle->set_item("MaxAmps",dynSets.MaxAmps) == ESP_OK);
  assert(nvsHandle->set_item("nCells",dynSets.nCells) == ESP_OK);
  assert(nvsHandle->set_item("PollFreq",dynSets.PollFreq) == ESP_OK);
  assert(nvsHandle->set_item("BattAH",dynSets.BattAH) == ESP_OK);
  assert(nvsHandle->set_item("ConvTime",dynSets.ConvTime) == ESP_OK);
  assert(nvsHandle->set_item("Avg",dynSets.Avg) == ESP_OK);
  assert(nvsHandle->set_item("savedTime",dynSets.savedTime) == ESP_OK);
  assert(nvsHandle->set_item("milliAmpMillis",dynSets.milliAmpMillis) == ESP_OK);
  assert(nvsHandle->set_item("cellSets.cnt",dynSets.cellSets.cnt) == ESP_OK);
  assert(nvsHandle->set_item("cellSets.delay",dynSets.cellSets.delay) == ESP_OK);
  assert(nvsHandle->set_item("cellSets.resPwrOn",dynSets.cellSets.resPwrOn) == ESP_OK);
  assert(nvsHandle->set_item("cellSets.time",dynSets.cellSets.time) == ESP_OK);
  assert(nvsHandle->set_item("TopAmps",dynSets.TopAmps) == ESP_OK);
}

void initDynSets() {
  if (nvsHandle->get_item("ConvTime",dynSets.ConvTime) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.ConvTime = 1000;
  if (nvsHandle->get_item("PVMaxAmps",dynSets.PVMaxAmps) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.PVMaxAmps = 100;
  if (nvsHandle->get_item("PVShuntUOhms",dynSets.PVShuntUOhms) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.PVShuntUOhms = 500;
  if (nvsHandle->get_item("ShuntUOhms",dynSets.ShuntUOhms) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.ShuntUOhms = 167;
  if (nvsHandle->get_item("MaxAmps",dynSets.MaxAmps) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.MaxAmps = 300;
  if (nvsHandle->get_item("nCells",dynSets.nCells) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.nCells=0;
  if (nvsHandle->get_item("PollFreq",dynSets.PollFreq) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.PollFreq = 500;
  if (nvsHandle->get_item("BattAH",dynSets.BattAH) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.BattAH = 1;
  if (nvsHandle->get_item("ConvTime",dynSets.ConvTime) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.ConvTime = 1000;
  if (nvsHandle->get_item("Avg",dynSets.Avg) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.Avg = 1000;
  if (nvsHandle->get_item("savedTime",dynSets.savedTime) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.savedTime = 0;
  if (nvsHandle->get_item("milliAmpMillis",dynSets.milliAmpMillis) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.milliAmpMillis = 0;
  if (nvsHandle->get_item("cellSets.cnt",dynSets.cellSets.cnt) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.cellSets.cnt = 4;
  if (nvsHandle->get_item("cellSets.delay",dynSets.cellSets.delay) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.cellSets.delay = 0;
  if (nvsHandle->get_item("cellSets.resPwrOn",dynSets.cellSets.resPwrOn) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.cellSets.resPwrOn = false;
  if (nvsHandle->get_item("cellSets.time",dynSets.cellSets.time) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.cellSets.time = 2000;
  if (nvsHandle->get_item("TopAmps",dynSets.TopAmps) == ESP_ERR_NVS_NOT_FOUND)
    dynSets.TopAmps = 6;
  writeDynSets();
}

void writeLim(int i,int j,int k,int l)
{
  char name[40];
  snprintf(name,sizeof(name),"L%d%d%d%d",i,j,k,l);
  assert(nvsHandle->set_item(name,statSets.limits[i][j][k][l]) == ESP_OK);
}

void initLim(int i,int j,int k,int l,uint16_t val)
{
  char name[40];
  snprintf(name,sizeof(name),"L%d%d%d%d",i,j,k,l);
  if (nvsHandle->get_item(name,statSets.limits[i][j][k][l]) == ESP_ERR_NVS_NOT_FOUND)
    statSets.limits[i][j][k][l] = val;
}

void writeStatSets() {
  assert(nvsHandle->set_item("useTemp1",statSets.useTemp1) == ESP_OK);
  assert(nvsHandle->set_item("useCellC",statSets.useCellC) == ESP_OK);
  assert(nvsHandle->set_item("bdVolts",statSets.bdVolts) == ESP_OK);
  assert(nvsHandle->set_item("ChargePct",statSets.ChargePct) == ESP_OK);
  assert(nvsHandle->set_item("ChargePctRec",statSets.ChargePctRec) == ESP_OK);
  assert(nvsHandle->set_item("ChargeRate",statSets.ChargeRate) == ESP_OK);
  assert(nvsHandle->set_item("CellsOutMax",statSets.CellsOutMax) == ESP_OK);
  assert(nvsHandle->set_item("CellsOutMin",statSets.CellsOutMin) == ESP_OK);
  assert(nvsHandle->set_item("CellsOutTime",statSets.CellsOutTime) == ESP_OK);
  assert(nvsHandle->set_item("FloatV",statSets.FloatV) == ESP_OK);
  WriteRelays(nvsHandle,&statSets.relays[0],C_RELAY_TOTAL);
  for (int i=LimitConsts::Cell;i<LimitConsts::Max1;i++) {
    writeLim(LimitConsts::Volt,i,LimitConsts::Max,LimitConsts::Trip);
    writeLim(LimitConsts::Volt,i,LimitConsts::Max,LimitConsts::Rec);
    writeLim(LimitConsts::Volt,i,LimitConsts::Min,LimitConsts::Trip);
    writeLim(LimitConsts::Volt,i,LimitConsts::Min,LimitConsts::Rec);
    writeLim(LimitConsts::Temp,i,LimitConsts::Max,LimitConsts::Trip);
    writeLim(LimitConsts::Temp,i,LimitConsts::Max,LimitConsts::Rec);
    writeLim(LimitConsts::Temp,i,LimitConsts::Min,LimitConsts::Trip);
    writeLim(LimitConsts::Temp,i,LimitConsts::Min,LimitConsts::Rec);
  }
  assert(nvsHandle->commit() == ESP_OK);
}
void initStatSets() {
  if (nvsHandle->get_item("useTemp1",statSets.useTemp1) == ESP_ERR_NVS_NOT_FOUND)
    statSets.useTemp1 = true;
  if (nvsHandle->get_item("useCellC",statSets.useCellC) == ESP_ERR_NVS_NOT_FOUND)
    statSets.useCellC = true;
  if (nvsHandle->get_item("bdVolts",statSets.bdVolts) == ESP_ERR_NVS_NOT_FOUND)
    statSets.bdVolts = 3300;
  if (nvsHandle->get_item("ChargePct",statSets.ChargePct) == ESP_ERR_NVS_NOT_FOUND)
    statSets.ChargePct = 100;
  if (nvsHandle->get_item("ChargePctRec",statSets.ChargePctRec) == ESP_ERR_NVS_NOT_FOUND)
    statSets.ChargePctRec = 0;
  if (nvsHandle->get_item("ChargeRate",statSets.ChargeRate) == ESP_ERR_NVS_NOT_FOUND)
    statSets.ChargeRate = 0;
  if (nvsHandle->get_item("CellsOutMax",statSets.CellsOutMax) == ESP_ERR_NVS_NOT_FOUND)
    statSets.CellsOutMax = 80;
  if (nvsHandle->get_item("CellsOutMin",statSets.CellsOutMin) == ESP_ERR_NVS_NOT_FOUND)
    statSets.CellsOutMin = 30;
  if (nvsHandle->get_item("CellsOutTime",statSets.CellsOutTime) == ESP_ERR_NVS_NOT_FOUND)
    statSets.CellsOutTime = 120;
  if (nvsHandle->get_item("FloatV",statSets.FloatV) == ESP_ERR_NVS_NOT_FOUND)
    statSets.FloatV = 3400;
  InitRelays(nvsHandle,&statSets.relays[0],C_RELAY_TOTAL);
  for (int i=LimitConsts::Cell;i<LimitConsts::Max1;i++) {
    int mul = !i?1:8;
    initLim(LimitConsts::Volt,i,LimitConsts::Max,LimitConsts::Trip,3500 * mul);
    initLim(LimitConsts::Volt,i,LimitConsts::Max,LimitConsts::Rec,3400 * mul);
    initLim(LimitConsts::Volt,i,LimitConsts::Min,LimitConsts::Trip,3100 * mul);
    initLim(LimitConsts::Volt,i,LimitConsts::Min,LimitConsts::Rec,3200 * mul);
    initLim(LimitConsts::Temp,i,LimitConsts::Max,LimitConsts::Trip,50);
    initLim(LimitConsts::Temp,i,LimitConsts::Max,LimitConsts::Rec,45);
    initLim(LimitConsts::Temp,i,LimitConsts::Min,LimitConsts::Trip,4);
    initLim(LimitConsts::Temp,i,LimitConsts::Min,LimitConsts::Rec,7);
  }
  writeStatSets();
  assert(nvsHandle->commit() == ESP_OK);
}

void DoSetting(uint8_t cmd,uint16_t val) {
  bool updateINAs=false;
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
      printf("NCells: %d\n",dynSets.nCells);
      if (dynSets.nCells < cellBLE.numCells) {
        for (int i=dynSets.nCells;i<cellBLE.numCells;i++) {
          printf("Deleting %d\n",i);
          NimBLEDevice::deleteClient(pClients[i]);
          cellBLE.addrs[i] = NimBLEAddress();
        }
        cellBLE.numCells = dynSets.nCells;
        writeCellBLE();
      } else if (dynSets.nCells > cellBLE.numCells) {
        for (int i=cellBLE.numCells;i<dynSets.nCells;i++)
          cellBLE.addrs[i] = NimBLEAddress();
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
  }
  if (updateINAs)
    setINAs();
  writeDynSets();
}

void DoDump(DumpMsg *dm) {
  printf("Doing dump\n");
    if (cellDumpSecs[dm->cell])
      cellDumpSecs[dm->cell] = 0;
    else cellDumpSecs[dm->cell] = dm->secs;
    BLERemoteService* pServ = pClients[dm->cell]->getService(NimBLEUUID((uint16_t)0x180F));
    if (pServ) {
      NimBLERemoteCharacteristic* pChar = pServ->getCharacteristic(NimBLEUUID((uint16_t)0X2AE2));
      if (pChar) {
        pChar->writeValue<uint32_t>(cellDumpSecs[dm->cell]);
        printf("Write %d %d\n",dm->cell,cellDumpSecs[dm->cell]);
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
        writeDynSets();
      }
      break;
    case DumpCell: DoDump((DumpMsg*)mp); break;
    case FullChg: st.doFullChg = !st.doFullChg; break;
    case ClrMaxDiff: st.maxDiffMilliVolts = 0; break;
    case StatQuery: BMSSend(&statSets); break;
    case DynQuery: BMSSend(&dynSets); break;
    case StatSets: statSets = *(StatSetts*)mp; writeStatSets(); break;
  }
}

void connectTask(void *arg) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    for (int i=0;i<cellBLE.numCells;i++)
      ConnectCell(i);
    vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS(CHECKCONNECT) );
  }
}

void pollINATask(void *arg) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  for (;;) {
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
    vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS(dynSets.PollFreq));
  }
}

extern "C" void app_main() {
  BMSADCInit();
  adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_11);

  gpio_set_direction(GREEN_LED, GPIO_MODE_OUTPUT);
  gpio_set_direction(RESISTOR_PWR, GPIO_MODE_OUTPUT);
  for (int i=0;i<C_RELAY_TOTAL;i++)
    gpio_set_direction(relayPins[i],GPIO_MODE_OUTPUT);
  gpio_set_level(GREEN_LED,HIGH);
  BMSInitCom(ConSerData);
  for (int i=0;i<MAX_CELLS;i++) pClients[i] = NULL;

  NimBLEDevice::init("ADVBMS");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new adCB(), false);
  pBLEScan->setMaxResults(0); // do not store the scan results, use callback only.

  esp_err_t result;
  nvsHandle = nvs::open_nvs_handle("storage", NVS_READWRITE, &result);
  assert(result == ESP_OK);
  initStatSets();
  initDynSets();
  initCellBLE();

  printf("ncells %d\n",cellBLE.numCells);
  dynSets.cmd = DynSets;
  statSets.cmd = StatSets;

  clearRelays();
  initState();
  assert(INA.begin(dynSets.MaxAmps, dynSets.ShuntUOhms) == 2);
  lastShuntMillis = millis();
  setBattAH();
  setINAs();

  setStateOfCharge((battMilliAmpMillis * 4)/5,false);
  InitCells();
  BMSSend(&statSets); 
  BMSSend(&dynSets); 
  gpio_set_level(GREEN_LED,LOW);

  TickType_t xLastWakeTime = xTaskGetTickCount();
  xTaskCreate(connectTask, "connect task", 2048, NULL, 10, NULL); // priority value?
  xTaskCreate(pollINATask, "poll task", 512, NULL, 10, NULL); // priority value?

  for (;;) {
    if (st.lastMillis > millis()) st.milliRolls++;
    st.lastMillis = millis(); // for uptime to continue after 50 days

  /* Need time from BMSWifi
    if (dynSets.savedTime) {
      time_t now = time(nullptr);
      if ((now - dynSets.savedTime) < 60) {
        setStateOfCharge(dynSets.milliAmpMillis,true);
        dynSets.savedTime = 0;
        dynSets.milliAmpMillis = 0;
        writeDynSets();
      } else if (saveTimeDiff && saveTimeDiff < (now - dynSets.savedTime)) {
        // difference is growing so give up
        dynSets.savedTime = 0;
        dynSets.milliAmpMillis = 0;
        writeDynSets();
      } else saveTimeDiff = now - dynSets.savedTime;
    } */

    checkStatus();
    vTaskDelayUntil( &xLastWakeTime, pdMS_TO_TICKS(dynSets.cellSets.time) );
  }
}
