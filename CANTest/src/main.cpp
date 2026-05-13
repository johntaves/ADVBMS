#include <Arduino.h>
#include <can.h>

union ArrTo8 {
  byte array[8];
  int64_t val;
} __attribute__((packed));
union ArrTo4 {
  byte array[4];
  int32_t val;
} __attribute__((packed));
union ArrTo2 {
  byte array[2];
  int16_t val;
} __attribute__((packed));

int cnt=0;
uint32_t last = 0;
int led=0;
uint16_t curId = 0x300;
struct chunk {
  long id;
  int packetSize;
  uint8_t code;
  int64_t val;
} buff[100];
int curWrite=0,curRead=0;

void canSender(uint16_t id,byte n,byte p[],int len) {
    Serial.printf("Asking 0x%x 0x%x, ",id | 0xFB,n);
    byte mod = 0xfb;
    if (len) mod = 0xfa;
    if (!CAN.beginPacket (id | mod))
      Serial.println("Cannot begin packet"); 
    CAN.write (n);
    for (int i=0;i<len;i++) {
      Serial.printf("%x",p[i]);
      CAN.write(p[i]);
    }
    CAN.endPacket();
    Serial.println ("done");
}
void canSetID(byte n,byte x) {
    if (!CAN.beginPacket (0x3fA))
      Serial.println("Cannot begin packet"); 
    CAN.write(0x11);
    CAN.write(0x03);
    CAN.write(x);
    CAN.write(n);
    CAN.write(x);
    CAN.endPacket();
}
void canSetIDs(byte n) {
  for (byte x = 0xF1;x< 0xf8;x++) {
    canSetID(n,x);
  }
  canSetID(n,0xFB);
  canSetID(n,0xFC);
  canSetID(n,0xFA);
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
  Serial.printf("Len is %d\n",len);
  while (len--)
    CAN.read();
  return 0;
}

void CANReceive(int packetSize) {
    // received a packet
  if (CAN.packetExtended()) {
    Serial.print("extended ");
  }

  if (CAN.packetRtr()) {
    // Remote transmission request, packet contains no data
    Serial.print("RTR ");
  }
  buff[curWrite].packetSize = packetSize;
  long id = buff[curWrite].id = CAN.packetId();

  if (CAN.packetRtr()) {
    Serial.print(" and requested length ");
    Serial.println(CAN.packetDlc());
  } else {
    id &= 0xff;
    if (id == 0xfc) {
      buff[curWrite].code = CAN.read();
      packetSize--;
      
    }
    if (id < 0xfa) buff[curWrite].val = ReadIt(packetSize,1);
    else buff[curWrite].val = ReadIt(packetSize,-1);
  }
  curWrite++;
  if (curWrite >= sizeof(buff)/sizeof(chunk))
    curWrite=0;
}

void FindIt()
{
  
}

void setup() {
  Serial.begin(9600);
  pinMode(GPIO_NUM_23, OUTPUT);
  digitalWrite(GPIO_NUM_23,1);

  CAN.setPins(GPIO_NUM_21, GPIO_NUM_22);
  if (!CAN.begin(1000E3)) {
    Serial.println("Starting CAN failed!");
    while (1);
  } else
    Serial.println("Started CAN");
    CAN.onReceive(CANReceive);
  Serial.println("Hi");
}

void loop() {
  if (Serial.available() > 0) {
    char b = Serial.read();
     Serial.println(b);
    switch (b) {
      case 'c': canSender(curId,0x04,NULL,0); break;
      case 'i': canSender(curId,0x31,NULL,0); break;
      case 't': canSender(curId,0x2,NULL,0); break;
      case 'v': canSender(curId,0x3,NULL,0); break;
      case 'e': canSender(curId,0x7,NULL,0); break;
      case 'a': canSender(curId,0x1,NULL,0); break;
      case 'f': canSender(curId,0x30,NULL,0); break;
      case 'A': {
        byte w[] = {0x44,0x5C };
        canSender(curId,0x17,w,2);
        byte x[] = {0x2,0x0};
        canSender(curId,0x16,x,2);
        byte y[] = { 0x03,0x1A  };
        canSender(curId,0x12,y,2); 
  //      clk=millis();
        }
        break; 
      case 'm': {
        char x[5];
        int amt=0;
        while (amt < 4)
          amt += Serial.read(&x[amt],4);
        x[4] = 0;
        Serial.println(x);
        byte y[] = { 0x1e,0x1b };
        byte val[3];
        sscanf(x,"%hx",val);
        val[2]=val[0];val[0]=val[1];val[1]=val[2];
        Serial.printf("0x%02x 0x%02x\n",val[0],val[1]);
        canSender(curId,0x12,val,2); }
        break; 
      case 'd': canSetIDs(curId >> 8); break;
      case '3': curId = 0x300; Serial.println(curId); break;
      case '4': curId = 0x400; Serial.println(curId); break;
      case '5': curId = 0x500; Serial.println(curId); break;
      case '6': curId = 0x600; Serial.println(curId); break;
      case 's': 
        { byte x[] = {0x0,0xf};
        canSender(curId,0x10,x,2); break; }
      case 'F': FindIt();
        break;
      default:
        break;
    }
  }
  if ((millis() - last) > 2000) {
    last = millis();
    //Serial.printf("Send it %d\n",snt++);
    digitalWrite(GPIO_NUM_23,led % 2);
    led++;
  }
  if (curRead != curWrite) {
    Serial.printf("i: %02x, c: 0x%x, l: %d, val:%lld\n",buff[curRead].id,buff[curRead].code,buff[curRead].packetSize,buff[curRead].val);
    curRead++;
    if (curRead >= sizeof(buff)/sizeof(chunk))
      curRead = 0;
  }
}
