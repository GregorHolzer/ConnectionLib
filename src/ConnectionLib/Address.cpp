#include "ConnectionLib/Address.hpp"

#include <algorithm>

namespace connectionlib {

Address Address::mac(const std::array<uint8_t, 6> &mac) {
    Address addr;
    addr.kind_ = AddressKind::Mac6;
    std::copy(mac.begin(), mac.end(), addr.bytes_.begin());
    return addr;
}

Address Address::xbee(uint64_t addr64, uint16_t addr16) {
    Address addr;
    addr.kind_ = AddressKind::XBee64_16;
    for (int i = 0; i < 8; ++i) {
        addr.bytes_[i] = static_cast<uint8_t>(addr64 >> (8 * (7 - i)));
    }
    addr.aux16_ = addr16;
    return addr;
}

Address Address::loRaNode(uint16_t nodeId) {
    Address addr;
    addr.kind_ = AddressKind::LoRaNodeId16;
    addr.bytes_[0] = static_cast<uint8_t>(nodeId >> 8);
    addr.bytes_[1] = static_cast<uint8_t>(nodeId & 0xFF);
    return addr;
}

Address Address::broadcast(AddressKind forKind) {
    switch (forKind) {
        case AddressKind::Mac6:
            return mac({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
        case AddressKind::XBee64_16:
            // Digi's well-known Zigbee broadcast address: 64-bit 0x000000000000FFFF, 16-bit 0xFFFE.
            return xbee(0x000000000000FFFFULL, 0xFFFE);
        case AddressKind::LoRaNodeId16:
            return loRaNode(0xFFFF);
        case AddressKind::Broadcast:
        default:
            return Address();
    }
}

AddressKind Address::kind() const {
    return kind_;
}

std::optional<std::array<uint8_t, 6>> Address::asMac() const {
    if (kind_ != AddressKind::Mac6) {
        return std::nullopt;
    }
    std::array<uint8_t, 6> mac{};
    std::copy(bytes_.begin(), bytes_.begin() + 6, mac.begin());
    return mac;
}

std::optional<XBeeAddressPair> Address::asXBee() const {
    if (kind_ != AddressKind::XBee64_16) {
        return std::nullopt;
    }
    uint64_t addr64 = 0;
    for (int i = 0; i < 8; ++i) {
        addr64 = (addr64 << 8) | bytes_[i];
    }
    return XBeeAddressPair{addr64, aux16_};
}

std::optional<uint16_t> Address::asLoRaNodeId() const {
    if (kind_ != AddressKind::LoRaNodeId16) {
        return std::nullopt;
    }
    return static_cast<uint16_t>((bytes_[0] << 8) | bytes_[1]);
}

bool Address::operator==(const Address &other) const {
    return kind_ == other.kind_ && bytes_ == other.bytes_ && aux16_ == other.aux16_;
}

bool Address::operator!=(const Address &other) const {
    return !(*this == other);
}

}   // namespace connectionlib
