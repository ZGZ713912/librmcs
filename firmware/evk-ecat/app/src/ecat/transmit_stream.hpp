#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "process_frame.hpp"

namespace librmcs::firmware::ecat {

class TransmitStream {
public:
    void open(uint32_t epoch) {
        epoch_ = epoch;
        sequence_ = 0;
        size_ = 0;
        pending_ = false;
    }

    void close() { open(0); }

    [[nodiscard]] bool pending() const { return pending_; }

    [[nodiscard]] bool enqueue(std::span<const std::byte> payload) {
        if (epoch_ == 0 || pending_ || payload.empty() || payload.size() > payload_.size())
            return false;
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload_[i] = payload[i];
        size_ = payload.size();
        sequence_ = sequence_ == UINT32_MAX ? 1 : sequence_ + 1;
        pending_ = true;
        return true;
    }

    // Epoch validation belongs to the session owner before calling this method.
    void acknowledge(uint32_t acknowledgement) {
        if (pending_ && acknowledgement == sequence_) {
            pending_ = false;
            size_ = 0;
        }
    }

    [[nodiscard]] bool encode(ProcessFrame::Storage& frame, uint32_t acknowledgement) const {
        return ProcessFrame::encode(
            frame,
            {epoch_, pending_ ? sequence_ : 0, acknowledgement, std::span{payload_}.first(size_)});
    }

private:
    std::array<std::byte, ProcessFrame::kPayloadSize> payload_{};
    std::size_t size_ = 0;
    uint32_t epoch_ = 0;
    uint32_t sequence_ = 0;
    bool pending_ = false;
};

} // namespace librmcs::firmware::ecat
