#pragma once

#include <cstdint>
#include <string>

namespace librmcs::board {

// Linux only. The interface must be dedicated to one EtherCAT master.
struct EthercatOptions {
    std::string interface;
    uint32_t cycle_us = 1000;
    uint32_t receive_timeout_us = 2000;
    uint32_t admission_timeout_us = 20'000;
};

} // namespace librmcs::board
