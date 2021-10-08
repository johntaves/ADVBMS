#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <EEPROM.h>
#include <HTTPClient.h>
#include <INA.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <ESP32_MailClient.h>
#include <Wire.h>
//#include <EasyBuzzer.h>
#include <NimBLEDevice.h>

#include <time.h>
#include <Ticker.h>
#include "defines.h"
#include "CellData.h"

// improve server
// adjustable cell history
// cell dump safety, stop if cell gets low, show dump status
// charge control feature to reduce amps
// improve the SoC allow to run logic. Make the code bad, and see if the charge/loads go off
// email did not work going from unset to sent. A reboot seemed to help
// plot V and A over time

#define AVEADJAVGS 16

char debugstr[200],lastEventMsg[1024];
int8_t analCnt=0,curAnal=0,lastEventMsgCnt=0;
struct AnalogInput anals[Max_Analog];
bool cellsOverDue = true,emailSetup=false,loadsOff = true,chgOff = true, doFullChg = true
  ,updateINAs=false,writeBattSet=false,writeCellSets=false,writeCommSet=false,writeWifiSet=false;
uint32_t lastSentMillis=0,sentMillis=1,receivedMillis=0,lastRoundMillis=0,numSent=0,failedSer=0,statusCnt=0,lastHitCnt=0,scanStart=0;
uint16_t resPwrMS=0;
HTTPClient http;
Ticker watchDog;
atomic_flag taskRunning(0);
bool OTAInProg = false;

CellData cells[MAX_CELLS];

#define frame (uint8_t)0x00

const int relayPins[RELAY_TOTAL] = { GPIO_NUM_17,GPIO_NUM_16,GPIO_NUM_2,GPIO_NUM_15,GPIO_NUM_13,GPIO_NUM_14,GPIO_NUM_27,GPIO_NUM_26 };

struct WiFiSettings wifiSets;
struct CommSettings commSets;
struct BattSettings battSets;
struct BLECells cellSets;

char spb[1024];

uint32_t statusMS=0,connectMS=0,shuntPollMS=0,pvPollMS=0,analogPollMS=0,lastSync=0;
bool ledState = false;
bool sendEmail = false,inAlertState = true;
AsyncWebServer server(80);
SMTPData smtpData;
uint8_t previousRelayState[RELAY_TOTAL];
String emailRes = "";

#define CHG_STEP 10
#define CHG_SAMPS 10
#define MAX_CELLV (battSets.limits[limits::Volt][limits::Cell][limits::Max][limits::Trip])
ChargeData chgData[CHG_SAMPS];
uint8_t chgDataCnt;
NimBLEScan* pBLEScan;

INA_Class         INA;
int INADevs;
int watchDogHits;
int stateOfCharge,milliRolls,curTemp1;
int32_t lastMicroAmps,lastAdjMillAmpHrs = 0,lastPVMicroAmps=0;
uint32_t lastMillis=0,lastShuntMillis;
int64_t milliAmpMillis,battMilliAmpMillis,accumMilliAmpMillis;
uint16_t lastPackMilliVolts = 0xffff;
int16_t maxDiffMilliVolts=0;
int adjCnt,curAdj;
int64_t aveAdjMilliAmpMillis,curAdjMilliAmpMillis[AVEADJAVGS];
uint32_t BatAHMeasured = 0;
int lastTrip;
bool stateOfChargeValid;
bool maxCellVState=false,minCellVState=false
  ,maxPackVState=false,minPackVState=false,maxChargePctState=false
  ,maxCellCState=false,minCellCState=false
  ,maxPackCState=false,minPackCState=false;

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

void trimLastEventMsg() {
  lastEventMsgCnt++;
  if (lastEventMsgCnt > LAST_EVT_MSG_CNT) {
    char* ptr = strchr(lastEventMsg,'|');
    if (ptr)
      snprintf(lastEventMsg,sizeof(lastEventMsg),"%s",ptr+2);
    lastEventMsgCnt = LAST_EVT_MSG_CNT;
  }
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
  lastMicroAmps = -INA.getBusMicroAmps(0);

  uint32_t thisMillis = millis();
  int64_t deltaMilliAmpMillis = (int64_t)lastMicroAmps * (thisMillis - lastShuntMillis) / 1000;
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
  return ((uint32_t)battSets.CellsOutTime < ((millis() - t)*1000) || !stateOfChargeValid || stateOfCharge < battSets.CellsOutMin || stateOfCharge > battSets.CellsOutMax);
}

void initRelays() {
  for (int i=0;i<RELAY_TOTAL;i++) {
    digitalWrite(relayPins[i], LOW);
    previousRelayState[i] = LOW;
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

void doWatchDog() {
  if (doShutOffNoStatus(statusMS)) {
    initRelays();
    trimLastEventMsg();
    snprintf(lastEventMsg,sizeof(lastEventMsg),"%s Watch Dog | ",lastEventMsg);
  }
  watchDogHits++;
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
      return previousRelayState[y] == LOW;
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
    cells[cell].connected = true;
    Serial.println("Con");
  }

  void onDisconnect(NimBLEClient* pclient) {
    cells[cell].connected = false;
    Serial.println("Disc");
  }
};

void sendCellSet(int i) {
  NimBLEClient*  pC = cells[i].pClient;
  if (!pC || !pC->isConnected())
    return;
  if (!cells[i].pSettings) {
    Serial.printf("Badness here\n");
  }
  cells[i].pSettings->writeValue<CellSettings>(cellSets.sets,false);
}

void syncCell(int i) {
  BLERemoteService* pServ = cells[i].pClient->getService(NimBLEUUID((uint16_t)0x180F));
  cells[i].last = 0;
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
  NimBLEClient*  pC = cells[i].pClient;
  if (pC->isConnected())
    return;
  // Connect to the remove BLE Server.
  if (!pC->connect(BLEAddress(cellSets.addrs[i]),true)) {
    Serial.printf("Failed to connect: %d, %s\n",i,cellSets.addrs[i].toString());
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
          cells[i].v = cd->volts;
          cells[i].t = cd->tempExt;
          cells[i].dumping = cd->drainSecs > 0;
          cells[i].last = millis();
        },false);
  cells[i].pSettings = pServ->getCharacteristic(NimBLEUUID((uint16_t)0x2B15)); // settings
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
  cells[i].pClient = pC;
  cells[i].dumping = false;
  cells[i].dumpSecs = 0;
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
          writeCellSets = true;
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

//  curTemp1 = calcStein(anals[Temp1_Analog].rawValue,&caliSets.temps[Temp1]);
  if (INADevs > 0) {
    if ((lastMicroAmps > 0 && chgOff) || (lastMicroAmps < 0 && loadsOff)) {
      if (!inAlertState) {
//        EasyBuzzer.beep(800);
        sendEmail = true;
        uint16_t maxCellV = 0;
        uint16_t minCellV = 0xffff;
        for (int j=0;j<battSets.nCells;j++) {
          uint16_t cellV = cells[j].v;
          if (cellV > maxCellV)
            maxCellV = cellV;
          if (cellV < minCellV)
            minCellV = cellV;
        }
        snprintf(spb,sizeof(spb),"uA=%d, chg: %d, Lds: %d, pack: %dmV, max cell: %dmV, min cell: %dmV, MxPV: %d, MxCV: %d, MnPV: %d, MnCV %d, MxCC: %d, MxPC: %d"
            ,lastMicroAmps,chgOff,loadsOff,(int)lastPackMilliVolts,(int)maxCellV,(int)minCellV
            ,maxPackVState,maxCellVState,minPackVState,minCellVState,maxCellCState,maxPackCState);
        smtpData.setMessage(spb, true);
        initRelays();
        inAlertState = true;
      }
    } else if (inAlertState) {
      inAlertState = false;
      smtpData.setMessage("OK", true);
      sendEmail = true;
//      EasyBuzzer.stopBeep();
    }
  }

  if (INADevs > 0) {
    if (battMilliAmpMillis != 0)
      stateOfCharge = milliAmpMillis * 100 / battMilliAmpMillis;
    lastPackMilliVolts = INA.getBusMilliVolts(0);
  }
  if (INADevs == 0 || lastPackMilliVolts < 1000) { // low side shunt
    lastPackMilliVolts = 0;
    for (int8_t i = 0; i < battSets.nCells; i++)
      lastPackMilliVolts += cells[i].v;
  }

  bool allovervoltrec = true,allundervoltrec = true,hitTop=false,hitUnder=false;
  bool allovertemprec = true,allundertemprec = true;
  int maxCell;
  uint16_t maxMV = 0;
  //Loop through cells
  cellsOverDue = false;
  for (int8_t i = 0; i < battSets.nCells; i++)
  {
    if (!cells[i].connected && doShutOffNoStatus(lastSentMillis)) {
      cellsOverDue = true;
      initRelays();
      trimLastEventMsg();
      snprintf(lastEventMsg,sizeof(lastEventMsg),"%s overdue | ",lastEventMsg);
      break;
    }
    uint16_t cellV = cells[i].v;
    if (cellV > maxMV) {
      maxMV = cellV;
      maxCell = i;
    }

    if (lastMicroAmps > 0 && chgDataCnt < CHG_SAMPS && chgData[chgDataCnt].mV < cellV) {
      chgData[chgDataCnt].mV = cellV;
      chgData[chgDataCnt].cell = i;
      chgData[chgDataCnt].milliAmpMillis = milliAmpMillis;
      chgDataCnt++;
    }

    if (cellV > battSets.limits[limits::Volt][limits::Cell][limits::Max][limits::Trip]) {
      if (!maxCellVState) {
        hitTop = true;
        snprintf(lastEventMsg,sizeof(lastEventMsg),"%s #%d %dV,",lastEventMsg,i,cellV);
      }
      maxCellVState = true;
    }
    if (cellV > battSets.limits[limits::Volt][limits::Cell][limits::Max][limits::Rec])
      allovervoltrec = false;

    if (cellV < battSets.limits[limits::Volt][limits::Cell][limits::Min][limits::Trip]) {
      if (!minCellVState) {
        hitUnder = true;
        snprintf(lastEventMsg,sizeof(lastEventMsg),"%s #%d %dV,",lastEventMsg,i,cellV);
      }
      minCellVState = true;
    }
    if (cellV < battSets.limits[limits::Volt][limits::Cell][limits::Min][limits::Rec])
      allundervoltrec = false;

    int8_t cellT = cells[i].t;
    if (battSets.useCellC && cellT != -40) {
      if (cellT > battSets.limits[limits::Temp][limits::Cell][limits::Max][limits::Trip]) {
        maxCellCState = true;
        snprintf(lastEventMsg,sizeof(lastEventMsg),"%s #%d %dC,",lastEventMsg,i, cellT);
      }

      if (cellT > battSets.limits[limits::Temp][limits::Cell][limits::Max][limits::Rec])
        allovertemprec = false;

      if (cellT < battSets.limits[limits::Temp][limits::Cell][limits::Min][limits::Trip]) {
        snprintf(lastEventMsg,sizeof(lastEventMsg),"%s #%d %dC,",lastEventMsg,i, cellT);
        minCellCState = true;
      }
      if (cellT < battSets.limits[limits::Temp][limits::Cell][limits::Min][limits::Rec])
        allundertemprec = false;
    }
  }
  if (maxCellVState && allovervoltrec)
    maxCellVState = false;
  if (minCellVState && allundervoltrec)
    minCellVState = false;
  if (!battSets.useCellC || (maxCellCState && allovertemprec))
    maxCellCState = false;
  if (!battSets.useCellC || (minCellCState && allundertemprec))
    minCellCState = false;

  if (battSets.useTemp1) {
    if (curTemp1 > battSets.limits[limits::Temp][limits::Pack][limits::Max][limits::Trip])
      maxPackCState = true;
    if (curTemp1 < battSets.limits[limits::Temp][limits::Pack][limits::Max][limits::Rec])
      maxPackCState = false;
    if (curTemp1 < battSets.limits[limits::Temp][limits::Pack][limits::Min][limits::Trip])
      minPackCState = true;
    if (curTemp1 > battSets.limits[limits::Temp][limits::Pack][limits::Min][limits::Rec])
      minPackCState = false;
  }
  if (lastPackMilliVolts > battSets.limits[limits::Volt][limits::Pack][limits::Max][limits::Trip]) {
    if (!maxPackVState) {
      hitTop = true;
      snprintf(lastEventMsg,sizeof(lastEventMsg),"%s P %dV,",lastEventMsg,lastPackMilliVolts);
    }
    maxPackVState = true;
  } else if (lastPackMilliVolts < battSets.limits[limits::Volt][limits::Pack][limits::Max][limits::Rec])
    maxPackVState = false;

  if (lastPackMilliVolts < battSets.limits[limits::Volt][limits::Pack][limits::Min][limits::Trip]) {
    if (!minPackVState) {
      hitUnder = true;
      snprintf(lastEventMsg,sizeof(lastEventMsg),"%s P %dV,",lastEventMsg,lastPackMilliVolts);
    }
    minPackVState = true;
  } else if (lastPackMilliVolts > battSets.limits[limits::Volt][limits::Pack][limits::Min][limits::Rec])
    minPackVState = false;

  if (hitTop || hitUnder || maxCellCState || maxCellCState) {
    trimLastEventMsg();
    snprintf(lastEventMsg,sizeof(lastEventMsg),"%s %dA | ",lastEventMsg, lastMicroAmps/1000000);
  }
  if (hitTop || hitUnder) {
    if (!lastHitCnt)
      lastHitCnt=statusCnt;
    else lastHitCnt++;
  } else
    lastHitCnt = 0;
  hitTop = hitTop && (int32_t)battSets.TopAmps > (lastMicroAmps/1000000) && (statusCnt == lastHitCnt); // We don't want to trigger a hittop 
  hitUnder = hitUnder && (int32_t)-battSets.TopAmps < (lastMicroAmps/1000000) && (statusCnt == lastHitCnt);
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
    stateOfChargeValid = true;
    doFullChg = false;
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
    stateOfChargeValid = true;
  }
  if (stateOfChargeValid && !doFullChg) {
    if (stateOfCharge > battSets.ChargePct)
      maxChargePctState = true;
    else if (stateOfCharge < battSets.ChargePctRec)
      maxChargePctState = false;
  } else
    maxChargePctState = false;

  uint8_t relay[RELAY_TOTAL];
  bool wasLoadsOff = loadsOff;
  bool wasChgOff = chgOff;
  loadsOff = minCellVState || minPackVState || maxCellCState || maxPackCState;
  chgOff = maxChargePctState || maxCellVState || maxPackVState || minCellCState || maxCellCState || minPackCState || maxPackCState;
  for (int8_t y = 0; y < RELAY_TOTAL; y++)
  {
    RelaySettings *rp = &battSets.relays[y];
    if (rp->off)
      relay[y] = LOW;
    else {
      relay[y] = previousRelayState[y]; // don't change it because we might be in the SOC trip/rec area
      switch (rp->type) {
        default: case Relay_Connect: relay[y] = cellsOverDue || (wasLoadsOff && loadsOff && lastMicroAmps < 0) || (wasChgOff && chgOff && lastMicroAmps > 0)?LOW:HIGH; break;
        case Relay_Load:
          if (isFromOff(rp))
            relay[y] = HIGH;
          else if (loadsOff || cellsOverDue || (rp->doSoC && (!stateOfChargeValid || stateOfCharge < rp->trip)))
            relay[y] = LOW; // turn if off
          else if (!rp->doSoC || (rp->doSoC && stateOfChargeValid && stateOfCharge > rp->rec))
            relay[y] = HIGH; // turn it on
          // else leave it as-is
          break;
        case Relay_Charge:
          if (chgOff || cellsOverDue || (rp->doSoC && (!stateOfChargeValid || stateOfCharge > rp->trip)))
            relay[y] = LOW; // off
          else if (!rp->doSoC || (rp->doSoC && stateOfChargeValid && stateOfCharge < rp->rec))
            relay[y] = HIGH; // on
          // else leave it as-is
          break;
      }
    }
  }
  for (int8_t n = 0; n < RELAY_TOTAL; n++)
  {
    if (previousRelayState[n] != relay[n])
    {
      digitalWrite(relayPins[n], relay[n]);
      previousRelayState[n] = relay[n];
    }
  }
  // balance calcs
  if (hitTop) {
    
  }
  if (!cellsOverDue) {
    watchDogHits = 0;
    watchDog.once_ms(CHECKSTATUS+WATCHDOGSLOP,doWatchDog);
  }
}

void emailCallback(SendStatus msg) {
  // Print the current status
  Serial.println(msg.info());

  // Do something when complete
  if (msg.success()) {
    Serial.println("----------------");
  }
}
void onRequest(AsyncWebServerRequest *request){
  //Handle Unknown Request
  request->send(404);
}

String UUID; // unused, maybe later for xss
void GenUUID() {
  uint32_t r;
  UUID = "";
  for (int i=0;i<2;i++) {
    r = esp_random();
    for (int j=0;j<(32/4);j++) {
      UUID += "0123456789abcdef"[r & 0xf];
      r >>= 4;
    }
  }
}

void doCommSettings() {
  smtpData.setLogin(commSets.senderServer, commSets.senderPort, commSets.senderEmail, commSets.senderPW);
  smtpData.setSender("Your Battery", commSets.senderEmail);
  smtpData.setPriority("High");
  smtpData.setSubject(commSets.senderSubject);
  smtpData.addRecipient(commSets.email);
  emailSetup = true;
/*  smtpData.setLogin("smtp.gmail.com", 465, "john.taves@gmail.com","erwsmuigvggpvmtf");
*/
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

void sendSuccess(AsyncWebServerRequest *request,const char* mess=NULL,bool suc=true) { 
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(100);
  doc["success"] = suc;
  doc["errmess"] = mess;
  serializeJson(doc, *response);
  request->send(response);
}

int toCel(String val) {
  if (battSets.doCelsius)
    return val.toInt();
  return (val.toInt() - 32) * 5/9;
}

int fromCel(int c) {
  if (battSets.doCelsius)
    return c;
  return c*9/5+32;
}

void settings(AsyncWebServerRequest *request){
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["apName"] = wifiSets.apName;
  root["email"] = commSets.email;
  root["senderEmail"] = commSets.senderEmail;
  root["senderSubject"] = commSets.senderSubject;
  root["senderServer"] = commSets.senderServer;
  root["senderSubject"] = commSets.senderSubject;
  root["senderPort"] = commSets.senderPort;
  root["logEmail"] = commSets.logEmail;
  root["doLogging"] = commSets.doLogging;
 
  root["Avg"] = battSets.Avg;
  root["PollFreq"] = battSets.PollFreq;
  root["ConvTime"] = battSets.ConvTime;
  root["BattAH"] = battSets.BattAH;
  root["TopAmps"] = battSets.TopAmps;
  root["socLastAdj"] = lastAdjMillAmpHrs;
  snprintf(spb,sizeof(spb),"%d:%d",(int)(aveAdjMilliAmpMillis / ((int64_t)1000 * 60 * 60)),adjCnt);
  root["socAvgAdj"] = spb;
  root["BatAHMeasured"] = BatAHMeasured > 0 ? String(BatAHMeasured) : String("N/A");

  root["MaxAmps"] = battSets.MaxAmps;
  root["ShuntUOhms"] = battSets.ShuntUOhms;
  root["PVMaxAmps"] = battSets.PVMaxAmps;
  root["PVShuntUOhms"] = battSets.PVShuntUOhms;
  root["nCells"] = battSets.nCells;
  root["useCellC"]=battSets.useCellC;
  root["useTemp1"]=battSets.useTemp1;
  root["ChargePct"]=battSets.ChargePct;
  root["ChargePctRec"]=battSets.ChargePctRec;
  root["FloatV"]=battSets.FloatV;
  root["ChargeRate"]=battSets.ChargeRate;
  root["CellsOutMin"]=battSets.CellsOutMin;
  root["CellsOutMax"]=battSets.CellsOutMax;
  root["CellsOutTime"]=battSets.CellsOutTime;

  root["cellCnt"] = cellSets.sets.cnt;
  root["cellDelay"] = cellSets.sets.delay;
  root["cellTime"] = cellSets.sets.time;

  JsonObject obj = root.createNestedObject("limitSettings");
  for (int l0=0;l0<limits::Max0;l0++) {
    for (int l1=0;l1<limits::Max1;l1++) {
      for (int l2=0;l2<limits::Max2;l2++) {
        for (int l3=0;l3<limits::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          if (l0 == limits::Temp)
            obj[name] = fromCel(battSets.limits[l0][l1][l2][l3]);
          else obj[name] = battSets.limits[l0][l1][l2][l3];
        }
      }
    }
  }

  JsonArray rsArray = root.createNestedArray("relaySettings");
  for (uint8_t r = 0; r < RELAY_TOTAL; r++) {
    JsonObject rule1 = rsArray.createNestedObject();
    RelaySettings *rp = &battSets.relays[r];
    rule1["name"] = rp->name;
    rule1["from"] = rp->from;
    switch (rp->type) {
      default: case Relay_Connect: rule1["type"] = "E"; break;
      case Relay_Load: rule1["type"] = (rp->doSoC?"LP":"L"); break;
      case Relay_Charge: rule1["type"] = (rp->doSoC?"CP":(rp->fullChg?"CF":"C")); break;
    }
    
    rule1["trip"] =rp->trip;
    rule1["rec"] =rp->rec;
    rule1["rank"] =rp->rank;
  }

  root["ssid"] = wifiSets.ssid;
  serializeJson(doc, *response);
  request->send(response);
}

void saveOff(AsyncWebServerRequest *request) {
  if (request->hasParam("relay", true)) {
    int r = request->getParam("relay", true)->value().toInt();
    RelaySettings *rp = &battSets.relays[r];
    rp->off = !rp->off;
    writeBattSet = true;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["relay"] = r;
    doc["val"] = rp->off ? "off" : "on";
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}

void clrMaxDiff(AsyncWebServerRequest *request) {
  maxDiffMilliVolts = 0;
  sendSuccess(request);
}

void fullChg(AsyncWebServerRequest *request) {
  doFullChg = !doFullChg;
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(100);
  doc["val"] = doFullChg ? "on" : "off";
  doc["success"] = true;
  serializeJson(doc, *response);
  request->send(response);
}

void dump(AsyncWebServerRequest *request) {
  if (request->hasParam("cell", true)) {
    int r = request->getParam("cell", true)->value().toInt();
    uint32_t h = request->getParam("hrs", true)->value().toInt();
    uint32_t m = request->getParam("min", true)->value().toInt();
    if (cells[r].dumpSecs)
      cells[r].dumpSecs = 0;
    else cells[r].dumpSecs = ((h*60)+m) * 60;
    BLERemoteService* pServ = cells[r].pClient->getService(NimBLEUUID((uint16_t)0x180F));
    if (pServ) {
      NimBLERemoteCharacteristic* pChar = pServ->getCharacteristic(NimBLEUUID((uint16_t)0X2AE2));
      if (pChar) {
        pChar->writeValue<uint32_t>(cells[r].dumpSecs);
        Serial.printf("Write %d %d\n",r,cells[r].dumpSecs);
      }
    }
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}

void fillStatusDoc(JsonVariant root) {
  
  uint32_t upsecs = (milliRolls * 4294967ul) + (millis()/1000ul);
  int days = upsecs / (24ul*60*60);
  upsecs = upsecs % (24ul*60*60);
  int hrs = upsecs / (60*60);
  upsecs = upsecs % (60*60);
  int min = upsecs / 60;

  snprintf(spb,sizeof(spb),"%d:%02d:%02d:%02d",days,hrs,min,upsecs % 60);
  root["uptime"] = spb;
  root["now"]=time(nullptr);
  root["version"] = "V: 0.6";
  root["debugstr"] = debugstr;
  root["lastEventMsg"] = lastEventMsg;
  root["watchDogHits"] = watchDogHits;
  root["RELAY_TOTAL"] = RELAY_TOTAL;
  for (int i=0;i<RELAY_TOTAL;i++) {
    char dodad[16];
    if (strlen(battSets.relays[i].name) == 0)
      continue;
    sprintf(dodad,"relayStatus%d",i);
    root[dodad] = previousRelayState[i]==HIGH?"ON":"OFF";
    sprintf(dodad,"relayName%d",i);
    root[dodad] = battSets.relays[i].name;
    sprintf(dodad,"relayOff%d",i);
    root[dodad] = battSets.relays[i].off ? "off" : "on";
  }

  root["packcurrent"] = lastMicroAmps/1000;
  root["packvolts"] = lastPackMilliVolts;
  root["maxdiffvolts"] = maxDiffMilliVolts;
  root["pvcurrent"] = lastPVMicroAmps/1000;
  snprintf(spb,sizeof(spb),"%d%%",stateOfCharge);
  root["soc"] = spb;
  root["socvalid"] = stateOfChargeValid;
  root["temp1"] = fromCel(curTemp1);
  root["rawTemp1"] = anals[Temp1_Analog].rawValue;
  root["fullChg"] = doFullChg;

  root["maxCellVState"] = maxCellVState;
  root["minCellVState"] = minCellVState;
  root["maxPackVState"] = maxPackVState;
  root["minPackVState"] = minPackVState;
  root["maxCellCState"] = maxCellCState;
  root["minCellCState"] = minCellCState;
  root["maxPackCState"] = maxPackCState;
  root["minPackCState"] = minPackCState;
  root["pkts"] = lastRoundMillis;

  root["nCells"] = battSets.nCells;

  JsonArray data = root.createNestedArray("cells");
  for (uint8_t i = 0; i < battSets.nCells; i++) {
    JsonObject cell = data.createNestedObject();
    CellData *cd = &cells[i];
    cell["c"] = i;
    cell["v"] = cd->v;
    cell["t"] = fromCel(cd->t);
    cell["d"] = cd->dumping;
    cell["l"] = !cd->connected;
  }
}

void status(AsyncWebServerRequest *request){
//Serial.printf("SP: %d, C: %d H: %d\n",uxTaskPriorityGet(NULL),xPortGetCoreID(),uxTaskGetStackHighWaterMark(NULL));
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");

  DynamicJsonDocument doc(4096);
  fillStatusDoc(doc.to<JsonVariant>());
  serializeJson(doc, *response);
  request->send(response);
}

void initChgData() {
  chgDataCnt = 0; // should init all here (save .data space, I think)
  uint16_t mv = MAX_CELLV - (CHG_SAMPS * CHG_STEP);
  for (int i=0;i<CHG_SAMPS;i++) {
    chgData[i].mV = mv;
    mv += CHG_STEP;
  }
}

void saverules(AsyncWebServerRequest *request) {
  int16_t curCellVMax = MAX_CELLV;
  for (int l0=0;l0<limits::Max0;l0++) {
    for (int l1=0;l1<limits::Max1;l1++) {
      for (int l2=0;l2<limits::Max2;l2++) {
        for (int l3=0;l3<limits::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          if (request->hasParam(name, true, false)) {
            if (l0 == limits::Temp)
              battSets.limits[l0][l1][l2][l3] = toCel(request->getParam(name, true, false)->value());
            else battSets.limits[l0][l1][l2][l3] = request->getParam(name, true, false)->value().toInt();
          }
        }
      }
    }
  }
  if (MAX_CELLV != curCellVMax)
    initChgData();

  for (int relay=0;relay<RELAY_TOTAL;relay++) {
    char name[16],type[3];
    RelaySettings *rp = &battSets.relays[relay];
    sprintf(name,"relayName%d",relay);
    if (request->hasParam(name, true))
      request->getParam(name, true)->value().toCharArray(rp->name,sizeof(rp->name));;
    sprintf(name,"relayFrom%d",relay);
    if (request->hasParam(name, true))
      request->getParam(name, true)->value().toCharArray(rp->from,sizeof(rp->from));;
    
    sprintf(name,"relayType%d",relay);
    if (request->hasParam(name, true)) {
      request->getParam(name, true)->value().toCharArray(type,sizeof(type));
      switch (type[0]) {
        default: case 'E':rp->type = Relay_Connect;break;
        case 'L':rp->type = Relay_Load;break;
        case 'C':rp->type = Relay_Charge; break;
      }
      rp->doSoC = type[1] == 'P';
      rp->fullChg = type[1] == 'F';
    }

    sprintf(name,"relayTrip%d",relay);
    if (request->hasParam(name, true))
      rp->trip = request->getParam(name, true)->value().toInt();

    sprintf(name,"relayRec%d",relay);
    if (request->hasParam(name, true))
      rp->rec = request->getParam(name, true)->value().toInt();

  }
  battSets.useTemp1 = request->hasParam("useTemp1", true) && request->getParam("useTemp1", true)->value().equals("on");
  battSets.useCellC = request->hasParam("useCellC", true) && request->getParam("useCellC", true)->value().equals("on");

  if (request->hasParam("ChargePct", true))
    battSets.ChargePct = request->getParam("ChargePct", true)->value().toInt();
  if (request->hasParam("ChargePctRec", true))
    battSets.ChargePctRec = request->getParam("ChargePctRec", true)->value().toInt();
  if (request->hasParam("FloatV", true))
    battSets.FloatV = request->getParam("FloatV", true)->value().toInt();
  if (request->hasParam("ChargeRate", true))
    battSets.ChargeRate = request->getParam("ChargeRate", true)->value().toInt();
  if (request->hasParam("CellsOutMin", true))
    battSets.CellsOutMin = request->getParam("CellsOutMin", true)->value().toInt();
  if (request->hasParam("CellsOutMax", true))
    battSets.CellsOutMax = request->getParam("CellsOutMax", true)->value().toInt();
  if (request->hasParam("CellsOutTime", true))
    battSets.CellsOutTime = request->getParam("CellsOutTime", true)->value().toInt();

  if (!battSets.useCellC) {
    maxCellCState = false;
    minCellCState = false;
  }
  if (!battSets.useTemp1) {
    maxPackCState = false;
    minPackCState = false;
  }
  writeBattSet = true;

  sendSuccess(request);
}

void saveemail(AsyncWebServerRequest *request){
  if (request->hasParam("email", true))
    request->getParam("email", true)->value().toCharArray(commSets.email,sizeof(commSets.email));
  if (request->hasParam("senderEmail", true))
    request->getParam("senderEmail", true)->value().toCharArray(commSets.senderEmail,sizeof(commSets.senderEmail));
  if (request->hasParam("senderPW", true))
    request->getParam("senderPW", true)->value().toCharArray(commSets.senderPW,sizeof(commSets.senderPW));
  if (request->hasParam("senderServer", true))
    request->getParam("senderServer", true)->value().toCharArray(commSets.senderServer,sizeof(commSets.senderServer));
  if (request->hasParam("senderSubject", true))
    request->getParam("senderSubject", true)->value().toCharArray(commSets.senderSubject,sizeof(commSets.senderSubject));
  if (request->hasParam("senderPort", true))
    commSets.senderPort = request->getParam("senderPort", true)->value().toInt();
  if (request->hasParam("logEmail", true))
    request->getParam("logEmail", true)->value().toCharArray(commSets.logEmail,sizeof(commSets.logEmail));
  if (request->hasParam("logPW", true))
    request->getParam("logPW", true)->value().toCharArray(commSets.logPW,sizeof(commSets.logPW));
  commSets.doLogging = request->hasParam("doLogging", true) && request->getParam("doLogging", true)->value().equals("on");

  writeCommSet = true;
  doCommSettings();
  sendSuccess(request);
}

void onconnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println(WiFi.localIP());
}

void WiFiInit() {
  WiFi.persistent(false);
  WiFi.disconnect(true,true);
  if (strlen(wifiSets.apName)) {
    Serial.println(wifiSets.apName);
    WiFi.setHostname(wifiSets.apName);
    WiFi.softAP(wifiSets.apName,wifiSets.apPW,1,1);
  } else WiFi.softAP("ADVBMS_CONTROLLER");
  if (wifiSets.ssid[0] != 0)
    WiFi.begin(wifiSets.ssid,wifiSets.password);
}

void savewifi(AsyncWebServerRequest *request){
  if (request->hasParam("apName", true))
    request->getParam("apName", true)->value().toCharArray(wifiSets.apName,sizeof(wifiSets.apName));
  if (request->hasParam("apPW", true))
    request->getParam("apPW", true)->value().toCharArray(wifiSets.apPW,sizeof(wifiSets.apPW));
  if (strlen(wifiSets.apPW) && strlen(wifiSets.apPW) < 8) {
    sendSuccess(request,"apPW too short, 8 chars or none",false);
    return;
  }

  if (request->hasParam("ssid", true))
    request->getParam("ssid", true)->value().toCharArray(wifiSets.ssid,sizeof(wifiSets.ssid));
  if (request->hasParam("password", true))
    request->getParam("password", true)->value().toCharArray(wifiSets.password,sizeof(wifiSets.password));
  writeWifiSet = true;
  sendSuccess(request);
}

void setStateOfCharge(int64_t val,bool valid) {
  milliAmpMillis = val;
  stateOfChargeValid = valid;
  curAdj = 0;
  adjCnt = 0;
  curAdjMilliAmpMillis[0] = 0;
  lastTrip = 0;
  aveAdjMilliAmpMillis = 0;
  doFullChg = true;
}

void setBattAH() {
  battMilliAmpMillis = (uint64_t)battSets.BattAH * (1000 * 60 * 60) * 1000; // convert to milliampmilliseconds
}

void savecellset(AsyncWebServerRequest *request) {
  if (request->hasParam("cellCnt", true)) {
    AsyncWebParameter *p1 = request->getParam("cellCnt", true);
    cellSets.sets.cnt =p1->value().toInt();
    if (cellSets.sets.cnt < 1)
      cellSets.sets.cnt = 1;
  }
  if (request->hasParam("cellDelay", true)) {
    AsyncWebParameter *p1 = request->getParam("cellDelay", true);
    cellSets.sets.delay =p1->value().toInt();
  }
  if (request->hasParam("cellTime", true)) {
    AsyncWebParameter *p1 = request->getParam("cellTime", true);
    cellSets.sets.time =p1->value().toInt();
  }
  writeCellSets = true;
  sendCellSets();
  sendSuccess(request);
}

void savecapacity(AsyncWebServerRequest *request) {
  if (request->hasParam("CurSOC", true)) {
    AsyncWebParameter *p1 = request->getParam("CurSOC", true);
    if (p1->value().length() > 0) setStateOfCharge((battMilliAmpMillis * p1->value().toInt())/100,true);
  }
  if (request->hasParam("PollFreq", true)) {
    AsyncWebParameter *p1 = request->getParam("PollFreq", true);
    battSets.PollFreq =p1->value().toInt();
    if (battSets.PollFreq < 500)
      battSets.PollFreq = 500;
  }

  if (request->hasParam("Avg", true)) {
    AsyncWebParameter *p1 = request->getParam("Avg", true);
    battSets.Avg =p1->value().toInt();
  }
  if (request->hasParam("ConvTime", true)) {
    AsyncWebParameter *p1 = request->getParam("ConvTime", true);
    battSets.ConvTime =p1->value().toInt();
  }
  if (request->hasParam("BattAH", true)) {
    AsyncWebParameter *p1 = request->getParam("BattAH", true);
    battSets.BattAH =p1->value().toInt();
    setBattAH();
  }
  if (request->hasParam("MaxAmps", true)) {
    AsyncWebParameter *p1 = request->getParam("MaxAmps", true);
    battSets.MaxAmps =p1->value().toInt();
  }
  if (request->hasParam("ShuntUOhms", true)) {
    AsyncWebParameter *p1 = request->getParam("ShuntUOhms", true);
    battSets.ShuntUOhms =p1->value().toInt();
  }

  if (request->hasParam("PVMaxAmps", true)) {
    AsyncWebParameter *p1 = request->getParam("PVMaxAmps", true);
    battSets.PVMaxAmps =p1->value().toInt();
  }
  if (request->hasParam("PVShuntUOhms", true)) {
    AsyncWebParameter *p1 = request->getParam("PVShuntUOhms", true);
    battSets.PVShuntUOhms =p1->value().toInt();
  }
  if (request->hasParam("nCells", true)) {
    AsyncWebParameter *p1 = request->getParam("nCells", true);
    battSets.nCells = p1->value().toInt();
    Serial.printf("NCells: %d\n",battSets.nCells);
    if (battSets.nCells < cellSets.numCells) {
      for (int i=battSets.nCells;i<cellSets.numCells;i++) {
        Serial.printf("Deleting %d\n",i);
        NimBLEDevice::deleteClient(cells[i].pClient);
        cellSets.addrs[i] = NimBLEAddress();
      }
      cellSets.numCells = battSets.nCells;
      writeCellSets = true;
    } else if (battSets.nCells > cellSets.numCells) {
      for (int i=cellSets.numCells;i<battSets.nCells;i++)
        cellSets.addrs[i] = NimBLEAddress();
    }
    writeCellSets = true;
  }
  writeBattSet = true;
  updateINAs = true;

  sendSuccess(request);
}

void hideLastEventMsg(AsyncWebServerRequest *request) {
  lastEventMsg[0] = 0;
  lastEventMsgCnt = 0;
  sendSuccess(request);
}

void toggleTemp(AsyncWebServerRequest *request) {
  battSets.doCelsius = !battSets.doCelsius;
  writeBattSet = true;
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(100);
  doc["val"] = battSets.doCelsius;
  serializeJson(doc, *response);
  request->send(response);
}

void startServer() {
  server.on("/email", HTTP_POST, [](AsyncWebServerRequest *request){
    sendEmail = true;
    smtpData.setMessage("This is a test", true);
    request->send(200, "text/plain", "OK Gonna send it");
  });

  server.on("/toggleTemp", HTTP_GET, toggleTemp);
  server.on("/hideLastEventMsg", HTTP_GET, hideLastEventMsg);
  server.on("/saveemail", HTTP_POST, saveemail);
  server.on("/saveOff", HTTP_POST, saveOff);
  server.on("/fullChg", HTTP_POST, fullChg);
  server.on("/clrMaxDiff", HTTP_GET, clrMaxDiff);
  server.on("/dump", HTTP_POST, dump);
  server.on("/savewifi", HTTP_POST, savewifi);
  server.on("/savecapacity", HTTP_POST, savecapacity);
  server.on("/savecellset", HTTP_POST, savecellset);
  server.on("/saverules", HTTP_POST, saverules);
  server.on("/settings", HTTP_GET, settings);
  server.on("/status", HTTP_GET, status);

  server.serveStatic("/static", SPIFFS, "/static").setLastModified("Mon, 20 Jun 2016 14:00:00 GMT");
  server.serveStatic("/", SPIFFS, "/");
  server.onNotFound(onRequest);
  server.begin();
}

void initBattSets() {
  battSets.ConvTime = 1000;
  battSets.PVMaxAmps = 100;
  battSets.PVShuntUOhms = 500;
  battSets.ShuntUOhms = 167;
  battSets.MaxAmps = 300;
  battSets.nCells=0;
  battSets.doCelsius = true;
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

void readAnalogs() {
  for (int i=0;i<Max_Analog;i++) {
    AnalogInput* ap = &anals[i];
    uint16_t val = analogRead(ap->pin);
    if (analCnt == NUMANALOGSAMPS)
      ap->sumValue -= (uint32_t)ap->vals[curAnal];
    ap->sumValue += val;
    ap->vals[curAnal] = val;
  }
  if (analCnt < NUMANALOGSAMPS)
    analCnt++;
  for (int i=0;i<Max_Analog;i++)
    anals[i].rawValue = anals[i].sumValue / (uint32_t)analCnt;
  curAnal++;
  if (curAnal >= NUMANALOGSAMPS)
    curAnal = 0;
  digitalWrite(RESISTOR_PWR,LOW);
}

void sendStatus() {
//Serial.printf("XP: %d, C: %d H: %d\n",uxTaskPriorityGet(NULL),xPortGetCoreID(),uxTaskGetStackHighWaterMark(NULL));
  http.begin("http://advbms.com/PostData.aspx");
  http.addHeader("Content-Type", "application/json");
  DynamicJsonDocument doc(4096);
  fillStatusDoc(doc.to<JsonVariant>());
  doc["userid"] = 1;
  String json;
  serializeJson(doc, json);
//Serial.println(json);
  http.POST(json);
  String res = http.getString();
  if (res != "OK")
    Serial.println(res);
  http.end();
  taskRunning.clear();
}

void xSendStatus(void* unused) {
  sendStatus();
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(9600);
  EEPROM.begin(EEPROMSize);
  Wire.begin();
  for (int i=0;i<MAX_CELLS;i++) cells[i].pClient = NULL;
  if(!SPIFFS.begin()){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  if (!readEE((uint8_t*)&wifiSets, sizeof(wifiSets), EEPROM_WIFI)) {
    wifiSets.ssid[0] = 0;
    wifiSets.password[0] = 0;
    wifiSets.apName[0] = 0;
    wifiSets.apPW[0] = 0;
  }
  WiFi.onEvent(onconnect, WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
  WiFiInit();
  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new adCB(), false);
  pBLEScan->setMaxResults(0); // do not store the scan results, use callback only.

//  EasyBuzzer.setPin(SPEAKER);

  if (!readEE((uint8_t*)&commSets,sizeof(commSets),EEPROM_COMM)) {
    commSets.email[0] = 0;
    commSets.senderEmail[0] = 0;
    commSets.senderServer[0] = 0;
    commSets.senderPort = 587;
    commSets.senderPW[0] = 0;
    commSets.senderSubject[0] = 0;
    commSets.logEmail[0] = 0;
    commSets.logPW[0] = 0;
    commSets.doLogging = false;
  } else doCommSettings();

  if (!readEE((uint8_t*)&battSets,sizeof(battSets),EEPROM_BATT))
    initBattSets();

  if (!readEE((uint8_t*)&cellSets,sizeof(cellSets),EEPROM_BLE)) {
    cellSets.sets.cnt = 4;
    cellSets.sets.delay = 0;
    cellSets.sets.time = 2000;
    cellSets.numCells = 0;
  }
  Serial.printf("ncells %d\n",cellSets.numCells);
  
  anals[Temp1_Analog].pin = TEMP1;
  for (int i=0;i<Max_Analog;i++) {
    pinMode(anals[i].pin,INPUT);
    anals[i].rawValue = 0;
    anals[i].sumValue = 0;
  }
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RESISTOR_PWR, OUTPUT);
  for (int i=0;i<RELAY_TOTAL;i++)
    pinMode(relayPins[i],OUTPUT);
  initRelays();
  watchDogHits = 0;
  GenUUID();

  INADevs = INA.begin(battSets.MaxAmps, battSets.ShuntUOhms);
  lastShuntMillis = millis();
  setBattAH();
  if (INADevs)
    setINAs();

  smtpData.setSendCallback(emailCallback);
  startServer();
  configTime(0,0,"pool.ntp.org");
  InitOTA();
  initChgData();
  setStateOfCharge((battMilliAmpMillis * 4)/5,false);
  InitCells();
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
  if (writeBattSet) {
    writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);
    writeBattSet = false;
  } else if (writeCommSet) {
    writeEE((uint8_t*)&commSets,sizeof(commSets),EEPROM_COMM);
    writeCommSet = false;
  } else if (writeWifiSet) {
    writeEE((uint8_t*)&wifiSets,sizeof(wifiSets),EEPROM_WIFI);
    WiFiInit();
    writeWifiSet = false;
  } else if (writeCellSets) {
    writeEE((uint8_t*)&cellSets,sizeof(cellSets),EEPROM_BLE);
    WiFiInit();
    writeCellSets = false;
  } else if (cellSets.numCells > 1 && ((millis() - lastSync) > cellSets.sets.time * 3)) {
    uint32_t min=0xffffffff,max=0;
    int i=0;
    for (;i<cellSets.numCells && cells[i].connected;i++) {
      if (cells[i].last > max)
        max = cells[i].last;
      if (cells[i].last < min)
        min = cells[i].last;
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
      lastPVMicroAmps = INA.getBusMicroAmps(1);
      pvPollMS = millis();
    }
  }
  if (!resPwrMS && ((millis() - analogPollMS) > POLLANALOGS)) {
    resPwrMS = millis();
    digitalWrite(RESISTOR_PWR,HIGH); // get current flowing through resistors
  } else if (resPwrMS && (millis() - resPwrMS) > 3) {
    readAnalogs();
    analogPollMS = millis();
    resPwrMS = 0;
  }
  if ((millis() - connectMS) > CHECKCONNECT) {
    ConnectCells();
    connectMS = millis();
  }
  CheckBLEScan();

  if ((millis() - statusMS) > CHECKSTATUS) {
    checkStatus();
    if (!OTAInProg && commSets.doLogging && strlen(commSets.logEmail) && !taskRunning.test_and_set())
      xTaskCreate(xSendStatus,"sendStatus",2000,NULL,0,NULL);
    statusMS = millis();
  }

//  EasyBuzzer.update();
  if (sendEmail && emailSetup && strlen(commSets.senderServer)) {
    if (!MailClient.sendMail(smtpData))
      Serial.println("Error sending Email, " + MailClient.smtpErrorReason());
    sendEmail = false;
  }
  ArduinoOTA.handle(); // this does nothing until it is initialized
}
