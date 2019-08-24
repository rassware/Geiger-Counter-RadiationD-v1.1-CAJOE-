/*
   Geiger_Counter.ino

   This code interacts with the Alibaba RadiationD-v1.1 (CAJOE) Geiger counter board
   and reports readings in CPM (Counts Per Minute).

   Author: Christian Raßmann

   Based on initial work of Andreas Spiess

   License: MIT License

   Please use freely with attribution. Thank you!
*/

#include <ESP8266WiFi.h>
#include <IFTTTWebhook.h>
#include <ESP8266HTTPClient.h>

#define LOG_PERIOD 20000 //Logging period in milliseconds
#define MINUTE_PERIOD 60000
#define TUBE_FACTOR_SIEVERT 0.00812037037037

// WLAN
#define mySSID "xxx"
#define myPASSWORD "xxx"

//Thinspeak
#define THINKSPEAK_CHANNEL 123456
#define WRITE_API_KEY  "xxx"

// IFTTT
#define IFTTT_KEY "xxx"
#define EVENT_NAME "Radioactivity"
// fingerprint: openssl s_client -connect maker.ifttt.com:443  < /dev/null 2>/dev/null | openssl x509 -fingerprint -noout | cut -d'=' -f2
#define DEFAULT_IFTTT_FINGERPRINT "AA:75:CB:41:2E:D5:F9:97:FF:5D:A0:8B:7D:AC:12:21:08:4B:00:8C"
//#define IFTTT_WEBHOOK_DEBUG

// ThingSpeak Settings
const int channelID = THINKSPEAK_CHANNEL;
const char* server = "api.thingspeak.com";

WiFiClient client;

volatile unsigned long counts = 0;                       // Tube events
unsigned long cpm = 0;                                   // CPM
float mSvh = 0.0f;                                       // Micro Sievert
unsigned long previousMillis;                            // Time measurement

ICACHE_RAM_ATTR void isr_impulse() { // Captures count of events from Geiger counter board
  counts++;
}

void setup() {
  Serial.begin(115200);

  // Set WiFi to station mode and disconnect from an AP if it was Previously
  // connected
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  Serial.print("Connecting Wifi: ");
  Serial.println(mySSID);

  WiFi.begin(mySSID, myPASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  Serial.println("Wi-Fi Connected");
  Serial.println("IP address: ");
  IPAddress ip = WiFi.localIP();
  Serial.println(ip);

  pinMode(D7, INPUT);                                                // Set pin for capturing Tube events
  attachInterrupt(digitalPinToInterrupt(D7), isr_impulse, FALLING);  // Define interrupt on falling edge
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis > LOG_PERIOD) {
    previousMillis = currentMillis;
    cpm = counts * MINUTE_PERIOD / LOG_PERIOD;
    mSvh = cpm * TUBE_FACTOR_SIEVERT;
    counts = 0;
    postThinspeak(cpm, mSvh);
    if (cpm > 200 ) IFTTT(cpm, mSvh);
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

  }
  client.stop();
}

void IFTTT(int postValue, float postValue2) {
  char postValueChar[8];
  itoa(postValue, postValueChar, 10);
  char postValue2Char[8];
  dtostrf(postValue2, 6, 2, postValue2Char);
  if (trigger(IFTTT_KEY, DEFAULT_IFTTT_FINGERPRINT, EVENT_NAME, postValueChar, postValue2Char, NULL)) {
    Serial.println("IFTTT failed!");
  } else {
    Serial.println("Successfully sent to IFTTT");
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

  if(value1) {
    strcat(ifttt_payload, "\"value1\":\"");
    strcat(ifttt_payload, value1);
    strcat(ifttt_payload, "\"");
    if(value2 || value3) {
      strcat(ifttt_payload, ",");
    }
  }

  if(value2) {
    strcat(ifttt_payload, "\"value2\":\"");
    strcat(ifttt_payload, value2);
    strcat(ifttt_payload, "\"");
    if(value3) {
      strcat(ifttt_payload, ",");
    }
  }

  if(value3) {
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
    if(httpCode == HTTP_CODE_OK) {
      Serial.println(http.getString());
    }
  } else {
      Serial.printf("[HTTP] POST... failed, error %s\n", http.errorToString(httpCode).c_str());
  }
#endif

  http.end();

  return httpCode != HTTP_CODE_OK;
}
