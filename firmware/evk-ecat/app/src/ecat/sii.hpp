#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace librmcs::firmware::ecat {

class Sii {
public:
    static constexpr auto image() {
        std::array<uint8_t, 2048> bytes{};
        put(bytes, 0, 0x0c80);
        put(bytes, 1, 0x6681);
        // Engineering identity: vendor 0, product "RMCS", revision 1, serial 1.
        put(bytes, 10, 0x4353);
        put(bytes, 11, 0x524d);
        put(bytes, 12, 1);
        put(bytes, 14, 1);
        put(bytes, 0x18, 0x1000);
        put(bytes, 0x19, 128);
        put(bytes, 0x1a, 0x1080);
        put(bytes, 0x1b, 128);
        put(bytes, 0x1c, 0x1000);
        put(bytes, 0x1d, 128);
        put(bytes, 0x1e, 0x1080);
        put(bytes, 0x1f, 128);
        put(bytes, 0x20, 4);  // CoE mailbox protocol.
        put(bytes, 0x3e, 15); // 2048 bytes, encoded in 128-byte units minus one.
        put(bytes, 0x3f, 1);
        std::size_t word = 0x40;
        put(bytes, word++, 30); // General category, SDO and PDO assignment support.
        put(bytes, word++, 16);
        bytes[word * 2 + 5] = 0x0b;
        word += 16;
        put(bytes, word++, 40); // FMMU category.
        put(bytes, word++, 2);
        put(bytes, word++, 0x0201);
        put(bytes, word++, 0x0003);
        put(bytes, word++, 41); // Sync manager category.
        put(bytes, word++, 16);
        const std::array<uint16_t, 4> starts{0x1000, 0x1080, 0x1100, 0x1e00};
        const std::array<uint16_t, 4> sizes{128, 128, 1039, 1039};
        const std::array<uint16_t, 4> controls{0x26, 0x22, 0x24, 0x20};
        for (std::size_t i = 0; i < 4; ++i) {
            put(bytes, word++, starts[i]);
            put(bytes, word++, sizes[i]);
            put(bytes, word++, controls[i]);
            put(bytes, word++, static_cast<uint16_t>(1U | ((i + 1U) << 8U)));
        }
        for (unsigned direction = 0; direction < 2; ++direction) {
            put(bytes, word++, static_cast<uint16_t>(50 + direction));
            put(bytes, word++, 140);
            put(bytes, word++, direction == 0 ? 0x1a00 : 0x1600);
            put(bytes, word++, static_cast<uint16_t>(34U | ((direction == 0 ? 3U : 2U) << 8U)));
            put(bytes, word++, 0);
            put(bytes, word++, 0);
            for (unsigned chunk = 0; chunk < 34; ++chunk) {
                put(bytes, word++, direction == 0 ? 0x6000 : 0x7000);
                put(bytes, word++, static_cast<uint16_t>(chunk + 1));
                const unsigned length = chunk == 33 ? 16 : 31;
                put(bytes, word++, static_cast<uint16_t>(0x0aU | ((length * 8U) << 8U)));
                put(bytes, word++, 0);
            }
        }
        put(bytes, word, 0xffff);
        uint8_t crc = 0xff;
        for (std::size_t i = 0; i < 14; ++i) {
            crc ^= bytes[i];
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = static_cast<uint8_t>((crc << 1U) ^ ((crc & 0x80U) ? 7U : 0U));
        }
        bytes[14] = crc;
        return bytes;
    }

private:
    static constexpr void put(std::array<uint8_t, 2048>& bytes, std::size_t word, uint16_t value) {
        bytes[word * 2] = static_cast<uint8_t>(value);
        bytes[word * 2 + 1] = static_cast<uint8_t>(value >> 8U);
    }
};

} // namespace librmcs::firmware::ecat
