# The ESP8266 WiFi driver for mbed-os
The mbed OS driver for the ESP8266 WiFi module

## Firmware version
ESP8266 modules come in different shapes and forms, but most important difference is which firmware version it is programmed with. To make sure that your module has mbed-os compatible firmware follow update guide: https://developer.mbed.org/teams/ESP8266/wiki/Firmware-Update

## Testing
The ESP8266 library contains the core network tests taken from mbed OS. After installing mbed CLI and importing the mbed OS library, the tests can be ran with the `mbed test` command:
``` bash
# Runs the ESP8266 network tests, requires a wifi access point
mbed test -t <COMPILER HERE> -m <BOARD HERE> -n tests-net* --compile -DMBED_CFG_ESP8266_SSID='"<SSID HERE>"' -DMBED_CFG_ESP8266_PASS='"<PASS HERE>"'
mbed test -t <COMPILER HERE> -m <BOARD HERE> -n tests-net* --run --verbose
```
