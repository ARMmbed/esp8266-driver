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
#include "Callback.h"
#include "mbed_error.h"
#include "nsapi_types.h"
#include "PinNames.h"

#include <cstring>

#define ESP8266_DEFAULT_BAUD_RATE   115200
#define ESP8266_ALL_SOCKET_IDS      -1

ESP8266::ESP8266(PinName tx, PinName rx, bool debug, PinName rts, PinName cts)
    : _serial(tx, rx, ESP8266_DEFAULT_BAUD_RATE),
      _serial_rts(rts),
      _serial_cts(cts),
      _parser(&_serial),
      _tcp_passive(false),
      _packets(0),
      _packets_end(&_packets),
      _sdk_v(-1,-1,-1),
      _at_v(-1,-1,-1),
      _connect_error(0),
      _fail(false),
      _sock_already(false),
      _closed(false),
      _connection_status(NSAPI_STATUS_DISCONNECTED),
      _heap_usage(0)
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
    _parser.oob("WIFI ", callback(this, &ESP8266::_connection_status_handler));
    _parser.oob("UNLINK", callback(this, &ESP8266::_oob_socket_close_error));
    _parser.oob("ALREADY CONNECTED", callback(this, &ESP8266::_oob_cipstart_already_connected));

    for(int i= 0; i < SOCKET_COUNT; i++) {
        _sinfo[i].open = false;
        _sinfo[i].proto = NSAPI_UDP;
    }
}

bool ESP8266::at_available()
{
    _smutex.lock();
    bool ready = _parser.send("AT")
           && _parser.recv("OK\n");
    _smutex.unlock();

    return ready;
}

struct ESP8266::fw_sdk_version ESP8266::sdk_version()
{
    int major;
    int minor;
    int patch;

    _smutex.lock();
    bool done = _parser.send("AT+GMR")
        && _parser.recv("SDK version:%d.%d.%d", &major, &minor, &patch)
        && _parser.recv("OK\n");
    _smutex.unlock();

    if(done) {
        _sdk_v.major = major;
        _sdk_v.minor = minor;
        _sdk_v.patch = patch;
    }
    return _sdk_v;
}

struct ESP8266::fw_at_version ESP8266::at_version()
{
    int major;
    int minor;
    int patch;
    int nused;

    _smutex.lock();
    bool done = _parser.send("AT+GMR")
        && _parser.recv("AT version:%d.%d.%d.%d", &major, &minor, &patch, &nused)
        && _parser.recv("OK\n");
    _smutex.unlock();

    if(done) {
        _at_v.major = major;
        _at_v.minor = minor;
        _at_v.patch = patch;
    }
    return _at_v;
}

bool ESP8266::stop_uart_hw_flow_ctrl(void)
{
    bool done = true;

    if (_serial_rts != NC || _serial_cts != NC) {
        // Stop board's flow control
        _serial.set_flow_control(SerialBase::Disabled, _serial_rts, _serial_cts);

        // Stop ESP8266's flow control
        done = _parser.send("AT+UART_CUR=%u,8,1,0,0", ESP8266_DEFAULT_BAUD_RATE)
            && _parser.recv("OK\n");
    }

    return done;
}

bool ESP8266::start_uart_hw_flow_ctrl(void)
{
    bool done = true;

    if (_serial_rts != NC && _serial_cts != NC) {
        // Start board's flow control
        _serial.set_flow_control(SerialBase::RTSCTS, _serial_rts, _serial_cts);

        // Start ESP8266's flow control
        done = _parser.send("AT+UART_CUR=%u,8,1,0,3", ESP8266_DEFAULT_BAUD_RATE)
            && _parser.recv("OK\n");

    } else if (_serial_rts != NC) {
        _serial.set_flow_control(SerialBase::RTS, _serial_rts, NC);

        // Enable ESP8266's CTS pin
        done = _parser.send("AT+UART_CUR=%u,8,1,0,2", ESP8266_DEFAULT_BAUD_RATE)
            && _parser.recv("OK\n");

    } else if (_serial_cts != NC) {
        // Enable ESP8266's RTS pin
        done = _parser.send("AT+UART_CUR=%u,8,1,0,1", ESP8266_DEFAULT_BAUD_RATE)
            && _parser.recv("OK\n");

        _serial.set_flow_control(SerialBase::CTS, NC, _serial_cts);
    }

    return done;
}

bool ESP8266::startup(int mode)
{
    if (!(mode == WIFIMODE_STATION || mode == WIFIMODE_SOFTAP
        || mode == WIFIMODE_STATION_SOFTAP)) {
        return false;
    }

    _smutex.lock();
    set_timeout(ESP8266_CONNECT_TIMEOUT);
    bool done = _parser.send("AT+CWMODE_CUR=%d", mode)
            && _parser.recv("OK\n")
            &&_parser.send("AT+CIPMUX=1")
            && _parser.recv("OK\n");
    set_timeout(); //Restore default
    _smutex.unlock();

    return done;
}

bool ESP8266::reset(void)
{
    _smutex.lock();
    set_timeout(ESP8266_CONNECT_TIMEOUT);

    for (int i = 0; i < 2; i++) {
        if (_parser.send("AT+RST")
            && _parser.recv("OK\n")
            && _parser.recv("ready")) {
            _clear_socket_packets(ESP8266_ALL_SOCKET_IDS);
            _smutex.unlock();
            return true;
        }
    }
    set_timeout();
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

bool ESP8266::cond_enable_tcp_passive_mode()
{
    bool done = true;

    if (FW_AT_LEAST_VERSION(_at_v.major, _at_v.minor, _at_v.patch, 0, ESP8266_AT_VERSION_TCP_PASSIVE_MODE)) {
        _smutex.lock();
        done = _parser.send("AT+CIPRECVMODE=1")
                && _parser.recv("OK\n");
        _smutex.unlock();

        _tcp_passive = done ? true : false;
    }

    return done;
}


nsapi_error_t ESP8266::connect(const char *ap, const char *passPhrase)
{
    _smutex.lock();
    set_timeout(ESP8266_CONNECT_TIMEOUT);
    _connection_status = NSAPI_STATUS_CONNECTING;
    if(_connection_status_cb)
        _connection_status_cb(NSAPI_EVENT_CONNECTION_STATUS_CHANGE, _connection_status);

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
    set_timeout();
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

const char *ESP8266::ip_addr(void)
{
    _smutex.lock();
    set_timeout(ESP8266_CONNECT_TIMEOUT);
    if (!(_parser.send("AT+CIFSR")
        && _parser.recv("+CIFSR:STAIP,\"%15[^\"]\"", _ip_buffer)
        && _parser.recv("OK\n"))) {
        _smutex.unlock();
        return 0;
    }
    set_timeout();
    _smutex.unlock();

    return _ip_buffer;
}

const char *ESP8266::mac_addr(void)
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

const char *ESP8266::gateway()
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

const char *ESP8266::netmask()
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

int8_t ESP8266::rssi()
{
    int8_t rssi;
    char bssid[18];

    _smutex.lock();
    set_timeout(ESP8266_CONNECT_TIMEOUT);
    if (!(_parser.send("AT+CWJAP_CUR?")
        && _parser.recv("+CWJAP_CUR:\"%*[^\"]\",\"%17[^\"]\"", bssid)
        && _parser.recv("OK\n"))) {
       _smutex.unlock();
        return 0;
    }
    set_timeout();
   _smutex.unlock();

   _smutex.lock();
   set_timeout(ESP8266_CONNECT_TIMEOUT);
    if (!(_parser.send("AT+CWLAP=\"\",\"%s\",", bssid)
        && _parser.recv("+CWLAP:(%*d,\"%*[^\"]\",%hhd,", &rssi)
        && _parser.recv("OK\n"))) {
        _smutex.unlock();
        return 0;
    }
    set_timeout();
    _smutex.unlock();

    return rssi;
}

int ESP8266::scan(WiFiAccessPoint *res, unsigned limit)
{
    unsigned cnt = 0;
    nsapi_wifi_ap_t ap;

    _smutex.lock();
    set_timeout(ESP8266_CONNECT_TIMEOUT);

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
    set_timeout();
    _smutex.unlock();

    return cnt;
}

nsapi_error_t ESP8266::open_udp(int id, const char* addr, int port, int local_port)
{
    static const char *type = "UDP";
    bool done = false;

    if (id >= SOCKET_COUNT || _sinfo[id].open) {
        return NSAPI_ERROR_PARAMETER;
    }

    _smutex.lock();

    for (int i = 0; i < 2; i++) {
        if(local_port) {
            done = _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d,%d", id, type, addr, port, local_port);
        } else {
            done = _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d", id, type, addr, port);
        }

        if (done) {
            if (!_parser.recv("OK\n")) {
                if (_sock_already) {
                    _sock_already = false; // To be raised again by OOB msg
                    done = close(id);
                    if (!done) {
                        MBED_ERROR(MBED_MAKE_ERROR(MBED_MODULE_DRIVER, MBED_ERROR_CLOSE_FAILED), \
                                "ESP8266::_open_udp: device refused to close socket");
                    }
                }
                continue;
            }
            _sinfo[id].open = true;
            _sinfo[id].proto = NSAPI_UDP;
            break;
        }
    }
    _clear_socket_packets(id);

    _smutex.unlock();

    return done ? NSAPI_ERROR_OK : NSAPI_ERROR_DEVICE_ERROR;
}

nsapi_error_t ESP8266::open_tcp(int id, const char* addr, int port, int keepalive)
{
    static const char *type = "TCP";
    bool done = false;

    if (id >= SOCKET_COUNT || _sinfo[id].open) {
        return NSAPI_ERROR_PARAMETER;
    }

    _smutex.lock();

    for (int i = 0; i < 2; i++) {
        if(keepalive) {
            done = _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d,%d", id, type, addr, port, keepalive);
        } else {
            done = _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d", id, type, addr, port);
        }

        if (done) {
            if (!_parser.recv("OK\n")) {
                if (_sock_already) {
                    _sock_already = false; // To be raised again by OOB msg
                    done = close(id);
                    if (!done) {
                        MBED_ERROR(MBED_MAKE_ERROR(MBED_MODULE_DRIVER, MBED_ERROR_CLOSE_FAILED), \
                                "ESP8266::_open_tcp: device refused to close socket");
                    }
                }
                continue;
            }
            _sinfo[id].open = true;
            _sinfo[id].proto = NSAPI_TCP;
            break;
        }
    }
    _clear_socket_packets(id);

    _smutex.unlock();

    return done ? NSAPI_ERROR_OK : NSAPI_ERROR_DEVICE_ERROR;
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
        set_timeout(ESP8266_SEND_TIMEOUT);
        if (_parser.send("AT+CIPSEND=%d,%lu", id, amount)
            && _parser.recv(">")
            && _parser.write((char*)data, (int)amount) >= 0) {
            // No flow control, data overrun is possible
            if (_serial_rts == NC) {
                while (_parser.process_oob()); // Drain USART receive register
            }
            _smutex.unlock();
            return NSAPI_ERROR_OK;
        }
        set_timeout();
        _smutex.unlock();
    }

    return NSAPI_ERROR_DEVICE_ERROR;
}

void ESP8266::_packet_handler()
{
    int id;
    int amount;
    int pdu_len;

    // Get socket id
    if (!_parser.recv(",%d,", &id)) {
        return;
    }
    // In passive mode amount not used...
    if(_tcp_passive
            && _sinfo[id].open == true
            && _sinfo[id].proto == NSAPI_TCP) {
        if (!_parser.recv("%d\n", &amount)) {
            MBED_ERROR(MBED_MAKE_ERROR(MBED_MODULE_DRIVER, MBED_ERROR_CODE_ENODATA), \
                    "ESP8266::_packet_handler(): Data length missing");
        }
        return;
    // Amount required in active mode
    } else if (!_parser.recv("%d:", &amount)) {
        return;
    }

    pdu_len = sizeof(struct packet) + amount;

    if ((_heap_usage + pdu_len) > MBED_CONF_ESP8266_SOCKET_BUFSIZE) {
        MBED_WARNING(MBED_MAKE_ERROR(MBED_MODULE_DRIVER, MBED_ERROR_CODE_ENOBUFS), \
                "ESP8266::_packet_handler(): \"esp8266.socket-bufsize\"-limit exceeded, packet dropped");
        return;
    }

    struct packet *packet = (struct packet*)malloc(pdu_len);
    if (!packet) {
        MBED_WARNING(MBED_MAKE_ERROR(MBED_MODULE_DRIVER, MBED_ERROR_CODE_ENOMEM), \
                "ESP8266::_packet_handler(): Could not allocate memory for RX data");
        return;
    }
    _heap_usage += pdu_len;

    packet->id = id;
    packet->len = amount;
    packet->alloc_len = amount;
    packet->next = 0;

    if (_parser.read((char*)(packet + 1), amount) < amount) {
        free(packet);
        _heap_usage -= pdu_len;
        return;
    }

    // append to packet list
    *_packets_end = packet;
    _packets_end = &packet->next;
}

void ESP8266::process_oob(uint32_t timeout, bool all) {
    set_timeout(timeout);
    // Poll for inbound packets
    while (_parser.process_oob() && all) {
    }
    set_timeout();
}

int32_t ESP8266::_recv_tcp_passive(int id, void *data, uint32_t amount, uint32_t timeout)
{
    int32_t len;
    int32_t ret = (int32_t)NSAPI_ERROR_WOULD_BLOCK;

    _smutex.lock();

    // NOTE: documentation v3.0 says '+CIPRECVDATA:<data_len>,' but it's not how the FW responds...
    bool done = _parser.send("AT+CIPRECVDATA=%d,%lu", id, amount)
        && _parser.recv("+CIPRECVDATA,%ld:", &len)
        && _parser.read((char*)data, len)
        && _parser.recv("OK\n");

    if (done) {
        _smutex.unlock();
        return len;
    }

    // Socket closed, doesn't mean there couldn't be data left
    if (!_sinfo[id].open) {
        done = _parser.send("AT+CIPRECVDATA=%d,%lu", id, amount)
            && _parser.recv("+CIPRECVDATA,%ld:", &len)
            && _parser.read((char*)data, len)
            && _parser.recv("OK\n");

        ret = done ? len : 0;
    }

    _smutex.unlock();
    return ret;
}

int32_t ESP8266::recv_tcp(int id, void *data, uint32_t amount, uint32_t timeout)
{
    if (_tcp_passive) {
        return _recv_tcp_passive(id, data, amount, timeout);
    }

    _smutex.lock();

    // No flow control, drain the USART receive register ASAP to avoid data overrun
    if (_serial_rts == NC) {
        process_oob(timeout, true);
    }

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

                uint32_t pdu_len = sizeof(struct packet) + q->alloc_len;
                uint32_t len = q->len;
                free(q);
                _heap_usage -= pdu_len;
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
    if(!_sinfo[id].open) {
        _smutex.unlock();
        return 0;
    }

    // Flow control, read from USART receive register only when no more data is buffered, and as little as possible
    if (_serial_rts != NC) {
        process_oob(timeout, false);
    }
    _smutex.unlock();

    return NSAPI_ERROR_WOULD_BLOCK;
}

int32_t ESP8266::recv_udp(int id, void *data, uint32_t amount, uint32_t timeout)
{
    _smutex.lock();
    set_timeout(timeout);

    // No flow control, drain the USART receive register ASAP to avoid data overrun
    if (_serial_rts == NC) {
        process_oob(timeout, true);
    }

    set_timeout();

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

            uint32_t pdu_len = sizeof(struct packet) + q->alloc_len;
            free(q);
            _heap_usage -= pdu_len;
            return len;
        }
    }

    // Flow control, read from USART receive register only when no more data is buffered, and as little as possible
    if (_serial_rts != NC) {
        process_oob(timeout, false);
    }

    _smutex.unlock();

    return NSAPI_ERROR_WOULD_BLOCK;
}

void ESP8266::_clear_socket_packets(int id)
{
    struct packet **p = &_packets;

    while (*p) {
        if ((*p)->id == id || id == ESP8266_ALL_SOCKET_IDS) {
            struct packet *q = *p;
            int pdu_len = sizeof(struct packet) + q->alloc_len;

            if (_packets_end == &(*p)->next) {
                _packets_end = p; // Set last packet next field/_packets
            }
            *p = (*p)->next;
            free(q);
            _heap_usage -= pdu_len;
        } else {
            // Point to last packet next field
            p = &(*p)->next;
        }
    }
}

bool ESP8266::close(int id)
{
    //May take a second try if device is busy
    for (unsigned i = 0; i < 2; i++) {
        _smutex.lock();
        if (_parser.send("AT+CIPCLOSE=%d", id)) {
            if (!_parser.recv("OK\n")) {
                if (_closed) { // UNLINK ERROR
                    _closed = false;
                    _sinfo[id].open = false;
                    _clear_socket_packets(id);
                    _smutex.unlock();
                    // ESP8266 has a habit that it might close a socket on its own.
                    //debug("ESP8266: socket %d already closed when calling close\n", id);
                    return true;
                }
            } else {
                _socket_open[id].id = -1;
                _clear_socket_packets(id);
                _smutex.unlock();
                return true;
            }
        }
        _smutex.unlock();
    }

    return false;
}

void ESP8266::set_timeout(uint32_t timeout_ms)
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

void ESP8266::sigio(Callback<void()> func)
{
    _serial.sigio(func);
}

void ESP8266::attach(mbed::Callback<void(nsapi_event_t, intptr_t)> status_cb)
{
    _connection_status_cb = status_cb;
}

bool ESP8266::recv_ap(nsapi_wifi_ap_t *ap)
{
    int sec;
    int dummy;
    bool ret = _parser.recv("+CWLAP:(%d,\"%32[^\"]\",%hhd,\"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx\",%hhu,%d,%d)\n",
            &sec,
            ap->ssid,
            &ap->rssi,
            &ap->bssid[0], &ap->bssid[1], &ap->bssid[2], &ap->bssid[3], &ap->bssid[4], &ap->bssid[5],
            &ap->channel,
            &dummy,
            &dummy);

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


void ESP8266::_oob_cipstart_already_connected()
{
    _sock_already = true;
    _parser.abort();
}

void ESP8266::_oob_socket_close_error()
{
    if (_parser.recv("ERROR\n")) {
        _closed = true; // Not possible to pinpoint to a certain socket
        _parser.abort();
    }
}

void ESP8266::_oob_socket0_closed_handler()
{
    _sinfo[0].open = false;
}

void ESP8266::_oob_socket1_closed_handler()
{
    _sinfo[1].open = false;
}

void ESP8266::_oob_socket2_closed_handler()
{
    _sinfo[2].open = false;
}

void ESP8266::_oob_socket3_closed_handler()
{
    _sinfo[3].open = false;
}

void ESP8266::_oob_socket4_closed_handler()
{
    _sinfo[4].open = false;
}

void ESP8266::_connection_status_handler()
{
    char status[13];
    if (_parser.recv("%12[^\"]\n", status)) {
        if (strcmp(status, "GOT IP\n") == 0)
            _connection_status = NSAPI_STATUS_GLOBAL_UP;
        else if (strcmp(status, "DISCONNECT\n") == 0)
            _connection_status = NSAPI_STATUS_DISCONNECTED;
        else
            return;

        if(_connection_status_cb)
            _connection_status_cb(NSAPI_EVENT_CONNECTION_STATUS_CHANGE, _connection_status);
    }
}

int8_t ESP8266::default_wifi_mode()
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

nsapi_connection_status_t ESP8266::connection_status() const
{
    return _connection_status;
}
