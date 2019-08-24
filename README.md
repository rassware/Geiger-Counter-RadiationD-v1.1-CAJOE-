# ArduinoGeigerCounter

## Description

Arduino code for use with RadiationD-v1.1 (CAJOE) Geiger counter board.

This repo is a fork from Andreas Spiess´ repo. I added some improvements to the code:

* rewrite the sketch for a ESP8266
* calculates now the micro sievert per hour
* don´t rely on 3rd party libs for the IFTTT communication

Source for the micro sievert factor:
[www.cooking-hacks.com](https://www.cooking-hacks.com/documentation/tutorials/geiger-counter-radiation-sensor-board-arduino-raspberry-pi-tutorial#tube)

You can debug the IFTTT connection by comment in following line:
```
#define IFTTT_WEBHOOK_DEBUG
```

From time to time the fingerprint for the IFTTT communication changes. To get the new fingerprint simply
execute in a bash:
```bash
openssl s_client -connect maker.ifttt.com:443  < /dev/null 2>/dev/null | openssl x509 -fingerprint -noout | cut -d'=' -f2
```

## Contact info

Christian Raßmann

rc@rassware.de

@rassware
