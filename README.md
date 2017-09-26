# The ESP8266 WiFi driver for mbed-os
The mbed OS driver for the ESP8266 WiFi module

## Firmware version
ESP8266 modules come in different shapes and forms, but most important difference is which firmware version it is programmed with. To make sure that your module has mbed-os compatible firmware follow update guide: https://developer.mbed.org/teams/ESP8266/wiki/Firmware-Update

## Testing
The ESP8266 library contains the core network tests taken from mbed OS. To run the tests you will need mbed CLI and mbed OS.

First, setup the the esp8266-driver and mbed-os repositories for testing:
``` bash
# Sets up the ESP8266 for testing
mbed import esp8266-driver
cd esp8266-driver
mbed add mbed-os
```

Now you should be able to run the network tests with `mbed test`:
``` bash
# Runs the ESP8266 network tests, requires a wifi access point
mbed test -t <COMPILER HERE> -m <BOARD HERE> -n tests-net* --compile -DMBED_CFG_ESP8266_SSID=<SSID HERE> -DMBED_CFG_ESP8266_PASS=<PASS HERE>
mbed test -t <COMPILER HERE> -m <BOARD HERE> -n tests-net* --run --verbose
```

There are a couple other options that can be used during testing:
- MBED_CFG_ESP8266_SSID - SSID of the wifi access point to connect to
- MBED_CFG_ESP8266_PASS - Passphrase of the wifi access point to connect to
- MBED_CFG_ESP8266_TX - TX pin for the ESP8266 serial connection (defaults to D1)
- MBED_CFG_ESP8266_RX - TX pin for the ESP8266 serial connection (defaults to D0)
- MBED_CFG_ESP8266_DEBUG - Enabled debug output from the ESP8266

For example, here is how to enabled the debug output from the ESP8266:
``` bash
# Run the ESP8266 network tests with debug output, requires a wifi access point
mbed test -t <COMPILER HERE> -m <BOARD HERE> -n tests-net* --compile -DMBED_CFG_ESP8266_SSID=<SSID HERE> -DMBED_CFG_ESP8266_PASS=<PASS HERE> -DMBED_CFG_ESP8266_DEBUG=true
mbed test -t <COMPILER HERE> -m <BOARD HERE> -n tests-net* --run --verbose
```
