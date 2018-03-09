# The ESP8266 WiFi driver for mbed-os
The mbed OS driver for the ESP8266 WiFi module

## Firmware version
ESP8266 modules come in different shapes and forms, but most important difference is which firmware version it is programmed with. To make sure that your module has mbed-os compatible firmware follow update guide: https://developer.mbed.org/teams/ESP8266/wiki/Firmware-Update

## Restrictions
- The ESP8266 WiFi module does not allow TCP client to bind on a specific port
- Setting up an UDP server is not possible
- Serial port does not have hardware flow control enabled. Also AT-command set does not have any way to limit download rate. Therefore downloading anything larger that serial port input buffer is unreliable. Application should be able to read fast enough to stay ahead of the network. This affects mostly the TCP protocol where data would be lost with no notification. On UDP this would lead to only packet losses which the higher layer protocol should recover from.
