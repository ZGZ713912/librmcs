#pragma once

#include <cstdint>

#include "process_frame.hpp"

namespace librmcs::firmware::ecat {

// The caller must explicitly open a stream during session establishment. A PDO
// cannot change epochs by itself, otherwise a stale frame could reopen a session.
class ReceiveStream {
public:
    enum class Result { kRejected, kIdle, kDuplicate, kDelivered, kBackpressure };

    void open(uint32_t epoch) {
        epoch_ = epoch;
        acknowledgement_ = 0;
    }

    void close() { open(0); }

    [[nodiscard]] uint32_t acknowledgement() const { return acknowledgement_; }

    template <typename Consumer>
    Result receive(const ProcessFrame::View& frame, Consumer&& consume) {
        if (epoch_ == 0 || frame.epoch != epoch_)
            return Result::kRejected;
        if (frame.sequence == 0)
            return frame.payload.empty() ? Result::kIdle : Result::kRejected;
        if (frame.sequence == acknowledgement_)
            return Result::kDuplicate;
        // Sequence zero is reserved for idle. Wraparound advances UINT32_MAX to 1.
        const uint32_t next = acknowledgement_ == UINT32_MAX ? 1 : acknowledgement_ + 1;
        if (frame.sequence != next || frame.payload.size() > ProcessFrame::kPayloadSize)
            return Result::kRejected;
        if (!consume(frame.payload))
            return Result::kBackpressure;
        acknowledgement_ = frame.sequence;
        return Result::kDelivered;
    }

private:
    uint32_t epoch_ = 0;
    uint32_t acknowledgement_ = 0;
};

} // namespace librmcs::firmware::ecat
