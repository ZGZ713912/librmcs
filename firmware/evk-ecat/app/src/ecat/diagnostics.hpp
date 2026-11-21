#pragma once

#include <cstdint>

namespace librmcs::firmware::ecat {
class Diagnostics {
public:
    static constexpr uint8_t kCount = 13;

    inline static uint32_t protocol_errors = 0;
    inline static uint32_t backpressure = 0;
    inline static uint32_t rejected_device_requests = 0;
    inline static uint32_t uart_overruns = 0;
    inline static uint32_t can_receive_losses = 0;
    inline static uint32_t can_tx_submitted = 0;
    inline static uint32_t can_tx_completed = 0;
    inline static uint32_t can_rx_forwarded = 0;
    inline static uint32_t can_protocol_status = 0;
    inline static uint32_t can_error_counter = 0;
    inline static uint32_t can_interrupt_flags = 0;
    inline static uint32_t can_tx_fifo_status = 0;
    inline static uint32_t can_control = 0;
};
} // namespace librmcs::firmware::ecat
