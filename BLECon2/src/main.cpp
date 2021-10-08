#include <Arduino.h>
#include <EEPROM.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <PacketSerial.h>
#include <INA.h>

#include <time.h>
#include <Ticker.h>
#include "defines.h"
#include <BMSComm.h>

// improve server
// adjustable cell history
// cell dump safety, stop if cell gets low, show dump status
// charge control feature to reduce amps
// improve the SoC allow to run logic. Make the code bad, and see if the charge/loads go off

#define AVEADJAVGS 16

char debugstr[200];
int8_t analCnt=0,curAnal=0;
bool cellsOverDue = true,loadsOff = true,chgOff = true
  ,updateINAs=false,writeBattSet=false,writeCellSet=false;
uint32_t lastSentMillis=0,statusCnt=0,lastHitCnt=0,scanStart=0;
uint16_t resPwrMS=0;
Ticker watchDog;
bool OTAInProg = false;

PacketSerial_<COBS, 0, sizeof(union MaxData)+10> dataSer;

BLEClient* pClients[MAX_CELLS];
NimBLERemoteCharacteristic* pSettings[MAX_CELLS];
uint32_t cellDumpSecs[MAX_CELLS];
uint32_t cellLasts[MAX_CELLS];

#define frame (uint8_t)0x00

const int relayPins[RELAY_TOTAL] = { GPIO_NUM_19,GPIO_NUM_18,GPIO_NUM_2,GPIO_NUM_15,GPIO_NUM_13,GPIO_NUM_14,GPIO_NUM_27,GPIO_NUM_26 };

struct BattSettings battSets;
struct BLESettings cellSets;
BMSStatus st;
char spb[1024];

uint32_t statusMS=0,connectMS=0,shuntPollMS=0,pvPollMS=0,lastSync=0;
bool inAlertState = true;

#define MAX_CELLV (battSets.limits[limits::Volt][limits::Cell][limits::Max][limits::Trip])
NimBLEScan* pBLEScan;

INA_Class         INA;
int INADevs;
int milliRolls;
int32_t lastAdjMillAmpHrs = 0;
uint32_t lastMillis=0,lastShuntMillis;
int64_t milliAmpMillis,battMilliAmpMillis,accumMilliAmpMillis;
int adjCnt,curAdj;
int64_t aveAdjMilliAmpMillis,curAdjMilliAmpMillis[AVEADJAVGS];
uint32_t BatAHMeasured = 0;
int lastTrip;

void initState() {
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
}

uint8_t CRC8(const uint8_t *data,int length) 
{
   uint8_t crc = 0x00;
   uint8_t extract;
   uint8_t sum;
   for(int i=0;i<length;i++)
   {
      extract = *data;
      for (uint8_t tempI = 8; tempI; tempI--) 
      {
         sum = (crc ^ extract) & 0x01;
         crc >>= 1;
         if (sum)
            crc ^= 0x8C;
         extract >>= 1;
      }
      data++;
   }
   return crc;
}

bool readEE(uint8_t *p,size_t s,uint32_t start) {
  EEPROM.readBytes(start,p,s);
  uint8_t checksum = CRC8(p, s);
  uint8_t ck = EEPROM.read(start+s);
  return checksum == ck;
}

void writeEE(uint8_t *p,size_t s,uint32_t start) {
  uint8_t crc = CRC8(p, s);
  EEPROM.writeBytes(start,p,s);
  EEPROM.write(start+s,crc);
  EEPROM.commit();
}

void saveStatus() {
  battSets.milliAmpMillis = milliAmpMillis;
  battSets.savedTime = time(nullptr);
  writeBattSet = true;
  writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);
}

void InitOTA() {
    // Port defaults to 3232
  // ArduinoOTA.setPort(3232);

  // Hostname defaults to esp3232-[MAC]
  // ArduinoOTA.setHostname("myesp32");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";
      OTAInProg = true;
      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      saveStatus();
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();
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
  return ((uint32_t)battSets.CellsOutTime < ((millis() - t)*1000) || !st.stateOfChargeValid || st.stateOfCharge < battSets.CellsOutMin || st.stateOfCharge > battSets.CellsOutMax);
}

void initRelays() {
  for (int i=0;i<RELAY_TOTAL;i++) {
    digitalWrite(relayPins[i], LOW);
    st.previousRelayState[i] = LOW;
  }
}

void doAHCalcs() {
  if (adjCnt < AVEADJAVGS)
    adjCnt++;
  for (int i=0;i<adjCnt;i++)
    aveAdjMilliAmpMillis += curAdjMilliAmpMillis[i];
  aveAdjMilliAmpMillis = adjCnt;
  lastAdjMillAmpHrs = (int32_t)(curAdjMilliAmpMillis[curAdj] / ((int64_t)1000 * 60 * 60));
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
  dataSer.send((byte*)&evt,sizeof(evt));
}

void doWatchDog() {
  if (doShutOffNoStatus(statusMS)) {
    initRelays();
    SendEvent(WatchDog);
  }
  st.watchDogHits++;
  watchDog.once_ms(1000,doWatchDog); // every second
}

bool isFromOff(RelaySettings* rs) {
  if (!strlen(rs->from))
    return false;
  for (int8_t y = 0; y < RELAY_TOTAL; y++)
  {
    RelaySettings *rp = &battSets.relays[y];
    if (rp == rs) continue;
    if (!strcmp(rp->name,rs->from))
      return st.previousRelayState[y] == LOW;
  }
  return false;
}

class MyClientCallback : public NimBLEClientCallbacks {
  int cell;
  public:
  MyClientCallback(int cell) {
    this->cell = cell;
  }
  void onConnect(NimBLEClient* pclient) {
    st.cellConn[cell] = true;
    Serial.println("Con");
  }

  void onDisconnect(NimBLEClient* pclient) {
    st.cellConn[cell] = false;
    Serial.println("Disc");
  }
};

void sendCellSet(int i) {
  NimBLEClient*  pC = pClients[i];
  if (!pC || !pC->isConnected())
    return;
  if (!pSettings[i]) {
    Serial.printf("Badness here\n");
  }
  pSettings[i]->writeValue<CellSettings>(cellSets.sets,false);
}

void syncCell(int i) {
  BLERemoteService* pServ = pClients[i]->getService(NimBLEUUID((uint16_t)0x180F));
  cellLasts[i] = 0;
  if (pServ) {
    NimBLERemoteCharacteristic* pChar = pServ->getCharacteristic(NimBLEUUID((uint16_t)0x2B45));
    if (pChar)
      pChar->writeValue<uint16_t>(1);
  }
}

void syncCells() {
  for (int i=0;i<cellSets.numCells;i++)
    syncCell(i);
}

void ConnectCell(int i) {
  NimBLEClient*  pC = pClients[i];
  if (pC->isConnected())
    return;
  // Connect to the remove BLE Server.
  if (!pC->connect(BLEAddress(cellSets.addrs[i]),true)) {
    Serial.printf("Failed to connect: %d, %s\n",i,((std::string)cellSets.addrs[i]).c_str());
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
  pChar->subscribe(true,[i](NimBLERemoteCharacteristic* pBLERemoteCharacteristic,
                              uint8_t* pData, size_t length, bool isNotify) {
          CellStatus* cd = (CellStatus*)pData;
          st.cellVolts[i] = cd->volts;
          st.cellTemps[i] = cd->tempExt;
          st.cellDumping[i] = cd->drainSecs > 0;
          cellLasts[i] = millis();
        },false);
  pSettings[i] = pServ->getCharacteristic(NimBLEUUID((uint16_t)0x2B15)); // settings
  sendCellSet(i);
}

void sendCellSets() {
  for (int i=0;i<cellSets.numCells;i++)
    sendCellSet(i);
}

#define IntervalToms(x) (x * 1000 / BLE_HCI_CONN_ITVL)

void InitCell(int i) {
  NimBLEClient*  pC  = NimBLEDevice::createClient();

  pC->setClientCallbacks(new MyClientCallback(i),true);
  pC->setConnectionParams(IntervalToms(100),IntervalToms(100),25,600);
  pClients[i] = pC;
  st.cellDumping[i] = false;
  cellDumpSecs[i] = 0;
}
void InitCells() {
  for (int i=0;i<cellSets.numCells;i++) {
    InitCell(i);
    ConnectCell(i);
  }
}

void ConnectCells() {
  for (int i=0;i<cellSets.numCells;i++)
    ConnectCell(i);
}

void CheckBLEScan() {
  if (battSets.nCells > cellSets.numCells && !scanStart) {
    Serial.println("Starting scan");
    pBLEScan->start(0,false);
    scanStart = millis();
    return;
  }
  if (!scanStart)
    return;
  uint32_t x = millis() - scanStart;
  if (x > BLESCANREST) {
    pBLEScan->stop();
    scanStart = 0;
  } else if (x > BLESCANTIME && pBLEScan->isScanning()) {
    pBLEScan->stop(); // need to stop scanning to allow use of radio for other stuff
    Serial.println("Stop scan");
  }
}

class adCB: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* ad) {
      if (!ad->getName().compare("LiFePo4 Cell")
              && ad->haveServiceUUID()
              && ad->isAdvertisingService(NimBLEUUID((uint16_t)0x180F))) {

        int i=0;
        for (;i<cellSets.numCells && cellSets.addrs[i] != ad->getAddress();i++) ;
        Serial.printf("Found %d %d\n",cellSets.numCells,i);
        if (i==cellSets.numCells && cellSets.numCells < battSets.nCells) {
          cellSets.numCells++;
          cellSets.addrs[i] = ad->getAddress();
          Serial.printf("Add %d: %s\n",i,cellSets.addrs[i].toString().c_str());
          InitCell(i);
          writeCellSet = true;
        }
        if (cellSets.numCells == battSets.nCells) {
          pBLEScan->stop(); // need to stop scanning to connect
          scanStart = 0;
          Serial.println("Stop scan");
        }
      }
    }
};

void checkStatus()
{
  statusCnt++;

  st.curTemp1 = BMSGetTemp(TEMP1,3200,BCOEF,47000,47000,4);
  if (INADevs > 0) {
    if ((st.lastMicroAmps > 0 && chgOff) || (st.lastMicroAmps < 0 && loadsOff)) {
      if (!inAlertState) {
        uint16_t maxCellV = 0;
        uint16_t minCellV = 0xffff;
        for (int j=0;j<battSets.nCells;j++) {
          uint16_t cellV = st.cellVolts[j];
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
        dataSer.send((byte*)&msg,strlen(msg.msg)+sizeof(msg.cmd));
        initRelays();
        inAlertState = true;
      }
    } else if (inAlertState) {
      inAlertState = false;
      StrMsg msg;
      msg.cmd = NoPanic;
      dataSer.send((byte*)&msg,sizeof(msg.cmd));
    }
  }

  if (INADevs > 0) {
    if (battMilliAmpMillis != 0)
      st.stateOfCharge = milliAmpMillis * 100 / battMilliAmpMillis;
    st.lastPackMilliVolts = INA.getBusMilliVolts(0);
  }
  if (INADevs == 0 || st.lastPackMilliVolts < 1000) { // low side shunt
    st.lastPackMilliVolts = 0;
    for (int8_t i = 0; i < battSets.nCells; i++)
      st.lastPackMilliVolts += st.cellVolts[i];
  }

  bool allovervoltrec = true,allundervoltrec = true,hitTop=false,hitUnder=false;
  bool allovertemprec = true,allundertemprec = true;
  //Loop through cells
  cellsOverDue = false;
  for (int8_t i = 0; i < battSets.nCells; i++)
  {
    if (!st.cellConn[i] && doShutOffNoStatus(lastSentMillis)) {
      cellsOverDue = true;
      initRelays();
      SendEvent(CellsOverDue);
      break;
    }
    uint16_t cellV = st.cellVolts[i];

    if (cellV > battSets.limits[limits::Volt][limits::Cell][limits::Max][limits::Trip]) {
      if (!st.maxCellVState) {
        hitTop = true;
        SendEvent(CellTopV,st.lastMicroAmps,cellV,i);
      }
      st.maxCellVState = true;
    }
    if (cellV > battSets.limits[limits::Volt][limits::Cell][limits::Max][limits::Rec])
      allovervoltrec = false;

    if (cellV < battSets.limits[limits::Volt][limits::Cell][limits::Min][limits::Trip]) {
      if (!st.minCellVState) {
        hitUnder = true;
        SendEvent(CellBotV,st.lastMicroAmps,cellV,i);
      }
      st.minCellVState = true;
    }
    if (cellV < battSets.limits[limits::Volt][limits::Cell][limits::Min][limits::Rec])
      allundervoltrec = false;

    int8_t cellT = st.cellTemps[i];
    if (battSets.useCellC && cellT != -40) {
      if (cellT > battSets.limits[limits::Temp][limits::Cell][limits::Max][limits::Trip]) {
        st.maxCellCState = true;
        SendEvent(CellTopT,st.lastMicroAmps,cellT,i);
      }

      if (cellT > battSets.limits[limits::Temp][limits::Cell][limits::Max][limits::Rec])
        allovertemprec = false;

      if (cellT < battSets.limits[limits::Temp][limits::Cell][limits::Min][limits::Trip]) {
        st.minCellCState = true;
        SendEvent(CellBotT,st.lastMicroAmps,cellT,i);
      }
      if (cellT < battSets.limits[limits::Temp][limits::Cell][limits::Min][limits::Rec])
        allundertemprec = false;
    }
  }
  if (st.maxCellVState && allovervoltrec)
    st.maxCellVState = false;
  if (st.minCellVState && allundervoltrec)
    st.minCellVState = false;
  if (!battSets.useCellC || (st.maxCellCState && allovertemprec))
    st.maxCellCState = false;
  if (!battSets.useCellC || (st.minCellCState && allundertemprec))
    st.minCellCState = false;

  if (battSets.useTemp1) {
    if (st.curTemp1 > battSets.limits[limits::Temp][limits::Pack][limits::Max][limits::Trip])
      st.maxPackCState = true;
    if (st.curTemp1 < battSets.limits[limits::Temp][limits::Pack][limits::Max][limits::Rec])
      st.maxPackCState = false;
    if (st.curTemp1 < battSets.limits[limits::Temp][limits::Pack][limits::Min][limits::Trip])
      st.minPackCState = true;
    if (st.curTemp1 > battSets.limits[limits::Temp][limits::Pack][limits::Min][limits::Rec])
      st.minPackCState = false;
  }
  if (st.lastPackMilliVolts > battSets.limits[limits::Volt][limits::Pack][limits::Max][limits::Trip]) {
    if (!st.maxPackVState) {
      hitTop = true;
      SendEvent(PackTopV,st.lastMicroAmps,st.lastPackMilliVolts);
    }
    st.maxPackVState = true;
  } else if (st.lastPackMilliVolts < battSets.limits[limits::Volt][limits::Pack][limits::Max][limits::Rec])
    st.maxPackVState = false;

  if (st.lastPackMilliVolts < battSets.limits[limits::Volt][limits::Pack][limits::Min][limits::Trip]) {
    if (!st.minPackVState) {
      hitUnder = true;
      SendEvent(PackBotV,st.lastMicroAmps,st.lastPackMilliVolts);
    }
    st.minPackVState = true;
  } else if (st.lastPackMilliVolts > battSets.limits[limits::Volt][limits::Pack][limits::Min][limits::Rec])
    st.minPackVState = false;

  if (hitTop || hitUnder) {
    if (!lastHitCnt)
      lastHitCnt=statusCnt;
    else lastHitCnt++;
  } else
    lastHitCnt = 0;
  hitTop = hitTop && (int32_t)battSets.TopAmps > (st.lastMicroAmps/1000000) && (statusCnt == lastHitCnt); // We don't want to trigger a hittop 
  hitUnder = hitUnder && (int32_t)-battSets.TopAmps < (st.lastMicroAmps/1000000) && (statusCnt == lastHitCnt);
  if (hitTop) {
    if (lastTrip != 0) {
      if (milliAmpMillis < battMilliAmpMillis)
        curAdjMilliAmpMillis[curAdj] += battMilliAmpMillis - milliAmpMillis;
      if (lastTrip < 0)
        BatAHMeasured = (milliAmpMillis + curAdjMilliAmpMillis[curAdj]) / ((uint64_t)1000 * 1000 * 60 * 60);
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
        BatAHMeasured = (battMilliAmpMillis - milliAmpMillis + curAdjMilliAmpMillis[curAdj]) / ((uint64_t)1000 * 1000 * 60 * 60);
      doAHCalcs();
    }
    lastTrip = -1;
    milliAmpMillis = 0;
    st.stateOfChargeValid = true;
  }
  if (st.stateOfChargeValid && !st.doFullChg) {
    if (st.stateOfCharge > battSets.ChargePct)
      st.maxChargePctState = true;
    else if (st.stateOfCharge < battSets.ChargePctRec)
      st.maxChargePctState = false;
  } else
    st.maxChargePctState = false;

  uint8_t relay[RELAY_TOTAL];
  bool wasLoadsOff = loadsOff;
  bool wasChgOff = chgOff;
  loadsOff = st.minCellVState || st.minPackVState || st.maxCellCState || st.maxPackCState;
  chgOff = st.maxChargePctState || st.maxCellVState || st.maxPackVState || st.minCellCState || st.maxCellCState || st.minPackCState || st.maxPackCState;
  for (int8_t y = 0; y < RELAY_TOTAL; y++)
  {
    RelaySettings *rp = &battSets.relays[y];
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
  for (int8_t n = 0; n < RELAY_TOTAL; n++)
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
}

void setINAs() {
  INA.begin(battSets.MaxAmps, battSets.ShuntUOhms,0);
  INA.begin(battSets.PVMaxAmps,battSets.PVShuntUOhms,1);
  INA.setShuntConversion(300,0);
  INA.setBusConversion(300,0);
  INA.setAveraging(10000,0);
  INA.setShuntConversion(battSets.ConvTime,1);
  INA.setBusConversion(battSets.ConvTime,1);
  INA.setAveraging(battSets.Avg,1);
}

void setStateOfCharge(int64_t val,bool valid) {
  milliAmpMillis = val;
  st.stateOfChargeValid = valid;
  curAdj = 0;
  adjCnt = 0;
  curAdjMilliAmpMillis[0] = 0;
  lastTrip = 0;
  aveAdjMilliAmpMillis = 0;
  st.doFullChg = true;
}

void setBattAH() {
  battMilliAmpMillis = (uint64_t)battSets.BattAH * (1000 * 60 * 60) * 1000; // convert to milliampmilliseconds
}

void initCellSets() {
  cellSets.sets.cnt = 4;
  cellSets.sets.delay = 0;
  cellSets.sets.time = 2000;
  cellSets.numCells = 0;
}

void initBattSets() {
  battSets.cmd = BattSets;
  battSets.ConvTime = 1000;
  battSets.PVMaxAmps = 100;
  battSets.PVShuntUOhms = 500;
  battSets.ShuntUOhms = 167;
  battSets.MaxAmps = 300;
  battSets.nCells=0;
  battSets.PollFreq = 500;
  battSets.BattAH = 1;
  battSets.ConvTime = 1000;
  battSets.useTemp1 = true;
  battSets.useCellC = true;
  battSets.ChargePct = 100;
  battSets.ChargePctRec = 0;
  battSets.ChargeRate = 0;
  battSets.CellsOutMax = 80;
  battSets.CellsOutMin = 30;
  battSets.CellsOutTime = 120;
  battSets.FloatV = 3400;
  battSets.Avg = 1000;
  battSets.TopAmps = 6;
  battSets.savedTime = 0;
  battSets.milliAmpMillis = 0;
  for (int i=0;i<RELAY_TOTAL;i++) {
    RelaySettings* r = &battSets.relays[i];
    r->name[0] = 0;
    r->from[0] = 0;
    r->doSoC = false;
    r->off = true;
    r->fullChg = false;
    r->rec = 0;
    r->trip = 0;
    r->type = 0;
  }
  for (int i=limits::Cell;i<limits::Max1;i++) {
    int mul = !i?1:8;
    battSets.limits[limits::Volt][i][limits::Max][limits::Trip] = 3500 * mul;
    battSets.limits[limits::Volt][i][limits::Max][limits::Rec] = 3400 * mul;
    battSets.limits[limits::Volt][i][limits::Min][limits::Trip] = 3100 * mul;
    battSets.limits[limits::Volt][i][limits::Min][limits::Rec] = 3200 * mul;
    battSets.limits[limits::Temp][i][limits::Max][limits::Trip] = 50;
    battSets.limits[limits::Temp][i][limits::Max][limits::Rec] = 45;
    battSets.limits[limits::Temp][i][limits::Min][limits::Trip] = 4;
    battSets.limits[limits::Temp][i][limits::Min][limits::Rec] = 7;
  }
}

void DoSetting(uint8_t cmd,uint16_t val) {
  switch (cmd) {
    case SetCurSOC:
      setStateOfCharge((battMilliAmpMillis * val)/100,true);
      break;
    case SetPollFreq:
      battSets.PollFreq = val;
      if (battSets.PollFreq < 500)
        battSets.PollFreq = 500;
      break;
    case SetBattAH:
      battSets.BattAH = val;
      setBattAH();
      break;
    case SetNCells:
      battSets.nCells = val;
      Serial.printf("NCells: %d\n",battSets.nCells);
      if (battSets.nCells < cellSets.numCells) {
        for (int i=battSets.nCells;i<cellSets.numCells;i++) {
          Serial.printf("Deleting %d\n",i);
          NimBLEDevice::deleteClient(pClients[i]);
          cellSets.addrs[i] = NimBLEAddress();
        }
        cellSets.numCells = battSets.nCells;
        writeCellSet = true;
      } else if (battSets.nCells > cellSets.numCells) {
        for (int i=cellSets.numCells;i<battSets.nCells;i++)
          cellSets.addrs[i] = NimBLEAddress();
      }
      writeCellSet = true;
      break;
    case SetRelayOff: {
      RelaySettings *rp = &battSets.relays[val];
      rp->off = !rp->off;
      }
      break;
    case SetMaxAmps: battSets.MaxAmps = val; updateINAs = true; break;
    case SetShuntUOhms: battSets.ShuntUOhms = val; updateINAs = true; break;
    case SetPVMaxAmps: battSets.PVMaxAmps = val; updateINAs = true; break;
    case SetPVShuntUOhms: battSets.PVShuntUOhms = val; updateINAs = true; break;
    case SetAvg: battSets.Avg = val; updateINAs = true; break;
    case SetConvTime: battSets.ConvTime = val; updateINAs = true; break;
  }
  writeBattSet = true;
}

void DoCellSetting(uint8_t cmd,uint16_t val) {
  switch (cmd) {
    case SetCellCnt:
      cellSets.sets.cnt = val;
      if (cellSets.sets.cnt < 1)
        cellSets.sets.cnt = 1;
      break;
    case SetCellDelay:
      cellSets.sets.delay = val;
      break;
    case SetCellTime:
      cellSets.sets.time = val;
      break;
  }
  writeCellSet = true;
}
void DoDump(DumpMsg *dm) {
    if (cellDumpSecs[dm->cell])
      cellDumpSecs[dm->cell] = 0;
    else cellDumpSecs[dm->cell] = dm->secs;
    BLERemoteService* pServ = pClients[dm->cell]->getService(NimBLEUUID((uint16_t)0x180F));
    if (pServ) {
      NimBLERemoteCharacteristic* pChar = pServ->getCharacteristic(NimBLEUUID((uint16_t)0X2AE2));
      if (pChar) {
        pChar->writeValue<uint32_t>(cellDumpSecs[dm->cell]);
        Serial.printf("Write %d %d\n",dm->cell,cellDumpSecs[dm->cell]);
      }
    }
}

// dataSer.send((byte*)debugstr,strlen(debugstr)+2);
void onSerData(const uint8_t *receivebuffer, size_t len)
{
  digitalWrite(GREEN_LED,1);
  uint8_t cmd = *receivebuffer;
  if (cmd > FirstSetting && cmd < LastSetting)
    DoSetting(cmd,((SettingMsg*)receivebuffer)->val);
  else if (cmd > FirstCellSetting && cmd < LastCellSetting)
    DoCellSetting(cmd,((SettingMsg*)receivebuffer)->val);
  else switch (cmd) {
    case DumpCell: DoDump((DumpMsg*)receivebuffer); break;
    case FullChg: st.doFullChg = !st.doFullChg; break;
    case ClrMaxDiff: st.maxDiffMilliVolts = 0; break;
    case BattSets: dataSer.send((byte*)&battSets,sizeof(battSets));

  }
    

  digitalWrite(GREEN_LED,0);
}

void setup() {
  BMSInit();
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RESISTOR_PWR, OUTPUT);
  digitalWrite(GREEN_LED,1);
  Serial.begin(9600);
  EEPROM.begin(EEPROMSize);
  Wire.begin();
  for (int i=0;i<MAX_CELLS;i++) pClients[i] = NULL;

  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new adCB(), false);
  pBLEScan->setMaxResults(0); // do not store the scan results, use callback only.

  Serial2.begin(CPUBAUD);
  dataSer.setStream(&Serial2); // start serial for output
  dataSer.setPacketHandler(&onSerData);

  if (!readEE((uint8_t*)&battSets,sizeof(battSets),EEPROM_BATT))
    initBattSets();

  if (!readEE((uint8_t*)&cellSets,sizeof(cellSets),EEPROM_BLE))
    initCellSets();

  Serial.printf("ncells %d\n",cellSets.numCells);
  
  for (int i=0;i<RELAY_TOTAL;i++)
    pinMode(relayPins[i],OUTPUT);
  initRelays();
  initState();
  INADevs = INA.begin(battSets.MaxAmps, battSets.ShuntUOhms);
  lastShuntMillis = millis();
  setBattAH();
  if (INADevs)
    setINAs();

  configTime(0,0,"pool.ntp.org");
  InitOTA();
  setStateOfCharge((battMilliAmpMillis * 4)/5,false);
  InitCells();
  dataSer.send((byte*)&battSets,sizeof(battSets));
  digitalWrite(GREEN_LED,0);
}

time_t saveTimeDiff = 0;
void loop() {
  if (lastMillis > millis()) milliRolls++;
  lastMillis = millis(); // for uptime to continue after 50 days

  if (battSets.savedTime) {
    time_t now = time(nullptr);
    if ((now - battSets.savedTime) < 60) {
      setStateOfCharge(battSets.milliAmpMillis,true);
      battSets.savedTime = 0;
      battSets.milliAmpMillis = 0;
      writeBattSet = true;
    } else if (saveTimeDiff && saveTimeDiff < (now - battSets.savedTime)) {
      // difference is growing so give up
      battSets.savedTime = 0;
      battSets.milliAmpMillis = 0;
      writeBattSet = true;
    } else saveTimeDiff = now - battSets.savedTime;
  }
  dataSer.update();
  if (writeBattSet) {
    writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);
    writeBattSet = false;
  } else if (writeCellSet) {
    writeEE((uint8_t*)&cellSets,sizeof(cellSets),EEPROM_BLE);
    writeCellSet = false;
  } else if (cellSets.numCells > 1 && ((millis() - lastSync) > cellSets.sets.time * 3)) {
    uint32_t min=0xffffffff,max=0;
    int i=0;
    for (;i<cellSets.numCells && st.cellConn[i];i++) {
      if (cellLasts[i] > max)
        max = cellLasts[i];
      if (cellLasts[i] < min)
        min = cellLasts[i];
    }
    if (i == cellSets.numCells && (max - min) < (cellSets.sets.time-300) && (max-min) > 100) {
      syncCells();
      Serial.printf("%d,%d,%d,%d\n",min,max,max-min,cellSets.sets.time);
    }
  }
  if (INADevs > 0) {
    if (updateINAs)
     setINAs();
    updateINAs = false;
    if ((millis() - shuntPollMS) > battSets.PollFreq) {
      getAmps();
      shuntPollMS = millis();
    }
    if (INADevs > 1 && (millis() - pvPollMS) > POLLPV) {
      st.lastPVMicroAmps = INA.getBusMicroAmps(1);
      pvPollMS = millis();
    }
  }

  if ((millis() - connectMS) > CHECKCONNECT) {
    ConnectCells();
    connectMS = millis();
  }
  CheckBLEScan();

  ArduinoOTA.handle(); // this does nothing until it is initialized
}
