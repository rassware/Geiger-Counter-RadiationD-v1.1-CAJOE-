/*
   Geiger_Counter.ino

   This code interacts with the Alibaba RadiationD-v1.1 (CAJOE) Geiger counter board
   and reports readings in CPM (Counts Per Minute).

   Author: Christian Raßmann

   Based on initial work of Andreas Spiess

   License: MIT License

   Please use freely with attribution. Thank you!
*/

#include <WiFi.h>
#include <BluetoothSerial.h>
#include <EEPROM.h>
#include <WebServer.h>
#include <time.h>
#include <SPIFFS.h>
#include <esp32-hal-cpu.h>
#include <SSD1306.h>
#include <credentials.h>                       // or define mySSID and myPASSWORD and THINGSPEAK_API_KEY

#define LOG_PERIOD 60000                       // Logging period in milliseconds
#define BT_LOG_PERIOD 1000                     // Bluetooth logging period in milliseconds
#define APP_LOG_PERIOD 5000                    // App logging period in milliseconds
#define MINUTE_PERIOD 60000                    // minute period
#define TUBE_FACTOR_SIEVERT 0.00812037037037   // the factor for the J305ß tube
#define LAST_VALUES_SIZE 5                     // size of the history array
#define EEPROM_SIZE 5                          // size of the needed EEPROM
#define STRING_BUFFER_SIZE 50                  // size of the handy stringbuffer
#define FORMAT_SPIFFS_IF_FAILED false          // need to format only the first time
#define WEIGHT_AVERAGE 0.8                     // value for the weighted average filter

#ifndef CREDENTIALS

// WLAN
#define mySSID "xxx"
#define myPASSWORD "xxx"

//Thinspeak credentials
#define THINKSPEAK_CHANNEL 123456
#define WRITE_API_KEY  "xxx"
#define LATITUDE 0.0000000
#define LONGITUDE 0.0000000
#define ELEVATION 0

// IFTTT credentials
#define IFTTT_KEY "xxx"

#endif

// IFTTT settings
#define EVENT_NAME "Radioactivity"             // event name for IFTTT
#define CPM_THRESHOLD 50                       // Threshold for IFTTT warning

// ThingSpeak settings
const int channelID = THINKSPEAK_CHANNEL;
const char* thingspeakServer = "api.thingspeak.com";

// Check Bluetooth config
#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

// Init temperature sensor
#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif

WiFiClient client;
BluetoothSerial SerialBT;
WebServer server(80);
SSD1306  display(0x3c, 5, 4);

volatile unsigned long counts = 0;                       // Tube events

const int input_pin_geiger = 26;                         // input pin for geiger board

unsigned long previousMillis;                            // Time measurement
unsigned long previousLogMillis;                         // Time measurement for serial logging
unsigned long previousAppLogMillis;                      // Time measurement for app logging
unsigned long previousDisplayUpdateMillis;               // Time measurement for last display update
unsigned long lastCPMValues[LAST_VALUES_SIZE];           // last cpm values
unsigned long lastCPMForApp = 0;                         // last cpm for app
unsigned long displayUpdatePeriod = 1000;                // diplay update period
bool debug = false;                                      // flag send debug info via Bluetooth
bool monitoring = true;                                  // flag for connecting to WiFi
bool writeToFile = false;                                // flag for writing to local file
bool displayFlag = true;                                 // flag for switching display on or off
const char* fileName = "/cpms.txt";                      // file name to write
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;         // sync variable

void IRAM_ATTR isr_impulse() { // Captures count of events from Geiger counter board
  portENTER_CRITICAL_ISR(&mux);
  if (digitalRead(input_pin_geiger) == HIGH) counts++;
  SerialBT.println("t;");
  while (digitalRead(input_pin_geiger) == HIGH) {}
  portEXIT_CRITICAL_ISR(&mux);
}

void setup() {
  Serial.begin(115200);
  displayInit();
  displayString("rassware.de", 64, 15, TEXT_ALIGN_CENTER);
  SerialBT.begin("GeigerCounterBT");
  EEPROM.begin(EEPROM_SIZE);

  pinMode(input_pin_geiger, INPUT);                                                // Set pin for capturing Tube events
  attachInterrupt(digitalPinToInterrupt(input_pin_geiger), isr_impulse, RISING);   // Define interrupt on rising edge

  debug = EEPROM.read(0);
  monitoring = EEPROM.read(1);
  writeToFile = EEPROM.read(2);
  displayFlag = EEPROM.read(3);

  if (monitoring) {
    // Set WiFi to station mode and disconnect from an AP if it was Previously
    // connected
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    while (WiFi.status() != WL_CONNECTED) {
      connectWifi();
    }
    configTime(0, 0, "192.168.78.1", "pool.ntp.org");                             // Time server in Fritzbox
    setenv("TZ", "CET-1CEST,M3.5.0/02:00:00,M10.5.0/03:00:00", 1);                // timezone MEZ
    while (!time(nullptr)) {                                                      // wait for time
      Serial.print("Wait for time: ");
      Serial.print(".");
      delay(500);
    }

    server.on("/", HTTP_GET, handleIndex);
    server.on("/files", HTTP_GET, handleFileList);
    server.on("/delete", HTTP_GET, handleFileDelete);
    server.onNotFound([]() {
      if (!handleFileRead(server.uri())) {
        server.send(404, "text/plain", "FileNotFound");
      }
    });
    server.begin();
    displayUpdatePeriod = 5000;
  }

  if (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  setCpuFrequencyMhz(80);
}

void loop() {
  unsigned long currentMillis = millis();

  if (monitoring) {
    server.handleClient();
  }

  if (monitoring && WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  handleCommands();

  if (debug && currentMillis - previousLogMillis > BT_LOG_PERIOD) {
    previousLogMillis = currentMillis;
    int elapsedSeconds = (currentMillis - previousMillis) / 1000;
    if (elapsedSeconds == 0) elapsedSeconds = 1;
    float clicksPerSecound = (float)counts / elapsedSeconds;
    unsigned long cpm = clicksPerSecound * MINUTE_PERIOD / BT_LOG_PERIOD;
    float mSvh = cpm * TUBE_FACTOR_SIEVERT;
    char buf[100];
    snprintf(buf, sizeof buf, "counts since %d seconds: %lu, CPM: %lu, mSv/h: %f", elapsedSeconds, counts, cpm, mSvh);
    Serial.println(buf);
    SerialBT.println(buf);
  }

  // data for app
  if (currentMillis - previousAppLogMillis > APP_LOG_PERIOD) {
    previousAppLogMillis = currentMillis;
    unsigned long cpm = ((float) counts / ((currentMillis - previousMillis) / 1000)) * 60;
    unsigned long average = calcWeightedAverageFilter(cpm, WEIGHT_AVERAGE, lastCPMForApp);
    lastCPMForApp = cpm;
    float mSvh = average * TUBE_FACTOR_SIEVERT;
    SerialBT.print("cpm=");
    SerialBT.print(average, DEC);
    SerialBT.print(";");
    SerialBT.print("uSv/h=");
    SerialBT.println(mSvh, 4);
  }

  if (monitoring && currentMillis - previousMillis > LOG_PERIOD) {
    if (displayFlag) displayString("@", 110, 40, TEXT_ALIGN_LEFT);
    unsigned long cpm;
    float mSvh;
    previousMillis = currentMillis;
    cpm = counts * MINUTE_PERIOD / LOG_PERIOD;
    mSvh = cpm * TUBE_FACTOR_SIEVERT;
    portENTER_CRITICAL(&mux);
    counts = 0;
    portEXIT_CRITICAL(&mux);
    pushCPMValueToArray(cpm);
    printCPM(cpm, mSvh);
    printAverage();
    printTemperature();
    printHallValue();
    postThingspeak(cpm, mSvh);
    if (cpm > CPM_THRESHOLD ) postIFTTT(cpm, mSvh);
    if (writeToFile) appendToFile(cpm, mSvh);
  }

  if (displayFlag && currentMillis - previousDisplayUpdateMillis > displayUpdatePeriod) {
    previousDisplayUpdateMillis = millis();
    if (monitoring) {
      unsigned long cpm = 0;
      float mSvh = 0;
      unsigned long average = 0;
      average = calcWeightedAverageFilter(lastCPMValues[0], WEIGHT_AVERAGE, lastCPMValues[1]);
      mSvh = average * TUBE_FACTOR_SIEVERT;
      String actualString = String("actual = ") + counts;
      String cpmString = String("Ø CPM = ") + average;
      String mSvhString = String("µSv/h = ") + mSvh;
      clearDisplayGently(60, 0);
      displayString(actualString, 0, 0, TEXT_ALIGN_LEFT);
      clearDisplayGently(64, 20);
      displayString(cpmString, 0, 20, TEXT_ALIGN_LEFT);
      clearDisplayGently(56, 40);
      displayString(mSvhString, 0, 40, TEXT_ALIGN_LEFT);
      shouldTurnOffDisplay();
    } else {
      int elapsedSeconds = millis() / 1000;
      String actualString = String("actual = ") + counts;
      String elapsed = String("time = ") + elapsedSeconds + String(" sec");
      clearDisplayGently(60, 0);
      displayString(actualString, 0, 0, TEXT_ALIGN_LEFT);
      clearDisplayGently(40, 20);
      displayString(elapsed, 0, 20, TEXT_ALIGN_LEFT);
      if (elapsedSeconds > 0) {
        float clicksPerSecound = (float)counts / elapsedSeconds;
        unsigned long cpm = clicksPerSecound * MINUTE_PERIOD / 1000;
        String cpmString = String("cpm = ") + String(cpm);
        clearDisplayGently(40, 40);
        displayString(cpmString, 0, 40, TEXT_ALIGN_LEFT);
      }
    }
  }
}

void connectWifi() {
  displayString("Connect WiFi ...", 64, 35, TEXT_ALIGN_CENTER);

  Serial.print("Connecting Wifi: ");
  Serial.println(mySSID);

  SerialBT.print("Connecting Wifi: ");
  SerialBT.println(mySSID);

  WiFi.begin(mySSID, myPASSWORD);
  int wifi_counter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    wifi_counter++;
    if (wifi_counter >= 60) { //30 seconds timeout - reset board
      ESP.restart();
    }
  }

  Serial.println("Wi-Fi Connected");
  Serial.println("IP address: ");
  IPAddress ip = WiFi.localIP();
  Serial.println(ip);

  SerialBT.println("IP address: ");
  SerialBT.println(ip);

  display.clear();
  displayString("Connected!", 64, 15, TEXT_ALIGN_CENTER);
  delay(3000);
  display.clear();
}

void handleCommands() {
  char stringBuffer[STRING_BUFFER_SIZE];
  if (SerialBT.available() && readLine(stringBuffer, SerialBT)) {
    commandHandler(stringBuffer);
  } else if (Serial.available() && readLine(stringBuffer, Serial)) {
    commandHandler(stringBuffer);
  }
}

bool readLine(char* stringBuffer, Stream& stream) {
  int index = 0;
  while (stream.available()) {
    char c = stream.read();
    if (c >= 32 && index < STRING_BUFFER_SIZE - 1) {
      stringBuffer[index++] = c;
    }
    else if (c == '\n' && index > 0) {
      stringBuffer[index] = '\0';
      return true;
    }
  }
  return false;
}

void commandHandler(const char* command) {
  bool commitEEPROM = false;
  String msg;
  if (strcmp(command, "debug") == 0) {
    debug = !debug;
    commitEEPROM = true;
    if (debug) msg = "Turn on debug ...";
    else msg = "Turn off debug ...";
    Serial.println(msg);
    SerialBT.println(msg);
  }
  else if (strcmp(command, "monitoring") == 0) {
    monitoring = !monitoring;
    commitEEPROM = true;
    if (monitoring) msg = "Monitoring Mode";
    else msg = "Standalone Mode";
    Serial.println(msg);
    SerialBT.println(msg);
  }
  else if (strcmp(command, "file") == 0) {
    writeToFile = !writeToFile;
    commitEEPROM = true;
    if (writeToFile) msg = "Write to file on ...";
    else msg = "Write to file off ...";
    Serial.println(msg);
    SerialBT.println(msg);
  }
  else if (strcmp(command, "display") == 0) {
    displayFlag = !displayFlag;
    commitEEPROM = true;
    if (displayFlag) {
      msg = "Turn on display ...";
      display.displayOn();
    }
    else {
      msg = "Turn off display ...";
      display.displayOff();
    }
    Serial.println(msg);
    SerialBT.println(msg);
  }
  else if (strcmp(command, "temp") == 0) {
    printTemperature();
  }
  else if (strcmp(command, "hall") == 0) {
    printHallValue();
  }
  else if (strcmp(command, "last") == 0) {
    printLastCPMValues();
  }
  else if (strcmp(command, "restart") == 0) {
    ESP.restart();
  }
  else {
    msg = "unknown command: ";
    Serial.println(msg);
    Serial.println(command);
    SerialBT.println(msg);
    SerialBT.println(command);
  }
  if (commitEEPROM) {
    EEPROM.write(0, debug);
    EEPROM.write(1, monitoring);
    EEPROM.write(2, writeToFile);
    EEPROM.write(3, displayFlag);
    EEPROM.commit();
  }
}

void postThingspeak(unsigned long postValue, float postValue2) {
  if (client.connect(thingspeakServer, 80)) {

    // Construct API request body
    String body = "field1=";
    body += String(postValue);
    body += "&field2=";
    body += String(postValue2);
    body += "&latitude=";
    body += String(LATITUDE);
    body += "&longitude=";
    body += String(LONGITUDE);
    body += "&elevation=";
    body += String(ELEVATION);
    body += "&status=";
    body += String("OK");

    client.print("POST /update HTTP/1.1\n");
    client.print("Host: api.thingspeak.com\n");
    client.print("Connection: close\n");
    client.print("X-THINGSPEAKAPIKEY: " + String(WRITE_API_KEY) + "\n");
    client.print("Content-Type: application/x-www-form-urlencoded\n");
    client.print("Content-Length: ");
    client.print(body.length());
    client.print("\n\n");
    client.print(body);
    client.print("\n\n");
    String line = client.readStringUntil('\r');
    Serial.println(line);
    SerialBT.println(line);
  }
  client.stop();
}

void printTemperature() {
  // Convert raw temperature in F to Celsius degrees
  uint8_t temp = (temprature_sens_read() - 32) / 1.8;
  Serial.print("Temp in Celsius: ");
  Serial.println(temp);
  SerialBT.print("Temp in Celsius: ");
  SerialBT.println(temp);
}

void printHallValue() {
  Serial.print("Hall: ");
  Serial.println(hallRead());
  SerialBT.print("Hall: ");
  SerialBT.println(hallRead());
}

void pushCPMValueToArray(unsigned long cpm) {
  for (int i = LAST_VALUES_SIZE - 1; i > 0; i--) {
    lastCPMValues[i] = lastCPMValues[i - 1];
  }
  lastCPMValues[0] = cpm;
}

void printLastCPMValues() {
  char buf[100];
  snprintf(buf, sizeof buf, "Radioactivity (CPM) last values: %lu,%lu,%lu,%lu,%lu", lastCPMValues[0], lastCPMValues[1], lastCPMValues[2], lastCPMValues[3], lastCPMValues[4]);
  Serial.println(buf);
  SerialBT.println(buf);
}

void printCPM(unsigned long cpm, float mSvh) {
  Serial.print("Radioactivity (CPM): ");
  Serial.println(cpm);

  Serial.print("Radioactivity (mSv/h): ");
  Serial.println(mSvh);

  SerialBT.print("Radioactivity (CPM): ");
  SerialBT.println(cpm);

  SerialBT.print("Radioactivity (mSv/h): ");
  SerialBT.println(mSvh);
}

void printAverage() {
  unsigned long average = calcWeightedAverageFilter(lastCPMValues[0], WEIGHT_AVERAGE, lastCPMValues[1]);
  Serial.print("Radioactivity (CPM Average): ");
  Serial.println(average);
  Serial.print("Radioactivity (mSv/h Average): ");
  Serial.println(average * TUBE_FACTOR_SIEVERT);
  SerialBT.print("Radioactivity (CPM Average): ");
  SerialBT.println(average);
  SerialBT.print("Radioactivity (mSv/h Average): ");
  SerialBT.println(average * TUBE_FACTOR_SIEVERT);
}

unsigned long calcRunningAverage() {
  int countValues = 0;
  unsigned long sumValues = 0;
  for (int i = 0; i < LAST_VALUES_SIZE; i++) {
    sumValues += lastCPMValues[i];
    if (lastCPMValues[i] > 0) countValues++;
  }
  unsigned long average;
  if (countValues == 0) {
    return average = 0;
  } else {
    return average = sumValues / countValues;
  }
}

// filter the current result using a weighted average filter:
// weight must be between 0 and 1
unsigned long calcWeightedAverageFilter(unsigned long rawValue, float weight, unsigned long lastValue) {
  float result = weight * rawValue + (1.0 - weight) * lastValue;
  return result;
}

void postIFTTT(unsigned long postValue, float postValue2) {
  String msg;

  // Construct API request body
  String body = "value1=";
  body += String(postValue);
  body += "&value2=";
  body += String(postValue2);

  if (client.connect("maker.ifttt.com", 80)) {

    client.print("POST /trigger/");
    client.print(EVENT_NAME);
    client.print("/with/key/");
    client.print(IFTTT_KEY);
    client.println(" HTTP/1.1");
    client.println("Host: maker.ifttt.com");
    client.println("Connection: close");

    client.println("Content-Type: application/x-www-form-urlencoded");
    client.print("Content-Length: ");
    client.println(body.length());

    client.println();
    client.println(body);
    client.println();

    String line = client.readStringUntil('\r');
    Serial.println(line);
    SerialBT.println(line);

    msg = "Successfully sent to IFTTT";
  } else {
    msg = "IFTTT failed!";
  }
  client.stop();

  Serial.println(msg);
  SerialBT.println(msg);
}

void handleIndex() {
  unsigned long average = calcWeightedAverageFilter(lastCPMValues[0], WEIGHT_AVERAGE, lastCPMValues[1]);
  unsigned long averageRunning = calcRunningAverage();
  int elapsedSeconds = (millis() - previousMillis) / 1000;
  time_t now = time(nullptr);
  struct tm * timeinfo;
  timeinfo = localtime(&now);
  char time[100];
  char usvh[10];
  dtostrf(average * TUBE_FACTOR_SIEVERT,7,4,usvh);
  sprintf(time, "%02d:%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  String output = "<html><head><title>GeigerCounter</title><meta http-equiv='refresh' content='10'></head><body style='text-align: center;font-family: verdana;'><h1>&#9762; Geiger Counter &#9762;</h1>";
  output += "<h3>Radioactivity</h3>";
  output += "<p>Counts since " + String(elapsedSeconds) + " seconds: " + String(counts) + "</p>";
  output += "<p>CPM average: " + String(average) + "</p>";
  output += "<p>CPM average (running): " + String(averageRunning) + "</p>";
  output += "<p>&micro;Sv/h average: " + String(usvh) + "</p>";
  output += "<p>Last CPMs: " + String(lastCPMValues[0]);
  for (int i = 1; i < LAST_VALUES_SIZE; i++) {
    output += ", " + String(lastCPMValues[i]);
  }
  output += "</p>";
  output += "<h3>Mode</h3>";
  if (monitoring) {
    output += "<p>Monitoring Mode</p>";
  } else {
    output += "<p>Standalone Mode</p>";
  }
  output += "<h3>Time</h3>";
  output += "<p>" + String(time) + "</p>";
  output += "<h3>CPU</h3>";
  output += "<p>Frequency: " + String(getCpuFrequencyMhz()) + " MHz</p>";
  output += "<p>Temperature: " + String((temprature_sens_read() - 32) / 1.8) + " &deg;C</p>";
  output += "<h3>Sensors</h3>";
  output += "<p>Hall sensor: " + String(hallRead()) + "</p>";
  output += "<h3>Flags</h3>";
  output += "<p>Debug flag: " + String(debug) + "</p>";
  output += "<p>Write to file: " + String(writeToFile) + "</p>";
  output += "<p>Display active: " + String(displayFlag) + "</p>";
  output += "<h3>SPIFFS files</h3>";
  output += "<p><a href='/files'>Files</a></p>";
  output += "</body></html>";
  server.send(200, "text/html", output);
}

void handleFileList() {
  String output = "<html><head><title>GeigerCounter Files</title></head><body style='text-align: center;font-family: verdana;'><h1>Geiger Counter Files</h1>";
  output += "<table border='1'><thead><tr><td>type</td><td>name</td><td>size</td><td>actions</td></thead><tbody>";
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    output += "<tr><td>";
    output += (file.isDirectory()) ? "dir" : "file";
    output += "</td><td>";
    output += String(file.name()).substring(1);
    output += "</td><td>";
    output += String(file.size()) + " Bytes";
    output += "</td><td><a href='" + String(file.name()) + "'>show</a>&nbsp;<a href='/delete?a=" + String(file.name()) + "'>delete</a></td></tr>";
    file.close();
    file = root.openNextFile();
  }
  output += "</tbody></table></body></html>";
  server.send(200, "text/html", output);
}

void handleFileDelete() {
  if (server.args() == 0) {
    return server.send(500, "text/plain", "BAD ARGS");
  }
  String path = server.arg(0);
  if (path == "/") {
    return server.send(500, "text/plain", "BAD PATH");
  }
  if (!SPIFFS.exists(path)) {
    return server.send(404, "text/plain", "FileNotFound");
  }
  SPIFFS.remove(path);
  server.send(200, "text/plain", "DELETED");
  path = String();
}

bool handleFileRead(String path) {
  if (SPIFFS.exists(path)) {
    File file = SPIFFS.open(path, "r");
    server.streamFile(file, "text/plain");
    file.close();
    return true;
  }
  return false;
}

void appendToFile(unsigned long cpm, float mSvh) {
  File file = SPIFFS.open(fileName, FILE_APPEND);
  if (!file) {
    file = SPIFFS.open(fileName, FILE_WRITE);
    Serial.println("file created");
    SerialBT.println("file created");
  }
  char buf[100];
  snprintf(buf, sizeof buf, "%lu,%lu,%f", millis(), cpm, mSvh);
  if (file.println(buf)) {
    Serial.println("File updated");
    SerialBT.println("File updated");
  } else {
    Serial.println("File updated");
    SerialBT.println("File updated");
  }
  file.close();
}

void displayInit() {
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_16);
}

void displayString(String dispString, int x, int y, OLEDDISPLAY_TEXT_ALIGNMENT align) {
  display.setColor(WHITE);
  display.setTextAlignment(align);
  display.drawString(x, y, dispString);
  display.setFont(ArialMT_Plain_16);
  display.display();
}

void clearDisplayGently(int x, int y) {
  display.setColor(BLACK);
  display.fillRect(x, y, display.getWidth() - x, 20);
  display.display();
}

void shouldTurnOffDisplay() {
  time_t now = time(nullptr);
  struct tm * timeinfo;
  timeinfo = localtime(&now);
  int hours = timeinfo->tm_hour;
  if (hours >= 0 && hours <= 5) {
    display.displayOff();
    return;
  }
  display.displayOn();
}
