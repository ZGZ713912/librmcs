#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/src/protocol/deserializer.hpp"
#include "core/src/protocol/serializer.hpp"
#include "diagnostics.hpp"
#include "receive_stream.hpp"
#include "transmit_stream.hpp"

namespace librmcs::firmware::ecat {

class Bridge
    : private core::protocol::DeserializeCallback
    , private core::protocol::SerializeBuffer {
public:
    class Devices {
    public:
        virtual ~Devices() = default;
        virtual bool can(data::DataId id, const data::CanDataView& value) = 0;
        virtual bool uart(data::DataId id, const data::UartDataView& value) = 0;
        virtual bool configure_uart(data::DataId id, const data::UartConfigView& value) = 0;
        virtual void stop() = 0;
        virtual bool ready(std::size_t) const { return true; }
    };

    explicit Bridge(Devices& devices)
        : devices_(devices)
        , deserializer_(*this)
        , serializer_(*this) {}

    void cycle(
        std::span<const std::byte> output, ProcessFrame::Storage& input, uint64_t now_us,
        bool operational) {
        now_us_ = now_us;
        if (active_ && now_us - refreshed_us_ >= 4'000'000)
            stop();
        if (!operational) {
            if (!suspended_ && active_)
                devices_.stop();
            suspended_ = true;
            // Keep epoch, nonce and transport ACKs across a short link outage.
            // Never dequeue peripheral output while suspended.
            return;
        }
        suspended_ = false;
        const auto frame = ProcessFrame::decode(output);
        if (!frame)
            return;
        // Only a standalone, valid librmcs Start can open a transport epoch.
        if (epoch_ != frame->epoch) {
            if (frame->epoch == retired_epoch_ || frame->sequence != 1 || frame->payload.size() != 6
                || frame->payload[0] != std::byte{0x00} || frame->payload[1] != std::byte{0x01})
                return;
            stop();
            epoch_ = frame->epoch;
            receiver_.open(epoch_);
            sender_.open(epoch_);
        }
        sender_.acknowledge(frame->acknowledgement);
        receiver_.receive(*frame, [this](std::span<const std::byte> payload) {
            if (buffer_.size() - used_ < 16 || !devices_.ready(payload.size())) {
                ++Diagnostics::backpressure;
                return false;
            }
            deserializer_.feed(payload);
            deserializer_.finish_transfer();
            return true;
        });
        if (!sender_.pending() && used_ != 0) {
            if (sender_.enqueue(std::span{buffer_}.first(used_)))
                used_ = 0;
        }
        (void)sender_.encode(input, receiver_.acknowledgement());
    }

    core::protocol::Serializer& serializer() { return serializer_; }
    [[nodiscard]] bool active() const { return active_ && !suspended_; }
    [[nodiscard]] std::size_t uplink_available() const { return buffer_.size() - used_; }

private:
    std::span<std::byte> allocate(std::size_t size) noexcept override {
        if (size > buffer_.size() - used_)
            return {};
        auto result = std::span{buffer_}.subspan(used_, size);
        used_ += size;
        return result;
    }

    void stop() {
        if (active_)
            devices_.stop();
        active_ = false;
        if (epoch_ != 0)
            retired_epoch_ = epoch_;
        epoch_ = 0;
        used_ = 0;
        receiver_.close();
        sender_.close();
        deserializer_.finish_transfer();
    }

    bool can_deserialized_callback(data::DataId id, const data::CanDataView& value) override {
        const bool accepted = !active_ || devices_.can(id, value);
        if (!accepted)
            ++Diagnostics::rejected_device_requests;
        return accepted;
    }
    bool uart_deserialized_callback(data::DataId id, const data::UartDataView& value) override {
        const bool accepted = !active_ || devices_.uart(id, value);
        if (!accepted)
            ++Diagnostics::rejected_device_requests;
        return accepted;
    }
    bool uart_config_deserialized_callback(
        data::DataId id, const data::UartConfigView& value) override {
        const bool accepted = !active_ || devices_.configure_uart(id, value);
        if (!accepted)
            ++Diagnostics::rejected_device_requests;
        return accepted;
    }
    void session_control_deserialized_callback(const data::SessionControlView& value) override {
        if (value.type == data::SessionType::kStart) {
            if (active_ && nonce_ != value.nonce)
                devices_.stop();
            nonce_ = value.nonce;
            active_ = true;
            refreshed_us_ = now_us_;
            (void)serializer_.write_session_control({data::SessionType::kStartAck, value.nonce});
        } else if (
            active_ && value.nonce == nonce_ && value.type == data::SessionType::kKeepalive) {
            refreshed_us_ = now_us_;
            (void)serializer_.write_session_control(
                {data::SessionType::kKeepaliveAck, value.nonce});
        }
    }
    bool gpio_digital_data_deserialized_callback(
        uint8_t, const data::GpioDigitalDataView&) override {
        return false;
    }
    bool gpio_analog_data_deserialized_callback(uint8_t, const data::GpioAnalogDataView&) override {
        return false;
    }
    bool gpio_digital_read_config_deserialized_callback(
        uint8_t, const data::GpioReadConfigView&) override {
        return false;
    }
    bool gpio_analog_read_config_deserialized_callback(
        uint8_t, const data::GpioReadConfigView&) override {
        return false;
    }
    void accelerometer_deserialized_callback(const data::ImuAccelerometerDataView&) override {}
    void gyroscope_deserialized_callback(const data::ImuGyroscopeDataView&) override {}
    void temperature_deserialized_callback(const data::ImuTemperatureDataView&) override {}
    void error_callback() override { ++Diagnostics::protocol_errors; }

    Devices& devices_;
    core::protocol::Deserializer deserializer_;
    core::protocol::Serializer serializer_;
    ReceiveStream receiver_;
    TransmitStream sender_;
    std::array<std::byte, ProcessFrame::kPayloadSize> buffer_{};
    std::size_t used_ = 0;
    uint64_t now_us_ = 0;
    uint64_t refreshed_us_ = 0;
    uint32_t epoch_ = 0;
    uint32_t retired_epoch_ = 0;
    uint32_t nonce_ = 0;
    bool active_ = false;
    bool suspended_ = false;
};

} // namespace librmcs::firmware::ecat
