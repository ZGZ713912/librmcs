#pragma once

#include <stdexcept>

#include <librmcs/board/ethercat_options.hpp>
#include <librmcs/data/datas.hpp>
#include <librmcs/protocol/handler.hpp>

namespace librmcs::board {

// HPM5E31 EVK: one classic CAN channel, no UART/GPIO/IMU capabilities.
// Callback storage must outlive the board. Callbacks run on a dedicated dispatch
// thread and may begin during construction. Do not destroy the board in a callback.
class EvkEcat final {
public:
    class Callback : public data::DataCallback {
    public:
        virtual void can_receive_callback(const data::CanDataView& value) { (void)value; }

        bool can_receive_callback(data::DataId id, const data::CanDataView& value) final {
            if (id != data::DataId::kCan0)
                return false;
            can_receive_callback(value);
            return true;
        }

        bool uart_receive_callback(data::DataId, const data::UartDataView&) final { return false; }
        bool gpio_digital_read_result_callback(uint8_t, const data::GpioDigitalDataView&) final {
            return false;
        }
        bool gpio_analog_read_result_callback(uint8_t, const data::GpioAnalogDataView&) final {
            return false;
        }
        void accelerometer_receive_callback(const data::ImuAccelerometerDataView&) final {}
        void gyroscope_receive_callback(const data::ImuGyroscopeDataView&) final {}
        void temperature_receive_callback(const data::ImuTemperatureDataView&) final {}
    };

    explicit EvkEcat(const EthercatOptions& options, Callback& callback)
        : handler_(options, callback) {}

    EvkEcat(const EvkEcat&) = delete;
    EvkEcat& operator=(const EvkEcat&) = delete;
    EvkEcat(EvkEcat&&) = delete;
    EvkEcat& operator=(EvkEcat&&) = delete;

    class PacketBuilder {
    public:
        explicit PacketBuilder(host::protocol::Handler& handler)
            : builder_(handler.start_transmit()) {}

        PacketBuilder& can_transmit(const data::CanDataView& value) {
            if (value.is_fdcan || value.can_data.size() > 8
                || (value.is_remote_transmission && !value.can_data.empty())
                || value.can_id > (value.is_extended_can_id ? 0x1fffffffU : 0x7ffU)
                || !builder_.write_can(data::DataId::kCan0, value))
                throw std::invalid_argument{"Invalid classic CAN frame"};
            return *this;
        }

    private:
        host::protocol::Handler::PacketBuilder builder_;
    };

    PacketBuilder start_transmit() { return PacketBuilder{handler_}; }

    // False after a latched transport fault or session failure. No command replay
    // or automatic rearming: destroy and recreate the board to reconnect.
    [[nodiscard]] bool connected() const noexcept { return handler_.connected(); }

private:
    host::protocol::Handler handler_;
};

} // namespace librmcs::board
