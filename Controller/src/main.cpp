#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <EEPROM.h>
#include <INA.h>
#include "ArduinoJson.h"
#include "ESP32_MailClient.h"
#include <Wire.h>
//#include <EasyBuzzer.h>
#include <PacketSerial.h>
#include <time.h>
#include "defines.h"
#include "SerData.h"

#define GREEN_LED GPIO_NUM_4
#define BLUE_LED GPIO_NUM_0
#define RESISTOR_PWR GPIO_NUM_13
#define TEMP1 GPIO_NUM_34
#define TEMP2 GPIO_NUM_35
#define FSR GPIO_NUM_32
#define SPEAKER GPIO_NUM_19

// cell v adjust
// data dump
// ding server
// adjustable cell history
// cell dump safety, stop if cell gets low, show dump status
// break out battsets

bool cellsTimedOut,emailSetup=false,loadsOff = false,chgOff = false,resPwrOn=false;
uint8_t sentID=0;
uint32_t sentMillis,receivedMillis=0,lastRoundMillis=0,numSent=0,failedSer=0;
uint16_t resPwrMS=10;

struct CellData {
  uint16_t v,t,rawV,rawT;
  bool dump,dumping;
};
CellData cells[MAX_BANKS][MAX_CELLS];
CellsSerData outBuff;

#define frame (uint8_t)0x00

PacketSerial_<COBS, frame, sizeof(outBuff)+10> dataSer;

const int relayPins[RELAY_TOTAL] = { GPIO_NUM_23,GPIO_NUM_14,GPIO_NUM_27,GPIO_NUM_26,GPIO_NUM_25,GPIO_NUM_33 };

struct WiFiSettings wifiSets;
struct EmailSettings eSets;
struct BattSettings battSets;
struct SensSettings sensSets;

char spb[1024];

uint32_t ledFlashMS = 0,statusMS=0,shuntPollMS=0,pvPollMS=0;
bool ledState = false;
bool sendEmail = false,inAlertState = false;
AsyncWebServer server(80);
SMTPData smtpData;
uint8_t previousRelayState[RELAY_TOTAL];
String emailRes = "";

INA_Class         INA;
int INADevs;
uint16_t rawTemp1,rawTemp2,rawFSR;
int stateOfCharge,milliRolls,curTemp1,curTemp2;
int browserNBanks=-1,browserNCells=-1;
int32_t lastMicroAmps,lastAdjMillAmpHrs = 0,lastPVMicroAmps=0;
uint32_t lastMillis=0,lastShuntMillis;
int64_t milliAmpMillis,battMilliAmpMillis;
uint16_t lastPackMilliVolts = 0xffff;
int64_t aveAdjMilliAmpMillis = 0,curAdjMilliAmpMillis = 0;
uint32_t numAveAdj = 0,BatAHMeasured = 0;
int lastTrip = 0;
bool stateOfChargeValid=false;
bool maxCellVState,minCellVState
  ,maxPackVState,minPackVState
  ,maxCellCState=false,minCellCState=false
  ,maxPackCState,minPackCState;

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

void getAmps() {
  lastMicroAmps = -INA.getBusMicroAmps(0);
  uint32_t thisMillis = millis();
  milliAmpMillis += (int64_t)lastMicroAmps * (thisMillis - lastShuntMillis) / 1000;
  lastShuntMillis = thisMillis;
  if (milliAmpMillis < 0) {
    curAdjMilliAmpMillis -= milliAmpMillis;
    milliAmpMillis = 0;
  } else if (milliAmpMillis > battMilliAmpMillis) {
    curAdjMilliAmpMillis += battMilliAmpMillis - milliAmpMillis;
    milliAmpMillis = battMilliAmpMillis;
  }
}

int calcStein(int a,ADCTSet* tVals) {
  if (a == 0 || tVals->bCoef == 0)
    return -101;
  a += tVals->addr;
  if (tVals->div)
    a = a * tVals->mul / tVals->div;
  float steinhart = ((float)tVals->range/(float)a - 1.0);

  steinhart = log(steinhart); // ln(R/Ro)
  steinhart /= (float)tVals->bCoef; // 1/B * ln(R/Ro)
  steinhart += 1.0 / (25 + 273.15); // + (1/To)
  steinhart = 1.0 / steinhart; // Invert
    /*
  float r = 10000.0*(adcRange-a)/a;
  float l = log(r/0.01763226979); //0.01763226979 =10000*exp(-3950/298.15)
  float t = 3950.0/l;*/
  return (int)(steinhart-273.15);
}

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
void onSerData(const uint8_t *receivebuffer, size_t len)
{
  // Serial.print("Got something:");  Serial.print(len);  Serial.print(":");  Serial.println(cnt++);
  if (!len)
    return;
  digitalWrite(GREEN_LED,HIGH);
  CellsSerData* cc = (CellsSerData*)receivebuffer;
  int nCells = (len - sizeof(CellsHeader))/sizeof(CellSerData);
  uint8_t* p = (uint8_t*)cc;
  if (nCells > 0 && nCells <= MAX_CELLS && len > 0 && cc->hdr.crc == CRC8(p+1, len - 1)/* && sentID == cc->id */) {
    lastPackMilliVolts = 0;
    for (int i=0;i<nCells;i++) {
      CellData *p = &cells[cc->hdr.bank][i];
      p->rawV = cc->cells[i].v;
      p->rawT = cc->cells[i].t;
      p->dumping = cc->cells[i].dump;
      cc->cells[i].dump = false;
      p->v = (uint16_t)(((uint32_t)p->rawV * 2 * 22100) / 10000);
      p->t = calcStein(p->rawT,&sensSets.temps[TempC]);
      //printCell(&cc->cells[i]);
      lastPackMilliVolts += p->v;
    }
    receivedMillis = millis();
    lastRoundMillis = receivedMillis-sentMillis;
    sentMillis = 0;
  } else {
    Serial.print("F");
    for (int i=0;i<len;i++) {
      Serial.print(":");
      Serial.print(receivebuffer[i]);
    }
    Serial.println();
    failedSer++;
  }

  digitalWrite(GREEN_LED,LOW);
}

void doPollCells()
{
  digitalWrite(GREEN_LED,HIGH);

  uint8_t* p = (uint8_t*)&outBuff;
  int len = sizeof(CellsHeader) + (sizeof(CellSerData) * battSets.nCells);
  Serial2.write(frame);
  delay(3); // It would be nice to get rid of this.

  memset(&outBuff,0,sizeof(outBuff));
  receivedMillis = 0;
  cellsTimedOut = sentMillis > 0;
  sentMillis = millis();
  numSent++;
  outBuff.hdr.id = ++sentID;
  outBuff.hdr.bank = 0;
  for (int i=0;!cellsTimedOut && i<battSets.nCells;i++)
    outBuff.cells[i].dump=cells[0][i].dump?1:0;
  outBuff.hdr.crc = CRC8(p+1, len - 1);
  dataSer.send((byte *)&outBuff, len);
  digitalWrite(GREEN_LED,LOW);
}

void doAHCalcs() {
  aveAdjMilliAmpMillis = ((aveAdjMilliAmpMillis*numAveAdj) + curAdjMilliAmpMillis) / (numAveAdj+1);
  numAveAdj++;
  lastAdjMillAmpHrs = (int32_t)(curAdjMilliAmpMillis / ((int64_t)1000 * 60 * 60));
  curAdjMilliAmpMillis = 0;
}

void checkStatus()
{
  if (INADevs > 0) {
    if ((lastMicroAmps > 0 && chgOff) || (lastMicroAmps < 0 && loadsOff)) {
      if (!inAlertState) {
  Serial.printf("%d %d %d %d\n",maxPackVState,maxCellVState,minPackVState,minCellVState);
 //       EasyBuzzer.beep(800);
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
        snprintf(spb,sizeof(spb),"uA=%d, pack: %dmV, max cell: %dmV, min cell: %dmV",lastMicroAmps,(int)lastPackMilliVolts,(int)maxCellV,(int)minCellV);
        smtpData.setMessage(spb, true);
        inAlertState = true;
      }
    } else if (inAlertState) {
      inAlertState = false;
  Serial.println("Not in");
      smtpData.setMessage("OK", true);
      sendEmail = true;
  //    EasyBuzzer.stopBeep();
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

  bool allovervoltrec = true,allundervoltrec = true,hitOver=false,hitUnder=false;
  bool allovertemprec = true,allundertemprec = true;
  //Loop through cells
  for (int8_t m=0;m<battSets.nBanks && !cellsTimedOut;m++)
    for (int8_t i = 0; i < battSets.nCells; i++)
    {
      uint16_t cellV = cells[m][i].v;

      if (cellV > battSets.limits[limits::Volt][limits::Cell][limits::Max][limits::Trip]) {
        if (!maxCellVState)
          hitOver = true;
        maxCellVState = true;
      }
      if (cellV > battSets.limits[limits::Volt][limits::Cell][limits::Max][limits::Rec])
        allovervoltrec = false;

      if (cellV < battSets.limits[limits::Volt][limits::Cell][limits::Min][limits::Trip]) {
        if (!minCellVState)
          hitUnder = true;
        minCellVState = true;
      }
      if (cellV < battSets.limits[limits::Volt][limits::Cell][limits::Min][limits::Rec])
        allundervoltrec = false;

      int8_t cellT = cells[m][i].t;
      if (battSets.useCellC && cellT != -40) {
        if (cellT > battSets.limits[limits::Temp][limits::Cell][limits::Max][limits::Trip])
          maxCellCState = true;

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

  if (battSets.useTempC) {
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
    if (!maxPackVState)
      hitOver = true;
    maxPackVState = true;
  } else if (lastPackMilliVolts < battSets.limits[limits::Volt][limits::Pack][limits::Max][limits::Rec])
    maxPackVState = false;

  if (lastPackMilliVolts < battSets.limits[limits::Volt][limits::Pack][limits::Min][limits::Trip]) {
    if (!minPackVState)
      hitUnder = true;
    minPackVState = true;
  } else if (lastPackMilliVolts > battSets.limits[limits::Volt][limits::Pack][limits::Min][limits::Rec])
    minPackVState = false;

  if (hitOver && milliAmpMillis < battMilliAmpMillis) {
    if (lastTrip != 0) {
      curAdjMilliAmpMillis += battMilliAmpMillis - milliAmpMillis;
      if (lastTrip < 0)
        BatAHMeasured = (milliAmpMillis + curAdjMilliAmpMillis) / ((uint64_t)1000 * 1000 * 60 * 60);
      lastTrip = 1;
      doAHCalcs();
    }
    milliAmpMillis = battMilliAmpMillis;
    stateOfChargeValid = true;
  }
  if (hitUnder && milliAmpMillis > 0) {
    if (lastTrip != 0) {
      curAdjMilliAmpMillis -= milliAmpMillis;
      if (lastTrip > 0)
        BatAHMeasured = (battMilliAmpMillis - milliAmpMillis + curAdjMilliAmpMillis) / ((uint64_t)1000 * 1000 * 60 * 60);
      lastTrip = -1;
      doAHCalcs();
    }
    milliAmpMillis = 0;
    stateOfChargeValid = true;
  }

  uint8_t relay[RELAY_TOTAL];
  //Set defaults based on configuration
  loadsOff = minCellVState || minPackVState || maxCellCState || maxPackCState;
  chgOff = maxCellVState || maxPackVState || minCellCState || maxCellCState || minPackCState || maxPackCState;
  for (int8_t y = 0; y < RELAY_TOTAL; y++)
  {
    RelaySettings *rp = &battSets.relays[y];
    if (rp->off)
      relay[y] = LOW;
    else {
      relay[y] = previousRelayState[y]; // don't change it because we might be in the SOC trip/rec area
      if (rp->type == Relay_Load) {
        if (loadsOff || (rp->doSOC && (!stateOfChargeValid || stateOfCharge < rp->trip))
             || (cellsTimedOut && (!stateOfChargeValid || (stateOfCharge < rp->trip && rp->trip > 0))))
          relay[y] = LOW; // turn if off
        else if (!rp->doSOC || (rp->doSOC && stateOfChargeValid && stateOfCharge > rp->rec))
          relay[y] = HIGH; // turn it on
      } else {
        if (chgOff || (rp->doSOC && (!stateOfChargeValid || stateOfCharge > rp->trip))
            || (cellsTimedOut && (!stateOfChargeValid || (stateOfCharge > rp->trip && rp->trip > 0))))
          relay[y] = LOW; // off
        else if (!rp->doSOC || (rp->doSOC && stateOfChargeValid && stateOfCharge < rp->rec))
          relay[y] = HIGH; // on
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
  doPollCells();
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

void doEmailSettings() {
  smtpData.setLogin(eSets.senderServer, eSets.senderPort, eSets.senderEmail, eSets.senderPW);
  smtpData.setSender("Your Battery", eSets.senderEmail);
  smtpData.setPriority("High");
  smtpData.setSubject(eSets.senderSubject);
  smtpData.addRecipient(eSets.email);
  emailSetup = true;
/*  smtpData.setLogin("smtp.gmail.com", 465, "john.taves@gmail.com","erwsmuigvggpvmtf");
*/
}

void setINAs() {
  if (INADevs == 0)
    return;
  INA.begin(battSets.MaxAmps, battSets.ShuntUOhms,0);
  INA.begin(battSets.PVMaxAmps,battSets.PVShuntUOhms,1);
  INA.setShuntConversion(300,0);
  INA.setBusConversion(300,0);
  INA.setAveraging(10000,0);
  INA.setShuntConversion(battSets.ConvTime,1);
  INA.setBusConversion(battSets.ConvTime,1);
  INA.setAveraging(battSets.Avg,1);
}

void sendSuccess(AsyncWebServerRequest *request) { 
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(100);
  doc["success"] = true;
  serializeJson(doc, *response);
  request->send(response);
}

void settings(AsyncWebServerRequest *request){
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(4096);
  JsonObject root = doc.to<JsonObject>();
  root["email"] = eSets.email;
  root["senderEmail"] = eSets.senderEmail;
  root["senderSubject"] = eSets.senderSubject;
  root["senderServer"] = eSets.senderServer;
  root["senderSubject"] = eSets.senderSubject;
  root["senderPort"] = eSets.senderPort;
 
  root["Avg"] = battSets.Avg;
  root["PollFreq"] = battSets.PollFreq;
  root["ConvTime"] = battSets.ConvTime;
  root["BattAH"] = battSets.BattAH;
  root["socLastAdj"] = lastAdjMillAmpHrs;
  snprintf(spb,sizeof(spb),"%d:%d",(int)(aveAdjMilliAmpMillis / ((int64_t)1000 * 60 * 60)),numAveAdj);
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
    temp["bCoef"] = sensSets.temps[i].bCoef;
    temp["addr"] = sensSets.temps[i].addr;
    temp["mul"] = sensSets.temps[i].mul;
    temp["div"] = sensSets.temps[i].div;
    temp["range"] = sensSets.temps[i].range;
  }
  root["resPwrMS"]=resPwrMS;
  root["useCellC"]=battSets.useCellC;
  root["useTempC"]=battSets.useTempC;

  JsonObject obj = root.createNestedObject("limitSettings");
  for (int l0=0;l0<limits::Max0;l0++) {
    for (int l1=0;l1<limits::Max1;l1++) {
      for (int l2=0;l2<limits::Max2;l2++) {
        for (int l3=0;l3<limits::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          obj[name] = battSets.limits[l0][l1][l2][l3];
        }
      }
    }
  }

  JsonArray rsArray = root.createNestedArray("relaySettings");
  for (uint8_t r = 0; r < RELAY_TOTAL; r++) {
    JsonObject rule1 = rsArray.createNestedObject();
    rule1["name"] =battSets.relays[r].name;
    rule1["type"] =battSets.relays[r].type;
    rule1["doSOC"] =battSets.relays[r].doSOC;
    rule1["trip"] =battSets.relays[r].trip;
    rule1["rec"] =battSets.relays[r].rec;
  }

  root["ssid"] = wifiSets.ssid;
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

char* milliToStr(int32_t val) {
  int32_t v = val/10;
  snprintf(spb,sizeof(spb),"%s%d.%02d",(v < 0 ?"-":""),abs(v)/100,(abs(v) % 100));
  return spb;
}

void saveOff(AsyncWebServerRequest *request) {
  if (request->hasParam("relay", true)) {
    int r = request->getParam("relay", true)->value().toInt();
    RelaySettings *rp = &battSets.relays[r];
    rp->off = (rp->off + 1) % 2;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);
    doc["relay"] = r;
    doc["val"] = rp->off == 1 ? "off" : "on";
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
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

void status(AsyncWebServerRequest *request){
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");

  DynamicJsonDocument doc(2048);
  JsonObject root = doc.to<JsonObject>();
 
  uint32_t upsecs = (milliRolls * 4294967ul) + (millis()/1000ul);
  int days = upsecs / (24ul*60*60);
  upsecs = upsecs % (24ul*60*60);
  int hrs = upsecs / (60*60);
  upsecs = upsecs % (60*60);
  int min = upsecs / 60;

  snprintf(spb,sizeof(spb),"%d:%02d:%02d:%02d",days,hrs,min,upsecs % 60);
  root["uptime"] = spb;
  time_t now;
  if (time(&now))
    root["now"]=now;
  root["version"] = "V: 0.5";
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
    root[dodad] = battSets.relays[i].off == 1 ? "off" : "on";
  }

//  root["debuginfo"] = debugstr;

  root["packcurrent"] = milliToStr(lastMicroAmps/1000);
  root["packvolts"] = milliToStr(lastPackMilliVolts);
  root["pvcurrent"] = milliToStr(lastPVMicroAmps/1000);
  root["loadcurrent"] = milliToStr( (lastPVMicroAmps - lastMicroAmps)/1000);
  snprintf(spb,sizeof(spb),"%d%%",stateOfCharge);
  root["soc"] = spb;
  root["socvalid"] = stateOfChargeValid;
  root["temp1"] = fromCel(curTemp1);
  root["temp2"] = fromCel(curTemp2);
  root["fsr"] = rawFSR;
  root["rawTemp1"] = rawTemp1;
  root["rawTemp2"] = rawTemp2;

  root["maxCellVState"] = maxCellVState;
  root["minCellVState"] = minCellVState;
  root["maxPackVState"] = maxPackVState;
  root["minPackVState"] = minPackVState;
  root["maxCellCState"] = maxCellCState;
  root["minCellCState"] = minCellCState;
  root["maxPackCState"] = maxPackCState;
  root["minPackCState"] = minPackCState;
  root["nocells"] = cellsTimedOut;
  snprintf(spb,sizeof(spb),"%d %d %dms",numSent,failedSer,lastRoundMillis);
  root["pkts"] = spb;

  if (browserNBanks != battSets.nBanks || browserNCells != battSets.nCells) {
    root["nBanks"] = battSets.nBanks;
    root["nCells"] = battSets.nCells;
    browserNBanks = battSets.nBanks;
    browserNCells = battSets.nCells;
  }

  JsonArray bankArray = root.createNestedArray("bank");
  for (uint8_t bank = 0; bank < battSets.nBanks; bank++) {
    JsonArray data = bankArray.createNestedArray();

    for (uint8_t i = 0; i < battSets.nCells; i++) {
      JsonObject cell = data.createNestedObject();
      cell["v"] = cells[bank][i].v;
      cell["t"] = cells[bank][i].t;
      cell["dumping"] = cells[bank][i].dumping;
    }
  }

  serializeJson(doc, *response);
  request->send(response);
}
void firstStatus(AsyncWebServerRequest *request){
  browserNBanks = -1;
  return status(request);
}

void saverules(AsyncWebServerRequest *request) {
  for (int l0=0;l0<limits::Max0;l0++) {
    for (int l1=0;l1<limits::Max1;l1++) {
      for (int l2=0;l2<limits::Max2;l2++) {
        for (int l3=0;l3<limits::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          if (request->hasParam(name, true, false))
            battSets.limits[l0][l1][l2][l3] = request->getParam(name, true, false)->value().toInt();
        }
      }
    }
  }

  for (int relay=0;relay<RELAY_TOTAL;relay++) {
    char name[16];
    RelaySettings *rp = &battSets.relays[relay];
    sprintf(name,"relayName%d",relay);
    if (request->hasParam(name, true))
      request->getParam(name, true)->value().toCharArray(rp->name,sizeof(rp->name));;
    
    sprintf(name,"relayType%d",relay);
    if (request->hasParam(name, true))
      rp->type = request->getParam(name, true)->value().toInt();

    sprintf(name,"relayDoSOC%d",relay);
    if (request->hasParam(name, true))
      rp->doSOC = request->getParam(name, true)->value().equals("on");
    else
      rp->doSOC = false;

    sprintf(name,"relayTrip%d",relay);
    if (request->hasParam(name, true))
      rp->trip = request->getParam(name, true)->value().toInt();

    sprintf(name,"relayRec%d",relay);
    if (request->hasParam(name, true))
      rp->rec = request->getParam(name, true)->value().toInt();

  }
  battSets.useTempC = request->hasParam("useTempC", true) && request->getParam("useTempC", true)->value().equals("on");
  battSets.useCellC = request->hasParam("useCellC", true) && request->getParam("useCellC", true)->value().equals("on");

  if (!battSets.useCellC) {
    maxCellCState = false;
    minCellCState = false;
  }
  if (!battSets.useTempC) {
    maxPackCState = false;
    minPackCState = false;
  }
  writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);

  sendSuccess(request);
}

void saveemail(AsyncWebServerRequest *request){
  if (request->hasParam("email", true))
    request->getParam("email", true)->value().toCharArray(eSets.email,sizeof(eSets.email));
  if (request->hasParam("senderEmail", true))
    request->getParam("senderEmail", true)->value().toCharArray(eSets.senderEmail,sizeof(eSets.senderEmail));
  if (request->hasParam("senderPW", true))
    request->getParam("senderPW", true)->value().toCharArray(eSets.senderPW,sizeof(eSets.senderPW));
  if (request->hasParam("senderServer", true))
    request->getParam("senderServer", true)->value().toCharArray(eSets.senderServer,sizeof(eSets.senderServer));
  if (request->hasParam("senderSubject", true))
    request->getParam("senderSubject", true)->value().toCharArray(eSets.senderSubject,sizeof(eSets.senderSubject));
  if (request->hasParam("senderPort", true))
    eSets.senderPort = request->getParam("senderPort", true)->value().toInt();
  writeEE((uint8_t*)&eSets,sizeof(eSets),EEPROM_EMAIL);
  doEmailSettings();
  sendSuccess(request);
}

int discoCnt;
extern void WiFiInit(bool doSTA);
void onconnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.print("In Connect: ");
  Serial.print(event);
  Serial.print("-");
  int mode = WiFi.getMode();
  switch (mode) {
    case WIFI_MODE_NULL: Serial.print("NULL, "); break;
    case WIFI_MODE_STA: Serial.print("STA, "); break;
    case WIFI_MODE_AP: Serial.print("AP, "); break;
    case WIFI_MODE_APSTA: Serial.print("APSTA, "); break;
  }
  Serial.println(WiFi.localIP());
  switch (event) {
    case SYSTEM_EVENT_STA_DISCONNECTED:
      if (discoCnt++ > 4)
        WiFiInit(false);
      break;
    case SYSTEM_EVENT_STA_GOT_IP:
      server.end();
      Serial.println("Begin Server");
      server.begin();
      configTime(0,0,"pool.ntp.org");
      break;
    default: break;
  }
}

void WiFiInit(bool doSTA) {
  WiFi.persistent(false);
  WiFi.disconnect(true,true);
  WiFi.mode(WIFI_OFF);
  discoCnt = 0;
  if (doSTA)
    WiFi.mode(WIFI_MODE_APSTA);
  else WiFi.mode(WIFI_AP);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  WiFi.onEvent(onconnect, WiFiEvent_t::SYSTEM_EVENT_MAX);
  WiFi.softAP("ADVBMS_CONTROLLER");
  if (doSTA && wifiSets.ssid[0] != 0)
    WiFi.begin(wifiSets.ssid,wifiSets.password);
}

void kickwifi(AsyncWebServerRequest *request) {
  WiFiInit(true);
}

void savewifi(AsyncWebServerRequest *request){
  if (request->hasParam("ssid", true))
    request->getParam("ssid", true)->value().toCharArray(wifiSets.ssid,sizeof(wifiSets.ssid));
  if (request->hasParam("password", true))
    request->getParam("password", true)->value().toCharArray(wifiSets.password,sizeof(wifiSets.password));
  writeEE((uint8_t*)&wifiSets,sizeof(wifiSets),EEPROM_WIFI);
  WiFiInit(true);
  sendSuccess(request);
}

void setStateOfCharge(int val) {
  milliAmpMillis = battMilliAmpMillis * val/100;
  stateOfChargeValid = true;
  curAdjMilliAmpMillis = 0;
  lastTrip = 0;
  aveAdjMilliAmpMillis = 0;
  numAveAdj = 0;
}

void savesens(AsyncWebServerRequest *request) {
  for (int i=0;i<MAX_TEMPS;i++) {
    char name[16];
    ADCTSet *rp = &sensSets.temps[i];
    sprintf(name,"bCoef%d",i);
    if (request->hasParam(name, true))
      rp->bCoef = request->getParam(name, true)->value().toInt();
    
    sprintf(name,"addr%d",i);
    if (request->hasParam(name, true))
      rp->addr = request->getParam(name, true)->value().toInt();

    sprintf(name,"mul%d",i);
    if (request->hasParam(name, true))
      rp->mul = request->getParam(name, true)->value().toInt();

    sprintf(name,"div%d",i);
    if (request->hasParam(name, true))
      rp->div = request->getParam(name, true)->value().toInt();

    sprintf(name,"range%d",i);
    if (request->hasParam(name, true))
      rp->range = request->getParam(name, true)->value().toInt();

  }
  if (request->hasParam("resPwrMS", true))
    resPwrMS = request->getParam("resPwrMS", true)->value().toInt();

  writeEE((uint8_t*)&sensSets, sizeof(sensSets), EEPROM_SENS);

  sendSuccess(request);
}
void savecapacity(AsyncWebServerRequest *request) {
  if (request->hasParam("CurSOC", true)) {
    AsyncWebParameter *p1 = request->getParam("CurSOC", true);
    if (p1->value().length() > 0) setStateOfCharge(p1->value().toInt());
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

  writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);

  setINAs();

  sendSuccess(request);
}

void toggleTemp(AsyncWebServerRequest *request) {
  battSets.doCelsius = !battSets.doCelsius;
  writeEE((uint8_t*)&battSets, sizeof(battSets), EEPROM_BATT);
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
  server.on("/saveemail", HTTP_POST, saveemail);
  server.on("/saveOff", HTTP_POST, saveOff);
  server.on("/dump", HTTP_POST, dump);
  server.on("/kickwifi", HTTP_GET, kickwifi);
  server.on("/savewifi", HTTP_POST, savewifi);
  server.on("/savecapacity", HTTP_POST, savecapacity);
  server.on("/savesens", HTTP_POST, savesens);
  server.on("/saverules", HTTP_POST, saverules);
  server.on("/settings", HTTP_GET, settings);
  server.on("/status1", HTTP_GET, firstStatus);
  server.on("/status", HTTP_GET, status);

  server.serveStatic("/static", SPIFFS, "/static").setLastModified("Mon, 20 Jun 2016 14:00:00 GMT");
  server.serveStatic("/", SPIFFS, "/");
  server.onNotFound(onRequest);
  server.begin();
}
void initSens() {
    sensSets.temps[Temp1].bCoef = 3950;
    sensSets.temps[Temp2].bCoef = 3950;
    sensSets.temps[TempC].bCoef = 4050;
    sensSets.temps[Temp1].range = 4095;
    sensSets.temps[Temp2].range = 4095;
    sensSets.temps[TempC].range = 1023;
    for (int i=0;i<MAX_TEMPS;i++) {
      sensSets.temps[i].addr = 0;
      sensSets.temps[i].mul = 1;
      sensSets.temps[i].div = 1;
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
  battSets.useTempC = true;
  battSets.useCellC = true;
  battSets.Avg = 1000;
  for (int i=0;i<RELAY_TOTAL;i++) {
    RelaySettings* r = &battSets.relays[i];
    r->name[0] = 0;
    r->doSOC = false;
    r->off = 0;
    r->rec = 0;
    r->trip = 0;
    r->type = 0;
  }
  for (int i=limits::Cell;i<limits::Max1;i++) {
    int mul = !i?1:8;
    battSets.limits[limits::Volt][i][limits::Max][limits::Trip] = 3500 * mul;
    battSets.limits[limits::Volt][i][limits::Max][limits::Rec] = 3400 * mul;
    battSets.limits[limits::Volt][i][limits::Min][limits::Trip] = 2800 * mul;
    battSets.limits[limits::Volt][i][limits::Min][limits::Rec] = 3200 * mul;
    battSets.limits[limits::Temp][i][limits::Max][limits::Trip] = 50;
    battSets.limits[limits::Temp][i][limits::Max][limits::Rec] = 45;
    battSets.limits[limits::Temp][i][limits::Min][limits::Trip] = 4;
    battSets.limits[limits::Temp][i][limits::Min][limits::Rec] = 7;
  }
}

void setup() {
  Serial.begin(9600);
  Serial2.begin(2400, SERIAL_8N1); // Serial for comms to modules
  EEPROM.begin(EEPROMSize);
  Wire.begin();
  if(!SPIFFS.begin()){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  if (!readEE((uint8_t*)&wifiSets, sizeof(wifiSets), EEPROM_WIFI)) {
    wifiSets.ssid[0] = 0;
    wifiSets.password[0] = 0;
  }
  WiFiInit(true);
  //EasyBuzzer.setPin(SPEAKER);

  if (!readEE((uint8_t*)&eSets,sizeof(eSets),EEPROM_EMAIL)) {
    eSets.email[0] = 0;
    eSets.senderEmail[0] = 0;
    eSets.senderServer[0] = 0;
    eSets.senderPort = 587;
    eSets.senderPW[0] = 0;
    eSets.senderSubject[0] = 0;
  } else doEmailSettings();

  if (!readEE((uint8_t*)&battSets,sizeof(battSets),EEPROM_BATT))
    initBattSets();

  if (!readEE((uint8_t*)&sensSets,sizeof(sensSets),EEPROM_SENS))
    initSens();

  dataSer.setStream(&Serial2); // start serial for output
  dataSer.setPacketHandler(&onSerData);

  pinMode(FSR, INPUT);
  pinMode(TEMP1,INPUT);
  pinMode(TEMP2,INPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  pinMode(RESISTOR_PWR, OUTPUT);
  for (int i=0;i<RELAY_TOTAL;i++)
    pinMode(relayPins[i],OUTPUT);

  GenUUID();

  INADevs = INA.begin(battSets.MaxAmps, battSets.ShuntUOhms);
  lastShuntMillis = millis();
  battMilliAmpMillis = (uint64_t)battSets.BattAH * (1000 * 60 * 60) * 1000; // convert to milliampmilliseconds
  milliAmpMillis = battMilliAmpMillis * 80 / 100; /* No clue what the SOC was, so assume 80% */
  setINAs();

  smtpData.setSendCallback(emailCallback);
  startServer();
  doPollCells();
}

void loop() {
  uint32_t curMillis = millis();
  if (lastMillis > curMillis) milliRolls++;
  lastMillis = curMillis; // for uptime to continue after 50 days

  if (sendEmail && emailSetup) {
    if (!MailClient.sendMail(smtpData))
      Serial.println("Error sending Email, " + MailClient.smtpErrorReason());
    sendEmail = false;
  }
  if (INADevs > 0) {
    if ((curMillis - shuntPollMS) > battSets.PollFreq) {
      getAmps();
      shuntPollMS = curMillis;
    }
    if (INADevs > 1 && (curMillis - pvPollMS) > 2000) {
      lastPVMicroAmps = INA.getBusMicroAmps(1);
      pvPollMS = curMillis;
    }
  }
  if (!resPwrOn && (!resPwrMS || (curMillis - statusMS) > (3000-resPwrMS))) {
    resPwrOn = true;
    digitalWrite(RESISTOR_PWR,HIGH); // get current flowing through resistors
  } else if ((!resPwrMS || resPwrOn) && (curMillis - statusMS) > 3000) {
    rawTemp1 = analogRead(TEMP1);
    curTemp1 = calcStein(rawTemp1,&sensSets.temps[Temp1]);
    rawTemp2 = analogRead(TEMP2);
    curTemp2 = calcStein(rawTemp2,&sensSets.temps[Temp2]);
    rawFSR = analogRead(FSR);
    if (resPwrMS) {
      digitalWrite(RESISTOR_PWR,LOW);
      resPwrOn = false;
    }
    checkStatus();
    statusMS = curMillis;
  }
  //EasyBuzzer.update();
  dataSer.update();
  if ((curMillis - ledFlashMS) > 2000) {
    digitalWrite(BLUE_LED,ledState ? LOW : HIGH);
    ledState = !ledState;
    ledFlashMS = curMillis;
  }
}
