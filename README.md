# The ESP8266 WiFi driver for mbed-os
The mbed OS driver for the ESP8266 WiFi module

## Firmware version
ESP8266 modules come in different shapes and forms, but most important difference is which firmware version it is programmed with. To make sure that your module has mbed-os compatible firmware follow update guide: https://developer.mbed.org/teams/ESP8266/wiki/Firmware-Update

## Restrictions
- The ESP8266 WiFi module does not allow TCP client to bind on a specific port
- Setting up an UDP server is not possible
