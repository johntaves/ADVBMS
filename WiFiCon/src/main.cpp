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
bool emailSetup=false,writeCommSet=false,writeWifiSet=false,writeDispSet=false,writeRelaySet=false;
uint32_t statusMS=0,tempMS=0;
HTTPClient http;
atomic_flag taskRunning(0);
Ticker watchDog,slider;

time_t lastEventTime=0;
int curEvent=0;
Event evts[MAX_EVENTS];

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

volatile uint32_t slideStart;
volatile int32_t slidePos[W_RELAY_TOTAL];
volatile bool slidingOut,doingAll = false;
int dirRelayPin,sliding = -1;
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
  bool notRecd = BMSWaitFor(&msg,StatSets);
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
  root["FloatV"]=statSets.FloatV;
  root["ChargeRate"]=statSets.ChargeRate;
  root["CellsOutMin"]=statSets.CellsOutMin;
  root["CellsOutMax"]=statSets.CellsOutMax;
  root["CellsOutTime"]=statSets.CellsOutTime;
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
  bool notRecd = BMSWaitFor(&msg,StatSets);
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = notRecd;
  root["slideMS"]=statSets.slideMS;
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
      case Relay_Slide: rule1["type"] = "S"; break;
      case Relay_Direction: rule1["type"] = "D"; break;
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
  Serial.printf("Es: %d\n",curEvent);
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

void slides(AsyncWebServerRequest *request){  
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  getRelayType(doc.to<JsonObject>(),Relay_Slide);

  serializeJson(doc, *response);
  request->send(response);
}

void batt(AsyncWebServerRequest *request){
  AMsg msg;
  msg.cmd = DynQuery;
  bool notRecd = BMSWaitFor(&msg,DynSets);
  AsyncResponseStream *response =
      request->beginResponseStream("application/json");
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  root["notRecd"] = notRecd;

  root["ShuntErrTime"] = dynSets.ShuntErrTime;
  root["MainID"] = dynSets.MainID;
  root["PVID"] = dynSets.PVID;
  root["BattAH"] = dynSets.BattAH;
  root["TopAmps"] = dynSets.TopAmps;

  root["socLastAdj"] = st.lastAdjMillAmpHrs;
  snprintf(spb,sizeof(spb),"%d:%d",(int)(st.aveAdjMilliAmpMillis / ((int64_t)1000 * 60 * 60)),st.adjCnt);
  root["socAvgAdj"] = spb;
  root["BatAHMeasured"] = st.BatAHMeasured > 0 ? String(st.BatAHMeasured) : String("N/A");

  root["nCells"] = dynSets.nCells;

  root["cellCnt"] = dynSets.cellSets.cnt;
  root["cellDelay"] = dynSets.cellSets.delay;
  root["resPwrOn"] = dynSets.cellSets.resPwrOn;
  root["cellTime"] = dynSets.cellSets.time;

  serializeJson(doc, *response);
  request->send(response);
}

void slideGo(int);
void _StopSlide() {
  if (sliding < 0)
    return;
  digitalWrite(relayPins[sliding],LOW);
  uint32_t diff = millis() - slideStart;
  if (slidingOut) slidePos[sliding] += diff;
  else slidePos[sliding] -= diff;
  if (slidePos[sliding] < 0) slidePos[sliding] = 0;
//  Serial.printf("Stop: %d\n",slidePos[sliding]);
  slider.detach();
  if (dirRelayPin)
    digitalWrite(dirRelayPin,LOW);
}

int nextSlide(int cur) {
  for (int i=0;i<W_RELAY_TOTAL;i++)
    if (relSets.relays[i].type == Relay_Slide && slidePos[i] < 0)
      return -1;
  for (int i=cur+1;i<W_RELAY_TOTAL;i++)
    if (relSets.relays[i].type == Relay_Slide)
      return i;
  return -1;
}

void slideTimeUp() {
  _StopSlide();
  if (doingAll) {
    sliding = nextSlide(sliding);
    if (sliding < 0)
      doingAll = false;
    else
      slideGo(sliding);
  } else
    sliding = -1;
}

void stopSlide() {
  doingAll = false;
  _StopSlide();
  sliding = -1;
}

void slideGo(int r) {
  uint32_t rem;
//Serial.printf("G: %d %d %d\n",r,slidePos[r],sliding);
  if (slidePos[r] < 0) {
    slidingOut = false;
    rem = statSets.slideMS;
  } else if (slidingOut)
    rem = statSets.slideMS - slidePos[r];
  else rem = slidePos[r]+2000; // because the in will stop at the stops OK, so add extra to eliminate accumulated error
  if (rem <= statSets.slideMS || !slidingOut) {
    slideStart = millis();
  //  Serial.printf("Sliding %d %d %s\n",r,rem,(slidingOut?"out":"in"));
    if (rem) {
      sliding = r;
      digitalWrite(relayPins[r],HIGH);
      digitalWrite(dirRelayPin,slidingOut ? LOW : HIGH);
      slider.once_ms(rem,slideTimeUp);
    }
  }

}
void slideIn(int r) {
  stopSlide();
  slidingOut = false;
  slideGo(r);
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
    if (rp->type == Relay_Slide)
      slideIn(r - C_RELAY_TOTAL);
    else rp->off = !rp->off;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument doc(100);
    doc["relay"] = msg.val;
    doc["val"] = rp->off ? "off" : "on";
    doc["success"] = true;
    serializeJson(doc, *response);
    request->send(response);

  } else request->send(500, "text/plain", "Missing parameters");
}

void slide(AsyncWebServerRequest *request) {
  stopSlide();
  if (request->hasParam("relay", true)) {
    int r = request->getParam("relay", true)->value().toInt();
    slidingOut = request->getParam("dir", true)->value() == "true";

    if (dirRelayPin)
      slideGo(r);
  }
  sendSuccess(request);
}

void slideStop(AsyncWebServerRequest *request) {
  stopSlide();
  sendSuccess(request);
}

void goAll() {
  doingAll = true;
  int i = nextSlide(-1);
  if (i>=0) slideGo(i);
}
void allOut(AsyncWebServerRequest *request) {
  stopSlide();
  slidingOut = true;
  goAll();
  sendSuccess(request);
}

void allIn(AsyncWebServerRequest *request) {
  stopSlide();
  slidingOut = false;
  goAll();
  sendSuccess(request);
}

void fullChg(AsyncWebServerRequest *request) {
  AMsg msg;
  msg.cmd = FullChg;
  BMSSend(&msg);
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
  int days = upsecs / (24ul*60*60);
  upsecs = upsecs % (24ul*60*60);
  int hrs = upsecs / (60*60);
  upsecs = upsecs % (60*60);
  int min = upsecs / 60;

  snprintf(spb,sizeof(spb),"%d:%02d:%02d:%02d",days,hrs,min,upsecs % 60);
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
  root["version"] = "V: 1.0";
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
    if (strlen(rp->name) == 0 || rp->type == Relay_Direction)
      continue;
    sprintf(dodad,"relayStatus%d",i);
    root[dodad] = state==HIGH?"ON":"OFF";
    sprintf(dodad,"relayName%d",i);
    root[dodad] = rp->name;
    sprintf(dodad,"relayOff%d",i);
    root[dodad] = rp->off ? "off" : "on";
    sprintf(dodad,"relaySlide%d",i);
    if (rp->type == Relay_Slide && i >= C_RELAY_TOTAL) {
      int r = i - C_RELAY_TOTAL;
      int32_t pos = slidePos[r];
      if (pos < 0) root[dodad] = "?";
      else {
        if (sliding == r) {
          if (slidingOut)
            pos += millis() - slideStart;
          else pos -= millis() - slideStart;
          if (pos < 0)
            pos=0;
        }
        root[dodad] = pos*100 / statSets.slideMS;
      }
    }
  }

  root["packcurrent"] = st.lastMilliAmps;
  root["packvolts"] = st.lastPackMilliVolts;
  root["pvvolts"] = st.lastPVMilliVolts;
  root["pvcurrent"] = st.lastPVMilliAmps;
  snprintf(spb,sizeof(spb),"%d%%",st.stateOfCharge);
  root["soc"] = spb;
  root["socvalid"] = st.stateOfChargeValid;
  root["BoardTemp"] = fromCel(st.curBoardTemp);
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
  for (uint8_t i = 0; i < dynSets.nCells; i++) {
    JsonObject cell = data.createNestedObject();
    cell["c"] = i;
    cell["v"] = st.cells[i].volts;
    cell["t"] = fromCel(st.cells[i].exTemp);
    cell["bt"] = fromCel(st.cells[i].bdTemp);
    cell["d"] = st.cells[i].dumping;
    cell["l"] = !st.cells[i].conn;
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
              statSets.limits[l0][l1][l2][l3] = toCel(request->getParam(name, true, false)->value());
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
  if (request->hasParam("FloatV", true))
    statSets.FloatV = request->getParam("FloatV", true)->value().toInt();
  if (request->hasParam("ChargeRate", true))
    statSets.ChargeRate = request->getParam("ChargeRate", true)->value().toInt();
  if (request->hasParam("CellsOutMin", true))
    statSets.CellsOutMin = request->getParam("CellsOutMin", true)->value().toInt();
  if (request->hasParam("CellsOutMax", true))
    statSets.CellsOutMax = request->getParam("CellsOutMax", true)->value().toInt();
  if (request->hasParam("CellsOutTime", true))
    statSets.CellsOutTime = request->getParam("CellsOutTime", true)->value().toInt();

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
  dirRelayPin = 0;
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
        case 'D':
          stopSlide();
          if (!dirRelayPin) {
            rp->type = Relay_Direction;
            dirRelayPin = relayPins[relay - C_RELAY_TOTAL];
          } else 
            rp->type = Relay_Unused;
          break;
        case 'S':
          stopSlide();
          rp->type = Relay_Slide;
          break;
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
  if (request->hasParam("slideMS", true))
    statSets.slideMS = request->getParam("slideMS", true)->value().toInt();
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

void onconnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  Serial.println(WiFi.localIP());
}

void WiFiInit() {
  WiFi.persistent(false);
  WiFi.disconnect(true,true);
  if (strlen(wifiSets.apName)) {
    Serial.printf("%s:%s",wifiSets.apName,wifiSets.apPW);
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

void savecellset(AsyncWebServerRequest *request) {
  CellSetts msg;
  msg.cmd = SetCellSetts;
  msg.cellSets.cnt = request->getParam("cellCnt", true)->value().toInt();
  msg.cellSets.delay = request->getParam("cellDelay", true)->value().toInt();
  msg.cellSets.resPwrOn = request->hasParam("resPwrOn", true) && request->getParam("resPwrOn", true)->value().equals("on");
  msg.cellSets.time = request->getParam("cellTime", true)->value().toInt();
  dynSets.cellSets = msg.cellSets;
  BMSSend(&msg);
  sendSuccess(request);
}

void saveItem(AsyncWebServerRequest *request,const char* n,uint8_t cmd,uint16_t val) {
  SettingMsg msg;
  if (!request->hasParam(n, true)) return;
  AsyncWebParameter *p1 = request->getParam(n, true);
  if (p1->value().length() == 0)
    return;
  msg.cmd = cmd;
  msg.val = p1->value().toInt();
  if (msg.val != val)
    BMSSend(&msg);
}

void savecapacity(AsyncWebServerRequest *request) {
  saveItem(request,"CurSOC",SetCurSOC,101);
  saveItem(request,"ShuntErrTime",ShuntErrTime,dynSets.ShuntErrTime);
  saveItem(request,"MainID",SetMainID,dynSets.MainID);
  saveItem(request,"PVID", SetPVID,dynSets.PVID);
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
  server.on("/slide", HTTP_POST, slide);
  server.on("/slideStop", HTTP_GET, slideStop);
  server.on("/allOut", HTTP_GET, allOut);
  server.on("/allIn", HTTP_GET, allIn);
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
  server.on("/slides", HTTP_GET, slides);
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
  if (dynSets.cellSets.delay)
    delay(dynSets.cellSets.delay);
//  uint16_t vp;
//  uint32_t rt;
//  double T;
  Temp1 = BMSReadTemp(TEMP1,false,statSets.bdVolts,dispSets.t1B,dispSets.t1R,47000,dynSets.cellSets.cnt);
//  Serial.printf("1: %d %d %d %f, ",vp,Temp1,rt,T);
  Temp2 = BMSReadTemp(TEMP2,false,statSets.bdVolts,dispSets.t2B,dispSets.t2R,47000,dynSets.cellSets.cnt);

  // 1657 is 0 inches
  // 3000 is known R
  // 157 is slope
  // 8inches is 100%
  uint32_t v = BMSReadVoltage(WATER,dynSets.cellSets.cnt);
  Serial.printf("W: %d %d\n",v,((v * 300000) / (statSets.bdVolts - v)));
  if (v > 1200)
    Water = 200;
  else Water = (165700 - ((v * 300000) / (statSets.bdVolts - v))) / (157*8);

  // min R 0, max R 90
  // 180 is known R, * 100 to get %
  v = BMSReadVoltage(GAS,dynSets.cellSets.cnt);
  Serial.printf("G: %d %d\n",v,((v * 18000) / (statSets.bdVolts - v)));
  if (v > 1200)
    Gas = 200;
  else Gas = ((v * 18000) / (statSets.bdVolts - v)) / 90;
//  Serial.printf("2: %d %d\n",vp,Temp2);
  if (!dynSets.cellSets.resPwrOn)
    digitalWrite(RESISTOR_PWR,LOW);

  struct tm t;
  getLocalTime(&t);
  int curMin = (t.tm_hour * 60) + t.tm_min;
  if (t.tm_year < 100)
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
      if (!(ts->dows & 1 << t.tm_wday)) continue;
      if (ts->startMin > curMin || ts->endMin < curMin) continue;
    } else {
      if (ts->startMin > curMin && ts->endMin < curMin) continue;
      if (ts->startMin < curMin && !(ts->dows & 1 << t.tm_wday)) continue;
      int dow = t.tm_wday - 1;
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
    if (rp->type == Relay_Direction || rp->type == Relay_Slide) 
      continue;
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
    { // no effect on slide and dir and ampinv, because previous was set above to match
      digitalWrite(relayPins[n], relay[n]);
      previousRelayState[n] = relay[n];
      Serial.printf("Chg: %d to %d\n",n,previousRelayState[n]);
    }
  }
  watchDog.once_ms(CHECKSTATUS+WATCHDOGSLOP,doWatchDog);
}

void MsgEvent(EventMsg *mp) {
  Serial.println("Event");
  NextEvent(mp);
}
void WonSerData(const AMsg *mp)
{
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

void xSendStatus(void* unused) {
  sendStatus();
  vTaskDelete(NULL);
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
  adc1_config_channel_atten(TEMP1, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(TEMP2, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(WATER, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(GAS, ADC_ATTEN_DB_11);
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
  WiFi.onEvent(onconnect, WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
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
  dirRelayPin = 0;
  for (int i=0;i<W_RELAY_TOTAL;i++) {
    slidePos[i] = -1;
    if (relSets.relays[i].type == Relay_Direction) {
      if (dirRelayPin)
        relSets.relays[i].type = Relay_Unused;
      else dirRelayPin = relayPins[i];
    }
  }
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
  BMSWaitFor(&msg,StatSets);
  msg.cmd = DynQuery;
  BMSWaitFor(&msg,DynSets);
  for (int i=0;i<MAX_EVENTS;i++)
    evts[i].when = 0;
  ArduinoOTA.begin();
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

  ArduinoOTA.handle(); // this does nothing until it is initialized
}
