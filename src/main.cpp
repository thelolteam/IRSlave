#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <set>
#include <FS.h>
#include <IRremoteESP8266.h>
#include <IRac.h>
#include <IRrecv.h>

#define EEPROM_SIZE 48
#define powerBtn 2
#define led 16
#define minDown 100
#define clickGap 400
#define hold 1200
#define frequency 38
#define recvPin 14
#define sendPin 4
#define kTimeout 50
#define minRawLen 30

const uint16_t kCaptureBufferSize = 1024;

IRrecv irrecv(recvPin, kCaptureBufferSize, kTimeout);
IRsend irsend(sendPin);

decode_results results;

using namespace std;

unsigned long cur;

int ipAssigned = 0;
int mode = 0;
const char* default_SSID="ESP32";
const char* default_pass="12345678";
const char* default_name = "IRNode";
const int ssidLoc = 0, passLoc = 10, nameLoc = 20; 
char ssid[11], password[11], name[11];

const int port = 8080;
IPAddress masterIP(192, 168, 1, 1);
ESP8266WebServer server(port);
HTTPClient client;

int id = -1;
int conStat = 0;
const int type = 3;
String action;
set<String> fileName;
set<String>::iterator setIterator;
File file;
int irLen;
uint16_t *irSignal;

String message, url="";

String parameter[7];

WiFiEventHandler gotIpEventHandler;
WiFiEventHandler stationModeDisconnectedHandler;

void printDetails(){
  Serial.print("\n\nId: ");
  Serial.println(id);
  Serial.print("Constat: ");
  Serial.println(conStat);
  Serial.print("ssid: ");
  Serial.println(ssid);
  Serial.print("password: ");
  Serial.println(password);
  Serial.print("Name: ");
  Serial.println(name);
  Serial.println("Buttons: ");
  for(setIterator = fileName.begin(); setIterator!=fileName.end(); setIterator++){
    Serial.print(*setIterator);
    Serial.print(", ");
  }
}

/*void blink(int times){
  for(int i=0; i<times; i++){
    digitalWrite(led, HIGH);
    delay(500);
    digitalWrite(led, LOW);
    delay(300);
  }
}*/


void writeMemory(char addr, char *data){
  int i;
  for(i=0; data[i]!='\0' && i<10; i++){
    EEPROM.write(addr+i, data[i]);
  }
  EEPROM.write(addr+i, '\0');
  EEPROM.commit();
}

void readMemory(char addr, char *data){
  int l = 0;
  char k = EEPROM.read(addr);
  while(k!='\0' && l<10){
    k = EEPROM.read(addr+l);
    data[l] = k;
    l++;
  }
  data[l] = '\0';
}

void getMetaData(){
  readMemory(ssidLoc, ssid);
  readMemory(passLoc, password);
}

void setMetaData(){
  writeMemory(ssidLoc, ssid);
  writeMemory(passLoc, password);
}

void getName()
{
  readMemory(nameLoc, name);
}

void setName()
{
  writeMemory(nameLoc, name);
}

void listFileNames(){
  Serial.println("\nFILE LIST:");
  Dir root = SPIFFS.openDir("/");
  Serial.println("Root Opened");
  while(root.next()){
    Serial.print("FILE Detected: ");
    Serial.println(root.fileName());
  }
}

void getFileNames(){
  Dir root = SPIFFS.openDir("/");
  Serial.println("Root Opened");
  while(root.next()){
    Serial.print("FILE Detected: ");
    Serial.println(root.fileName().substring(1));
    fileName.insert(root.fileName().substring(1));
  }
}

uint16_t * readFile(String fname){
  File file = SPIFFS.open(fname.c_str(), "r");
  irLen = file.parseInt();
  char c = file.read();
  c = file.read();
  uint16_t *d = (uint16_t *)malloc(sizeof(uint16_t)*irLen);
  if(!file){
    irLen = 0;
    return NULL;
  }else{
    int i=0;
    while(file.available()){
      d[i] = file.parseInt();
      Serial.print("Read: ");
      Serial.println(d[i]);
      c = file.read();
      c = file.read();
      i++;
    }
    file.close();
    Serial.print("File Read: ");
    Serial.println(fname);
    return d;
  }
}

void restartDevice(){
  Serial.println("Restarting....");
  ESP.restart();
}

void deleteAllFiles(){
  Dir root = SPIFFS.openDir("/");
  while(root.next()){
      Serial.print("FILE Removed: ");
      Serial.println(root.fileName());
      SPIFFS.remove(root.fileName());
  }
}

void resetDevice(){
  strcpy(ssid, default_SSID);
  strcpy(password, default_pass);
  strcpy(name, default_name);
  setMetaData();
  setName();
  deleteAllFiles();
  Serial.println("Device Reset");
  restartDevice();
}

void separateParameters(String &body){
  int startI = 0, endI = 0, i;
  for(i=0; i<7; i++){
    parameter[i] = "";
    if(startI<body.length()){
      endI = body.indexOf('$', startI);
      parameter[i] = body.substring(startI, endI);
      startI = endI+1;
    }
  }
}

void sendReply(String message){
  Serial.print("Replying with: ");
  Serial.println(message);
  server.send(200, "text/plain", message);
}

bool saveIR(String &action){
  bool success = false;
  String fn = "/" + action;
  file = SPIFFS.open(fn.c_str(), "w");
  if(!file)
    return success;
  else{
    file.println(irLen);
    Serial.print("Wrote Size: ");
    Serial.println(irLen);
    for(int i=0; i<irLen; i++){
      if(!file.println(irSignal[i])){
        Serial.println("Write Fail");
        return success;
      }
    }
    file.close();
    Serial.print("File Wrote: ");
    Serial.println(fn);
    listFileNames();
    success = true;
    fileName.insert(action);
    return success;
  }
}

void recordIR(){
  irrecv.enableIRIn();
  digitalWrite(led, HIGH);
  unsigned long c = millis();
  while(1 && millis()-c < 10000){
    yield();
    if(irrecv.decode(&results)){
      digitalWrite(led, LOW);
      irLen = results.rawlen;
      if(irLen<minRawLen){
        irrecv.resume();
        digitalWrite(led, HIGH);
        continue;
      }
      irSignal = (uint16_t *)malloc(sizeof(uint16_t)*(irLen-1));
      for(int i=1; i<irLen; i++){
        if(results.rawbuf[i]<0)
          irSignal[i-1] = 0 - results.rawbuf[i] * kRawTick;
        else
          irSignal[i-1] = results.rawbuf[i] * kRawTick;
      }
      irLen = results.rawlen-1;
      irrecv.disableIRIn();
      Serial.println("Read From IR Receiver Success");
      digitalWrite(led, LOW);
      delay(3000);
      if(irLen!=0){
        irsend.begin();
        delay(100);
        Serial.print("Testing IR...");
        digitalWrite(led, HIGH);
        irsend.sendRaw(irSignal, irLen, frequency);
        digitalWrite(led, LOW);
      }
      break;
    }
  }
  Serial.println("Receiver Stopped");
  digitalWrite(led, LOW);
}

void blastIR(String &action){
  if(action[0] != '/')
    action = "/" + action;
  uint16_t *tempSignal = readFile(action);
  if(irLen!=0){
    irsend.begin();
    delay(100);
    Serial.print("Sending IR...of: ");
    Serial.println(action);
    digitalWrite(led, HIGH);
    irsend.sendRaw(tempSignal, irLen, frequency);
    digitalWrite(led, LOW);
  }
}

void sendPacket(IPAddress ip, int port, String &message){
  url = "http://";
  url.concat(ip.toString());
  url.concat(":8080/message?data=");
  url.concat(message);

  Serial.print("URL: ");
  Serial.println(url);
  client.begin(url);

  int retry = 5;
  int httpCode = client.GET();
  if(httpCode > 0){
    if(httpCode == HTTP_CODE_OK){
      Serial.printf("\nRequest Sent: %d\n", httpCode);
      client.end();
      retry--;
    }
  }else{
    Serial.println("HTTP GET Error");
    client.end();
    delay(1000);
    if(retry>0)
      sendPacket(ip, port, message);
  }
}

void sendNodeStat(){
  message = "client@node$action@stat$3$";
  message.concat(id);
  message.concat("$");
  message.concat(name);
  bool hasIrNames = false;
  for(setIterator = fileName.begin(); setIterator!=fileName.end(); setIterator++){
      message.concat("_");
      message.concat(*setIterator);
      hasIrNames = true;
  }
  if(hasIrNames)
    message.concat("_");
  message.concat("$");
  message.concat(conStat);
  message.concat("$0$");

  sendPacket(masterIP, port, message);
}

void refactorFileNames(){
  Dir root = SPIFFS.openDir("/");
  for(setIterator = fileName.begin(); setIterator!=fileName.end(); setIterator++){
    bool present = false;
    while(root.next()){
      if((*setIterator).equals(root.fileName())){
        present = true;
      }
      if(!present)
        fileName.erase(*setIterator);
    }
  }
}

void setStat(){
  String temp = parameter[4];
  String tempName = temp;
  conStat = parameter[5].toInt();
  int i = tempName.indexOf("_");
  if(i!=-1){
    tempName = tempName.substring(0, i);
    i++;
    String temp = temp.substring(i);
    int startI = 0, endI = 0, i;
    for(i=0; startI<temp.length(); i++){
      endI = temp.indexOf('_', startI);
      if(fileName.find(temp.substring(startI, endI)) != fileName.end())
        fileName.insert(temp.substring(startI, endI));
      startI = endI+1;
    }
  }
  if(strcmp(name, tempName.c_str()) != 0){
      strcpy(name, tempName.c_str());
      setName();
  }
}

void parameterDecode()
{
  if(parameter[1].equals("action@stat"))
  { 
    sendReply("Node: Stat RCVD");
    setStat();
  }
  else if(parameter[1].equals("action@task")){
    sendReply("IRNode: IR Request");
    action = parameter[6];
    if(fileName.find(action) != fileName.end()){
      blastIR(action);
    }else{
      //Meaning App has wrong infor about available IR Actions
      sendNodeStat();
    }
  }
  else if(parameter[1].equals("action@config"))
  {
    if(parameter[4].equals("APP")){
      //Handshake with APP
      sendReply("IRNode: Config Request From APP");
      //The id sent in this message is for app. Just a shell so app feels like successfull handshake
      message = "client@IRNode$action@config$3$1$";
      message.concat(name);
      message.concat("$");
      conStat = 1;
      message.concat(conStat);
      message.concat("$0$");
      masterIP = server.client().remoteIP();
      sendPacket(masterIP, port, message);
      //this is the id of this node when its connected to app directly. App requires this for event handling.
      //this will be sent to app via sendNodeStat when app requests getNodeList
      id = 0;
    }else{
      //Handshake with master
      sendReply("Node: Config RCVD from Master"); 
      id = parameter[3].toInt();
      conStat = parameter[5].toInt();
    }
    digitalWrite(led, LOW);
  }
  else if(parameter[1].equals("action@apconfig"))
  {
    sendReply("NODE: APConfig RCVD");
    strcpy(ssid, parameter[2].c_str());
    strcpy(password, parameter[3].c_str());
    setMetaData();
    setName();
    delay(7000);
    restartDevice();
  }
  else if(parameter[1].equals("action@reset"))
  {
    sendReply("Node: Reset RCVD");
    resetDevice();
  }
  else if(parameter[1].equals("action@getnodelist")){
    sendReply("IRNode: NodeList Request");
    sendNodeStat();
  }
  else if(parameter[1].equals("action@recordIR")){
    sendReply("IRNode: Record IR Request");
    recordIR();
  }else if(parameter[1].equals("action@saveIR")){
    sendReply("IRNode: IR Save Request");
    action = parameter[6];
    bool success = saveIR(action);
    sendNodeStat();
  }else if(parameter[1].equals("action@remove")){
    sendReply("IRNode: File Remove Request");
    action = parameter[6];
    fileName.erase(action);
    action = "/" + action;
    SPIFFS.remove(action);
    Serial.print("File Removed: ");
    Serial.println(action);
    listFileNames();
    sendNodeStat();
  }
  printDetails();
}

void configure()
{
  message = "client@node$action@config$3$0$";
  message.concat(name);
  bool hasIrNames = false;
  Serial.println(fileName.size());
  for(setIterator = fileName.begin(); setIterator!=fileName.end(); setIterator++){
    message.concat("_");
    message.concat(*setIterator);
    hasIrNames = true;
  }
  if(hasIrNames)
    message.concat("_");
  message.concat("$0$0$");
  sendPacket(masterIP, port, message);
}

void handleRoot(){
  Serial.println("Root page accessed by a client!");
  server.send ( 200, "text/plain", "Node: Hello, you are at root!");
}


void handleNotFound(){
  server.send ( 404, "text/plain", "Node: 404, No resource found");
}

void handleMessage(){
  if(server.hasArg("data")){
    message = server.arg("data");
    separateParameters(message);
    parameterDecode();
  }else{
    server.send(200, "text/plain", "Node: Message Without Body");
  }
}

void startAPMode(){
  digitalWrite(led, HIGH);
  Serial.println("AP mode");
  mode = 1;
  conStat = 0;
  WiFi.mode(WIFI_AP);
  String apSsid = "Node_";
  apSsid.concat(name);
  WiFi.softAP(apSsid, "", 1, 0, 1);
  delay(100);
  IPAddress myIP(192, 168, 1, 1);
  IPAddress mask(255, 255, 255, 0);
  WiFi.softAPConfig(myIP, myIP, mask);

  Serial.print("Node AP IP: ");
  Serial.println(WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/message", handleMessage);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("Server Started: %d\n\n", port);
}

void setup() {
  pinMode(powerBtn, INPUT_PULLUP);
  pinMode(led, OUTPUT);

  Serial.begin(115200);

  EEPROM.begin(EEPROM_SIZE);
  delay(200);
  SPIFFS.begin();
  delay(200);
  getMetaData();
  getName();
  Serial.println("here");
  getFileNames();
  printDetails();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,password);
  gotIpEventHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& event)
  {
    Serial.print("Connected to AP, IP: ");
    Serial.println(WiFi.localIP());
    ipAssigned = 1;
  });

  stationModeDisconnectedHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event)
  {
    ipAssigned = 0;
  });
  mode = 0;
}

void loop() {
  server.handleClient();
  if(WiFi.status() != WL_CONNECTED && mode == 0){
    unsigned long c;
    Serial.print("Connecting in STA Mode..");
    while (digitalRead(powerBtn) == HIGH && WiFi.status() != WL_CONNECTED && mode==0) {
      yield();
      digitalWrite(led, HIGH);
      Serial.print(".");
      c = millis();
      while(digitalRead(powerBtn) == HIGH && millis()-c<200);
      digitalWrite(led, LOW);
      if(digitalRead(powerBtn) == HIGH)
        delay(150);
    }
    if(WiFi.status() == WL_CONNECTED){
      unsigned long i = millis();
      while(!ipAssigned){
        if(millis() - i < 5000)
          yield();
        delay(200);
      }
      configure();
      server.on("/", handleRoot);
      server.on("/message", handleMessage);
      server.onNotFound(handleNotFound);
      server.begin();
      Serial.printf("\nServer ON: %d\n", port);
    }
  }

  if(digitalRead(powerBtn) == LOW){
    cur = millis();
    while(digitalRead(powerBtn) == LOW && (millis() - cur) < hold);

    if(millis() - cur < clickGap){
      delay(minDown);
      cur = millis();
      boolean doubleClick = true;
      while(digitalRead(powerBtn) == HIGH){
        if(millis() - cur > clickGap){
          doubleClick = false;
          break;
        }
      }
      if(doubleClick){
        Serial.println("Double Tap");
        if(mode == 0)
          startAPMode();
        else
          restartDevice();
      }else{
        Serial.println("Single Tap");
      }
    }else{
      Serial.println("Press and Hold");
      resetDevice();
    }
  }
}