#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <ESP32_MailClient.h>
#include <Wire.h>

#include <time.h>
#include <Ticker.h>
#include <BMSADC.h>
#include <BMSCommArd.h>
#include "defines.h"

// improve server
// adjustable cell history
// cell dump safety, stop if cell gets low, show dump status
// charge control feature to reduce amps
// improve the SoC allow to run logic. Make the code bad, and see if the charge/loads go off
// email did not work going from unset to sent. A reboot seemed to help

char debugstr[200];
bool emailSetup=false,writeCommSet=false,writeWifiSet=false
  ,writeDispSet=false,writeRelaySet=false;
uint32_t statusMS=0,tempMS=0,wifiMS=0;
uint8_t wifiDeadCnt=0;
HTTPClient http;
Ticker watchDog;
struct tm curTime;

time_t lastEventTime=0;
int curEvent=0;
uint8_t curDay = 0;
Event evts[MAX_EVENTS];

uint32_t firstCellBalanceTime;
uint16_t CellsDiff[MAX_CELLS];

BMSStatus st;

const int relayPins[W_RELAY_TOTAL] = { GPIO_NUM_2,GPIO_NUM_15,GPIO_NUM_13,GPIO_NUM_12,GPIO_NUM_14,GPIO_NUM_27 };

WiFiSettings wifiSets;
CommSettings commSets;
DynSetts dynSets;
StatSetts statSets;
DispSettings dispSets;
WRelaySettings relSets;
int8_t Temp1,Temp2;
uint8_t Water,Gas;

char spb[1024];

bool sendEmail = false,inAlertState = true;
AsyncWebServer server(80);
SMTPData smtpData;
uint8_t previousRelayState[W_RELAY_TOTAL];
uint8_t previousHeaterOnSource[W_RELAY_TOTAL];

String emailRes = "";

uint8_t milliRolls=0;
uint32_t lastMillis=0;

Event* NextEvent(EventMsg* mp = nullptr) {
  Event* ep = &evts[curEvent++];
  if (curEvent == MAX_EVENTS)
    curEvent = 0;
  if (mp) ep->msg = *mp;
  else {
    ep->msg.cell = MAX_CELLS;
    ep->msg.tC = 0;
    ep->msg.mV = 0;
    ep->msg.amps = 0;
    ep->msg.ms = 0;
    ep->msg.relay = 0;
    ep->msg.xtra = 0;
  }
  ep->when = time(nullptr);
  lastEventTime = ep->when;
  return ep;
}

void clearRelays() {
  for (int i=0;i<W_RELAY_TOTAL;i++) {
    digitalWrite(relayPins[i], LOW);
    previousRelayState[i] = LOW;
    previousHeaterOnSource[i] = Relay_Unused;
  }
}

void doWatchDog() {
  clearRelays();
  st.watchDogHits++;
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

void sendSuccess(AsyncWebServerRequest *request,const char* mess=NULL,bool suc=true) { 
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(100);
  doc["success"] = suc;
  doc["errmess"] = mess;
  serializeJson(doc, *response);
  request->send(response);
}

int toCel(String val) {
  if (dispSets.doCelsius)
    return val.toInt();
  return (val.toInt() - 32) * 5/9;
}

int fromCel(int8_t c) {
  if (c == INT8_MIN) return -300;
  if (dispSets.doCelsius)
    return c;
  return c*9/5+32;
}

void cells(AsyncWebServerRequest *request){
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = false;

  serializeJson(doc, *response);
  request->send(response);
}

void net(AsyncWebServerRequest *request){
  AsyncResponseStream *response =
    request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = false;
  root["apName"] = wifiSets.apName;
  root["ssid"] = wifiSets.ssid;
  root["email"] = commSets.email;
  root["senderEmail"] = commSets.senderEmail;
  root["senderSubject"] = commSets.senderSubject;
  root["senderServer"] = commSets.senderServer;
  root["senderSubject"] = commSets.senderSubject;
  root["senderPort"] = commSets.senderPort;
  root["logEmail"] = commSets.logEmail;
  root["doLogging"] = commSets.doLogging;
  serializeJson(doc, *response);
  request->send(response);
}

void limits(AsyncWebServerRequest *request){  
  AMsg msg;
  msg.cmd = StatQuery;
  bool notRecd = BMSWaitFor(&msg,StatSets,200);
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = notRecd;
  root["useCellC"]=statSets.useCellC;
  root["useBoardTemp"]=statSets.useBoardTemp;
  root["bdVolts"]=statSets.bdVolts;
  root["ChargePct"]=statSets.ChargePct;
  root["ChargePctRec"]=statSets.ChargePctRec;
  root["CellsOutMin"]=statSets.CellsOutMin;
  root["CellsOutMax"]=statSets.CellsOutMax;
  root["CellsOutTime"]=statSets.CellsOutTime;
  root["ShuntErrTime"] = statSets.ShuntErrTime;
  root["MainID"] = statSets.MainID;
  root["PVID"] = statSets.PVID;

  JsonObject obj = root.createNestedObject("limitSettings");
  for (int l0=0;l0<LimitConsts::Max0;l0++) {
    for (int l1=0;l1<LimitConsts::Max1;l1++) {
      for (int l2=0;l2<LimitConsts::Max2;l2++) {
        for (int l3=0;l3<LimitConsts::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          obj[name] = statSets.limits[l0][l1][l2][l3];
        }
      }
    }
  }

  serializeJson(doc, *response);
  request->send(response);
}

void relays(AsyncWebServerRequest *request){  
  AMsg msg;
  msg.cmd = StatQuery;
  bool notRecd = BMSWaitFor(&msg,StatSets,200);
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = notRecd;
  JsonArray rsArray = root.createNestedArray("relaySettings");
  for (uint8_t r = 0; r < RELAY_TOTAL; r++) {
    JsonObject rule1 = rsArray.createNestedObject();
    RelaySettings *rp;
    if (r < C_RELAY_TOTAL) rp = &statSets.relays[r];
    else rp = &relSets.relays[r - C_RELAY_TOTAL];
    rule1["name"] = rp->name;
    rule1["from"] = rp->from;
    switch (rp->type) {
      default: case Relay_Connect: rule1["type"] = "E"; break;
      case Relay_Load: rule1["type"] = (rp->doSoC?"LP":"L"); break;
      case Relay_Charge: rule1["type"] = (rp->doSoC?"CP":(rp->fullChg?"CF":"C")); break;
      case Relay_Therm: rule1["type"] = "T";break;
      case Relay_Heat: rule1["type"] = "H";break;
      case Relay_Unused: rule1["type"] = "U"; break;
      case Relay_Ampinvt: rule1["type"] = "A"; break;
    }
    
    rule1["trip"] = rp->trip;
    rule1["rec"] = rp->rec;
  }

  serializeJson(doc, *response);
  request->send(response);
}

void getRelayType(JsonObject root,uint8_t type,uint8_t type2 = 255) {
  JsonArray rsArray = root.createNestedArray("relaySettings");
  for (uint8_t r = 0; r < W_RELAY_TOTAL; r++) {
    RelaySettings *rp = &relSets.relays[r];
    if (rp->type != type && rp->type != type2)
      continue;
    JsonObject rule1 = rsArray.createNestedObject();
    rule1["relay"] = r;
    rule1["name"] = rp->name;
  }

}

void temps(AsyncWebServerRequest *request)
{
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["t1B"]=dispSets.t1B;
  root["t1R"]=dispSets.t1R;
  root["t2B"]=dispSets.t2B;
  root["t2R"]=dispSets.t2R;
  root["nTSets"]=dispSets.nTSets;
  JsonArray tSetArr = root.createNestedArray("tSets");
  for (int i=0;i<dispSets.nTSets;i++) {
    JsonObject tset = tSetArr.createNestedObject();
    TempSet* ts = &dispSets.tSets[i];
    tset["Sens"] = ts->sens;
    tset["Relay"] = ts->relay;
    tset["Trip"] = ts->tripTemp;
    tset["Rec"] = ts->recTemp;
    tset["Start"] = ts->startMin;
    tset["End"] = ts->endMin;
    tset["dows"] = ts->dows;
  }
  getRelayType(root,Relay_Therm,Relay_Heat);
  serializeJson(doc, *response);
  request->send(response);
}

void events(AsyncWebServerRequest *request){  
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  JsonArray data = root.createNestedArray("events");
  for (int i=curEvent-1;i != curEvent;i--) {
    if (i < 0) i = MAX_EVENTS-1;
    Event* ep = &evts[i];
    if (!ep->when)
      break;
    JsonObject evt = data.createNestedObject();
    switch (ep->msg.cmd) {
      case WatchDog: evt["cmd"] = "Watch Dog"; break;
      case CellsOverDue: evt["cmd"] = "Cell Overdue"; break;
      case CellTopV: evt["cmd"] = "Cell Over V"; break;
      case CellBotV: evt["cmd"] = "Cell Under V"; break;
      case CellTopT: evt["cmd"] = "Cell Over T"; break;
      case CellBotT: evt["cmd"] = "Cell Under T"; break;
      case PackTopV: evt["cmd"] = "Pack Over V"; break;
      case PackBotV: evt["cmd"] = "Pack Under V"; break;
      case PackTopT: evt["cmd"] = "Pack Over T"; break;
      case PackBotT: evt["cmd"] = "Pack Under T"; break;
      case HeaterOn: evt["cmd"] = "Heat On"; break;
      case HeaterOff: evt["cmd"] = "Heat Off"; break;
      case ShuntOverDue: evt["cmd"] = "Shunt Over Due"; break;
      case ConnCell: evt["cmd"] = "Connected"; break;
      case DiscCell: evt["cmd"] = "Disconnected"; break;
      default: evt["cmd"] = ep->msg.cmd; break;
    }
    evt["cell"] = ep->msg.cell;
    evt["tC"] = ep->msg.tC;
    evt["mV"] = ep->msg.mV;
    evt["ms"] = ep->msg.ms;
    evt["amps"] = ep->msg.amps;
    evt["when"] = ep->when;
    evt["relay"] = ep->msg.relay;
    evt["xtra"] = ep->msg.xtra;
  }

  serializeJson(doc, *response);
  request->send(response);
}

void batt(AsyncWebServerRequest *request){
  AMsg msg;
  msg.cmd = DynQuery;
  bool notRecd = BMSWaitFor(&msg,DynSets,200);
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = notRecd;

  root["BattAH"] = dynSets.BattAH;
  root["TopAmps"] = dynSets.TopAmps;

  root["socLastAdj"] = st.lastAdjCoulomb;
  root["BatAHMeasured"] = st.BatAHMeasured > 0 ? String(st.BatAHMeasured) : String("N/A");
  root["nCells"] = dynSets.nCells;

  root["cellCnt"] = dynSets.cnt;
  root["cellDelay"] = dynSets.delay;
  root["resPwrOn"] = dynSets.resPwrOn;
  root["drainV"] = dynSets.cellSets.drainV;
  root["cellTime"] = dynSets.cellSets.time;
  JsonArray data = root.createNestedArray("cells");
  for (uint8_t i = 0; i < dynSets.nCells; i++) {
    JsonObject cell = data.createNestedObject();
    cell["c"] = i;
    cell["s"] = CellsDiff[i];
  }

  serializeJson(doc, *response);
  request->send(response);
}

void saveOff(AsyncWebServerRequest *request) {
  SettingMsg msg;
  if (request->hasParam("relay", true)) {
    RelaySettings *rp;
    int r = request->getParam("relay", true)->value().toInt();
    if (r < C_RELAY_TOTAL) {
      msg.cmd = SetRelayOff;
      msg.val = r;
      BMSSend(&msg);
      rp = &statSets.relays[r];
    } else {
      rp = &relSets.relays[r - C_RELAY_TOTAL];
      writeRelaySet = true;
    }
    rp->off = !rp->off;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["relay"] = msg.val;
    doc["val"] = rp->off ? "off" : "on";
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}

void ResetCellsDiff()
{
    for (int i=0;i<MAX_CELLS;i++) CellsDiff[i] = 0xffff;
}

void doFullChg(uint16_t val) {
  SettingMsg msg;
  msg.cmd = FullChg;
  msg.val = val;
  if (val)
    ResetCellsDiff();
  BMSSend(&msg);
}
void fullChg(AsyncWebServerRequest *request) {
  doFullChg(!st.doFullChg);
  sendSuccess(request);
}

void dump(AsyncWebServerRequest *request) {
  if (request->hasParam("cell", true)) {
    DumpMsg dm;
    dm.cmd = DumpCell;
    dm.cell = request->getParam("cell", true)->value().toInt();
    dm.secs = request->getParam("min", true)->value().toInt() * 60;
    BMSSend(&dm);
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}
void forget(AsyncWebServerRequest *request) {
  if (request->hasParam("cell", true)) {
    SettingMsg dm;
    dm.cmd = ForgetCell;
    dm.val = request->getParam("cell", true)->value().toInt();
    BMSSend(&dm);
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}
void doMove(AsyncWebServerRequest *request) {
  if (request->hasParam("cell", true)) {
    SettingMsg dm;
    dm.cmd = MoveCell;
    dm.val = request->getParam("cell", true)->value().toInt();
    BMSSend(&dm);
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);
  } else request->send(500, "text/plain", "Missing parameters");
}
char *getUpTimeStr(uint32_t ms,uint8_t rolls) {
  uint32_t upsecs = (rolls * 4294967ul) + (ms/1000ul);
  int yrs = upsecs / (365ul*24*60*60);
  upsecs %= (365ul*24*60*60);
  int days = upsecs / (24ul*60*60);
  upsecs %= (24ul*60*60);
  int hrs = upsecs / (60*60);
  upsecs %= (60*60);
  int min = upsecs / 60;
  upsecs %= 60;

  if (yrs) 
    snprintf(spb,sizeof(spb),"%dy %dd",yrs,days);
  else if (days)
    snprintf(spb,sizeof(spb),"%dd %dh",days, hrs);
  else 
    snprintf(spb,sizeof(spb),"%02d:%02d:%02d",hrs,min,upsecs % 60);
  return spb;
}

void fillStatusDoc(JsonVariant root) {
  if (dynSets.cmd == Nada) {
    AMsg msg;
    msg.cmd = DynQuery;
    BMSWaitFor(&msg,DynSets);
  }
  root["uptimew"] = getUpTimeStr(millis(),milliRolls);
  root["uptimec"] = getUpTimeStr(st.lastMillis,st.milliRolls);
  root["now"]=time(nullptr);
  root["debugstr"] = debugstr;
  if (lastEventTime)
    root["lastEventTime"] = lastEventTime;
  root["watchDogHits"] = st.watchDogHits;
  root["RELAY_TOTAL"] = RELAY_TOTAL;
  root["W_RELAY_TOTAL"] = W_RELAY_TOTAL;
  for (int i=0;i<RELAY_TOTAL;i++) {
    char dodad[16];
    int state;
    RelaySettings *rp;
    if (i < C_RELAY_TOTAL) {
      rp = &statSets.relays[i];
      if (rp->type == Relay_Ampinvt)
        state = digitalRead(INV);
      else 
        state = st.previousRelayState[i];
    } else {
      rp = &relSets.relays[i - C_RELAY_TOTAL];
      state = previousRelayState[i - C_RELAY_TOTAL];
    }
    if (strlen(rp->name) == 0)
      continue;
    if (rp->type == Relay_Ampinvt){
      sprintf(dodad,"relayGoal%d",i);
      root[dodad] = st.ampInvtGoal?"ON":"OFF";
    }
    sprintf(dodad,"relayStatus%d",i);
    root[dodad] = state==HIGH?"ON":"OFF";
    sprintf(dodad,"relayName%d",i);
    root[dodad] = rp->name;
    sprintf(dodad,"relayOff%d",i);
    root[dodad] = rp->off ? "off" : "on";
  }

  root["packcurrent"] = st.lastMilliAmps;
  root["packvolts"] = st.lastPackMilliVolts;
  root["pvvolts"] = st.lastPVMilliVolts;
  root["pvcurrent"] = st.lastPVMilliAmps;
  root["invcurrent"] = st.lastInvMilliAmps;
  snprintf(spb,sizeof(spb),"%d%%",st.stateOfCharge);
  root["soc"] = spb;
  root["socvalid"] = st.stateOfChargeValid;
  root["BoardTemp"] = fromCel(st.curBoardTemp);
  root["AmpTemp"] = fromCel(st.ampTemp/10);
  root["Temp1"] = fromCel(Temp1);
  root["Temp2"] = fromCel(Temp2);
  root["Water"] = Water;
  root["Gas"] = Gas;
  root["fullChg"] = st.doFullChg;

  root["maxCellVState"] = st.maxCellVState;
  root["minCellVState"] = st.minCellVState;
  root["maxPackVState"] = st.maxPackVState;
  root["minPackVState"] = st.minPackVState;
  root["maxCellCState"] = st.maxCellCState;
  root["minCellCState"] = st.minCellCState;
  root["maxPackCState"] = st.maxPackCState;
  root["minPackCState"] = st.minPackCState;

  root["nCells"] = dynSets.nCells;

  JsonArray data = root.createNestedArray("cells");
  uint16_t sumV = 0;
  for (uint8_t i = 0; i < dynSets.nCells; i++) {
    JsonObject cell = data.createNestedObject();
    cell["c"] = i;
    cell["v"] = st.cells[i].volts;
    cell["t"] = fromCel(st.cells[i].exTemp);
    cell["bt"] = fromCel(st.cells[i].bdTemp);
    cell["d"] = st.cells[i].draining;
    cell["l"] = !st.cells[i].conn;
    sumV += st.cells[i].volts;
  }
  root["mVDiff"] = sumV - st.lastPackMilliVolts;
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

void savetemps(AsyncWebServerRequest *request) {
  if (request->hasParam("t1B", true))
    dispSets.t1B = request->getParam("t1B", true)->value().toInt();
  if (request->hasParam("t1R", true))
    dispSets.t1R = request->getParam("t1R", true)->value().toInt();
  if (request->hasParam("t2B", true))
    dispSets.t2B = request->getParam("t2B", true)->value().toInt();
  if (request->hasParam("t2R", true))
    dispSets.t2R = request->getParam("t2R", true)->value().toInt();
  if (request->hasParam("nTSets", true))
    dispSets.nTSets = request->getParam("nTSets", true)->value().toInt();
  for (int i=0;i<dispSets.nTSets && i<NUM_TEMPSETS;i++) {
    char name[30];
    TempSet* ts = &dispSets.tSets[i];
    ts->dows = 0;
    for (int j=0;j<7;j++) {
      sprintf(name,"tset%d_%d",j,i);
      ts->dows |= request->hasParam(name,true) ? 1 << j: 0;
    }
    sprintf(name,"tsetSens%d",i);
    if (request->hasParam(name,true))
      ts->sens=request->getParam(name, true)->value().toInt();
    sprintf(name,"tsetRelay%d",i);
    if (request->hasParam(name,true))
      ts->relay=request->getParam(name, true)->value().toInt();
    sprintf(name,"tsetStart%d",i);
    if (request->hasParam(name,true))
      ts->startMin=request->getParam(name, true)->value().toInt();
    sprintf(name,"tsetEnd%d",i);
    if (request->hasParam(name,true))
      ts->endMin=request->getParam(name, true)->value().toInt();
    sprintf(name,"tsetTrip%d",i);
    if (request->hasParam(name,true))
      ts->tripTemp=request->getParam(name, true)->value().toInt();
    sprintf(name,"tsetRec%d",i);
    if (request->hasParam(name,true))
      ts->recTemp=request->getParam(name, true)->value().toInt();
  }
  for (int i=dispSets.nTSets;i<NUM_TEMPSETS;i++)
    dispSets.tSets[i].relay = 255;
  writeDispSet = true;
  sendSuccess(request);
}

void savelimits(AsyncWebServerRequest *request) {
  for (int l0=0;l0<LimitConsts::Max0;l0++) {
    for (int l1=0;l1<LimitConsts::Max1;l1++) {
      for (int l2=0;l2<LimitConsts::Max2;l2++) {
        for (int l3=0;l3<LimitConsts::Max3;l3++) {
          char name[5];
          sprintf(name,"%d%d%d%d",l0,l1,l2,l3);
          if (request->hasParam(name, true, false)) {
            if (l0 == LimitConsts::Temp)
              statSets.limits[l0][l1][l2][l3] = request->getParam(name, true, false)->value().toInt();
            else statSets.limits[l0][l1][l2][l3] = request->getParam(name, true, false)->value().toInt();
          }
        }
      }
    }
  }
  statSets.useBoardTemp = request->hasParam("useBoardTemp", true) && request->getParam("useBoardTemp", true)->value().equals("on");
  statSets.useCellC = request->hasParam("useCellC", true) && request->getParam("useCellC", true)->value().equals("on");

  if (request->hasParam("ChargePct", true))
    statSets.ChargePct = request->getParam("ChargePct", true)->value().toInt();
  if (request->hasParam("bdVolts", true))
    statSets.bdVolts = request->getParam("bdVolts", true)->value().toInt();
  if (request->hasParam("ChargePctRec", true))
    statSets.ChargePctRec = request->getParam("ChargePctRec", true)->value().toInt();

  if (request->hasParam("CellsOutMin", true))
    statSets.CellsOutMin = request->getParam("CellsOutMin", true)->value().toInt();
  if (request->hasParam("CellsOutMax", true))
    statSets.CellsOutMax = request->getParam("CellsOutMax", true)->value().toInt();
  if (request->hasParam("CellsOutTime", true))
    statSets.CellsOutTime = request->getParam("CellsOutTime", true)->value().toInt();

  if (request->hasParam("ShuntErrTime", true))
    statSets.ShuntErrTime = request->getParam("ShuntErrTime", true)->value().toInt();
  if (request->hasParam("MainID", true))
    statSets.MainID = request->getParam("MainID", true)->value().toInt();
  if (request->hasParam("PVID", true))
    statSets.PVID = request->getParam("PVID", true)->value().toInt();

  if (!statSets.useCellC) {
    st.maxCellCState = false;
    st.minCellCState = false;
  }
  if (!statSets.useBoardTemp) {
    st.maxPackCState = false;
    st.minPackCState = false;
  }
  writeRelaySet = true;
  BMSSend(&statSets);

  sendSuccess(request);
}

void saverelays(AsyncWebServerRequest *request) {
  for (int relay=0;relay<RELAY_TOTAL;relay++) {
    char name[16],type[3];
    RelaySettings *rp;
    if (relay < C_RELAY_TOTAL)
      rp = &statSets.relays[relay];
    else rp = &relSets.relays[relay - C_RELAY_TOTAL];
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
        case 'A':
          rp->type = Relay_Ampinvt; 
          break;
        case 'L':rp->type = Relay_Load;break;
        case 'C':rp->type = Relay_Charge; break;
        case 'T':rp->type = Relay_Therm; break;
        case 'H':rp->type = Relay_Heat; break;
        case 'U':rp->type = Relay_Unused; break;
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
  writeRelaySet = true;
  BMSSend(&statSets);

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

void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  //Serial.printf("%d\n",event);
  if (event == WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP)
  Serial.println(WiFi.localIP());
}

void SetPW(AsyncWebServerRequest *request,const char* src,char* dest) {
  if (request->hasParam(src, true)) {
    String pw = request->getParam(src,true)->value();
    if (pw == "null") dest[0] = 0;
    else if (pw.length()) {
      if (pw.length() < 8) {
        sendSuccess(request,"PW too short, 8 chars or none, or null",false);
        return;
      }
      pw.toCharArray(dest,sizeof(wifiSets.apPW));
    }
  }
}

void savewifi(AsyncWebServerRequest *request){
  if (request->hasParam("apName", true))
    request->getParam("apName", true)->value().toCharArray(wifiSets.apName,sizeof(wifiSets.apName));
  SetPW(request,"apPW",wifiSets.apPW);
  
  if (request->hasParam("ssid", true))
    request->getParam("ssid", true)->value().toCharArray(wifiSets.ssid,sizeof(wifiSets.ssid));
  SetPW(request,"password",wifiSets.password);
  writeWifiSet = true;
  sendSuccess(request);
}

void savecellset(AsyncWebServerRequest *request) {
  CellSetts msg;
  msg.cmd = SetCellSetts;
  msg.cellSets.time = request->getParam("cellTime", true)->value().toInt();
  msg.cellSets.drainV = request->getParam("drainV", true)->value().toInt();
  dynSets.cellSets = msg.cellSets;
  BMSSend(&msg);
  sendSuccess(request);
}

void saveItem(const AsyncWebServerRequest *request,const char* n,uint8_t cmd,uint16_t val) {
  SettingMsg msg;
  if (!request->hasParam(n, true)) return;
  const AsyncWebParameter *p1 = request->getParam(n, true);
  if (p1->value().length() == 0)
    return;
  msg.cmd = cmd;
  msg.val = p1->value().toInt();
  if (msg.val != val)
    BMSSend(&msg);
}

void savecapacity(AsyncWebServerRequest *request) {
  saveItem(request,"cellDelay",SetDelay,dynSets.delay);
  saveItem(request,"cellCnt",SetCnt,dynSets.cnt);
  bool rpo = request->hasParam("resPwrOn", true) && request->getParam("resPwrOn", true)->value().equals("on");
  if (rpo != dynSets.resPwrOn)
    {
      SettingMsg msg;
      msg.cmd = SetResPwrOn;
      msg.val = rpo;
      BMSSend(&msg);
    }
  dynSets.resPwrOn = rpo;
  saveItem(request,"CurSOC",SetCurSOC,101);
  saveItem(request,"BattAH",SetBattAH,dynSets.BattAH);
  saveItem(request,"TopAmps",SetTopAmps,dynSets.TopAmps);
  saveItem(request,"nCells",SetNCells,dynSets.nCells);
  sendSuccess(request);
}

void hideLastEventTime(AsyncWebServerRequest *request) {
  lastEventTime = 0;
  sendSuccess(request);
}

void toggleTemp(AsyncWebServerRequest *request) {
  dispSets.doCelsius = !dispSets.doCelsius;
  writeDispSet = true;
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  DynamicJsonDocument doc(100);
  doc["val"] = dispSets.doCelsius;
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
  server.on("/hideLastEventTime", HTTP_GET, hideLastEventTime);
  server.on("/saveemail", HTTP_POST, saveemail);
  server.on("/saveOff", HTTP_POST, saveOff);
  server.on("/fullChg", HTTP_POST, fullChg);
  server.on("/dump", HTTP_POST, dump);
  server.on("/forget", HTTP_POST, forget);
  server.on("/move", HTTP_POST, doMove);
  server.on("/savewifi", HTTP_POST, savewifi);
  server.on("/savecapacity", HTTP_POST, savecapacity);
  server.on("/savecellset", HTTP_POST, savecellset);
  server.on("/savetemps", HTTP_POST, savetemps);
  server.on("/savelimits", HTTP_POST, savelimits);
  server.on("/saverelays", HTTP_POST, saverelays);
  server.on("/cells", HTTP_GET, cells);
  server.on("/limits", HTTP_GET, limits);
  server.on("/relays", HTTP_GET, relays);
  server.on("/events", HTTP_GET, events);
  server.on("/temps", HTTP_GET, temps);
  server.on("/batt", HTTP_GET, batt);
  server.on("/net", HTTP_GET, net);
  server.on("/status", HTTP_GET, status);

  server.serveStatic("/static", SPIFFS, "/static").setLastModified("Mon, 20 Jun 2016 14:00:00 GMT");
  server.serveStatic("/", SPIFFS, "/");
  server.onNotFound(onRequest);
  server.begin();
}

void CheckWifi() {
  wifiMS = millis();
  if (WiFi.status() != WL_CONNECTED) {
    wifiDeadCnt++;
    Serial.printf("WiFi not connected %d\n",wifiDeadCnt);
    if (wifiDeadCnt >= 5) {
      if (WiFi.getMode() != WIFI_MODE_AP && WiFi.getMode() != WIFI_MODE_APSTA) {
         Serial.println("Starting AP");
         WiFi.disconnect();
        WiFi.softAP("TheBatt");
        startServer();
      }
    } else {
      WiFi.disconnect();
      WiFi.reconnect();
    }
  } else if (wifiDeadCnt) {
    wifiDeadCnt = 0;
    WiFi.softAPdisconnect();
  }
}

void WiFiInit() {
  WiFi.persistent(false);
  WiFi.disconnect(true,true);
  WiFi.mode(WIFI_MODE_NULL);
  WiFi.setHostname("TheBatt");
  if (wifiSets.ssid[0] != 0) {
    Serial.printf("%s:%s\n",wifiSets.ssid,wifiSets.password);
    WiFi.begin(wifiSets.ssid,wifiSets.password);
    wifiDeadCnt = 0;
  } else {
    WiFi.softAP("TheBatt");
  }
  Serial.printf("WIFI mode %d\n",WiFi.getMode());
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

struct ThermState {
  bool thermAct;
  int8_t heat,therm; // 1 heat, 0 no change, -1 off
  int8_t hVal,tVal;
  int8_t cell;
};
ThermState thermState[W_RELAY_TOTAL];
void checkTemps()
{
  tempMS = millis();
  digitalWrite(RESISTOR_PWR,HIGH);
  if (dynSets.delay)
    delay(dynSets.delay);
//  uint16_t vp;
//  uint32_t rt;
//  double T;
  Temp1 = BMSReadTemp(TEMP1,false,statSets.bdVolts,dispSets.t1B,dispSets.t1R,47000,dynSets.cnt);
//  Serial.printf("1: %d %d %d %f, ",vp,Temp1,rt,T);
  Temp2 = BMSReadTemp(TEMP2,false,statSets.bdVolts,dispSets.t2B,dispSets.t2R,47000,dynSets.cnt);
  // 2678 is 0 inches
  // 3000 is known R
  // 150 is slope
  // 8inches is 100%
  uint32_t v = BMSReadVoltage(WATER,dynSets.cnt);
//  Serial.printf("W: Cnt: %d %d %d\n",dynSets.cnt,v,((v * 300000) / (statSets.bdVolts - v)));
  if (v > 1210)
    Water = 200;
  else Water = 1000*(2678 - ((v * 3000) / (statSets.bdVolts - v))) / (1636*8);

  // min R 0, max R 90
  // 180 is known R, * 100 to get %
  v = BMSReadVoltage(GAS,dynSets.cnt);
//  Serial.printf("G: %d %d %d\n",statSets.bdVolts,v,((v * 18000) / (statSets.bdVolts - v)));
  if (v > 1210)
    Gas = 200;
  else Gas = ((v * 18000) / (statSets.bdVolts - v)) / 120; // should be a 90ohm
//  Serial.printf("2: %d %d\n",vp,Temp2);
  if (!dynSets.resPwrOn)
    digitalWrite(RESISTOR_PWR,LOW);

  int curMin = (curTime.tm_hour * 60) + curTime.tm_min;
  if (curTime.tm_year < 100)
    return;
  for (int y=0;y<W_RELAY_TOTAL;y++) {
    RelaySettings *rp = &relSets.relays[y];
    ThermState* tsp = &thermState[y];
    tsp->heat = tsp->therm = 0;
    tsp->cell = MAX_CELLS;
    tsp->thermAct = false;

    if (rp->type == Relay_Heat) {
      tsp->hVal = INT8_MAX;
      for (int i=0;i<dynSets.nCells;i++)
        if (st.cells[i].exTemp < tsp->hVal && st.cells[i].conn && st.cells[i].volts) { // find lowest temp
          tsp->hVal = st.cells[i].exTemp;
          tsp->cell = i;
        }
      if (tsp->hVal < rp->trip) tsp->heat = 1;
      else if (tsp->hVal > rp->rec) tsp->heat = -1;
    }
  }
  for (int i=dispSets.nTSets-1;i>=0;i--) { // work backwards to find first active on that relay
    TempSet* ts = &dispSets.tSets[i];
    if (ts->relay == 255) continue;
    if (ts->startMin < ts->endMin) {
      if (!(ts->dows & 1 << curTime.tm_wday)) continue;
      if (ts->startMin > curMin || ts->endMin < curMin) continue;
    } else {
      if (ts->startMin > curMin && ts->endMin < curMin) continue;
      if (ts->startMin < curMin && !(ts->dows & 1 << curTime.tm_wday)) continue;
      int dow = curTime.tm_wday - 1;
      if (dow < 0) dow = 6;
      if (ts->endMin > curMin && !(ts->dows & 1 << dow)) continue;
    }
    ThermState* tsp = &thermState[ts->relay];
    if (tsp->thermAct) continue;
    tsp->thermAct = true;
    tsp->tVal = ts->sens == 1 ? Temp1 : Temp2;
    if (tsp->tVal < ts->tripTemp) tsp->therm = 1;
    else if (tsp->tVal > ts->recTemp) tsp->therm = -1;
  }
  for (int y=0;y<W_RELAY_TOTAL;y++) {   // turn off any that were not active
    RelaySettings *rp = &relSets.relays[y];
    if (rp->type != Relay_Heat && rp->type != Relay_Therm) 
      continue;
    if (!thermState[y].thermAct)
      thermState[y].therm = -1;
  }
  for (int8_t y = 0; y < W_RELAY_TOTAL; y++)
  {
    RelaySettings *rp = &relSets.relays[y];
    if (rp->type != Relay_Heat && rp->type != Relay_Therm) 
      continue;
    ThermState* tsp = &thermState[y];
    if (rp->off) {
      digitalWrite(relayPins[y], LOW);
      if (previousRelayState[y] == HIGH) {
        Event *ep = NextEvent();
        ep->msg.cmd = HeaterOff;
        ep->msg.amps = previousHeaterOnSource[y];
        ep->msg.cell = MAX_CELLS;
      }
      previousHeaterOnSource[y] = Relay_Unused;
      previousRelayState[y] = LOW;
    } else if (previousRelayState[y] == LOW) {
      if (tsp->heat > 0 || tsp->therm > 0) {
        Event* ep = NextEvent();
        ep->msg.cmd = HeaterOn;
        ep->msg.relay = y;
        if (tsp->heat > 0) {
          ep->msg.cell = tsp->cell;
          ep->msg.tC = tsp->hVal;
          ep->msg.xtra = Relay_Heat;
          previousHeaterOnSource[y] = Relay_Heat;
        } else {
          ep->msg.cell = MAX_CELLS;
          ep->msg.tC = tsp->tVal;
          ep->msg.xtra = Relay_Therm;
          previousHeaterOnSource[y] = Relay_Therm;
        }
        digitalWrite(relayPins[y], HIGH);
    //    Serial.printf("T:%d on prev: %d, %d\n",y,previousRelayState[y],y);
        previousRelayState[y] = HIGH;
      }
    } else if ((tsp->heat < 0 && tsp->therm < 1 && previousHeaterOnSource[y] != Relay_Therm)
         || (tsp->therm < 0 && tsp->heat < 1 && previousHeaterOnSource[y] != Relay_Heat)) {
      Event* ep = NextEvent();
      ep->msg.cmd = HeaterOff;
      ep->msg.relay = y;
      ep->msg.xtra = previousHeaterOnSource[y];
      if (previousHeaterOnSource[y] == Relay_Heat) {
        ep->msg.cell = tsp->cell;
        ep->msg.tC = tsp->hVal;
      } else {
        ep->msg.cell = MAX_CELLS;
        ep->msg.tC = tsp->tVal;
      }
      previousHeaterOnSource[y] = Relay_Unused;
      digitalWrite(relayPins[y], LOW);
 //       Serial.printf("T:%d off prev: %d, %d\n",y,previousRelayState[y],y);
      previousRelayState[y] = LOW;
    }
  }
}

void checkStatus()
{
  statusMS = millis();
  uint8_t relay[W_RELAY_TOTAL];
  for (int8_t y = 0; y < W_RELAY_TOTAL; y++)
  {
    RelaySettings *rp = &relSets.relays[y];
    relay[y] = previousRelayState[y]; // don't change it because we might be in the SOC trip/rec area
    if (rp->off || rp->type == Relay_Unused)
      relay[y] = LOW;
    else {
      switch (rp->type) {
        case Relay_Connect: relay[y] = LOW; break; // don't put this on this CPU
        case Relay_Load:
          if (isFromOff(rp))
            relay[y] = HIGH;
          else if (rp->doSoC && (!st.stateOfChargeValid || st.stateOfCharge < rp->trip))
            relay[y] = LOW; // turn it off
          else if (!rp->doSoC || (rp->doSoC && st.stateOfChargeValid && st.stateOfCharge > rp->rec))
            relay[y] = HIGH; // turn it on
          // else leave it as-is
          break;
        case Relay_Charge:
          if (rp->doSoC && (!st.stateOfChargeValid || st.stateOfCharge > rp->trip))
            relay[y] = LOW; // off
          else if (!rp->doSoC || (rp->doSoC && st.stateOfChargeValid && st.stateOfCharge < rp->rec))
            relay[y] = HIGH; // on
          // else leave it as-is
          break;
      }
    }
  }
  for (int8_t n = 0; n < W_RELAY_TOTAL; n++)
  {
    if (previousRelayState[n] != relay[n])
    { // no effect on ampinv, because previous was set above to match
      digitalWrite(relayPins[n], relay[n]);
      previousRelayState[n] = relay[n];
      Serial.printf("Chg: %d to %d\n",n,previousRelayState[n]);
    }
  }
  if (st.doFullChg) {
    int i=0;
    for (i=0;i<dynSets.nCells && CellsDiff[i] == 0xffff;i++);
    bool isFirst = !(i<dynSets.nCells);
    
    for (int i=0;i<dynSets.nCells;i++) {
      if (CellsDiff[i] != 0xffff)
        continue;
      if (st.cells[i].volts < dynSets.cellSets.drainV)
        continue;
      if (isFirst)
        firstCellBalanceTime = millis();
      CellsDiff[i] = (uint16_t)(firstCellBalanceTime - millis())/1000;
    }
  }
  watchDog.once_ms(CHECKSTATUS+WATCHDOGSLOP,doWatchDog);
}

void MsgEvent(EventMsg *mp) {
  NextEvent(mp);
}
void WonSerData(const AMsg *mp)
{
//  Serial.printf("Msg: %d\n",mp->cmd);
  if (mp->cmd > FirstEvent && mp->cmd < LastEvent)
    MsgEvent((EventMsg*)mp);
  else switch(mp->cmd) {
    case DiscCell:
    case ConnCell: 
      { Event* ep = NextEvent();
      ep->msg.cmd = mp->cmd;
      ep->msg.cell = ((SettingMsg*)mp)->val; }
      break;
    case StatSets: statSets = *(StatSetts*)mp; break;
    case DynSets: dynSets = *(DynSetts*)mp; break;
    case DebugStr:
      snprintf(debugstr,sizeof(debugstr),"%s",((StrMsg*)mp)->msg);
      break;
    case Status:
      st = *(BMSStatus*)mp;
      checkStatus();
      break;  
  }
  digitalWrite(BLUE_LED,0);
}

void setup() {
  Serial.begin(9600);
  Serial.println("Alive");
  BMSADCInit();
  pinMode(IGN, INPUT);
  pinMode(INV, INPUT);
  pinMode(RESISTOR_PWR, OUTPUT);
  pinMode(BLUE_LED, OUTPUT);
  digitalWrite(BLUE_LED,1);
  adc1_config_channel_atten(TEMP1, ADC_ATTEN_DB_12);
  adc1_config_channel_atten(TEMP2, ADC_ATTEN_DB_12);
  adc1_config_channel_atten(WATER, ADC_ATTEN_DB_12);
  adc1_config_channel_atten(GAS, ADC_ATTEN_DB_12);
  BMSInitCom(&WonSerData);
  Wire.begin();
  if(!SPIFFS.begin()){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  debugstr[0] = 0;
  dynSets.cmd = Nada;
  if (!readEE("wifi",(uint8_t*)&wifiSets, sizeof(wifiSets))) {
    wifiSets.ssid[0] = 0;
    wifiSets.password[0] = 0;
    wifiSets.apName[0] = 0;
    wifiSets.apPW[0] = 0;
  }
//    strcpy(wifiSets.ssid,"Taves");
//    strcpy(wifiSets.password,"scruffy2023");
//    strcpy(wifiSets.ssid,"Eldorado Guest");
//    strcpy(wifiSets.password,"mauisunset");
  WiFi.onEvent(wifiEvent);
  WiFiInit();
  BMSInitStatus(&st);
  if (!readEE("comm",(uint8_t*)&commSets,sizeof(commSets))) {
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

  if (!readEE("relay",(uint8_t*)&relSets,sizeof(relSets)))
    InitRelays(&relSets.relays[0],W_RELAY_TOTAL);
  if (!readEE("disp",(uint8_t*)&dispSets,sizeof(dispSets))) {
    memset(&dispSets,0,sizeof(dispSets));
    dispSets.doCelsius = true;
    dispSets.t1B=dispSets.t2B=4050;
    dispSets.t1R=dispSets.t2R=47000;
    dispSets.nTSets = 1;
    dispSets.tSets[0].relay = 255;
  }

  for (int i=0;i<W_RELAY_TOTAL;i++)
    pinMode(relayPins[i],OUTPUT);
  clearRelays();
  GenUUID();

  smtpData.setSendCallback(emailCallback);
  startServer();
  configTzTime("PST8PDT,M3.2.0,M11.1.0","pool.ntp.org");
  AMsg msg;
  msg.cmd = StatQuery;
  while (BMSWaitFor(&msg,StatSets))
    Serial.println("Again");
  Serial.println("got static sets");
  msg.cmd = DynQuery;
  BMSWaitFor(&msg,DynSets);
  for (int i=0;i<MAX_EVENTS;i++)
    evts[i].when = 0;
  ArduinoOTA.begin();
  ResetCellsDiff();
  digitalWrite(BLUE_LED,0);
}

void loop() {
  if (lastMillis > millis()) milliRolls++;
  lastMillis = millis(); // for uptime to continue after 50 days

  if (writeCommSet) {
    writeEE("comm",(uint8_t*)&commSets,sizeof(commSets));
    writeCommSet = false;
  } else if (writeWifiSet) {
    writeEE("wifi",(uint8_t*)&wifiSets,sizeof(wifiSets));
    WiFiInit();
    writeWifiSet = false;
  } else if (writeDispSet) {
    writeEE("disp",(uint8_t*)&dispSets,sizeof(dispSets));
    writeDispSet = false;
  } else if (writeRelaySet) {
    writeEE("relay",(uint8_t*)&relSets,sizeof(relSets));
    writeRelaySet = false;
  }
  BMSGetSerial();

  if (sendEmail && emailSetup && strlen(commSets.senderServer)) {
    if (!MailClient.sendMail(smtpData))
      Serial.println("Error sending Email, " + MailClient.smtpErrorReason());
    sendEmail = false;
  }
  if ((millis() - statusMS) > (CHECKSTATUS+100)) /* +100 to deal with slop so this doesn't trigger if the Status message triggered it */
    checkStatus();
  if ((millis() - tempMS) > 6000)
    checkTemps();
  if ((millis() - wifiMS) > 15000)
    CheckWifi();

  ArduinoOTA.handle(); // this does nothing until it is initialized
}
