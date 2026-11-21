#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace librmcs::firmware::ecat {

// One independent stop-and-wait stream per direction. Acknowledgement is carried
// even when the opposite direction has no payload. All wire fields are little-endian.
class ProcessFrame {
public:
    static constexpr std::size_t kPayloadSize = 1023;
    static constexpr std::size_t kHeaderSize = 16;
    static constexpr std::size_t kSize = kHeaderSize + kPayloadSize;
    using Storage = std::array<std::byte, kSize>;

    struct View {
        uint32_t epoch;
        uint32_t sequence;
        uint32_t acknowledgement;
        std::span<const std::byte> payload;
    };

    [[nodiscard]] static std::optional<View> decode(std::span<const std::byte> bytes) {
        if (bytes.size() != kSize || bytes[0] != std::byte{1} || bytes[1] != std::byte{0})
            return std::nullopt;
        const auto length = read(bytes.subspan(2, 2));
        if (length > kPayloadSize)
            return std::nullopt;
        const auto epoch = read(bytes.subspan(4, 4));
        const auto sequence = read(bytes.subspan(8, 4));
        if (epoch == 0 || (sequence == 0 && length != 0))
            return std::nullopt;
        return View{
            epoch, sequence, read(bytes.subspan(12, 4)), bytes.subspan(kHeaderSize, length)};
    }

    [[nodiscard]] static bool encode(Storage& bytes, const View& view) {
        if (view.epoch == 0 || view.payload.size() > kPayloadSize
            || (view.sequence == 0 && !view.payload.empty()))
            return false;
        // Build separately so a payload view into bytes remains valid during encoding.
        Storage encoded{};
        encoded[0] = std::byte{1};
        write(std::span{encoded}.subspan(2, 2), static_cast<uint32_t>(view.payload.size()));
        write(std::span{encoded}.subspan(4, 4), view.epoch);
        write(std::span{encoded}.subspan(8, 4), view.sequence);
        write(std::span{encoded}.subspan(12, 4), view.acknowledgement);
        for (std::size_t i = 0; i < view.payload.size(); ++i)
            encoded[kHeaderSize + i] = view.payload[i];
        bytes = encoded;
        return true;
    }

private:
    static uint32_t read(std::span<const std::byte> bytes) {
        uint32_t value = 0;
        for (std::size_t i = 0; i < bytes.size(); ++i)
            value |= std::to_integer<uint32_t>(bytes[i]) << (8U * i);
        return value;
    }

    static void write(std::span<std::byte> bytes, uint32_t value) {
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::byte>((value >> (8U * i)) & 0xffU);
    }
};

} // namespace librmcs::firmware::ecat
