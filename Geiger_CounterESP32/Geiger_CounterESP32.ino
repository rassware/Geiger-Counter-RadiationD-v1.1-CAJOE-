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
#include <credentials.h> // or define mySSID and myPASSWORD and THINGSPEAK_API_KEY

#define LOG_PERIOD 60000 //Logging period in milliseconds
#define BT_LOG_PERIOD 1000 //Bluetooth logging period in milliseconds
#define MINUTE_PERIOD 60000
#define TUBE_FACTOR_SIEVERT 0.00812037037037

#ifndef CREDENTIALS

// WLAN
#define mySSID "xxx"
#define myPASSWORD "xxx"

//Thinspeak
#define THINKSPEAK_CHANNEL 123456
#define WRITE_API_KEY  "xxx"

// IFTTT
#define IFTTT_KEY "xxx"

#endif

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to and enable it
#endif

#define EVENT_NAME "Radioactivity"
// fingerprint: openssl s_client -connect maker.ifttt.com:443  < /dev/null 2>/dev/null | openssl x509 -fingerprint -noout | cut -d'=' -f2
#define DEFAULT_IFTTT_FINGERPRINT "AA:75:CB:41:2E:D5:F9:97:FF:5D:A0:8B:7D:AC:12:21:08:4B:00:8C"
//#define IFTTT_WEBHOOK_DEBUG

// ThingSpeak Settings
const int channelID = THINKSPEAK_CHANNEL;
const char* server = "api.thingspeak.com";

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
const int LED_BUILTIN = 2;                               // status LED
const int input_pin_geiger = 18;                         // input pin for geiger board
unsigned long cpm = 0;                                   // CPM
float mSvh = 0.0f;                                       // Micro Sievert
unsigned long previousMillis;                            // Time measurement
unsigned long previousBTMillis;                          // Time measurement for bluetooth
unsigned long isrMillis;                                 // Time measurement for last ISR
int wifi_counter = 0;                                    // WiFi connection attempts
uint8_t temprature_sens_read();                          // internal temperature sensor
int hall_value = 0;                                      // internal hall sensor
bool isrFired = false;                                   // flag for ISR
bool btDebug = false;                                    // flag send debug info via Bluetooth
String btMsg;                                            // Bluetooth message

void IRAM_ATTR isr_impulse() { // Captures count of events from Geiger counter board
  detachInterrupt(digitalPinToInterrupt(input_pin_geiger));
  isrMillis = millis();
  isrFired = true;
  counts++;
}

void setup() {
  Serial.begin(115200);
  SerialBT.begin("GeigerCounterBT");

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
}

void loop() {
  unsigned long currentMillis = millis();

  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  handleBTCommands();

  if (btDebug && currentMillis - previousBTMillis > BT_LOG_PERIOD) {
    previousBTMillis = currentMillis;
    Serial.print("Counts: ");
    Serial.println(counts);
    SerialBT.print("Counts: ");
    SerialBT.println(counts);
  }

  if (isrFired && ( millis() - isrMillis) >= 100  ) {
    attachInterrupt(digitalPinToInterrupt(input_pin_geiger), isr_impulse, FALLING);
    isrFired = false;
  }

  if (currentMillis - previousMillis > LOG_PERIOD) {
    digitalWrite(LED_BUILTIN, HIGH);
    previousMillis = currentMillis;
    cpm = counts * MINUTE_PERIOD / LOG_PERIOD;
    mSvh = cpm * TUBE_FACTOR_SIEVERT;
    counts = 0;
    postThinspeak(cpm, mSvh);
    getTemperature();
    getHallValue();
    if (cpm > 100 ) IFTTT(cpm, mSvh);
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
  wifi_counter = 0;
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

void handleBTCommands() {
  if (SerialBT.available()) {
    btMsg = SerialBT.readStringUntil('\r');
    if (btMsg == "debugon") {
      btDebug = true;
      SerialBT.println("Turn on debug ...");
    }
    else if (btMsg == "debugoff") {
      btDebug = false;
      SerialBT.println("Turn off debug ...");
    }
    else if (btMsg == "temp") {
      getTemperature();
    }
    else if (btMsg == "hall") {
      getHallValue();
    }
  }
}

void postThinspeak(int postValue, float postValue2) {
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

void getTemperature() {
  // Convert raw temperature in F to Celsius degrees
  uint8_t temp = (temprature_sens_read() - 32) / 1.8;
  Serial.print("Temp in Celsius: ");
  Serial.println(temp);
  SerialBT.print("Temp in Celsius: ");
  SerialBT.println(temp);
}

void getHallValue() {
  Serial.print("Hall: ");
  Serial.println(hallRead());
  SerialBT.print("Hall: ");
  SerialBT.println(hallRead());
}

void IFTTT(int postValue, float postValue2) {
  char postValueChar[8];
  itoa(postValue, postValueChar, 10);
  char postValue2Char[8];
  dtostrf(postValue2, 6, 2, postValue2Char);
  if (trigger(IFTTT_KEY, DEFAULT_IFTTT_FINGERPRINT, EVENT_NAME, postValueChar, postValue2Char, NULL)) {
    Serial.println("IFTTT failed!");
    SerialBT.println("IFTTT failed!");
  } else {
    Serial.println("Successfully sent to IFTTT");
    SerialBT.println("Successfully sent to IFTTT");
  }
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

#ifdef IFTTT_WEBHOOK_DEBUG
  Serial.print("URL length: ");
  Serial.println(url_length);
  Serial.print("Payload length: ");
  Serial.println(payload_length);
#endif

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

#ifdef IFTTT_WEBHOOK_DEBUG
  Serial.print("URL: ");
  Serial.println(ifttt_url);
  Serial.print("Payload: ");
  Serial.println(ifttt_payload);
#endif

  http.begin(ifttt_url, ifttt_fingerprint);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(ifttt_payload);

#ifdef IFTTT_WEBHOOK_DEBUG
  Serial.printf("[HTTP] POST... code: %d\n", httpCode);
  if (httpCode > 0) {
    if (httpCode == HTTP_CODE_OK) {
      Serial.println(http.getString());
    }
  } else {
    Serial.printf("[HTTP] POST... failed, error %s\n", http.errorToString(httpCode).c_str());
  }
#endif

  http.end();

  return httpCode != HTTP_CODE_OK;
}
