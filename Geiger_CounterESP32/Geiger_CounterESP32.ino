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
#include <HTTPClient.h>
#include <BluetoothSerial.h>
#include <EEPROM.h>
#include <credentials.h>                       // or define mySSID and myPASSWORD and THINGSPEAK_API_KEY

#define LOG_PERIOD 60000                       // Logging period in milliseconds
#define BT_LOG_PERIOD 1000                     // Bluetooth logging period in milliseconds
#define MINUTE_PERIOD 60000                    // minute period
#define TUBE_FACTOR_SIEVERT 0.00812037037037   // the factor for the J305ß tube
#define LAST_VALUES_SIZE 60                    // size of the history array
#define EEPROM_SIZE 1                          // size of the needed EEPROM
#define STRING_BUFFER_SIZE 50                  // size of the handy stringbuffer

#ifndef CREDENTIALS

// WLAN
#define mySSID "xxx"
#define myPASSWORD "xxx"

//Thinspeak credentials
#define THINKSPEAK_CHANNEL 123456
#define WRITE_API_KEY  "xxx"

// IFTTT credentials
#define IFTTT_KEY "xxx"

#endif

// IFTTT settings
#define EVENT_NAME "Radioactivity"             // event name for IFTTT
#define CPM_THRESHOLD 100                      // Threshold for IFTTT warning
// fingerprint: openssl s_client -connect maker.ifttt.com:443  < /dev/null 2>/dev/null | openssl x509 -fingerprint -noout | cut -d'=' -f2
#define DEFAULT_IFTTT_FINGERPRINT "AA:75:CB:41:2E:D5:F9:97:FF:5D:A0:8B:7D:AC:12:21:08:4B:00:8C"

// ThingSpeak settings
const int channelID = THINKSPEAK_CHANNEL;
const char* server = "api.thingspeak.com";

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

volatile unsigned long counts = 0;                       // Tube events
volatile unsigned long isrMillis;                        // Time measurement for last ISR
volatile bool isrFired = false;                          // flag for ISR

const int LED_BUILTIN = 2;                               // status LED
const int input_pin_geiger = 18;                         // input pin for geiger board

unsigned long previousMillis;                            // Time measurement
unsigned long previousLogMillis;                         // Time measurement for serial logging
unsigned long lastCPMValues[LAST_VALUES_SIZE];           // last cpm values
bool debug = false;                                      // flag send debug info via Bluetooth

void IRAM_ATTR isr_impulse() { // Captures count of events from Geiger counter board
  detachInterrupt(digitalPinToInterrupt(input_pin_geiger));
  isrMillis = millis();
  isrFired = true;
  counts++;
}

void setup() {
  Serial.begin(115200);
  SerialBT.begin("GeigerCounterBT");
  EEPROM.begin(EEPROM_SIZE);

  pinMode(input_pin_geiger, INPUT);                                                // Set pin for capturing Tube events
  pinMode(LED_BUILTIN, OUTPUT);                                                    // status LED init
  attachInterrupt(digitalPinToInterrupt(input_pin_geiger), isr_impulse, FALLING);  // Define interrupt on falling edge

  // Set WiFi to station mode and disconnect from an AP if it was Previously
  // connected
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  while (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  debug = EEPROM.read(0);
}

void loop() {
  unsigned long currentMillis = millis();

  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  handleCommands();

  if (debug && currentMillis - previousLogMillis > BT_LOG_PERIOD) {
    previousLogMillis = currentMillis;
    unsigned long cpm = counts * MINUTE_PERIOD / BT_LOG_PERIOD;
    float mSvh = cpm * TUBE_FACTOR_SIEVERT;
    char buf[100];
    snprintf(buf, sizeof buf, "Actual CPM: %lu, CPM: %lu, mSv/h: %f", counts, cpm, mSvh);
    Serial.println(buf);
    SerialBT.println(buf);

  }

  if (isrFired && ( currentMillis - isrMillis) >= 100) {
    isrFired = false;
    attachInterrupt(digitalPinToInterrupt(input_pin_geiger), isr_impulse, FALLING);
  }

  if (currentMillis - previousMillis > LOG_PERIOD) {
    digitalWrite(LED_BUILTIN, HIGH);
    unsigned long cpm;
    float mSvh;
    previousMillis = currentMillis;
    cpm = counts * MINUTE_PERIOD / LOG_PERIOD;
    mSvh = cpm * TUBE_FACTOR_SIEVERT;
    counts = 0;
    postThinspeak(cpm, mSvh);
    pushCPMValueToArray(cpm);
    printAverage();
    printTemperature();
    printHallValue();
    if (cpm > CPM_THRESHOLD ) IFTTT(cpm, mSvh);
    checkInterrupt(cpm);
    digitalWrite(LED_BUILTIN, LOW);
  }
}

void connectWifi() {
  digitalWrite(LED_BUILTIN, HIGH);

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

  SerialBT.println("Wi-Fi Connected");
  SerialBT.println("IP address: ");
  SerialBT.println(ip);
  digitalWrite(LED_BUILTIN, LOW);
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
  if (strcmp(command, "debugon") == 0) {
    debug = true;
    commitEEPROM = true;
    msg = "Turn on debug ...";
    Serial.println(msg);
    SerialBT.println(msg);
  }
  else if (strcmp(command, "debugoff") == 0) {
    debug = false;
    commitEEPROM = true;
    msg = "Turn off debug ...";
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
    EEPROM.commit();
  }
}

void postThinspeak(unsigned long postValue, float postValue2) {
  if (client.connect(server, 80)) {

    // Construct API request body
    String body = "field1=";
    body += String(postValue);
    body += "&field2=";
    body += String(postValue2);

    Serial.print("Radioactivity (CPM): ");
    Serial.println(postValue);

    Serial.print("Radioactivity (mSvh): ");
    Serial.println(postValue2);

    SerialBT.print("Radioactivity (CPM): ");
    SerialBT.println(postValue);

    SerialBT.print("Radioactivity (mSvh): ");
    SerialBT.println(postValue2);

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

// sometimes the interrupt get lost
void checkInterrupt(unsigned long cpm) {
  if ((cpm + lastCPMValues[0]) == 0) {
    String msg = "Possible interrupt mess up! Reattaching interrupt";
    Serial.println(msg);
    SerialBT.println(msg);
    detachInterrupt(digitalPinToInterrupt(input_pin_geiger));
    attachInterrupt(digitalPinToInterrupt(input_pin_geiger), isr_impulse, FALLING);
  }
}

void printAverage() {
  unsigned long average = calcAverage();
  Serial.print("Radioactivity (CPM Average): ");
  Serial.println(average);
  SerialBT.print("Radioactivity (CPM Average): ");
  SerialBT.println(average);
}

unsigned long calcAverage() {
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

void IFTTT(int postValue, float postValue2) {
  char postValueChar[8];
  itoa(postValue, postValueChar, 10);
  char postValue2Char[8];
  dtostrf(postValue2, 6, 2, postValue2Char);
  String msg;
  if (trigger(IFTTT_KEY, DEFAULT_IFTTT_FINGERPRINT, EVENT_NAME, postValueChar, postValue2Char, NULL)) {
    msg = "IFTTT failed!";
  } else {
    msg = "Successfully sent to IFTTT";
  }
  Serial.println(msg);
  SerialBT.println(msg);
}


int trigger(const char* api_key, const char* ifttt_fingerprint, const char* event_name, const char* value1, const char* value2, const char* value3) {
  HTTPClient http;
  const char* ifttt_base = "https://maker.ifttt.com/trigger";

  // Compute URL length
  int url_length = 1 + strlen(ifttt_base) + strlen("/") + strlen(event_name) + strlen("/with/key/") + strlen(api_key);
  char ifttt_url[url_length];

  // Compute Payload length
  int payload_length = 37 + (value1 ? strlen(value1) : 0) + (value2 ? strlen(value2) : 0) + (value3 ? strlen(value3) : 0);
  char ifttt_payload[payload_length];

  // Compute URL
  snprintf(ifttt_url, url_length, "%s/%s/with/key/%s", ifttt_base, event_name, api_key);

  // Compute Payload (JSON), e.g. {value1:"A",value2:"B",value3:"C"}
  snprintf(ifttt_payload, payload_length, "{");

  if (value1) {
    strcat(ifttt_payload, "\"value1\":\"");
    strcat(ifttt_payload, value1);
    strcat(ifttt_payload, "\"");
    if (value2 || value3) {
      strcat(ifttt_payload, ",");
    }
  }
  if (value2) {
    strcat(ifttt_payload, "\"value2\":\"");
    strcat(ifttt_payload, value2);
    strcat(ifttt_payload, "\"");
    if (value3) {
      strcat(ifttt_payload, ",");
    }
  }
  if (value3) {
    strcat(ifttt_payload, "\"value3\":\"");
    strcat(ifttt_payload, value3);
    strcat(ifttt_payload, "\"");
  }
  strcat(ifttt_payload, "}");

  http.begin(ifttt_url, ifttt_fingerprint);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(ifttt_payload);
  http.end();

  return httpCode != HTTP_CODE_OK;
}
