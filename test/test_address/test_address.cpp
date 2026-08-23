#include <unity.h>

#include "ConnectionLib/Address.hpp"

using namespace connectionlib;

void setUp() {}
void tearDown() {}

void test_mac_roundtrip() {
    std::array<uint8_t, 6> mac = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    Address addr = Address::mac(mac);

    TEST_ASSERT_TRUE(addr.kind() == AddressKind::Mac6);
    auto readBack = addr.asMac();
    TEST_ASSERT_TRUE(readBack.has_value());
    TEST_ASSERT_TRUE(*readBack == mac);
    TEST_ASSERT_FALSE(addr.asXBee().has_value());
    TEST_ASSERT_FALSE(addr.asLoRaNodeId().has_value());
}

void test_xbee_roundtrip() {
    Address addr = Address::xbee(0x0013A20012345678ULL, 0x1234);

    TEST_ASSERT_TRUE(addr.kind() == AddressKind::XBee64_16);
    auto readBack = addr.asXBee();
    TEST_ASSERT_TRUE(readBack.has_value());
    TEST_ASSERT_EQUAL_UINT64(0x0013A20012345678ULL, readBack->addr64);
    TEST_ASSERT_EQUAL_UINT16(0x1234, readBack->addr16);
}

void test_xbee_default_addr16() {
    Address addr = Address::xbee(0x0013A20012345678ULL);
    auto readBack = addr.asXBee();
    TEST_ASSERT_TRUE(readBack.has_value());
    TEST_ASSERT_EQUAL_UINT16(0xFFFE, readBack->addr16);
}

void test_lora_node_roundtrip() {
    Address addr = Address::loRaNode(42);

    TEST_ASSERT_TRUE(addr.kind() == AddressKind::LoRaNodeId16);
    auto readBack = addr.asLoRaNodeId();
    TEST_ASSERT_TRUE(readBack.has_value());
    TEST_ASSERT_EQUAL_UINT16(42, *readBack);
}

void test_broadcast_mac() {
    Address addr = Address::broadcast(AddressKind::Mac6);
    auto mac = addr.asMac();
    TEST_ASSERT_TRUE(mac.has_value());
    for (uint8_t b : *mac) {
        TEST_ASSERT_EQUAL_UINT8(0xFF, b);
    }
}

void test_broadcast_xbee() {
    Address addr = Address::broadcast(AddressKind::XBee64_16);
    auto xbee = addr.asXBee();
    TEST_ASSERT_TRUE(xbee.has_value());
    TEST_ASSERT_EQUAL_UINT64(0x000000000000FFFFULL, xbee->addr64);
    TEST_ASSERT_EQUAL_UINT16(0xFFFE, xbee->addr16);
}

void test_broadcast_lora() {
    Address addr = Address::broadcast(AddressKind::LoRaNodeId16);
    auto nodeId = addr.asLoRaNodeId();
    TEST_ASSERT_TRUE(nodeId.has_value());
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, *nodeId);
}

void test_equality() {
    Address a = Address::mac({0x01, 0x02, 0x03, 0x04, 0x05, 0x06});
    Address b = Address::mac({0x01, 0x02, 0x03, 0x04, 0x05, 0x06});
    Address c = Address::mac({0x01, 0x02, 0x03, 0x04, 0x05, 0x07});

    TEST_ASSERT_TRUE(a == b);
    TEST_ASSERT_FALSE(a == c);
    TEST_ASSERT_TRUE(a != c);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_mac_roundtrip);
    RUN_TEST(test_xbee_roundtrip);
    RUN_TEST(test_xbee_default_addr16);
    RUN_TEST(test_lora_node_roundtrip);
    RUN_TEST(test_broadcast_mac);
    RUN_TEST(test_broadcast_xbee);
    RUN_TEST(test_broadcast_lora);
    RUN_TEST(test_equality);
    return UNITY_END();
}
