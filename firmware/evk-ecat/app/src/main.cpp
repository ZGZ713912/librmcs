#include <cstdio>
#include <cstring>

#include "board.h"
#include "devices.hpp"
#include "ecat/bridge.hpp"
#include "hpm_mchtmr_drv.h"

extern "C" {
#include "ecat_slv.h"
void evk_eeprom_handler();
alignas(4) uint8_t rxpdo[MAX_RXPDO_SIZE]{};
alignas(4) uint8_t txpdo[MAX_TXPDO_SIZE]{};
void cb_get_inputs();
void cb_set_outputs();
}

class Application {
public:
    static void output_received() { last_output_us_ = mchtmr_get_count(HPM_MCHTMR) / 24U; }

    static void cycle() {
        const auto now = mchtmr_get_count(HPM_MCHTMR) / 24U;
        // Pause peripheral output after 20 ms without SM2. Bridge preserves the
        // session during short outages; the host is responsible for reconnecting.
        const bool healthy = ESCvar.ALstatus == ESCop && now - last_output_us_ < 20'000;
        bridge_.cycle(std::as_bytes(std::span{rxpdo}), input_, now, healthy);
        std::memcpy(txpdo, input_.data(), input_.size());
    }

    static void run() {
        board_init();
        if (!devices_.init()) {
            std::printf("CAN/UART init failed\n");
            while (true) {}
        }
        std::printf("RMCS EVK SOES startup\n");
        esc_cfg_t config{};
        // SOES decrements this per polling iteration, not per millisecond.
        // Use the explicit 20-ms wall-clock SM2 guard for output suspension.
        config.watchdog_cnt = 2'000'000'000;
        config.esc_hw_eep_handler = evk_eeprom_handler;
        ecat_slv_init(&config);
        std::printf("SOES initialized\n");
        unsigned previous = 0xff;
        while (true) {
            evk_eeprom_handler();
            ecat_slv();
            // Lease expiry must run even when no new SM2/PDO event arrives.
            cycle();
            devices_.poll(bridge_, mchtmr_get_count(HPM_MCHTMR) / 24U);
            if (ESCvar.ALstatus != previous) {
                previous = ESCvar.ALstatus;
                std::printf("AL status=%02x error=%04x\n", previous, ESCvar.ALerror);
            }
        }
    }

private:
    inline static uint64_t last_output_us_ = 0;
    inline static librmcs::firmware::ecat::EvkDevices devices_;
    inline static librmcs::firmware::ecat::Bridge bridge_{devices_};
    inline static librmcs::firmware::ecat::ProcessFrame::Storage input_{};
};

extern "C" void cb_get_inputs() { Application::cycle(); }
extern "C" void cb_set_outputs() { Application::output_received(); }

int main() { Application::run(); }
