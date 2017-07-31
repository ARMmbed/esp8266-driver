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

ESP8266::ESP8266(PinName tx, PinName rx, Callback<void(SignalingAction, int)> signalingCallback, bool debug)
    : _serial(tx, rx, 1024), _parser(_serial), _signalingCallback(signalingCallback)
    , _packets(0), _packets_end(&_packets)
{
    _serial.baud(115200);
    _parser.debugOn(debug);

    _in_server_mode = false;
    _ipd_packet = NULL;
    _global_socket_counter = 0;
}

int ESP8266::get_firmware_version()
{
    _parser.send("AT+GMR");
    int version;
    if(_parser.recv("SDK version:%d", &version) && _parser.recv("OK")) {
        return version;
    } else {
        // Older firmware versions do not prefix the version with "SDK version: "
        return -1;
    }

}

bool ESP8266::startup(int mode)
{
    //only 3 valid modes
    if(mode < 1 || mode > 3) {
        return false;
    }

    bool success = _parser.send("AT+CWMODE_CUR=%d", mode)
        && _parser.recv("OK")
        && _parser.send("AT+CIPMUX=1")
        && _parser.recv("OK");

    _parser.oob("+IPD", this, &ESP8266::_packet_handler);

    return success;
}

bool ESP8266::reset(void)
{
    for (int i = 0; i < 2; i++) {
        if (_parser.send("AT+RST")
            && _parser.recv("OK\r\nready")) {
            return true;
        }
    }

    return false;
}

bool ESP8266::dhcp(bool enabled, int mode)
{
    //only 3 valid modes
    if(mode < 0 || mode > 2) {
        return false;
    }

    return _parser.send("AT+CWDHCP_CUR=%d,%d", enabled?1:0, mode)
        && _parser.recv("OK");
}

bool ESP8266::connect(const char *ap, const char *passPhrase)
{
    return _parser.send("AT+CWJAP_CUR=\"%s\",\"%s\"", ap, passPhrase)
        && _parser.recv("OK");
}

bool ESP8266::disconnect(void)
{
    return _parser.send("AT+CWQAP") && _parser.recv("OK");
}

const char *ESP8266::getIPAddress(void)
{
    if (!(_parser.send("AT+CIFSR")
        && _parser.recv("+CIFSR:STAIP,\"%15[^\"]\"", _ip_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _ip_buffer;
}

const char *ESP8266::getMACAddress(void)
{
    if (!(_parser.send("AT+CIFSR")
        && _parser.recv("+CIFSR:STAMAC,\"%17[^\"]\"", _mac_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _mac_buffer;
}

const char *ESP8266::getGateway()
{
    if (!(_parser.send("AT+CIPSTA_CUR?")
        && _parser.recv("+CIPSTA_CUR:gateway:\"%15[^\"]\"", _gateway_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _gateway_buffer;
}

const char *ESP8266::getNetmask()
{
    if (!(_parser.send("AT+CIPSTA_CUR?")
        && _parser.recv("+CIPSTA_CUR:netmask:\"%15[^\"]\"", _netmask_buffer)
        && _parser.recv("OK"))) {
        return 0;
    }

    return _netmask_buffer;
}

int8_t ESP8266::getRSSI()
{
    int8_t rssi;
    char bssid[18];

   if (!(_parser.send("AT+CWJAP_CUR?")
        && _parser.recv("+CWJAP_CUR::\"%*[^\"]\",\"%17[^\"]\"", bssid)
        && _parser.recv("OK"))) {
        return 0;
    }

    if (!(_parser.send("AT+CWLAP=\"\",\"%s\",", bssid)
        && _parser.recv("+CWLAP:(%*d,\"%*[^\"]\",%hhd,", &rssi)
        && _parser.recv("OK"))) {
        return 0;
    }

    return rssi;
}

bool ESP8266::isConnected(void)
{
    return getIPAddress() != 0;
}

int ESP8266::scan(WiFiAccessPoint *res, unsigned limit)
{
    unsigned cnt = 0;
    nsapi_wifi_ap_t ap;

    if (!_parser.send("AT+CWLAP")) {
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

    return cnt;
}

bool ESP8266::open(const char *type, int id, const char* addr, int port)
{
    //IDs only 0-4
    if(id > 4) {
        return false;
    }
    return _parser.send("AT+CIPSTART=%d,\"%s\",\"%s\",%d", id, type, addr, port)
        && _parser.recv("OK");
}

bool ESP8266::dns_lookup(const char* name, char* ip)
{
    return _parser.send("AT+CIPDOMAIN=\"%s\"", name) && _parser.recv("+CIPDOMAIN:%s%*[\r]%*[\n]", ip);
}

bool ESP8266::send(int id, const void *data, uint32_t amount)
{
    //May take a second try if device is busy
    for (unsigned i = 0; i < 2; i++) {
        if (_parser.send("AT+CIPSEND=%d,%d", id, amount)
            && _parser.recv(">")
            && _parser.write((char*)data, (int)amount) >= 0) {
            return true;
        }
    }

    return false;
}

void ESP8266::_packet_handler()
{
    if (_in_server_mode) return;

    int id;
    uint32_t amount;

    // parse out the packet
    if (!_parser.recv(",%d,%d:", &id, &amount)) {
        return;
    }

    struct packet *packet = (struct packet*)malloc(
            sizeof(struct packet) + amount);
    if (!packet) {
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

int32_t ESP8266::recv(int id, void *data, uint32_t amount)
{
    bool exit_when_not_found = false;

    // see if the underlying socket changed while in the recv() call
    // if you don't do this check it might be that a CLOSED,CONNECT happens on the same esp8266 socket id
    // and thus we associate the data with the wrong mbed TCPSocket
    uint32_t incoming_socket_id = _incoming_socket_status[id];

    while (true) {
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

                    uint32_t len = q->len;
                    free(q);
                    return len;
                } else { // return only partial packet
                    memcpy(data, q+1, amount);

                    q->len -= amount;
                    memmove(q+1, (uint8_t*)(q+1) + amount, q->len);

                    return amount;
                }
            }
        }

        if (exit_when_not_found) {
            return -1;
        }

        // Here's a problem... recv() blocks for forever, but it might be that the underlying socket closes in the meantime.
        // We know when it happens (due to monitoring the RX IRQ channel)
        // but there's no way of signaling this thread and actually abort the request...

        if (incoming_socket_id > 0) {
            int timeout = _parser.getTimeout();
            _parser.setTimeout(1000);

            if (!_parser.recv("OK")) {
                _parser.setTimeout(timeout);

                // socket gone
                if (incoming_socket_id != _incoming_socket_status[id]) return NSAPI_ERROR_NO_SOCKET;

                // otherwise, just continue trying to get data...
                continue;
            }
        }
        else {
            // Wait for inbound packet
            if (!_parser.recv("OK")) {
                // so this is weird... the message just received by the parser could actually be one of ours (in TCPServer mode)...
                // so do one more pass...
                exit_when_not_found = true;
                continue;
            }
        }
    }
}

bool ESP8266::close(int id)
{
    //May take a second try if device is busy
    for (unsigned i = 0; i < 2; i++) {
        if (_parser.send("AT+CIPCLOSE=%d", id)
            && _parser.recv("OK")) {
            return true;
        }
    }

    return false;
}

void ESP8266::setTimeout(uint32_t timeout_ms)
{
    _parser.setTimeout(timeout_ms);
}

bool ESP8266::readable()
{
    return _serial.readable();
}

bool ESP8266::writeable()
{
    return _serial.writeable();
}

void ESP8266::attach(Callback<void(int)> func)
{
    _serial.attach(func);
}

bool ESP8266::recv_ap(nsapi_wifi_ap_t *ap)
{
    int sec;
    bool ret = _parser.recv("+CWLAP:(%d,\"%32[^\"]\",%hhd,\"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx\",%d", &sec, ap->ssid,
                            &ap->rssi, &ap->bssid[0], &ap->bssid[1], &ap->bssid[2], &ap->bssid[3], &ap->bssid[4],
                            &ap->bssid[5], &ap->channel);

    ap->security = sec < 5 ? (nsapi_security_t)sec : NSAPI_SECURITY_UNKNOWN;

    return ret;
}

bool ESP8266::bind(const SocketAddress& address)
{
    // we need an event queue to dispatch from serial RX IRQ -> non-IRQ thread
    event_queue = new EventQueue();
    event_thread = new Thread(osPriorityNormal, 2048);
    if (!event_queue || !event_thread) {
        return NSAPI_ERROR_NO_MEMORY;
    }
    event_thread->start(callback(event_queue, &EventQueue::dispatch_forever));

    // buffer to store RX data in
    rx_buffer = (char*)malloc(1024);
    rx_ix = 0;
    if (!rx_buffer) {
        return NSAPI_ERROR_NO_MEMORY;
    }

    // clear incoming socket status
    memset(_incoming_socket_status, 0, sizeof(_incoming_socket_status));

    // attach to the serial
    _serial.attach(callback(this, &ESP8266::attach_rx));

    _in_server_mode = true;

    // and start the actual server
    return _parser.send("AT+CIPSERVER=1,%d", address.get_port())
        && _parser.recv("OK");
}

void ESP8266::process_command(char* cmd, size_t size) {
    if (_ipd_packet) {
        memcpy(_ipd_packet_data_ptr, cmd, size);
        _ipd_packet_data_ptr += size;

        _ipd_packet_data_ptr[0] = '\r';
        _ipd_packet_data_ptr[1] = '\n';
        _ipd_packet_data_ptr += 2;

        if (_ipd_packet_data_ptr == ((char*)(_ipd_packet + 1)) + _ipd_packet->len) {
            // append to packet list
            *_packets_end = _ipd_packet;
            _packets_end = &_ipd_packet->next;

            _ipd_packet = NULL;
        }
    }
    else if (size == 9 /* 0,CONNECT */
        && (cmd[0] >= '0' && cmd[0] <= '9')
        && (cmd[1] == ',')
        && (strcmp(&cmd[2], "CONNECT") == 0)) {

        _incoming_socket_status[cmd[0] - '0'] = ++_global_socket_counter;

        _signalingCallback(ESP8266_SOCKET_CONNECT, cmd[0] - '0');
    }
    else if (size == 8 /* 0,CLOSED */
        && (cmd[0] >= '0' && cmd[0] <= '9')
        && (cmd[1] == ',')
        && (strcmp(&cmd[2], "CLOSED") == 0)) {

        _incoming_socket_status[cmd[0] - '0'] = 0;

        _signalingCallback(ESP8266_SOCKET_CLOSE, cmd[0] - '0');
    }
    else if (cmd[0] == '+' && cmd[1] == 'I' && cmd[2] == 'P' && cmd[3] == 'D') {
        int id = cmd[5] - '0';

        // parse out the length param...
        size_t length_ix = 6;
        while (cmd[length_ix] != ':' && length_ix < size) length_ix++;
        char* temp_length_buff = (char*)calloc(length_ix - 7 + 1, 1);
        if (!temp_length_buff) return;
        memcpy(temp_length_buff, cmd + 7, length_ix - 7);
        int amount = atoi(temp_length_buff);

        // alloc a new packet (and store it in a global var. we'll get the data for this packet straight after this msg)
        _ipd_packet = (struct packet*)malloc(
                sizeof(struct packet) + amount);
        if (!_ipd_packet) {
            return;
        }

        _ipd_packet->id = id;
        _ipd_packet->len = amount;
        _ipd_packet->next = 0;

        _ipd_packet_data_ptr = (char*)(_ipd_packet + 1);

        size_t data_len = size - length_ix - 1;
        memcpy(_ipd_packet_data_ptr, cmd + length_ix + 1, data_len);
        _ipd_packet_data_ptr += data_len;

        // re-add the newline \r\n again...
        _ipd_packet_data_ptr[0] = '\r';
        _ipd_packet_data_ptr[1] = '\n';
        _ipd_packet_data_ptr += 2;
    }
    free(cmd);
}

void ESP8266::attach_rx(int c) {
    // store value in buffer
    rx_buffer[rx_ix] = c;
    rx_buffer[rx_ix + 1] = 0;

    if (rx_ix > 0 && c == '\n') {
        // got a whole command
        char* cmd = (char*)calloc(rx_ix, 1);
        memcpy(cmd, rx_buffer, rx_ix - 1);
        event_queue->call(callback(this, &ESP8266::process_command), cmd, rx_ix - 1);

        rx_ix = 0;
        return;
    }

    rx_ix++;
}
