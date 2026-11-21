#pragma once

#include <array>
#include <cstring>

#include "board.h"
#include "ecat/bridge.hpp"
#include "hpm_gpio_drv.h"
#include "hpm_mcan_drv.h"
#include "hpm_uart_drv.h"

namespace librmcs::firmware::ecat {

class EvkDevices : public Bridge::Devices {
public:
    bool init() {
        board_init_can(HPM_MCAN1);
        // Drive the J7 transceiver's active-high STB pin low in normal operation.
        gpio_set_pin_output_with_initial(HPM_GPIO0, GPIO_DI_GPIOE, 3, 0);
        HPM_IOC->PAD[IOC_PAD_PE03].FUNC_CTL = IOC_PE03_FUNC_CTL_GPIO_E_03;
        const auto can_clock = board_init_can_clock(HPM_MCAN1);
        const mcan_msg_buf_attr_t memory{
            reinterpret_cast<uintptr_t>(can_memory_), sizeof(can_memory_)};
        if (mcan_set_msg_buf_attr(HPM_MCAN1, &memory) != status_success)
            return false;
        mcan_config_t can_config;
        mcan_get_default_config(HPM_MCAN1, &can_config);
        can_config.baudrate = 1'000'000;
        can_config.mode = mcan_mode_normal;
        can_config.enable_canfd = false;
        can_config.disable_auto_retransmission = true;
        can_config.ram_config.txbuf_dedicated_txbuf_elem_count = 0;
        can_config.ram_config.txbuf_fifo_or_queue_elem_count = MCAN_TXBUF_SIZE_CAN_DEFAULT;
        can_config.ram_config.txfifo_or_txqueue_mode = MCAN_TXBUF_OPERATION_MODE_FIFO;
        can_config.ram_config.enable_tx_evt_fifo = true;
        can_config.ram_config.tx_evt_fifo_elem_count = MCAN_TXBUF_SIZE_CAN_DEFAULT;
        can_config.ram_config.tx_evt_fifo_watermark = 1;
        if (mcan_init(HPM_MCAN1, &can_config, can_clock) != status_success)
            return false;
        board_init_uart(HPM_UART4);
        uart_clock_ = board_init_uart_clock(HPM_UART4);
        uart_config_t uart_config;
        uart_default_config(HPM_UART4, &uart_config);
        uart_config.src_freq_in_hz = uart_clock_;
        uart_config.baudrate = 115200;
        uart_config.fifo_enable = true;
        const bool initialized = uart_init(HPM_UART4, &uart_config) == status_success;
        return initialized;
    }

    bool can(data::DataId id, const data::CanDataView& value) override {
        if (id != data::DataId::kCan0 || value.can_data.size() > 8 || value.is_fdcan)
            return false;
        mcan_tx_frame_t frame{};
        frame.use_ext_id = value.is_extended_can_id;
        if (frame.use_ext_id)
            frame.ext_id = value.can_id;
        else
            frame.std_id = value.can_id;
        frame.rtr = value.is_remote_transmission;
        frame.dlc = value.can_data.size();
        frame.event_fifo_control = true;
        if (!value.can_data.empty())
            std::memcpy(frame.data_8, value.can_data.data(), value.can_data.size());
        if (can_size_ == can_tx_.size())
            return false;
        can_tx_[(can_head_ + can_size_) % can_tx_.size()] = frame;
        ++can_size_;
        return true;
    }

    bool uart(data::DataId id, const data::UartDataView& value) override {
        if (id != data::DataId::kUart0 || value.uart_data.size() > tx_.size() - tx_size_)
            return false;
        for (const auto byte : value.uart_data) {
            tx_[(tx_head_ + tx_size_) % tx_.size()] = byte;
            ++tx_size_;
        }
        return true;
    }

    bool configure_uart(data::DataId id, const data::UartConfigView& value) override {
        if (id != data::DataId::kUart0Config || !value.baudrate || *value.baudrate == 0)
            return false;
        tx_size_ = 0;
        return uart_set_baudrate(HPM_UART4, *value.baudrate, uart_clock_) == status_success;
    }

    void stop() override {
        can_size_ = 0;
        tx_size_ = 0;
        rx_size_ = 0;
        HPM_MCAN1->TXBCR = 0xffffffffU;
        // Remove telemetry from the old session before admitting a new one.
        for (unsigned i = 0; i < 64; ++i) {
            mcan_rx_message_t discarded;
            if (mcan_read_rxfifo(HPM_MCAN1, 0, &discarded) != status_success)
                break;
        }
        // Discard UART FIFO bytes that have not reached the wire yet.
        HPM_UART4->FCR = UART_FCR_FIFOE_MASK | UART_FCR_TFIFORST_MASK | UART_FCR_RFIFORST_MASK;
    }

    bool ready(std::size_t protocol_bytes) const override {
        // A classic CAN field occupies at least three protocol bytes. Reserving
        // the worst case before parsing avoids partially executing/replaying a PDO.
        return tx_.size() - tx_size_ >= protocol_bytes
            && can_tx_.size() - can_size_ >= protocol_bytes / 3 + 1;
    }

    void poll(Bridge& bridge, uint64_t now_us) {
        if (HPM_UART4->LSR & UART_LSR_OE_MASK)
            ++Diagnostics::uart_overruns;
        poll_can_transmit();
        if (tx_size_ && (HPM_UART4->LSR & UART_LSR_THRE_MASK)) {
            HPM_UART4->THR = std::to_integer<uint8_t>(tx_[tx_head_]);
            tx_head_ = (tx_head_ + 1) % tx_.size();
            --tx_size_;
        }
        for (unsigned count = 0; count < 64 && (HPM_UART4->LSR & UART_LSR_DR_MASK)
                                 && (!bridge.active() || rx_size_ < rx_.size());
             ++count) {
            const auto byte = static_cast<std::byte>(uart_read_byte(HPM_UART4));
            if (bridge.active() && rx_size_ < rx_.size())
                rx_[rx_size_++] = byte;
            last_rx_us_ = now_us;
        }
        if (rx_size_ && (rx_size_ == rx_.size() || now_us - last_rx_us_ >= 1000)) {
            if (!bridge.active()
                || bridge.serializer().write_uart(
                       data::DataId::kUart0, {.uart_data = std::span{rx_}.first(rx_size_),
                                              .idle_delimited = now_us - last_rx_us_ >= 1000})
                       == core::protocol::Serializer::SerializeResult::kSuccess)
                rx_size_ = 0;
        }
        for (unsigned count = 0; count < 16; ++count) {
            if (bridge.active() && bridge.uplink_available() < 16)
                break;
            mcan_rx_message_t frame;
            if (mcan_read_rxfifo(HPM_MCAN1, 0, &frame) != status_success)
                break;
            if (!bridge.active() || frame.dlc > 8)
                continue;
            data::CanDataView value{
                .can_id = frame.use_ext_id ? frame.ext_id : frame.std_id,
                .can_data =
                    {reinterpret_cast<const std::byte*>(frame.data_8), frame.rtr ? 0U : frame.dlc},
                .is_extended_can_id = static_cast<bool>(frame.use_ext_id),
                .is_remote_transmission = static_cast<bool>(frame.rtr)
            };
            if (bridge.serializer().write_can(data::DataId::kCan0, value)
                == core::protocol::Serializer::SerializeResult::kSuccess)
                ++Diagnostics::can_rx_forwarded;
        }
    }

private:
    void poll_can_transmit() {
        constexpr uint32_t error_flags = MCAN_EVENT_ERROR | MCAN_INT_MSG_RAM_ACCESS_FAILURE
                                       | MCAN_INT_WATCHDOG_INT | MCAN_INT_TX_EVT_FIFO_EVT_LOST;
        const uint32_t flags = mcan_get_interrupt_flags(HPM_MCAN1);
        if (flags & MCAN_INT_RXFIFO0_MSG_LOST)
            ++Diagnostics::can_receive_losses;
        if (flags & error_flags) {
            Diagnostics::can_interrupt_flags |= flags & error_flags;
            Diagnostics::can_protocol_status = HPM_MCAN1->PSR;
            Diagnostics::can_error_counter = HPM_MCAN1->ECR;
        }
        const uint32_t handled_flags = flags & (error_flags | MCAN_INT_RXFIFO0_MSG_LOST);
        if (handled_flags)
            mcan_clear_interrupt_flags(HPM_MCAN1, handled_flags);

        for (unsigned count = 0; count < 16; ++count) {
            mcan_tx_event_fifo_elem_t event;
            if (mcan_read_tx_evt_fifo(HPM_MCAN1, &event) != status_success)
                break;
            ++Diagnostics::can_tx_completed;
        }
        if (!can_size_)
            return;
        const auto status =
            mcan_transmit_via_txfifo_nonblocking(HPM_MCAN1, &can_tx_[can_head_], nullptr);
        if (status != status_success) {
            Diagnostics::can_tx_fifo_status = HPM_MCAN1->TXFQS;
            Diagnostics::can_control = HPM_MCAN1->CCCR;
            return;
        }
        ++Diagnostics::can_tx_submitted;
        can_head_ = (can_head_ + 1) % can_tx_.size();
        --can_size_;
    }

    std::array<mcan_tx_frame_t, 512> can_tx_{};
    std::size_t can_head_ = 0;
    std::size_t can_size_ = 0;
    inline static uint32_t can_memory_[2048] __attribute__((section(".ahb_sram"), aligned(8)));
    std::array<std::byte, 4096> tx_{};
    std::array<std::byte, 256> rx_{};
    std::size_t tx_head_ = 0;
    std::size_t tx_size_ = 0;
    std::size_t rx_size_ = 0;
    uint32_t uart_clock_ = 0;
    uint64_t last_rx_us_ = 0;
};

} // namespace librmcs::firmware::ecat
