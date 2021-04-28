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
#include <EasyBuzzer.h>
#include <PacketSerial.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#include <time.h>
#include <Ticker.h>
#include "defines.h"
#include "SerData.h"

// improve server
// adjustable cell history
// cell dump safety, stop if cell gets low, show dump status
// charge control feature to reduce amps
// improve the SoC allow to run logic. Make the code bad, and see if the charge/loads go off
// email did not work going from unset to sent. A reboot seemed to help
// fix cells so that they recover when being hit with 9600 baud and expected 
// plot V and A over time

#define AVEADJAVGS 16

uint16_t bauds[] = {2400,4800,9600,14400,19200,28800,38400,57600 };

char debugstr[200],lastEventMsg[1024];
int8_t analCnt=0,curAnal=0,lastEventMsgCnt=0;
struct AnalogInput anals[Max_Analog];
bool cellsTimedOut = true,cellsOverDue = true,emailSetup=false,loadsOff = true,chgOff = true, doFullChg = true
  ,updateINAs=false,writeBattSet=false,writeCommSet=false,writeCaliSet=false,writeWifiSet=false;
uint8_t newCellBaud;
uint32_t lastSentMillis=0,sentMillis=1,receivedMillis=0,lastRoundMillis=0,numSent=0,failedSer=0,statusCnt=0,lastHitCnt=0;
uint16_t resPwrMS=0;
HTTPClient http;
Ticker watchDog;
atomic_flag taskRunning(0);
bool OTAInProg = false;

CellData cells[MAX_BANKS][MAX_CELLS];
CellsSerData outBuff;

#define frame (uint8_t)0x00

PacketSerial_<COBS, frame, sizeof(outBuff)+10> dataSer;

const int relayPins[RELAY_TOTAL] = { GPIO_NUM_18,GPIO_NUM_19,GPIO_NUM_23,GPIO_NUM_14,GPIO_NUM_26,GPIO_NUM_27,GPIO_NUM_33,GPIO_NUM_25 };

struct WiFiSettings wifiSets;
struct CommSettings commSets;
struct BattSettings battSets;
struct CaliSettings caliSets;

char spb[1024];

uint32_t ledFlashMS = 0,statusMS=0,shuntPollMS=0,pvPollMS=0,analogPollMS=0;
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

INA_Class         INA;
int INADevs;
int watchDogHits;
int stateOfCharge,milliRolls,curTemp1,curTemp2;
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

int16_t calcStein(uint16_t a,ADCTSet* tVals) {
  if (a == 0 || tVals->bCoef == 0)
    return -101;
  float steinhart = ((float)tVals->adc.range/(float)a - 1.0);

  steinhart = log(steinhart); // ln(R/Ro)
  steinhart /= (float)tVals->bCoef; // 1/B * ln(R/Ro)
  steinhart += 1.0 / (25 + 273.15); // + (1/To)
  steinhart = 1.0 / steinhart; // Invert
    /*
  float r = 10000.0*(adcRange-a)/a;
  float l = log(r/0.01763226979); //0.01763226979 =10000*exp(-3950/298.15)
  float t = 3950.0/l;*/
  int32_t cel = (int)(steinhart-273.15);
  cel += tVals->adc.addr;
  if (tVals->adc.div)
    cel = cel * (int32_t)tVals->adc.mul / (int32_t)tVals->adc.div;
  return cel;
}
/*
void printCell(CellSerData *c) {
  Serial.print("D:");
  Serial.print(c->dump);
  Serial.print(" U:");
  Serial.print(c->used);
  Serial.print(" V:");
  Serial.print(c->v);
  Serial.print(" T:");
  Serial.println(c->t);
}
int cnt=0;
*/

void onSerData(const uint8_t *receivebuffer, size_t len)
{
//  Serial.print("Got something:");  Serial.print(len);  Serial.print(":");  Serial.println(cnt++);
  if (!len)
    return;
  digitalWrite(GREEN_LED,HIGH);
  CellsSerData* cc = (CellsSerData*)receivebuffer;
  uint8_t* p = (uint8_t*)cc;
  int nCells = (len - sizeof(CellsHeader))/sizeof(CellSerData);
  if (cc->hdr.crc != CRC8(p+1, len - 1)) {
    Serial.print("F");
    for (int i=0;i<len;i++) {
      Serial.print(":");
      Serial.print(receivebuffer[i]);
    }
    Serial.println();
    failedSer++;
  } else if (cc->hdr.cmd) {
    if (cc->hdr.arg != commSets.cellBaud) {
      commSets.cellBaud = cc->hdr.arg;
      writeCommSet = true;
    }
    Serial2.begin(bauds[cc->hdr.arg], SERIAL_8N1); // Serial for comms to modules
  } else {
    uint16_t lastPackSumMilliVolts = 0;
    for (int i=0;i<nCells;i++) {
      CellData *p = &cells[cc->hdr.arg][i];
      CellSerData *c = &cc->cells[i];
      if (c->used) {
        p->rawV = c->v;
        p->rawT = c->t;
        p->dumping = c->dump;
        c->dump = false;
        p->fails += c->fails;
        p->v = (uint16_t)((((uint32_t)p->rawV + caliSets.cells[i].vSet.addr) * caliSets.cells[i].vSet.mul) / caliSets.cells[i].vSet.div);
        p->t = calcStein(p->rawT,&caliSets.cells[i].tSet);
        //printCell(&cc->cells[i]);
      } else {
        p->v = 0;
        p->t = -101;
        p->dumping = 0;
      }
      lastPackSumMilliVolts += p->v;
    }
    if (abs(lastPackSumMilliVolts - lastPackMilliVolts) > abs(maxDiffMilliVolts))
      maxDiffMilliVolts = lastPackSumMilliVolts - lastPackMilliVolts;
    receivedMillis = millis();
    lastRoundMillis = receivedMillis-sentMillis;
    sentMillis = 0;

  }

  digitalWrite(GREEN_LED,LOW);
}

bool doShutOffNoStatus(uint32_t t) {
  return ((uint32_t)battSets.CellsOutTime < ((millis() - t)*1000) || !stateOfChargeValid || stateOfCharge < battSets.CellsOutMin || stateOfCharge > battSets.CellsOutMax);
}

void initRelays(bool fullStop) {
  for (int i=0;i<RELAY_TOTAL;i++) {
    digitalWrite(relayPins[i], LOW);
    previousRelayState[i] = LOW;
    if (fullStop)
      battSets.relays[i].off = true;
  }
}

void doPollCells(int bank)
{
  digitalWrite(GREEN_LED,HIGH);
  int len;

  uint8_t* p = (uint8_t*)&outBuff;
  if (newCellBaud != commSets.cellBaud) {
    len = sizeof(CellsHeader);
    outBuff.hdr.cmd = 1;
    outBuff.hdr.arg = newCellBaud;
    outBuff.hdr.ver = 0;
  } else {
    len = sizeof(CellsHeader) + (sizeof(CellSerData) * battSets.nCells);
    Serial2.write(frame);
    delay(3); // It would be nice to get rid of this.

    memset(&outBuff,0,sizeof(outBuff));
    uint32_t curMillis = millis();
    receivedMillis = 0;
    cellsTimedOut = sentMillis > 0;
    cellsOverDue = cellsTimedOut && doShutOffNoStatus(lastSentMillis);
    if (cellsOverDue) {
      initRelays(false);
      trimLastEventMsg();
      snprintf(lastEventMsg,sizeof(lastEventMsg),"%s overdue | ",lastEventMsg);
    }
    if (!cellsTimedOut)
      lastSentMillis = curMillis;
    sentMillis = curMillis;
    numSent++;
    outBuff.hdr.ver = VER;
    outBuff.hdr.arg = bank;
    for (int i=0;!cellsTimedOut && i<battSets.nCells;i++)
      outBuff.cells[i].dump=cells[0][i].dump?1:0;
  }
  outBuff.hdr.crc = CRC8(p+1, len - 1);
  dataSer.send((byte *)&outBuff, len);
  digitalWrite(GREEN_LED,LOW);
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
    initRelays(false);
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

void checkStatus()
{
  statusCnt++;
  curTemp1 = calcStein(anals[Temp1_Analog].rawValue,&caliSets.temps[Temp1]);
  curTemp2 = calcStein(anals[Temp2_Analog].rawValue,&caliSets.temps[Temp2]);
  if (INADevs > 0) {
    if ((lastMicroAmps > 0 && chgOff) || (lastMicroAmps < 0 && loadsOff)) {
      if (!inAlertState) {
        EasyBuzzer.beep(800);
        sendEmail = true;
        uint16_t maxCellV = 0;
        uint16_t minCellV = 0xffff;
        for (int i=0;i<battSets.nBanks;i++)
          for (int j=0;j<battSets.nCells;j++) {
            uint16_t cellV = cells[i][j].v;
            if (cellV > maxCellV)
              maxCellV = cellV;
            if (cellV < minCellV)
              minCellV = cellV;
          }
        snprintf(spb,sizeof(spb),"uA=%d, chg: %d, Lds: %d, pack: %dmV, max cell: %dmV, min cell: %dmV, MxPV: %d, MxCV: %d, MnPV: %d, MnCV %d, MxCC: %d, MxPC: %d"
            ,lastMicroAmps,chgOff,loadsOff,(int)lastPackMilliVolts,(int)maxCellV,(int)minCellV
            ,maxPackVState,maxCellVState,minPackVState,minCellVState,maxCellCState,maxPackCState);
        smtpData.setMessage(spb, true);
        initRelays(false);
        inAlertState = true;
      }
    } else if (inAlertState) {
      inAlertState = false;
      smtpData.setMessage("OK", true);
      sendEmail = true;
      EasyBuzzer.stopBeep();
    }
  }

  if (INADevs > 0) {
    if (battMilliAmpMillis != 0)
      stateOfCharge = milliAmpMillis * 100 / battMilliAmpMillis;
    lastPackMilliVolts = INA.getBusMilliVolts(0);
  }
  if (battSets.nBanks > 0 && (INADevs == 0 || lastPackMilliVolts < 1000)) { // low side shunt
    lastPackMilliVolts = 0;
    for (int8_t i = 0; i < battSets.nCells; i++)
      lastPackMilliVolts += cells[0][i].v;
  }

  bool allovervoltrec = true,allundervoltrec = true,hitTop=false,hitUnder=false;
  bool allovertemprec = true,allundertemprec = true;
  int maxCell,maxBank;
  uint16_t maxMV = 0;
  //Loop through cells
  for (int8_t m=0;m<battSets.nBanks && !cellsTimedOut;m++)
    for (int8_t i = 0; i < battSets.nCells; i++)
    {
      uint16_t cellV = cells[m][i].v;
      if (cellV > maxMV) {
        maxMV = cellV;
        maxCell = i;
        maxBank = m;
      }

      if (lastMicroAmps > 0 && chgDataCnt < CHG_SAMPS && chgData[chgDataCnt].mV < cellV) {
        chgData[chgDataCnt].mV = cellV;
        chgData[chgDataCnt].bank = m;
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

      int8_t cellT = cells[m][i].t;
      if (battSets.useCellC && cellT != -40) {
        if (cellT > battSets.limits[limits::Temp][limits::Cell][limits::Max][limits::Trip]) {
          maxCellCState = true;
          snprintf(lastEventMsg,sizeof(lastEventMsg),"%s #%d %dC",lastEventMsg,i, cellT);

        }

        if (cellT > battSets.limits[limits::Temp][limits::Cell][limits::Max][limits::Rec])
          allovertemprec = false;

        if (cellT < battSets.limits[limits::Temp][limits::Cell][limits::Min][limits::Trip])
          minCellCState = true;

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
  if (battSets.useTemp2) {
    if (curTemp2 > battSets.limits[limits::Temp][limits::Pack][limits::Max][limits::Trip])
      maxPackCState = true;
    if (curTemp2 < battSets.limits[limits::Temp][limits::Pack][limits::Max][limits::Rec])
      maxPackCState = false;
    if (curTemp2 < battSets.limits[limits::Temp][limits::Pack][limits::Min][limits::Trip])
      minPackCState = true;
    if (curTemp2 > battSets.limits[limits::Temp][limits::Pack][limits::Min][limits::Rec])
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

  if (hitTop || hitUnder) {
    trimLastEventMsg();
    snprintf(lastEventMsg,sizeof(lastEventMsg),"%s %dA | ",lastEventMsg, lastMicroAmps/1000000);
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
  watchDogHits = 0;
  watchDog.once_ms(CHECKSTATUS+WATCHDOGSLOP,doWatchDog);
  doPollCells(0);
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

void setADCJson(JsonObject temp,ADCSet* a,const char* pre) {
  char name[16];
  sprintf(name,"%saddr",pre);
  temp[name] = a->addr;
  sprintf(name,"%smul",pre);
  temp[name] = a->mul;
  sprintf(name,"%sdiv",pre);
  temp[name] = a->div;
  sprintf(name,"%srange",pre);
  temp[name] = a->range;
}

void CalcADC(ADCSet *s,int cell) {
    uint32_t sumx = 0,sumy = 0,mx,my;
    uint32_t sumsx,sump;
    int n=1;
    for (int s=0;s<MAX_SAMPS;s++) {
      if (caliSets.cms[0][cell][s].mv) {
        sumx += caliSets.cms[0][cell][s].rawV;
        sumy += caliSets.cms[0][cell][s].mv;
        n++;
      }
    }
    mx = sumx / n;
    my = sumy / n;
    sumsx = mx * mx;
    sump = mx * my;
    for (int s=0;s<MAX_SAMPS;s++) {
      if (!caliSets.cms[0][cell][s].mv) continue;
      int32_t q = caliSets.cms[0][cell][s].rawV-mx;
      int32_t r = caliSets.cms[0][cell][s].mv-my;
      sumsx += q * q;
      sump += q * r;
    }
    s->mul = 44200;
    s->div = 10000;
    s->addr = 0;
    if (sumsx) {
      s->mul = (uint16_t)((uint64_t)sump * 10000 / sumsx);
      s->addr = my - ((uint32_t)s->mul * mx)/10000;
    }
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
  root["cellBaud"] = commSets.cellBaud;
 
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
  root["nBanks"] = battSets.nBanks;
  root["nCells"] = battSets.nCells;
  JsonArray temps = root.createNestedArray("tempSettings");
  for (int i=0;i<MAX_TEMPS;i++) {
    JsonObject temp = temps.createNestedObject();
    temp["bCoef"] = caliSets.temps[i].bCoef;
    setADCJson(temp,&caliSets.temps[i].adc,"");
  }
  temps = root.createNestedArray("cellSettings");
  for (int i=0;i<battSets.nCells;i++) {
    JsonObject temp = temps.createNestedObject();
    temp["bCoef"] = caliSets.cells[i].tSet.bCoef;
    setADCJson(temp,&caliSets.cells[i].tSet.adc,"cellC");
    setADCJson(temp,&caliSets.cells[i].vSet,"cell");
    for (int s=0;s<MAX_SAMPS;s++) {
      if (caliSets.cms[0][i][s].mv) {
        snprintf(spb,sizeof(spb),"samp%d",s);
        temp[spb] = caliSets.cms[0][i][s].mv;
        snprintf(spb,sizeof(spb),"sampadc%d",s);
        temp[spb] = caliSets.cms[0][i][s].rawV;
        snprintf(spb,sizeof(spb),"sampt%d",s);
        temp[spb] = fromCel(caliSets.cms[0][i][s].tempC);
      }
    }
    ADCSet adc;
    CalcADC(&adc,i);
    temp["sampb"] = adc.mul;
    temp["sampa"] = adc.addr;
  }
  root["useCellC"]=battSets.useCellC;
  root["useTemp1"]=battSets.useTemp1;
  root["useTemp2"]=battSets.useTemp2;
  root["ChargePct"]=battSets.ChargePct;
  root["ChargePctRec"]=battSets.ChargePctRec;
  root["FloatV"]=battSets.FloatV;
  root["ChargeRate"]=battSets.ChargeRate;
  root["CellsOutMin"]=battSets.CellsOutMin;
  root["CellsOutMax"]=battSets.CellsOutMax;
  root["CellsOutTime"]=battSets.CellsOutTime;

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
    cells[0][r].dump = !cells[0][r].dump;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["relay"] = r;
    doc["val"] = cells[0][r].dump ? "on" : "off";
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
  root["temp2"] = fromCel(curTemp2);
  root["fsr"] = anals[FSR_Analog].rawValue;
  root["rawTemp1"] = anals[Temp1_Analog].rawValue;
  root["rawTemp2"] = anals[Temp2_Analog].rawValue;
  root["fullChg"] = doFullChg;

  root["maxCellVState"] = maxCellVState;
  root["minCellVState"] = minCellVState;
  root["maxPackVState"] = maxPackVState;
  root["minPackVState"] = minPackVState;
  root["maxCellCState"] = maxCellCState;
  root["minCellCState"] = minCellCState;
  root["maxPackCState"] = maxPackCState;
  root["minPackCState"] = minPackCState;
  root["nocells"] = cellsTimedOut;
  root["pkts"] = lastRoundMillis;

  root["nBanks"] = battSets.nBanks;
  root["nCells"] = battSets.nCells;

  JsonArray data = root.createNestedArray("cells");
  for (uint8_t bank = 0; bank < battSets.nBanks; bank++) {

    for (uint8_t i = 0; i < battSets.nCells; i++) {
      JsonObject cell = data.createNestedObject();
      CellData *cd = &cells[bank][i];
      cell["b"] = bank;
      cell["c"] = i;
      cell["v"] = cd->v;
      cell["t"] = fromCel(cd->t);
      cell["d"] = cd->dumping;
      cell["rv"] = cd->rawV;
      cell["rt"] = cd->rawT;
      cell["f"] = cd->fails;
    }
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
  battSets.useTemp2 = request->hasParam("useTemp2", true) && request->getParam("useTemp2", true)->value().equals("on");
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
  if (!battSets.useTemp1 && !battSets.useTemp2) {
    maxPackCState = false;
    minPackCState = false;
  }
  writeBattSet = true;

  sendSuccess(request);
}

void apply(AsyncWebServerRequest *request){
  if (request->hasParam("cell", true)) {
    int cell = request->getParam("cell", true)->value().toInt();
    ADCSet adc;
    adc.range = caliSets.cells[cell].vSet.range;
    CalcADC(&adc,cell);
    caliSets.cells[cell].vSet = adc;
    writeCaliSet = true;
  }
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
  if (request->hasParam("cellBaud", true))
    newCellBaud = request->getParam("cellBaud", true)->value().toInt();
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

void getADCSet(AsyncWebServerRequest *request,ADCSet* rp,int i,const char* pre) {
  char name[16];
  sprintf(name,"%saddr%d",pre,i);
  if (request->hasParam(name, true))
    rp->addr = request->getParam(name, true)->value().toInt();

  sprintf(name,"%smul%d",pre,i);
  if (request->hasParam(name, true))
    rp->mul = request->getParam(name, true)->value().toInt();

  sprintf(name,"%sdiv%d",pre,i);
  if (request->hasParam(name, true))
    rp->div = request->getParam(name, true)->value().toInt();

  sprintf(name,"%srange%d",pre,i);
  if (request->hasParam(name, true))
    rp->range = request->getParam(name, true)->value().toInt();

}
void savecali(AsyncWebServerRequest *request) {
  for (int i=0;i<MAX_TEMPS;i++) {
    char name[16];
    ADCTSet *rp = &caliSets.temps[i];
    sprintf(name,"bCoef%d",i);
    if (request->hasParam(name, true))
      rp->bCoef = request->getParam(name, true)->value().toInt();
    getADCSet(request,&rp->adc,i,"");
  }
  for (int i=0;i<battSets.nCells;i++) {
    char name[16];
    CellADCs *rp = &caliSets.cells[i];
    sprintf(name,"cellCbCoef%d",i);
    if (request->hasParam(name, true))
      rp->tSet.bCoef = request->getParam(name, true)->value().toInt();
    
    getADCSet(request,&rp->tSet.adc,i,"cellC");
    getADCSet(request,&rp->vSet,i,"cell");
    for (int s=0;s<MAX_SAMPS;s++) {
      snprintf(spb,sizeof(spb),"samp%d_%d",s,i);
      if (request->hasParam(spb, true)) {
        uint16_t val = 0;
        val = request->getParam(spb, true)->value().toInt();
        if (val != caliSets.cms[0][i][s].mv) {
          caliSets.cms[0][i][s].mv = val;
          caliSets.cms[0][i][s].rawV = cells[0][i].rawV;
          caliSets.cms[0][i][s].tempC = cells[0][i].t;
        }
      }
    }
  }
  writeCaliSet = true;

  sendSuccess(request);
}

void setBattAH() {
  battMilliAmpMillis = (uint64_t)battSets.BattAH * (1000 * 60 * 60) * 1000; // convert to milliampmilliseconds
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
  if (request->hasParam("nBanks", true)) {
    AsyncWebParameter *p1 = request->getParam("nBanks", true);
    battSets.nBanks =p1->value().toInt();
  }
  if (request->hasParam("nCells", true)) {
    AsyncWebParameter *p1 = request->getParam("nCells", true);
    battSets.nCells =p1->value().toInt();
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
  server.on("/resetBaud", HTTP_POST, [](AsyncWebServerRequest *request){
    newCellBaud = commSets.cellBaud = 0;
    writeCommSet = true;
    Serial2.begin(bauds[commSets.cellBaud],SERIAL_8N1);
    request->send(200, "text/plain", "OK");
  });

  server.on("/toggleTemp", HTTP_GET, toggleTemp);
  server.on("/hideLastEventMsg", HTTP_GET, hideLastEventMsg);
  server.on("/saveemail", HTTP_POST, saveemail);
  server.on("/apply", HTTP_POST, apply);
  server.on("/saveOff", HTTP_POST, saveOff);
  server.on("/fullChg", HTTP_POST, fullChg);
  server.on("/clrMaxDiff", HTTP_GET, clrMaxDiff);
  server.on("/dump", HTTP_POST, dump);
  server.on("/savewifi", HTTP_POST, savewifi);
  server.on("/savecapacity", HTTP_POST, savecapacity);
  server.on("/savecali", HTTP_POST, savecali);
  server.on("/saverules", HTTP_POST, saverules);
  server.on("/settings", HTTP_GET, settings);
  server.on("/status", HTTP_GET, status);

  server.serveStatic("/static", SPIFFS, "/static").setLastModified("Mon, 20 Jun 2016 14:00:00 GMT");
  server.serveStatic("/", SPIFFS, "/");
  server.onNotFound(onRequest);
  server.begin();
}

void initADCSet(ADCSet* a,uint16_t r,int16_t m,int16_t d) {
    a->range = r;
    a->addr = 0;
    a->mul = m;
    a->div = d;
}
void initCali() {
  for (int i=0;i<MAX_TEMPS;i++) {
    caliSets.temps[i].bCoef = 3950;
    initADCSet(&caliSets.temps[i].adc,4095,1,1);
  }
  for (int i=0;i<MAX_CELLS;i++) {
    caliSets.cells[i].tSet.bCoef = 4050;
    initADCSet(&caliSets.cells[i].tSet.adc,1023,1,1);
    initADCSet(&caliSets.cells[i].vSet,1023,442,100);
    memset(caliSets.cms,0,sizeof(caliSets.cms));
  }
}

void initBattSets() {
  battSets.ConvTime = 1000;
  battSets.PVMaxAmps = 100;
  battSets.PVShuntUOhms = 500;
  battSets.ShuntUOhms = 167;
  battSets.MaxAmps = 300;
  battSets.nBanks=0;
  battSets.nCells=0;
  battSets.doCelsius = true;
  battSets.PollFreq = 500;
  battSets.BattAH = 1;
  battSets.ConvTime = 1000;
  battSets.useTemp1 = true;
  battSets.useTemp2 = true;
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
  Serial.println("Wire begin");
  Wire.begin();
  Serial.println("Wire good");
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
  Serial.println("wifi start");
  WiFiInit();
  Serial.println("Wfif end");
  EasyBuzzer.setPin(SPEAKER);

  if (!readEE((uint8_t*)&commSets,sizeof(commSets),EEPROM_COMM)) {
    commSets.email[0] = 0;
    commSets.senderEmail[0] = 0;
    commSets.senderServer[0] = 0;
    commSets.senderPort = 587;
    commSets.senderPW[0] = 0;
    commSets.senderSubject[0] = 0;
    commSets.logEmail[0] = 0;
    commSets.logPW[0] = 0;
    commSets.cellBaud = 0;
    commSets.doLogging = false;
  } else doCommSettings();
  Serial2.begin(bauds[commSets.cellBaud], SERIAL_8N1); // Serial for comms to modules
  newCellBaud = commSets.cellBaud;

  if (!readEE((uint8_t*)&battSets,sizeof(battSets),EEPROM_BATT))
    initBattSets();

  if (!readEE((uint8_t*)&caliSets,sizeof(caliSets),EEPROM_CALI))
    initCali();

  dataSer.setStream(&Serial2); // start serial for output
  dataSer.setPacketHandler(&onSerData);

  anals[Temp1_Analog].pin = TEMP1;
  anals[Temp2_Analog].pin = TEMP2;
  anals[FSR_Analog].pin = FSR;
  for (int i=0;i<Max_Analog;i++) {
    pinMode(anals[i].pin,INPUT);
    anals[i].rawValue = 0;
    anals[i].sumValue = 0;
  }
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  pinMode(RESISTOR_PWR, OUTPUT);
  for (int i=0;i<RELAY_TOTAL;i++)
    pinMode(relayPins[i],OUTPUT);
  initRelays(true);
  watchDogHits = 0;
  GenUUID();

  INADevs = INA.begin(battSets.MaxAmps, battSets.ShuntUOhms);
  lastShuntMillis = millis();
  setBattAH();
  if (INADevs)
    setINAs();

  if (battSets.nCells)
    doPollCells(0);
  smtpData.setSendCallback(emailCallback);
  startServer();
  configTime(0,0,"pool.ntp.org");
  InitOTA();
  initChgData();
  setStateOfCharge((battMilliAmpMillis * 4)/5,false);
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
  } else if (writeCaliSet) {
    writeEE((uint8_t*)&caliSets, sizeof(caliSets), EEPROM_CALI);
    writeCaliSet = false;
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
  dataSer.update();
  if (!resPwrMS && ((millis() - analogPollMS) > POLLANALOGS)) {
    resPwrMS = millis();
    digitalWrite(RESISTOR_PWR,HIGH); // get current flowing through resistors
  } else if (resPwrMS && (millis() - resPwrMS) > 3) {
    readAnalogs();
    analogPollMS = millis();
    resPwrMS = 0;
  }
  if ((millis() - statusMS) > CHECKSTATUS) {
    checkStatus();
    if (!OTAInProg && commSets.doLogging && strlen(commSets.logEmail) && !taskRunning.test_and_set())
      xTaskCreate(xSendStatus,"sendStatus",2000,NULL,0,NULL);
    statusMS = millis();
  }

  EasyBuzzer.update();
  if ((millis() - ledFlashMS) > 2000) {
    digitalWrite(BLUE_LED,ledState ? LOW : HIGH);
    ledState = !ledState;
    ledFlashMS = millis();
  }
  if (sendEmail && emailSetup && strlen(commSets.senderServer)) {
    if (!MailClient.sendMail(smtpData))
      Serial.println("Error sending Email, " + MailClient.smtpErrorReason());
    sendEmail = false;
  }
  ArduinoOTA.handle(); // this does nothing until it is initialized
}
