/* ESP8266 Example
 * Copyright (c) 2015 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ESP8266.h"
#include "mbed_debug.h"
#include "nsapi_types.h"

#include <cstring>


#define TRACE_GROUP "Wifi"
#define ESP8266_DEFAULT_BAUD_RATE   115200


ESP8266::ESP8266(PinName tx, PinName rx, bool debug)
    : _serial(tx, rx, ESP8266_DEFAULT_BAUD_RATE), 
      _parser(&_serial), 
      _packets(0), 
      _packets_end(&_packets),
      _connect_error(0),
      _fail(false),
      _send_in_progress(false),
      _socket_open()
{
    _serial.set_baud( ESP8266_DEFAULT_BAUD_RATE );
    _parser.debug_on(debug);
    _parser.set_delimiter("\r\n");
    _parser.oob("+IPD", callback(this, &ESP8266::_packet_handler));
    //Note: espressif at command document says that this should be +CWJAP_CUR:<error code>
    //but seems that at least current version is not sending it
    //https://www.espressif.com/sites/default/files/documentation/4a-esp8266_at_instruction_set_en.pdf
    //Also seems that ERROR is not sent, but FAIL instead
    _parser.oob("+CWJAP:", callback(this, &ESP8266::_connect_error_handler));
    _parser.oob("0,CLOSED", callback(this, &ESP8266::_oob_socket0_closed_handler));
    _parser.oob("1,CLOSED", callback(this, &ESP8266::_oob_socket1_closed_handler));
    _parser.oob("2,CLOSED", callback(this, &ESP8266::_oob_socket2_closed_handler));
    _parser.oob("3,CLOSED", callback(this, &ESP8266::_oob_socket3_closed_handler));
    _parser.oob("4,CLOSED", callback(this, &ESP8266::_oob_socket4_closed_handler));
    _parser.oob("SEND OK", callback(this, &ESP8266::_oob_send_done_handler));
    _parser.oob("SEND FAIL", callback(this, &ESP8266::_oob_send_fail_handler));
}

int ESP8266::get_firmware_version()
{
    int version;

    _smutex.lock();
    bool done = _parser.send("AT+GMR")
           && _parser.recv("SDK version:%d", &version)
           && _parser.recv("OK\n");
    _smutex.unlock();

    if(done) {
        return version;
    } else { 
        // Older firmware versions do not prefix the version with "SDK version: "
        return -1;
    }
}

bool ESP8266::startup(int mode)
{
    if (!(mode == WIFIMODE_STATION || mode == WIFIMODE_SOFTAP
        || mode == WIFIMODE_STATION_SOFTAP)) {
        return false;
    }

    _smutex.lock();
    bool done = _parser.send("AT+CWMODE_CUR=%d", mode)
            && _parser.recv("OK\n")
            &&_parser.send("AT+CIPMUX=1")
            && _parser.recv("OK\n");
    _smutex.unlock();

    return done;
}

bool ESP8266::reset(void)
{
    _smutex.lock();
    for (int i = 0; i < 2; i++) {
        if (_parser.send("AT+RST")
            && _parser.recv("OK\n")
            && _parser.recv("ready")) {
            _smutex.unlock();
            return true;
        }
    }
    _smutex.unlock();

    return false;
}

bool ESP8266::dhcp(bool enabled, int mode)
{
    //only 3 valid modes
    if (mode < 0 || mode > 2) {
        return false;
    }

    _smutex.lock();
    bool done = _parser.send("AT+CWDHCP_CUR=%d,%d", mode, enabled?1:0)
                && _parser.recv("OK\n");
    _smutex.unlock();

    return done;
}

nsapi_error_t ESP8266::connect(const char *ap, const char *passPhrase)
{
    _smutex.lock();
    _parser.send("AT+CWJAP_CUR=\"%s\",\"%s\"", ap, passPhrase);
    if (!_parser.recv("OK\n")) {
        if (_fail) {
            _smutex.unlock();
            nsapi_error_t ret;
            if (_connect_error == 1)
                ret = NSAPI_ERROR_CONNECTION_TIMEOUT;
            else if (_connect_error == 2)
                ret = NSAPI_ERROR_AUTH_FAILURE;
            else if (_connect_error == 3)
                ret = NSAPI_ERROR_NO_SSID;
            else
                ret = NSAPI_ERROR_NO_CONNECTION;

            _fail = false;
            _connect_error = 0;
            return ret;
        }
    }
    _smutex.unlock();

    return NSAPI_ERROR_OK;
}

bool ESP8266::disconnect(void)
{
    _smutex.lock();
    bool done = _parser.send("AT+CWQAP") && _parser.recv("OK\n");
    _smutex.unlock();

    return done;
}

const char *ESP8266::getIPAddress(void)
{
    _smutex.lock();
    if (!(_parser.send("AT+CIFSR")
        && _parser.recv("+CIFSR:STAIP,\"%15[^\"]\"", _ip_buffer)
        && _parser.recv("OK\n"))) {
        _smutex.unlock();
        return 0;
    }
    _smutex.unlock();

    return _ip_buffer;
}

const char *ESP8266::getMACAddress(void)
{
    _smutex.lock();
    if (!(_parser.send("AT+CIFSR")
        && _parser.recv("+CIFSR:STAMAC,\"%17[^\"]\"", _mac_buffer)
        && _parser.recv("OK\n"))) {
        _smutex.unlock();
        return 0;
    }
    _smutex.unlock();

    return _mac_buffer;
}

const char *ESP8266::getGateway()
{
    _smutex.lock();
    if (!(_parser.send("AT+CIPSTA_CUR?")
        && _parser.recv("+CIPSTA_CUR:gateway:\"%15[^\"]\"", _gateway_buffer)
        && _parser.recv("OK\n"))) {
        _smutex.unlock();
        return 0;
    }
    _smutex.unlock();

    return _gateway_buffer;
}

const char *ESP8266::getNetmask()
{
    _smutex.lock();
    if (!(_parser.send("AT+CIPSTA_CUR?")
        && _parser.recv("+CIPSTA_CUR:netmask:\"%15[^\"]\"", _netmask_buffer)
        && _parser.recv("OK\n"))) {
        _smutex.unlock();
        return 0;
    }
    _smutex.unlock();

    return _netmask_buffer;
}

int8_t ESP8266::getRSSI()
{
    int8_t rssi;
    char bssid[18];

    _smutex.lock();
   if (!(_parser.send("AT+CWJAP_CUR?")
        && _parser.recv("+CWJAP_CUR:\"%*[^\"]\",\"%17[^\"]\"", bssid)
        && _parser.recv("OK\n"))) {
       _smutex.unlock();
        return 0;
    }
   _smutex.unlock();

   _smutex.lock();
    if (!(_parser.send("AT+CWLAP=\"\",\"%s\",", bssid)
        && _parser.recv("+CWLAP:(%*d,\"%*[^\"]\",%hhd,", &rssi)
        && _parser.recv("OK\n"))) {
        _smutex.unlock();
        return 0;
    }
    _smutex.unlock();

    return rssi;
}

int ESP8266::scan(WiFiAccessPoint *res, unsigned limit)
{
    unsigned cnt = 0;
    nsapi_wifi_ap_t ap;

    _smutex.lock();
    if (!_parser.send("AT+CWLAP")) {
        _smutex.unlock();
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    while (recv_ap(&ap)) {
        if (cnt < limit) {
            res[cnt] = WiFiAccessPoint(ap);
        }

        cnt++;
        if (limit != 0 && cnt >= limit) {
            break;
        }
    }
    _smutex.unlock();

    return cnt;
}

bool ESP8266::open_udp(int id, const char* addr, int port, int local_port)
{
    static const char *type = "UDP";
    bool done = false;

    if (id >= SOCKET_COUNT || _socket_open[id]) {
        return false;
    }

    _smutex.lock();
    if(local_port) {
        done = _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d,%d", id, type, addr, port, local_port)
                && _parser.recv("OK\n");
    } else {
        done = _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d", id, type, addr, port)
               && _parser.recv("OK\n");
    }

    if (done) {
        _socket_open[id] = 1;
    }

    _smutex.unlock();

    return done;
}

bool ESP8266::open_tcp(int id, const char* addr, int port, int keepalive)
{
    static const char *type = "TCP";
    bool done = false;

    if (id >= SOCKET_COUNT || _socket_open[id]) {
        return false;
    }

    _smutex.lock();

    if(keepalive) {
        done = _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d,%d", id, type, addr, port, keepalive)
                && _parser.recv("OK\n");
    } else {
        done = _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d", id, type, addr, port)
               && _parser.recv("OK\n");
    }

    if (done) {
        _socket_open[id] = 1;
    }

    _smutex.unlock();

    return done;
}

bool ESP8266::dns_lookup(const char* name, char* ip)
{
    _smutex.lock();
    bool done = _parser.send("AT+CIPDOMAIN=\"%s\"", name) && _parser.recv("+CIPDOMAIN:%s%*[\r]%*[\n]", ip);
    _smutex.unlock();

    return done;
}

nsapi_error_t ESP8266::send(int id, const void *data, uint32_t amount)
{
    //May take a second try if device is busy
    for (unsigned i = 0; i < 2; i++) {
        _smutex.lock();
        if (_parser.send("AT+CIPSEND=%d,%lu", id, amount)
            && _parser.recv(">")
            && _parser.write((char*)data, (int)amount) >= 0) {
            _send_in_progress = true;
            _fail = false;
            // wait for "SEND OK/FAIL" to be received
            // multiple sends in a row require this
            while (_parser.process_oob() && _send_in_progress); 
            nsapi_error_t result = (_fail) ? NSAPI_ERROR_DEVICE_ERROR : NSAPI_ERROR_OK;
            _smutex.unlock();
            return result;
        }
        _smutex.unlock();
    }

    return NSAPI_ERROR_DEVICE_ERROR;
}

void ESP8266::_packet_handler()
{
    int id;
    uint32_t amount;

    // parse out the packet
    if (!_parser.recv(",%d,%lu:", &id, &amount)) {
        return;
    }

    struct packet *packet = (struct packet*)malloc(
            sizeof(struct packet) + amount);
    if (!packet) {
        debug("Could not allocate memory for RX data");
        return;
    }

    packet->id = id;
    packet->len = amount;
    packet->next = 0;

    if (!(_parser.read((char*)(packet + 1), amount))) {
        free(packet);
        return;
    }

    // append to packet list
    *_packets_end = packet;
    _packets_end = &packet->next;
}

int32_t ESP8266::recv_tcp(int id, void *data, uint32_t amount)
{
    _smutex.lock();

    // Poll for inbound packets
    // Terminate after first available packet
    bool more = true;
    while (more) {
        more = _parser.process_oob();

        // check if any packets are ready for us
        for (struct packet **p = &_packets; *p; p = &(*p)->next) {
            if ((*p)->id == id) {
                struct packet *q = *p;

                if (q->len <= amount) { // Return and remove full packet
                    memcpy(data, q+1, q->len);

                    if (_packets_end == &(*p)->next) {
                        _packets_end = p;
                    }
                    *p = (*p)->next;
                    _smutex.unlock();

                    uint32_t len = q->len;
                    free(q);
                    return len;
                } else { // return only partial packet
                    memcpy(data, q+1, amount);

                    q->len -= amount;
                    memmove(q+1, (uint8_t*)(q+1) + amount, q->len);

                    _smutex.unlock();
                    return amount;
                }
            }
        }
    }
    if(!_socket_open[id]) {
        _smutex.unlock();
        return 0;
    }

    _smutex.unlock();

    return NSAPI_ERROR_WOULD_BLOCK;
}

int32_t ESP8266::recv_udp(int id, void *data, uint32_t amount)
{
    _smutex.lock();
    // Poll for inbound packets
    while (_parser.process_oob()) {
    }

    // check if any packets are ready for us
    for (struct packet **p = &_packets; *p; p = &(*p)->next) {
        if ((*p)->id == id) {
            struct packet *q = *p;

            // Return and remove packet (truncated if necessary)
            uint32_t len = q->len < amount ? q->len : amount;
            memcpy(data, q+1, len);

            if (_packets_end == &(*p)->next) {
                _packets_end = p;
            }
            *p = (*p)->next;
            _smutex.unlock();

            free(q);
            return len;
        }
    }

    _smutex.unlock();

    return NSAPI_ERROR_WOULD_BLOCK;
}

bool ESP8266::close(int id)
{
    //May take a second try if device is busy
    for (unsigned i = 0; i < 2; i++) {
        _smutex.lock();
        if (_parser.send("AT+CIPCLOSE=%d", id) && _parser.recv("OK\n")) {
            if (!_socket_open[id]) { // recv(processing OOBs) needs to be done first
                _smutex.unlock();
                return true;
            }
        }
        _smutex.unlock();
    }

    return false;
}

void ESP8266::setTimeout(uint32_t timeout_ms)
{
    _parser.set_timeout(timeout_ms);
}

bool ESP8266::readable()
{
    return _serial.FileHandle::readable();
}

bool ESP8266::writeable()
{
    return _serial.FileHandle::writable();
}

void ESP8266::attach(Callback<void()> func)
{
    _serial.sigio(func);
}

bool ESP8266::recv_ap(nsapi_wifi_ap_t *ap)
{
    int sec;
    bool ret = _parser.recv("+CWLAP:(%d,\"%32[^\"]\",%hhd,\"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx\",%hhu", &sec, ap->ssid,
                            &ap->rssi, &ap->bssid[0], &ap->bssid[1], &ap->bssid[2], &ap->bssid[3], &ap->bssid[4],
                            &ap->bssid[5], &ap->channel);

    ap->security = sec < 5 ? (nsapi_security_t)sec : NSAPI_SECURITY_UNKNOWN;

    return ret;
}

void ESP8266::_connect_error_handler()
{
    _fail = false;
    _connect_error = 0;

    if (_parser.recv("%d", &_connect_error) && _parser.recv("FAIL")) {
        _fail = true;
        _parser.abort();
    }
}

void ESP8266::_oob_socket0_closed_handler()
{
    _socket_open[0] = 0;
}

void ESP8266::_oob_socket1_closed_handler()
{
    _socket_open[1] = 0;
}

void ESP8266::_oob_socket2_closed_handler()
{
    _socket_open[2] = 0;
}

void ESP8266::_oob_socket3_closed_handler()
{
    _socket_open[3] = 0;
}

void ESP8266::_oob_socket4_closed_handler()
{
    _socket_open[4] = 0;
}

void ESP8266::_oob_send_done_handler()
{
    _send_in_progress = false;
}

void ESP8266::_oob_send_fail_handler()
{
    _send_in_progress = false;
    _fail = true;
}

int8_t ESP8266::get_default_wifi_mode()
{
    int8_t mode;

    _smutex.lock();
    if (_parser.send("AT+CWMODE_DEF?")
        && _parser.recv("+CWMODE_DEF:%hhd", &mode)
        && _parser.recv("OK\n")) {
        _smutex.unlock();
        return mode;
    }
    _smutex.unlock();

    return 0;
}

bool ESP8266::set_default_wifi_mode(const int8_t mode)
{
    _smutex.lock();
    bool done = _parser.send("AT+CWMODE_DEF=%hhd", mode)
                && _parser.recv("OK\n");
    _smutex.unlock();

    return done;
}
