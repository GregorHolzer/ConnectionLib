#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace connectionlib {

/** @brief Which concrete addressing scheme an Address holds. */
enum class AddressKind : uint8_t { Mac6, XBee64_16, LoRaNodeId16, Broadcast };

/** @brief XBee's two-part address: a 64-bit hardware address plus a 16-bit network address. */
struct XBeeAddressPair {
    uint64_t addr64;
    uint16_t addr16;
};

/**
 * @brief Technology-agnostic address, able to hold a 6-byte MAC (ESP-NOW), an XBee 64-bit+16-bit
 * pair (Zigbee), or a small software node ID (LoRa point-to-point, which has no native addressing).
 *
 * A tagged value type rather than std::variant, to avoid exception-throwing accessors on an
 * embedded target. Construct via the static factories; read back via asMac()/asXBee()/asLoRaNodeId(),
 * which return std::nullopt if the Address doesn't hold that kind.
 */
class Address {
  public:
    static Address mac(const std::array<uint8_t, 6> &mac);
    static Address xbee(uint64_t addr64, uint16_t addr16 = 0xFFFE);
    static Address loRaNode(uint16_t nodeId);

    /** @brief The well-known broadcast address for a given addressing scheme. */
    static Address broadcast(AddressKind forKind);

    [[nodiscard]] AddressKind kind() const;
    [[nodiscard]] std::optional<std::array<uint8_t, 6>> asMac() const;
    [[nodiscard]] std::optional<XBeeAddressPair> asXBee() const;
    [[nodiscard]] std::optional<uint16_t> asLoRaNodeId() const;

    bool operator==(const Address &other) const;
    bool operator!=(const Address &other) const;

  private:
    Address() = default;

    AddressKind kind_ = AddressKind::Broadcast;
    std::array<uint8_t, 8> bytes_{};   // holds MAC (6B) / XBee 64-bit addr / LoRa node id, kind-dependent
    uint16_t aux16_ = 0;               // XBee 16-bit network address only
};

}   // namespace connectionlib
