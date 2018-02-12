/* ESP8266 implementation of NetworkInterfaceAPI
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

#include <cstring>
#include "ESP8266.h"
#include "ESP8266Interface.h"
#include "mbed_debug.h"
#include "nsapi_types.h"

// Various timeouts for different ESP8266 operations
#ifndef ESP8266_CONNECT_TIMEOUT
#define ESP8266_CONNECT_TIMEOUT 15000
#endif
#ifndef ESP8266_SEND_TIMEOUT
#define ESP8266_SEND_TIMEOUT    500
#endif
#ifndef ESP8266_RECV_TIMEOUT
#define ESP8266_RECV_TIMEOUT    500
#endif
#ifndef ESP8266_MISC_TIMEOUT
#define ESP8266_MISC_TIMEOUT    500
#endif

// Firmware version
#define ESP8266_VERSION 2

// ESP8266Interface implementation
ESP8266Interface::ESP8266Interface(PinName tx, PinName rx, bool debug)
    : _esp(tx, rx, debug),
      _initialized(false),
      _started(false)
{
    memset(_ids, 0, sizeof(_ids));
    memset(_cbs, 0, sizeof(_cbs));
    memset(ap_ssid, 0, sizeof(ap_ssid));
    memset(ap_pass, 0, sizeof(ap_pass));
    memset(_local_ports, 0, sizeof(_local_ports));
    ap_sec = NSAPI_SECURITY_UNKNOWN;

    _esp.attach(this, &ESP8266Interface::event);
}

int ESP8266Interface::connect(const char *ssid, const char *pass, nsapi_security_t security,
                                        uint8_t channel)
{
    if (channel != 0) {
        return NSAPI_ERROR_UNSUPPORTED;
    }

    int err = set_credentials(ssid, pass, security);
    if(err) {
        return err;
    }

    return connect();
}

int ESP8266Interface::connect()
{
    nsapi_error_t status;

    if (strlen(ap_ssid) == 0) {
        return NSAPI_ERROR_NO_SSID;
    }

    if (ap_sec != NSAPI_SECURITY_NONE) {
        if (strlen(ap_pass) < ESP8266_PASSPHRASE_MIN_LENGTH) {
            return NSAPI_ERROR_PARAMETER;
        }
    }

    status = _init();
    if(status != NSAPI_ERROR_OK) {
        return status;
    }

    if(get_ip_address()) {
        return NSAPI_ERROR_IS_CONNECTED;
    }

    status = _startup(ESP8266::WIFIMODE_STATION);
    if(status != NSAPI_ERROR_OK) {
        return status;
    }
    _started = true;

    _esp.setTimeout(ESP8266_MISC_TIMEOUT);
    if (!_esp.dhcp(true, 1)) {
        return NSAPI_ERROR_DHCP_FAILURE;
    }
    _esp.setTimeout(ESP8266_CONNECT_TIMEOUT);
    int connect_error = _esp.connect(ap_ssid, ap_pass);
    if (connect_error) {
        return connect_error;
    }

    if (!get_ip_address()) {
        return NSAPI_ERROR_DHCP_FAILURE;
    }

    return NSAPI_ERROR_OK;
}

int ESP8266Interface::set_credentials(const char *ssid, const char *pass, nsapi_security_t security)
{
    ap_sec = security;

    if (!ssid) {
        return NSAPI_ERROR_PARAMETER;
    }

    int ssid_length = strlen(ssid);

    if (ssid_length > 0
        && ssid_length <= ESP8266_SSID_MAX_LENGTH) {
        memset(ap_ssid, 0, sizeof(ap_ssid));
        strncpy(ap_ssid, ssid, sizeof(ap_ssid));
    } else {
        return NSAPI_ERROR_PARAMETER;
    }

    if (ap_sec != NSAPI_SECURITY_NONE) {

        if (!pass) {
            return NSAPI_ERROR_PARAMETER;
        }

        int pass_length = strlen(pass);
        if (pass_length >= ESP8266_PASSPHRASE_MIN_LENGTH
            && pass_length <= ESP8266_PASSPHRASE_MAX_LENGTH ) {
            memset(ap_pass, 0, sizeof(ap_pass));
            strncpy(ap_pass, pass, sizeof(ap_pass));
        } else {
            return NSAPI_ERROR_PARAMETER;
        }
    } else {
        memset(ap_pass, 0, sizeof(ap_pass));
    }

    return NSAPI_ERROR_OK;
}

int ESP8266Interface::set_channel(uint8_t channel)
{
    return NSAPI_ERROR_UNSUPPORTED;
}


int ESP8266Interface::disconnect()
{
    _started = false;
    _initialized = false;

    _esp.setTimeout(ESP8266_MISC_TIMEOUT);
    if (!_esp.disconnect()) {
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    return NSAPI_ERROR_OK;
}

const char *ESP8266Interface::get_ip_address()
{
    if(!_started) {
        return NULL;
    }

    _esp.setTimeout(ESP8266_CONNECT_TIMEOUT);
    const char *ip_buff = _esp.getIPAddress();
    if(!ip_buff || std::strcmp(ip_buff, "0.0.0.0") == 0) {
        return NULL;
    }

    return ip_buff;
}

const char *ESP8266Interface::get_mac_address()
{
    _esp.setTimeout(ESP8266_MISC_TIMEOUT);
    return _esp.getMACAddress();
}

const char *ESP8266Interface::get_gateway()
{
    if(!_started) {
        return NULL;
    }
    _esp.setTimeout(ESP8266_MISC_TIMEOUT);
    return _esp.getGateway();
}

const char *ESP8266Interface::get_netmask()
{
    if(!_started) {
        return NULL;
    }
    _esp.setTimeout(ESP8266_MISC_TIMEOUT);
    return _esp.getNetmask();
}

int8_t ESP8266Interface::get_rssi()
{
    if(!_started) {
        return 0;
    }
    _esp.setTimeout(ESP8266_CONNECT_TIMEOUT);
    return _esp.getRSSI();
}

int ESP8266Interface::scan(WiFiAccessPoint *res, unsigned count)
{
    nsapi_error_t status;

    status = _init();
    if(status != NSAPI_ERROR_OK) {
        return status;
    }

    status = _startup(ESP8266::WIFIMODE_STATION);
    if(status != NSAPI_ERROR_OK) {
        return status;
    }

    _esp.setTimeout(ESP8266_CONNECT_TIMEOUT);
    return _esp.scan(res, count);
}

bool ESP8266Interface::_get_firmware_ok()
{
    _esp.setTimeout(ESP8266_MISC_TIMEOUT);
    if (_esp.get_firmware_version() != ESP8266_VERSION) {
        debug("ESP8266: ERROR: Firmware incompatible with this driver.\
               \r\nUpdate to v%d - https://developer.mbed.org/teams/ESP8266/wiki/Firmware-Update\r\n",ESP8266_VERSION);
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    return NSAPI_ERROR_OK;
}

bool ESP8266Interface::_disable_default_softap()
{
    static int disabled = false;

    _esp.setTimeout(ESP8266_MISC_TIMEOUT);
    if (disabled || _esp.get_default_wifi_mode() == ESP8266::WIFIMODE_STATION) {
        disabled = true;
        return true;
    }
    if (_esp.set_default_wifi_mode(ESP8266::WIFIMODE_STATION)) {
        disabled = true;
        return true;
    }

    return false;
}

nsapi_error_t ESP8266Interface::_init(void)
{
    if (!_initialized) {
        _esp.setTimeout(ESP8266_CONNECT_TIMEOUT);
        if (!_esp.reset()) {
            return NSAPI_ERROR_DEVICE_ERROR;
        }
        if (_get_firmware_ok() != NSAPI_ERROR_OK) {
            return NSAPI_ERROR_DEVICE_ERROR;
        }
        if (_disable_default_softap() == false) {
            return NSAPI_ERROR_DEVICE_ERROR;
        }
        _initialized = true;
    }
    return NSAPI_ERROR_OK;
}

nsapi_error_t ESP8266Interface::_startup(const int8_t wifi_mode)
{
    if (!_started) {
        _esp.setTimeout(ESP8266_CONNECT_TIMEOUT);
        if (!_esp.startup(wifi_mode)) {
            return NSAPI_ERROR_DEVICE_ERROR;
        }
    }
    return NSAPI_ERROR_OK;
}

struct esp8266_socket {
    int id;
    nsapi_protocol_t proto;
    bool connected;
    SocketAddress addr;
    int keepalive; // TCP
};

int ESP8266Interface::socket_open(void **handle, nsapi_protocol_t proto)
{
    // Look for an unused socket
    int id = -1;
 
    for (int i = 0; i < ESP8266_SOCKET_COUNT; i++) {
        if (!_ids[i]) {
            id = i;
            _ids[i] = true;
            break;
        }
    }
 
    if (id == -1) {
        return NSAPI_ERROR_NO_SOCKET;
    }
    
    struct esp8266_socket *socket = new struct esp8266_socket;
    if (!socket) {
        return NSAPI_ERROR_NO_SOCKET;
    }
    
    socket->id = id;
    socket->proto = proto;
    socket->connected = false;
    socket->keepalive = 0;
    *handle = socket;
    return 0;
}

int ESP8266Interface::socket_close(void *handle)
{
    struct esp8266_socket *socket = (struct esp8266_socket *)handle;
    int err = 0;
    _esp.setTimeout(ESP8266_MISC_TIMEOUT);
 
    if (socket->connected && !_esp.close(socket->id)) {
        err = NSAPI_ERROR_DEVICE_ERROR;
    }

    socket->connected = false;
    _ids[socket->id] = false;
    _local_ports[socket->id] = 0;
    delete socket;
    return err;
}

int ESP8266Interface::socket_bind(void *handle, const SocketAddress &address)
{
    struct esp8266_socket *socket = (struct esp8266_socket *)handle;

    if (socket->proto == NSAPI_UDP) {
        if(address.get_addr().version != NSAPI_UNSPEC) {
            return NSAPI_ERROR_UNSUPPORTED;
        }

        for(int id = 0; id < ESP8266_SOCKET_COUNT; id++) {
            if(_local_ports[id] == address.get_port() && id != socket->id) { // Port already reserved by another socket
                return NSAPI_ERROR_PARAMETER;
            } else if (id == socket->id && socket->connected) {
                return NSAPI_ERROR_PARAMETER;
            }
        }
        _local_ports[socket->id] = address.get_port();
        return 0;
    }

    return NSAPI_ERROR_UNSUPPORTED;
}

int ESP8266Interface::socket_listen(void *handle, int backlog)
{
    return NSAPI_ERROR_UNSUPPORTED;
}

int ESP8266Interface::socket_connect(void *handle, const SocketAddress &addr)
{
    struct esp8266_socket *socket = (struct esp8266_socket *)handle;

    _esp.setTimeout(ESP8266_MISC_TIMEOUT);

    if (socket->proto == NSAPI_UDP) {
        if (!_esp.open_udp(socket->id, addr.get_ip_address(), addr.get_port(), _local_ports[socket->id])) {
            return NSAPI_ERROR_DEVICE_ERROR;
        }
    } else {
        if (!_esp.open_tcp(socket->id, addr.get_ip_address(), addr.get_port(), socket->keepalive)) {
            return NSAPI_ERROR_DEVICE_ERROR;
        }
    }

    socket->connected = true;
    return 0;
}
    
int ESP8266Interface::socket_accept(void *server, void **socket, SocketAddress *addr)
{
    return NSAPI_ERROR_UNSUPPORTED;
}

int ESP8266Interface::socket_send(void *handle, const void *data, unsigned size)
{
    nsapi_error_t status;
    struct esp8266_socket *socket = (struct esp8266_socket *)handle;
    _esp.setTimeout(ESP8266_SEND_TIMEOUT);
 
    status = _esp.send(socket->id, data, size);

    if (status != NSAPI_ERROR_OK) {
        return status;
    }
 
    return size;
}

int ESP8266Interface::socket_recv(void *handle, void *data, unsigned size)
{
    struct esp8266_socket *socket = (struct esp8266_socket *)handle;
    _esp.setTimeout(ESP8266_RECV_TIMEOUT);
 
    int32_t recv = _esp.recv(socket->id, data, size);
    if(recv == 0) {
        socket->connected = false; // No more data, ESP has closed the socket
    }
    if (recv < 0) {
        return NSAPI_ERROR_WOULD_BLOCK;
    }
 
    return recv;
}

int ESP8266Interface::socket_sendto(void *handle, const SocketAddress &addr, const void *data, unsigned size)
{
    struct esp8266_socket *socket = (struct esp8266_socket *)handle;

    if((strcmp(addr.get_ip_address(), "0.0.0.0") == 0) || !addr.get_port())  {
        return NSAPI_ERROR_DNS_FAILURE;
    }

    if (socket->connected && socket->addr != addr) {
        _esp.setTimeout(ESP8266_MISC_TIMEOUT);
        if (!_esp.close(socket->id)) {
            return NSAPI_ERROR_DEVICE_ERROR;
        }
        socket->connected = false;
    }

    if (!socket->connected) {
        int err = socket_connect(socket, addr);
        if (err < 0) {
            return err;
        }
        socket->addr = addr;
    }
    
    return socket_send(socket, data, size);
}

int ESP8266Interface::socket_recvfrom(void *handle, SocketAddress *addr, void *data, unsigned size)
{
    struct esp8266_socket *socket = (struct esp8266_socket *)handle;
    int ret = socket_recv(socket, data, size);
    if (ret >= 0 && addr) {
        *addr = socket->addr;
    }

    return ret;
}

void ESP8266Interface::socket_attach(void *handle, void (*callback)(void *), void *data)
{
    struct esp8266_socket *socket = (struct esp8266_socket *)handle;    
    _cbs[socket->id].callback = callback;
    _cbs[socket->id].data = data;
}

nsapi_error_t ESP8266Interface::setsockopt(nsapi_socket_t handle, int level,
        int optname, const void *optval, unsigned optlen)
{
    struct esp8266_socket *socket = (struct esp8266_socket *)handle;

    if (!optlen || !socket) {
        return NSAPI_ERROR_PARAMETER;
    }

    if (level == NSAPI_SOCKET && socket->proto == NSAPI_TCP) {
        switch (optname) {
            case NSAPI_KEEPALIVE: {
                if(socket->connected) {// ESP8266 limitation, keepalive needs to be given before connecting
                    return NSAPI_ERROR_UNSUPPORTED;
                }

                if (optlen == sizeof(int)) {
                    int secs = *(int *)optval;
                    if (secs  >= 0 && secs <= 7200) {
                        socket->keepalive = secs;
                        return NSAPI_ERROR_OK;
                    }
                }
                return NSAPI_ERROR_PARAMETER;
            }
        }
    }

    return NSAPI_ERROR_UNSUPPORTED;
}

nsapi_error_t ESP8266Interface::getsockopt(nsapi_socket_t handle, int level, int optname, void *optval, unsigned *optlen)
{
    struct esp8266_socket *socket = (struct esp8266_socket *)handle;

    if (!optval || !optlen || !socket) {
        return NSAPI_ERROR_PARAMETER;
    }

    if (level == NSAPI_SOCKET && socket->proto == NSAPI_TCP) {
        switch (optname) {
            case NSAPI_KEEPALIVE: {
                if(*optlen > sizeof(int)) {
                    *optlen = sizeof(int);
                }
                memcpy(optval, &(socket->keepalive), *optlen);
                return NSAPI_ERROR_OK;
            }
        }
    }

    return NSAPI_ERROR_UNSUPPORTED;
}


void ESP8266Interface::event() {
    for (int i = 0; i < ESP8266_SOCKET_COUNT; i++) {
        if (_cbs[i].callback) {
            _cbs[i].callback(_cbs[i].data);
        }
    }
}
