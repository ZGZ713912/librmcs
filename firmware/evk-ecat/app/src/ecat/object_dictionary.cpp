#include <array>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "esc_coe.h"
}

#include "diagnostics.hpp"
#include "process_frame.hpp"

namespace librmcs::firmware::ecat {

class ObjectDictionary {
public:
    static constexpr std::size_t kChunkSize = 31;
    static constexpr std::size_t kChunks = (ProcessFrame::kSize + kChunkSize - 1) / kChunkSize;

    static constexpr auto mapping(uint16_t index) {
        std::array<_objd, kChunks + 1> result{};
        result[0] = {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", kChunks, nullptr};
        for (std::size_t i = 0; i < kChunks; ++i) {
            const auto remaining = ProcessFrame::kSize - i * kChunkSize;
            const auto size = remaining < kChunkSize ? remaining : kChunkSize;
            result[i + 1] = {
                static_cast<uint16_t>(i + 1),
                DTYPE_UNSIGNED32,
                32,
                ATYPE_RO,
                "Mapping",
                (static_cast<uint32_t>(index) << 16) | (static_cast<uint32_t>(i + 1) << 8)
                    | static_cast<uint32_t>(size * 8),
                nullptr};
        }
        return result;
    }

    static constexpr auto payload(std::byte* storage, uint16_t access) {
        std::array<_objd, kChunks + 1> result{};
        result[0] = {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", kChunks, nullptr};
        for (std::size_t i = 0; i < kChunks; ++i) {
            const auto remaining = ProcessFrame::kSize - i * kChunkSize;
            const auto size = remaining < kChunkSize ? remaining : kChunkSize;
            result[i + 1] = {
                static_cast<uint16_t>(i + 1),
                DTYPE_OCTET_STRING,
                static_cast<uint16_t>(size * 8),
                access,
                "Protocol bytes",
                0,
                storage + i * kChunkSize};
        }
        return result;
    }

    inline static ProcessFrame::Storage input{};
    inline static ProcessFrame::Storage output{};
};

const auto receive_mapping = ObjectDictionary::mapping(0x7000);
const auto transmit_mapping = ObjectDictionary::mapping(0x6000);
const auto inputs = ObjectDictionary::payload(ObjectDictionary::input.data(), ATYPE_RO);
const auto outputs = ObjectDictionary::payload(ObjectDictionary::output.data(), ATYPE_RW);

const _objd device_type[] = {
    {0, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Device type", 0, nullptr}
};
const _objd identity[] = {
    {0,  DTYPE_UNSIGNED8,  8, ATYPE_RO,  "Entries",          4, nullptr},
    // Internal engineering identity, not an allocated production ETG Vendor ID.
    {1, DTYPE_UNSIGNED32, 32, ATYPE_RO,   "Vendor",          0, nullptr},
    {2, DTYPE_UNSIGNED32, 32, ATYPE_RO,  "Product", 0x524d4353, nullptr},
    {3, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Revision",          1, nullptr},
    {4, DTYPE_UNSIGNED32, 32, ATYPE_RO,   "Serial",          1, nullptr},
};
const _objd diagnostics[] = {
    {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", Diagnostics::kCount, nullptr},
    {1, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Protocol errors", 0, &Diagnostics::protocol_errors},
    {2, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Backpressure polls", 0, &Diagnostics::backpressure},
    {3, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Rejected requests", 0,
     &Diagnostics::rejected_device_requests},
    {4, DTYPE_UNSIGNED32, 32, ATYPE_RO, "UART overruns", 0, &Diagnostics::uart_overruns},
    {5, DTYPE_UNSIGNED32, 32, ATYPE_RO, "CAN RX losses", 0, &Diagnostics::can_receive_losses},
    {6, DTYPE_UNSIGNED32, 32, ATYPE_RO, "CAN TX submitted", 0, &Diagnostics::can_tx_submitted},
    {7, DTYPE_UNSIGNED32, 32, ATYPE_RO, "CAN TX completed", 0, &Diagnostics::can_tx_completed},
    {8, DTYPE_UNSIGNED32, 32, ATYPE_RO, "CAN RX forwarded", 0, &Diagnostics::can_rx_forwarded},
    {9, DTYPE_UNSIGNED32, 32, ATYPE_RO, "CAN error PSR", 0, &Diagnostics::can_protocol_status},
    {10, DTYPE_UNSIGNED32, 32, ATYPE_RO, "CAN error ECR", 0, &Diagnostics::can_error_counter},
    {11, DTYPE_UNSIGNED32, 32, ATYPE_RO, "CAN error flags", 0, &Diagnostics::can_interrupt_flags},
    {12, DTYPE_UNSIGNED32, 32, ATYPE_RO, "CAN blocked TXFQS", 0, &Diagnostics::can_tx_fifo_status},
    {13, DTYPE_UNSIGNED32, 32, ATYPE_RO, "CAN blocked CCCR", 0, &Diagnostics::can_control},
};
static_assert(std::size(diagnostics) == Diagnostics::kCount + 1);
const _objd communication[] = {
    {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", 4, nullptr},
    {1, DTYPE_UNSIGNED8, 8, ATYPE_RO,     "SM0", 1, nullptr},
    {2, DTYPE_UNSIGNED8, 8, ATYPE_RO,     "SM1", 2, nullptr},
    {3, DTYPE_UNSIGNED8, 8, ATYPE_RO,     "SM2", 3, nullptr},
    {4, DTYPE_UNSIGNED8, 8, ATYPE_RO,     "SM3", 4, nullptr},
};
const _objd receive_assignment[] = {
    {0,  DTYPE_UNSIGNED8,  8, ATYPE_RO, "Entries",      1, nullptr},
    {1, DTYPE_UNSIGNED16, 16, ATYPE_RO,   "RxPDO", 0x1600, nullptr},
};
const _objd transmit_assignment[] = {
    {0,  DTYPE_UNSIGNED8,  8, ATYPE_RO, "Entries",      1, nullptr},
    {1, DTYPE_UNSIGNED16, 16, ATYPE_RO,   "TxPDO", 0x1a00, nullptr},
};

} // namespace librmcs::firmware::ecat

extern "C" {
extern const _objectlist SDOobjects[] = {
    {0x1000, OTYPE_VAR, 0, 0, "Device type", librmcs::firmware::ecat::device_type},
    {0x1018, OTYPE_RECORD, 4, 0, "Identity", librmcs::firmware::ecat::identity},
    {0x1600, OTYPE_RECORD, librmcs::firmware::ecat::ObjectDictionary::kChunks, 0, "Receive mapping",
     librmcs::firmware::ecat::receive_mapping.data()},
    {0x1a00, OTYPE_RECORD, librmcs::firmware::ecat::ObjectDictionary::kChunks, 0,
     "Transmit mapping", librmcs::firmware::ecat::transmit_mapping.data()},
    {0x1c00, OTYPE_ARRAY, 4, 0, "SM communication", librmcs::firmware::ecat::communication},
    {0x1c12, OTYPE_ARRAY, 1, 0, "Rx assignment", librmcs::firmware::ecat::receive_assignment},
    {0x1c13, OTYPE_ARRAY, 1, 0, "Tx assignment", librmcs::firmware::ecat::transmit_assignment},
    {0x6000, OTYPE_RECORD, librmcs::firmware::ecat::ObjectDictionary::kChunks, 0, "Inputs",
     librmcs::firmware::ecat::inputs.data()},
    {0x7000, OTYPE_RECORD, librmcs::firmware::ecat::ObjectDictionary::kChunks, 0, "Outputs",
     librmcs::firmware::ecat::outputs.data()},
    {0xf100, OTYPE_RECORD, librmcs::firmware::ecat::Diagnostics::kCount, 0, "Diagnostics",
     librmcs::firmware::ecat::diagnostics},
    {0xffff, 0xff, 0xff, 0xff, nullptr, nullptr},
};
}
