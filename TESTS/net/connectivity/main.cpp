#include "mbed.h"
#include "greentea-client/test_env.h"
#include "unity.h"
#include "utest.h"

#include "ESP8266Interface.h"

using namespace utest::v1;

#ifndef MBED_CFG_ESP8266_TX
#define MBED_CFG_ESP8266_TX D1
#endif

#ifndef MBED_CFG_ESP8266_RX
#define MBED_CFG_ESP8266_RX D0
#endif

#ifndef MBED_CFG_ESP8266_DEBUG
#define MBED_CFG_ESP8266_DEBUG false
#endif


// Bringing the network up and down
template <int COUNT>
void test_bring_up_down() {
    ESP8266Interface net(MBED_CFG_ESP8266_TX, MBED_CFG_ESP8266_RX, MBED_CFG_ESP8266_DEBUG);
    net.set_credentials(MBED_CFG_ESP8266_SSID, MBED_CFG_ESP8266_PASS);

    for (int i = 0; i < COUNT; i++) {
        int err = net.connect();
        TEST_ASSERT_EQUAL(0, err);

        printf("MBED: IP Address %s\r\n", net.get_ip_address());
        printf("MBED: Netmask %s\r\n", net.get_netmask());
        printf("MBED: Gateway %s\r\n", net.get_gateway());
        TEST_ASSERT(net.get_ip_address());
        TEST_ASSERT(net.get_netmask());
        TEST_ASSERT(net.get_gateway());

        UDPSocket udp;
        err = udp.open(&net);
        TEST_ASSERT_EQUAL(0, err);
        err = udp.close();
        TEST_ASSERT_EQUAL(0, err);

        TCPSocket tcp;
        err = tcp.open(&net);
        TEST_ASSERT_EQUAL(0, err);
        err = tcp.close();
        TEST_ASSERT_EQUAL(0, err);

        err = net.disconnect();
        TEST_ASSERT_EQUAL(0, err);
    }
}


// Test setup
utest::v1::status_t test_setup(const size_t number_of_cases) {
    char uuid[48] = {0};
    GREENTEA_SETUP_UUID(120, "default_auto", uuid, sizeof(uuid));

    // create mac address based on uuid
    uint64_t mac = 0;
    for (int i = 0; i < sizeof(uuid); i++) {
        mac += uuid[i];
    }
    mbed_set_mac_address((const char*)mac, /*coerce control bits*/ 1);

    return verbose_test_setup_handler(number_of_cases);
}

Case cases[] = {
    Case("Bringing the network up and down", test_bring_up_down<1>),
    Case("Bringing the network up and down twice", test_bring_up_down<2>),
};

Specification specification(test_setup, cases);

int main() {
    return !Harness::run(specification);
}
