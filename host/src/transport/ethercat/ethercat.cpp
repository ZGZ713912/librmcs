#include <stdexcept>

#include "host/src/transport/transport.hpp"

#if defined(LIBRMCS_ENABLE_ETHERCAT)

# include <algorithm>
# include <array>
# include <atomic>
# include <cerrno>
# include <chrono>
# include <condition_variable>
# include <cstring>
# include <mutex>
# include <random>
# include <thread>
# include <utility>

# include <poll.h>
# include <sys/eventfd.h>
# include <sys/timerfd.h>
# include <unistd.h>

extern "C" {
# include <ethercat.h>
}

# include "host/src/logging/logging.hpp"

namespace librmcs::host::transport::ethercat {

class EthercatTransport final : public Transport {
public:
    explicit EthercatTransport(const board::EthercatOptions& options)
        : options_(options) {
        if (options.interface.empty() || options.cycle_us < 100 || options.cycle_us > 10'000
            || options.receive_timeout_us == 0 || options.receive_timeout_us > 10'000
            || options.admission_timeout_us < options.cycle_us
            || options.admission_timeout_us > 100'000)
            throw std::invalid_argument{"Invalid EtherCAT interface or timing options"};
        initialize_context();
        std::random_device random;
        epoch_ = random();
        if (epoch_ == 0)
            epoch_ = 1;
        try {
            if (!ecx_init(&context_, options.interface.c_str()))
                throw std::runtime_error{
                    "Cannot open EtherCAT interface (NET_RAW/NET_ADMIN required)"};
            opened_ = true;
            if (ecx_config_init(&context_, FALSE) != 1 || slaves_[1].eep_man != 0
                || slaves_[1].eep_id != 0x524d4353U)
                throw std::runtime_error{"Expected exactly one RMCS EVK EtherCAT slave"};
            // Bound the IO map before SOEM writes into it. This transport accepts
            // only the fixed engineering SII used by the current EVK firmware.
            if (slaves_[1].SM[2].SMlength != kFrameSize || slaves_[1].SM[3].SMlength != kFrameSize)
                throw std::runtime_error{"Unexpected EVK sync manager sizes"};
            if (ecx_config_map_group(&context_, iomap_.data(), 0) != 2 * kFrameSize
                || slaves_[1].Obytes != kFrameSize || slaves_[1].Ibytes != kFrameSize)
                throw std::runtime_error{"Unexpected EVK PDO mapping"};
            if (ecx_statecheck(&context_, 0, EC_STATE_SAFE_OP, 500'000) != EC_STATE_SAFE_OP)
                throw std::runtime_error{"EVK did not reach SAFEOP"};
            expected_wkc_ = groups_[0].outputsWKC * 2 + groups_[0].inputsWKC;
            std::memset(slaves_[1].outputs, 0, kFrameSize);
            ecx_send_processdata(&context_);
            ecx_receive_processdata(&context_, options.receive_timeout_us);
            slaves_[0].state = EC_STATE_OPERATIONAL;
            ecx_writestate(&context_, 0);
            if (ecx_statecheck(&context_, 0, EC_STATE_OPERATIONAL, 500'000) != EC_STATE_OPERATIONAL)
                throw std::runtime_error{"EVK did not reach OP"};
            timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
            wake_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (timer_ < 0 || wake_ < 0)
                throw std::runtime_error{"Cannot create EtherCAT event descriptors"};
        } catch (...) {
            close();
            throw;
        }
    }

    ~EthercatTransport() override {
        healthy_.store(false);
        stopping_.store(true);
        notify_worker();
        received_cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
        if (dispatcher_.joinable())
            dispatcher_.join();
        close();
    }

    std::unique_ptr<TransportBuffer> acquire_transmit_buffer() noexcept override {
        if (!healthy_.load())
            return nullptr;
        try {
            return std::make_unique<Buffer>(this);
        } catch (...) {
            return nullptr;
        }
    }

    void release_transmit_buffer(std::unique_ptr<TransportBuffer> buffer) override {
        if (!buffer || static_cast<Buffer&>(*buffer).owner != this)
            fault("Invalid transmit buffer owner");
    }

    void transmit(std::unique_ptr<TransportBuffer> buffer, size_t size) override {
        if (!buffer || static_cast<Buffer&>(*buffer).owner != this || size == 0
            || size > kPayloadSize) {
            fault("Invalid EtherCAT transmit buffer");
            return;
        }
        bool overflow = false;
        {
            const std::scoped_lock lock{transmit_mutex_};
            if (!healthy_.load())
                return;
            if (transmit_count_ == kQueueSize) {
                overflow = true;
            } else {
                Packet packet;
                packet.size = size;
                packet.created = Clock::now();
                std::copy_n(buffer->data().begin(), size, packet.bytes.begin());
                transmit_queue_[(transmit_head_ + transmit_count_) % kQueueSize] = packet;
                ++transmit_count_;
            }
        }
        if (overflow)
            fault("EtherCAT transmit queue full; output inhibited");
    }

    void receive(std::function<void(std::span<const std::byte>)> callback) override {
        if (started_ || !callback)
            throw std::logic_error{"EtherCAT receive must be started exactly once"};
        started_ = true;
        callback_ = std::move(callback);
        itimerspec schedule{};
        schedule.it_interval.tv_nsec = (options_.cycle_us % 1'000'000) * 1000;
        schedule.it_interval.tv_sec = options_.cycle_us / 1'000'000;
        schedule.it_value = schedule.it_interval;
        if (timerfd_settime(timer_, 0, &schedule, nullptr) < 0)
            throw std::runtime_error{"Cannot arm EtherCAT cycle timer"};
        try {
            dispatcher_ = std::thread{[this] { dispatch(); }};
            worker_ = std::thread{[this] { run(); }};
        } catch (...) {
            stopping_.store(true);
            received_cv_.notify_all();
            if (dispatcher_.joinable())
                dispatcher_.join();
            throw;
        }
    }

    [[nodiscard]] bool connected() const noexcept override { return healthy_.load(); }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t kPayloadSize = 1023;
    static constexpr size_t kFrameSize = 16 + kPayloadSize;
    static constexpr size_t kQueueSize = 32;

    class Buffer final : public TransportBuffer {
    public:
        explicit Buffer(const EthercatTransport* transport)
            : owner(transport) {}
        BufferSpanType data() const noexcept override { return bytes_; }
        const EthercatTransport* owner;

    private:
        mutable std::array<std::byte, core::protocol::kProtocolBufferSize> bytes_{};
    };

    struct Packet {
        std::array<std::byte, kPayloadSize> bytes{};
        size_t size = 0;
        Clock::time_point created{};
    };

    void initialize_context() {
        context_.port = &port_;
        context_.slavelist = slaves_.data();
        context_.slavecount = &slave_count_;
        context_.maxslave = slaves_.size();
        context_.grouplist = groups_.data();
        context_.maxgroup = groups_.size();
        context_.esibuf = eeprom_.data();
        context_.esimap = eeprom_map_.data();
        context_.elist = &errors_;
        context_.idxstack = &indices_;
        context_.ecaterror = &error_;
        context_.DCtime = &dc_time_;
        context_.SMcommtype = sm_types_.data();
        context_.PDOassign = assignments_.data();
        context_.PDOdesc = descriptions_.data();
        context_.eepSM = &eeprom_sm_;
        context_.eepFMMU = &eeprom_fmmu_;
    }

    void close() noexcept {
        if (opened_) {
            slaves_[0].state = EC_STATE_INIT;
            ecx_writestate(&context_, 0);
            ecx_close(&context_);
            opened_ = false;
        }
        if (timer_ >= 0)
            ::close(std::exchange(timer_, -1));
        if (wake_ >= 0)
            ::close(std::exchange(wake_, -1));
    }

    void fault(const char* message) noexcept {
        if (healthy_.exchange(false))
            logging::get_logger().error("{}", message);
        notify_worker();
        received_cv_.notify_all();
    }

    void notify_worker() const noexcept {
        if (wake_ < 0)
            return;
        const uint64_t value = 1;
        ssize_t result;
        do {
            result = ::write(wake_, &value, sizeof(value));
        } while (result < 0 && errno == EINTR);
    }

    static uint32_t read32(const uint8_t* bytes) {
        return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8) | (uint32_t{bytes[2]} << 16)
             | (uint32_t{bytes[3]} << 24);
    }

    static void write32(uint8_t* bytes, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    }

    void exchange() {
        if (!pending_) {
            const std::scoped_lock lock{transmit_mutex_};
            if (transmit_count_ != 0) {
                pending_packet_ = transmit_queue_[transmit_head_];
                transmit_head_ = (transmit_head_ + 1) % kQueueSize;
                --transmit_count_;
                sequence_ = sequence_ == UINT32_MAX ? 1 : sequence_ + 1;
                pending_ = true;
            }
        }
        if (pending_
            && Clock::now() - pending_packet_.created
                   > std::chrono::microseconds{options_.admission_timeout_us}) {
            fault("EtherCAT admission deadline exceeded; delivery unknown, no replay");
            return;
        }
        auto* output = slaves_[1].outputs;
        std::memset(output, 0, kFrameSize);
        output[0] = 1;
        const auto size = pending_ ? pending_packet_.size : 0;
        output[2] = static_cast<uint8_t>(size);
        output[3] = static_cast<uint8_t>(size >> 8);
        write32(output + 4, epoch_);
        write32(output + 8, pending_ ? sequence_ : 0);
        write32(output + 12, acknowledgement_);
        std::memcpy(output + 16, pending_packet_.bytes.data(), size);
        if (!healthy_.load())
            return;
        ecx_send_processdata(&context_);
        if (ecx_receive_processdata(&context_, options_.receive_timeout_us) != expected_wkc_) {
            fault("EtherCAT WKC loss; output inhibited, recreate board to reconnect");
            return;
        }
        const auto* input = slaves_[1].inputs;
        const size_t length = input[2] | (size_t{input[3]} << 8);
        if (input[0] != 1 || input[1] != 0 || length > kPayloadSize || read32(input + 4) != epoch_)
            return; // Ignore stale input until the new session epoch is accepted.
        if (pending_ && read32(input + 12) == sequence_)
            pending_ = false;
        const auto sequence = read32(input + 8);
        if (sequence == 0 || sequence == acknowledgement_)
            return;
        if (sequence != (acknowledgement_ == UINT32_MAX ? 1 : acknowledgement_ + 1)) {
            fault("Nonconsecutive EtherCAT input sequence");
            return;
        }
        {
            const std::scoped_lock lock{receive_mutex_};
            if (receive_count_ == kQueueSize)
                return; // Backpressure: retain ACK until the packet can be queued.
            Packet packet;
            packet.size = length;
            std::memcpy(packet.bytes.data(), input + 16, length);
            receive_queue_[(receive_head_ + receive_count_) % kQueueSize] = packet;
            ++receive_count_;
            acknowledgement_ = sequence;
        }
        received_cv_.notify_one();
    }

    void run() noexcept {
        try {
            pollfd events[2]{
                {timer_, POLLIN, 0},
                { wake_, POLLIN, 0}
            };
            while (healthy_.load() && !stopping_.load()) {
                const auto result = ::poll(events, 2, -1);
                if (result < 0 && errno == EINTR)
                    continue;
                if (result < 0 || (events[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    fault("EtherCAT event wait failed");
                    break;
                }
                if (events[1].revents)
                    break;
                uint64_t expirations = 0;
                if (::read(timer_, &expirations, sizeof(expirations)) != sizeof(expirations))
                    continue;
                // Never issue a burst of stale control cycles to catch up.
                if (expirations * uint64_t{options_.cycle_us} > 10'000) {
                    fault("EtherCAT scheduler stalled for more than 10 ms");
                    break;
                }
                exchange();
            }
        } catch (...) {
            fault("Unhandled EtherCAT worker error");
        }
        healthy_.store(false);
        received_cv_.notify_all();
        slaves_[0].state = EC_STATE_INIT;
        ecx_writestate(&context_, 0);
    }

    void dispatch() noexcept {
        try {
            while (true) {
                Packet packet;
                {
                    std::unique_lock lock{receive_mutex_};
                    received_cv_.wait_for(lock, std::chrono::milliseconds{10}, [this] {
                        return stopping_.load() || !healthy_.load() || receive_count_ != 0;
                    });
                    if (stopping_.load() || !healthy_.load())
                        return;
                    if (receive_count_ == 0)
                        continue;
                    packet = receive_queue_[receive_head_];
                    receive_head_ = (receive_head_ + 1) % kQueueSize;
                    --receive_count_;
                }
                callback_(std::span{packet.bytes}.first(packet.size));
            }
        } catch (...) {
            fault("EtherCAT receive callback threw an exception");
        }
    }

    board::EthercatOptions options_;
    ecx_contextt context_{};
    ecx_portt port_{};
    std::array<ec_slavet, EC_MAXSLAVE> slaves_{};
    std::array<ec_groupt, EC_MAXGROUP> groups_{};
    std::array<uint8_t, EC_MAXEEPBUF> eeprom_{};
    std::array<uint32_t, EC_MAXEEPBITMAP> eeprom_map_{};
    ec_eringt errors_{};
    ec_idxstackT indices_{};
    boolean error_ = FALSE;
    int64 dc_time_ = 0;
    std::array<ec_SMcommtypet, EC_MAX_MAPT> sm_types_{};
    std::array<ec_PDOassignt, EC_MAX_MAPT> assignments_{};
    std::array<ec_PDOdesct, EC_MAX_MAPT> descriptions_{};
    ec_eepromSMt eeprom_sm_{};
    ec_eepromFMMUt eeprom_fmmu_{};
    std::array<uint8_t, 8192> iomap_{};
    int slave_count_ = 0;
    int expected_wkc_ = 0;
    int timer_ = -1;
    int wake_ = -1;
    bool opened_ = false;
    bool started_ = false;
    std::atomic<bool> healthy_{true};
    std::atomic<bool> stopping_{false};
    std::mutex transmit_mutex_;
    std::mutex receive_mutex_;
    std::condition_variable received_cv_;
    std::array<Packet, kQueueSize> transmit_queue_{};
    std::array<Packet, kQueueSize> receive_queue_{};
    size_t transmit_head_ = 0;
    size_t transmit_count_ = 0;
    size_t receive_head_ = 0;
    size_t receive_count_ = 0;
    std::function<void(std::span<const std::byte>)> callback_;
    std::thread worker_;
    std::thread dispatcher_;
    Packet pending_packet_;
    uint32_t epoch_ = 0;
    uint32_t sequence_ = 0;
    uint32_t acknowledgement_ = 0;
    bool pending_ = false;
};

} // namespace librmcs::host::transport::ethercat

#endif

namespace librmcs::host::transport {

std::unique_ptr<Transport> Transport::create_ethercat(const board::EthercatOptions& options) {
#if defined(LIBRMCS_ENABLE_ETHERCAT)
    return std::make_unique<ethercat::EthercatTransport>(options);
#else
    (void)options;
    throw std::runtime_error{"Rebuild librmcs with LIBRMCS_ENABLE_ETHERCAT=ON"};
#endif
}

} // namespace librmcs::host::transport
