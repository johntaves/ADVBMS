
#include <EEPROM.h>
#include <string.h>
#include "BMSCommArd.h"
#include <PacketSerial.h>

#define CPUBAUD 115200

PacketSerial_<COBS, 0, sizeof(union MaxData)+10> dataSer;
void (*commCB)(const AMsg* buf);

uint8_t CRC8(const uint8_t *data,uint16_t length) 
{
   uint8_t crc = 0x00;
   uint8_t extract;
   uint8_t sum;
   for(uint16_t i=0;i<length;i++)
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

void InitRelays(RelaySettings* rp,int num) {
  RelaySettings* r=rp;
  for (int i=0;i<num;i++,r++) {
      r->name[0] = 0;
      r->from[0] = 0;
      r->doSoC = false;
      r->off = true;
      r->fullChg = false;
      r->rec = 0;
      r->trip = 0;
      r->type = 0;
  }
}

uint8_t txBuffer[sizeof(MaxData)]; // this is so that we do not mangle the data sent to BMSSendRaw

void BMSSendRaw(uint8_t *d,uint16_t len) {
  if (!len) return;
  *d = CRC8(d+1,len-1);
  if (((AMsg*)d)->cmd != Status)
    printf("Sending: %d, %d, 0x%x\n",d[1],len,*d);
  dataSer.send((byte*)d,len);
}

void BMSSend(StrMsg* m) {
  BMSSendRaw((uint8_t*)m,sizeof(StrMsg) - sizeof(m->msg) + strlen(m->msg));
}

uint8_t lastCmd;

bool BMSWaitFor(AMsg* mp,uint8_t cmd) {
  uint32_t stMs = millis();
  BMSSend(mp);
  lastCmd = Panic;
  while (lastCmd != cmd && (millis() - stMs) < 100)
    dataSer.update();
  return lastCmd != cmd;
}

void onSerData(const uint8_t *d, size_t cnt)
{
  if (cnt && CRC8(&d[1],cnt-1) == d[0] && commCB) {
    AMsg* m = (AMsg*)d;
    if (m->cmd != Status)
      printf("Rec: %d, %d, 0x%x\n",d[1],cnt,d[0]);
    commCB(m);
    lastCmd = m->cmd;
  }
}

void BMSInitCom(size_t sz,void (*func)(const AMsg* buf)) {

  commCB = func;
  Serial2.begin(CPUBAUD);
  dataSer.setStream(&Serial2); // start serial for output
  dataSer.setPacketHandler(&onSerData);
  EEPROM.begin(sz);

}

void BMSGetSerial() {
  dataSer.update();
}
