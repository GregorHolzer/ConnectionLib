#include <unity.h>

#include <deque>
#include <numeric>
#include <vector>

#include "ConnectionLib/FragmentingTransport.hpp"

using namespace connectionlib;

void setUp() {}
void tearDown() {}

namespace {

constexpr size_t kMtu = 13;   // 3-byte header + 10 application bytes per packet

// Scriptable stand-in for a backend: records what is sent, replays what the test queues up.
class FakeTransport : public Transport {
  public:
    struct Sent {
        Address destination;
        std::vector<uint8_t> bytes;
    };

    bool locked = false;       // duty-cycle locked: send() rejects, timeUntilNextAllowedMs() != 0
    int lockAfterSends = -1;   // become locked once this many more sends have gone out
    int failSendNumber = -1;   // 0-based index of the accepted send that reports Failed
    std::vector<Sent> sent;
    std::deque<std::optional<ReceivedMessage>> incoming;   // nullopt = that listen() times out
    std::vector<uint32_t> listenTimeouts;

    bool setup() override { return true; }
    [[nodiscard]] int lastErrorCode() const override { return 0; }

    SendResult send(const Address &destination, const uint8_t *payload, size_t len) override {
        SendResult result;
        result.payloadBytes = len;
        if (len > kMtu) {
            result.status = SendStatus::PayloadTooLarge;
            return result;
        }
        if (locked) {
            result.status = SendStatus::DutyCycleExceeded;
            return result;
        }
        bool fail = static_cast<int>(sent.size()) == failSendNumber;
        sent.push_back({destination, std::vector<uint8_t>(payload, payload + len)});
        if (lockAfterSends > 0 && --lockAfterSends == 0) {
            locked = true;
        }
        result.totalBytes = len;
        result.acknowledged = true;
        result.status = fail ? SendStatus::Failed : SendStatus::Ok;
        return result;
    }

    std::optional<ReceivedMessage> listen(uint32_t timeoutMs) override {
        listenTimeouts.push_back(timeoutMs);
        if (incoming.empty()) {
            return std::nullopt;
        }
        std::optional<ReceivedMessage> next = incoming.front();
        incoming.pop_front();
        return next;
    }

    [[nodiscard]] Address broadcastAddress() const override { return Address::loRaNode(0xFFFF); }
    [[nodiscard]] size_t maxPayloadBytes() const override { return kMtu; }
    [[nodiscard]] const char *technologyName() const override { return "Fake"; }
    [[nodiscard]] float currentDutyCyclePercent() const override { return 0.0f; }
    [[nodiscard]] uint32_t timeUntilNextAllowedMs(size_t) const override { return locked ? 1000 : 0; }
};

const Address kGateway = Address::loRaNode(1);
const Address kNodeA = Address::loRaNode(10);
const Address kNodeB = Address::loRaNode(11);

std::vector<uint8_t> pattern(size_t len) {
    std::vector<uint8_t> bytes(len);
    std::iota(bytes.begin(), bytes.end(), static_cast<uint8_t>(1));
    return bytes;
}

ReceivedMessage packetFrom(const Address &source, const std::vector<uint8_t> &bytes) {
    ReceivedMessage packet;
    packet.source = source;
    packet.payload = bytes;
    packet.totalBytes = bytes.size();
    return packet;
}

// The packets a wrapped sender would put on the air for one message.
std::vector<ReceivedMessage> packetsFor(const Address &source, const std::vector<uint8_t> &payload) {
    FakeTransport air;
    FragmentingTransport sender(air);
    sender.send(kGateway, payload.data(), payload.size());
    std::vector<ReceivedMessage> packets;
    for (const auto &sent : air.sent) {
        packets.push_back(packetFrom(source, sent.bytes));
    }
    return packets;
}

void assertPayload(const std::vector<uint8_t> &expected, const std::optional<ReceivedMessage> &message) {
    TEST_ASSERT_TRUE(message.has_value());
    TEST_ASSERT_EQUAL_size_t(expected.size(), message->payload.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), message->payload.data(), expected.size());
}

}   // namespace

void test_small_payload_is_one_packet_with_header() {
    FakeTransport air;
    FragmentingTransport transport(air);
    auto payload = pattern(4);

    SendResult result = transport.send(kGateway, payload.data(), payload.size());

    TEST_ASSERT_TRUE(result.status == SendStatus::Ok);
    TEST_ASSERT_EQUAL_size_t(4, result.payloadBytes);
    TEST_ASSERT_EQUAL_size_t(7, result.totalBytes);
    TEST_ASSERT_TRUE(result.acknowledged);
    TEST_ASSERT_EQUAL_size_t(1, air.sent.size());
    TEST_ASSERT_EQUAL_size_t(7, air.sent[0].bytes.size());
    TEST_ASSERT_EQUAL_UINT8(0, air.sent[0].bytes[1]);   // fragIndex
    TEST_ASSERT_EQUAL_UINT8(1, air.sent[0].bytes[2]);   // fragCount

    FakeTransport rxAir;
    FragmentingTransport receiver(rxAir);
    rxAir.incoming.push_back(packetFrom(kNodeA, air.sent[0].bytes));
    assertPayload(payload, receiver.listen(100));
}

void test_large_payload_splits_and_reassembles_in_one_listen() {
    FakeTransport air;
    FragmentingTransport transport(air);
    auto payload = pattern(25);   // 10 + 10 + 5

    SendResult result = transport.send(kGateway, payload.data(), payload.size());

    TEST_ASSERT_TRUE(result.status == SendStatus::Ok);
    TEST_ASSERT_EQUAL_size_t(3, air.sent.size());
    TEST_ASSERT_EQUAL_size_t(25 + 3 * 3, result.totalBytes);
    for (const auto &sent : air.sent) {
        TEST_ASSERT_TRUE(sent.bytes.size() <= kMtu);
        TEST_ASSERT_TRUE(sent.destination == kGateway);
    }

    FakeTransport rxAir;
    FragmentingTransport receiver(rxAir, FragmentingConfig{1024, 4, 4, 777});
    for (const auto &sent : air.sent) {
        rxAir.incoming.push_back(packetFrom(kNodeA, sent.bytes));
    }
    auto message = receiver.listen(100);

    assertPayload(payload, message);
    TEST_ASSERT_TRUE(message->source == kNodeA);
    TEST_ASSERT_EQUAL_size_t(25 + 3 * 3, message->totalBytes);
    // Caller's timeout for the first packet, then fragmentTimeoutMs while the message is arriving.
    TEST_ASSERT_EQUAL_size_t(3, rxAir.listenTimeouts.size());
    TEST_ASSERT_EQUAL_UINT32(100, rxAir.listenTimeouts[0]);
    TEST_ASSERT_EQUAL_UINT32(777, rxAir.listenTimeouts[1]);
    TEST_ASSERT_EQUAL_UINT32(777, rxAir.listenTimeouts[2]);
}

void test_max_payload_bytes() {
    FakeTransport air;
    TEST_ASSERT_EQUAL_size_t(1024, FragmentingTransport(air).maxPayloadBytes());
    // 10 application bytes per packet, at most 255 packets.
    TEST_ASSERT_EQUAL_size_t(2550, FragmentingTransport(air, FragmentingConfig{4096, 4, 4, 5000}).maxPayloadBytes());
}

void test_too_large_payload_is_rejected_without_sending() {
    FakeTransport air;
    FragmentingTransport transport(air, FragmentingConfig{20, 4, 4, 5000});
    auto payload = pattern(21);

    SendResult result = transport.send(kGateway, payload.data(), payload.size());

    TEST_ASSERT_TRUE(result.status == SendStatus::PayloadTooLarge);
    TEST_ASSERT_EQUAL_size_t(0, air.sent.size());
}

void test_failed_fragment_stops_the_message() {
    FakeTransport air;
    air.failSendNumber = 1;
    FragmentingTransport transport(air);
    auto payload = pattern(25);

    SendResult result = transport.send(kGateway, payload.data(), payload.size());

    TEST_ASSERT_TRUE(result.status == SendStatus::Failed);
    TEST_ASSERT_FALSE(result.acknowledged);
    TEST_ASSERT_EQUAL_size_t(2, air.sent.size());   // third fragment never attempted
    TEST_ASSERT_EQUAL_size_t(0, transport.queuedMessages());
}

void test_locked_send_is_queued_and_flushed_in_order() {
    FakeTransport air;
    air.locked = true;
    FragmentingTransport transport(air);
    auto first = pattern(25);
    auto second = pattern(4);

    TEST_ASSERT_TRUE(transport.send(kGateway, first.data(), first.size()).status == SendStatus::Queued);
    TEST_ASSERT_TRUE(transport.send(kGateway, second.data(), second.size()).status == SendStatus::Queued);
    TEST_ASSERT_EQUAL_size_t(0, air.sent.size());
    TEST_ASSERT_EQUAL_size_t(2, transport.queuedMessages());

    transport.listen(0);   // still locked: nothing goes out
    TEST_ASSERT_EQUAL_size_t(0, air.sent.size());

    air.locked = false;
    transport.listen(0);

    TEST_ASSERT_EQUAL_size_t(0, transport.queuedMessages());
    TEST_ASSERT_EQUAL_size_t(4, air.sent.size());   // 3 fragments of first, then second
    TEST_ASSERT_EQUAL_UINT8(3, air.sent[0].bytes[2]);
    TEST_ASSERT_EQUAL_UINT8(1, air.sent[3].bytes[2]);
}

void test_send_stays_behind_queued_messages() {
    FakeTransport air;
    air.locked = true;
    FragmentingTransport transport(air);
    auto first = pattern(4);
    auto second = pattern(5);
    transport.send(kGateway, first.data(), first.size());

    air.locked = false;
    SendResult result = transport.send(kGateway, second.data(), second.size());

    TEST_ASSERT_TRUE(result.status == SendStatus::Ok);
    TEST_ASSERT_EQUAL_size_t(2, air.sent.size());
    TEST_ASSERT_EQUAL_size_t(3 + 4, air.sent[0].bytes.size());
    TEST_ASSERT_EQUAL_size_t(3 + 5, air.sent[1].bytes.size());
}

void test_lock_part_way_through_queues_the_rest() {
    FakeTransport air;
    air.lockAfterSends = 1;
    FragmentingTransport transport(air);
    auto payload = pattern(25);

    SendResult result = transport.send(kGateway, payload.data(), payload.size());

    TEST_ASSERT_TRUE(result.status == SendStatus::Queued);
    TEST_ASSERT_EQUAL_size_t(1, air.sent.size());
    TEST_ASSERT_EQUAL_size_t(1, transport.queuedMessages());

    air.locked = false;
    transport.listen(0);
    TEST_ASSERT_EQUAL_size_t(3, air.sent.size());
    TEST_ASSERT_EQUAL_size_t(0, transport.queuedMessages());

    FakeTransport rxAir;
    FragmentingTransport receiver(rxAir);
    for (const auto &sent : air.sent) {
        rxAir.incoming.push_back(packetFrom(kNodeA, sent.bytes));
    }
    assertPayload(payload, receiver.listen(100));
}

void test_full_or_disabled_queue_rejects() {
    auto payload = pattern(4);

    FakeTransport air;
    air.locked = true;
    FragmentingTransport oneSlot(air, FragmentingConfig{1024, 4, 1, 5000});
    TEST_ASSERT_TRUE(oneSlot.send(kGateway, payload.data(), payload.size()).status == SendStatus::Queued);
    TEST_ASSERT_TRUE(oneSlot.send(kGateway, payload.data(), payload.size()).status ==
                     SendStatus::DutyCycleExceeded);

    FakeTransport air2;
    air2.locked = true;
    FragmentingTransport noQueue(air2, FragmentingConfig{1024, 4, 0, 5000});
    TEST_ASSERT_TRUE(noQueue.send(kGateway, payload.data(), payload.size()).status ==
                     SendStatus::DutyCycleExceeded);
    TEST_ASSERT_EQUAL_size_t(0, noQueue.queuedMessages());
}

void test_failed_queued_message_is_dropped_and_next_still_sent() {
    FakeTransport air;
    air.locked = true;
    FragmentingTransport transport(air);
    auto first = pattern(25);
    auto second = pattern(4);
    transport.send(kGateway, first.data(), first.size());
    transport.send(kGateway, second.data(), second.size());

    air.locked = false;
    air.failSendNumber = 0;   // first fragment of the first message fails
    transport.listen(0);

    TEST_ASSERT_EQUAL_size_t(0, transport.queuedMessages());
    TEST_ASSERT_EQUAL_size_t(2, air.sent.size());
    TEST_ASSERT_EQUAL_size_t(3 + 4, air.sent[1].bytes.size());   // the second message
}

void test_missing_fragment_times_out_then_completes_later() {
    auto payload = pattern(25);
    auto packets = packetsFor(kNodeA, payload);
    FakeTransport air;
    FragmentingTransport receiver(air);

    air.incoming.push_back(packets[0]);
    air.incoming.push_back(std::nullopt);   // sender paused, e.g. duty-cycle locked
    TEST_ASSERT_FALSE(receiver.listen(100).has_value());

    air.incoming.push_back(packets[1]);
    air.incoming.push_back(packets[2]);
    assertPayload(payload, receiver.listen(100));
}

void test_dropped_middle_fragment_loses_only_that_message() {
    auto lost = packetsFor(kNodeA, pattern(25));
    auto next = pattern(12);
    FakeTransport air;
    FragmentingTransport receiver(air);

    air.incoming.push_back(lost[0]);
    air.incoming.push_back(lost[2]);   // fragment 1 never arrived
    TEST_ASSERT_FALSE(receiver.listen(100).has_value());

    for (const auto &packet : packetsFor(kNodeA, next)) {
        air.incoming.push_back(packet);
    }
    assertPayload(next, receiver.listen(100));
}

void test_duplicated_fragment_is_ignored() {
    auto payload = pattern(25);
    auto packets = packetsFor(kNodeA, payload);
    FakeTransport air;
    FragmentingTransport receiver(air);

    air.incoming.push_back(packets[0]);
    air.incoming.push_back(packets[1]);
    air.incoming.push_back(packets[1]);
    air.incoming.push_back(packets[2]);
    assertPayload(payload, receiver.listen(100));
    TEST_ASSERT_FALSE(receiver.listen(100).has_value());
}

void test_two_sources_interleaved() {
    auto fromA = pattern(25);
    auto fromB = pattern(18);
    auto a = packetsFor(kNodeA, fromA);
    auto b = packetsFor(kNodeB, fromB);
    FakeTransport air;
    FragmentingTransport receiver(air);
    air.incoming = {a[0], b[0], a[1], b[1], a[2]};

    auto first = receiver.listen(100);
    assertPayload(fromB, first);
    TEST_ASSERT_TRUE(first->source == kNodeB);
    auto second = receiver.listen(100);
    assertPayload(fromA, second);
    TEST_ASSERT_TRUE(second->source == kNodeA);
}

void test_malformed_packets_are_dropped() {
    FakeTransport air;
    FragmentingTransport receiver(air);
    air.incoming.push_back(packetFrom(kNodeA, {1, 0}));            // shorter than the header
    air.incoming.push_back(packetFrom(kNodeA, {1, 0, 0, 9}));      // fragCount 0
    air.incoming.push_back(packetFrom(kNodeA, {1, 2, 2, 9}));      // fragIndex >= fragCount
    air.incoming.push_back(packetFrom(kNodeA, {1, 1, 2, 9}));      // tail of a message never started

    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_FALSE(receiver.listen(100).has_value());
    }
}

void test_oversized_incoming_message_is_dropped() {
    auto packets = packetsFor(kNodeA, pattern(25));
    FakeTransport air;
    FragmentingTransport receiver(air, FragmentingConfig{20, 4, 4, 5000});
    for (const auto &packet : packets) {
        air.incoming.push_back(packet);
    }
    TEST_ASSERT_FALSE(receiver.listen(100).has_value());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_small_payload_is_one_packet_with_header);
    RUN_TEST(test_large_payload_splits_and_reassembles_in_one_listen);
    RUN_TEST(test_max_payload_bytes);
    RUN_TEST(test_too_large_payload_is_rejected_without_sending);
    RUN_TEST(test_failed_fragment_stops_the_message);
    RUN_TEST(test_locked_send_is_queued_and_flushed_in_order);
    RUN_TEST(test_send_stays_behind_queued_messages);
    RUN_TEST(test_lock_part_way_through_queues_the_rest);
    RUN_TEST(test_full_or_disabled_queue_rejects);
    RUN_TEST(test_failed_queued_message_is_dropped_and_next_still_sent);
    RUN_TEST(test_missing_fragment_times_out_then_completes_later);
    RUN_TEST(test_dropped_middle_fragment_loses_only_that_message);
    RUN_TEST(test_duplicated_fragment_is_ignored);
    RUN_TEST(test_two_sources_interleaved);
    RUN_TEST(test_malformed_packets_are_dropped);
    RUN_TEST(test_oversized_incoming_message_is_dropped);
    return UNITY_END();
}
