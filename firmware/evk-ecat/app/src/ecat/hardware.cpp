#include <cstddef>
#include <cstdint>

#include "board.h"
#include "hpm_esc_drv.h"
#include "hpm_gpio_drv.h"

extern "C" {
#include "esc.h"
}

#include "sii.hpp"

namespace librmcs::firmware::ecat {

class Hardware {
public:
    static void read(uint16_t address, void* destination, uint16_t length) {
        auto* bytes = static_cast<uint8_t*>(destination);
        // HPM PDI requires aligned 32-bit accesses, including small registers.
        for (uint32_t i = 0; i < length;) {
            const uint32_t current = address + i;
            const auto value =
                *reinterpret_cast<volatile uint32_t*>(HPM_ESC_BASE + (current & ~3U));
            for (uint32_t offset = current & 3U; offset < 4 && i < length; ++offset, ++i)
                bytes[i] = static_cast<uint8_t>(value >> (offset * 8U));
        }
        update_events();
    }

    static void write(uint16_t address, const void* source, uint16_t length) {
        const auto* bytes = static_cast<const uint8_t*>(source);
        for (uint32_t i = 0; i < length;) {
            const uint32_t current = address + i;
            auto* target = reinterpret_cast<volatile uint32_t*>(HPM_ESC_BASE + (current & ~3U));
            uint32_t value = (current % 4U == 0 && length - i >= 4) ? 0 : *target;
            for (uint32_t offset = current & 3U; offset < 4 && i < length; ++offset, ++i) {
                value = (value & ~(0xffU << (offset * 8U)))
                      | (static_cast<uint32_t>(bytes[i]) << (offset * 8U));
            }
            *target = value;
        }
        update_events();
    }

    static void eeprom() {
        const auto status = HPM_ESC->EEPROM_CTRL_STAT;
        if ((status & ESC_EEPROM_CTRL_STAT_BUSY_MASK) == 0)
            return;
        const auto command = static_cast<esc_eeprom_cmd_t>(esc_get_eeprom_cmd(HPM_ESC));
        bool error = false;
        if (command == esc_eeprom_reload_cmd) {
            esc_write_eeprom_data(HPM_ESC, 0);
        } else if (command == esc_eeprom_read_cmd) {
            const uint32_t address = HPM_ESC->EEPROM_ADDR * 2U;
            uint64_t data = 0;
            if (address > image_.size() - 8) {
                error = true;
            } else {
                for (unsigned i = 0; i < 8; ++i)
                    data |= static_cast<uint64_t>(image_[address + i]) << (8U * i);
                HPM_ESC->EEPROM_DATA = data;
            }
        } else if (command == esc_eeprom_write_cmd) {
            error = true; // Read-only engineering SII, no persistent writes.
        }
        esc_eeprom_emulation_ack(HPM_ESC, command, error, false);
    }

    static void init() {
        board_init_ethercat(HPM_ESC);
        clock_set_source_divider(clock_esc0, clk_src_pll1_clk0, 4);
        esc_eeprom_clock_config_t config{true, false, true, true};
        esc_config_eeprom_and_clock(HPM_ESC, &config);
        board_delay_ms(10);
        eeprom();
        gpio_write_pin(
            BOARD_ECAT_PHY0_RESET_GPIO, BOARD_ECAT_PHY0_RESET_GPIO_PORT_INDEX,
            BOARD_ECAT_PHY0_RESET_PIN_INDEX, 1);
        board_delay_ms(10);
        esc_set_phy_offset(HPM_ESC, BOARD_ECAT_PHY_ADDR_OFFSET);
        esc_config_ctrl_signal_function(
            HPM_ESC, BOARD_ECAT_NMII_LINK0_CTRL_INDEX, esc_ctrl_signal_func_alt_nmii_link0,
            BOARD_ECAT_PORT0_LINK_INVERT);
        esc_config_ctrl_signal_function(
            HPM_ESC, BOARD_ECAT_NMII_LINK1_CTRL_INDEX, esc_ctrl_signal_func_alt_nmii_link1,
            BOARD_ECAT_PORT1_LINK_INVERT);
        esc_config_nmii_link_source(HPM_ESC, true, true, false);
    }

private:
    static void update_events() {
        ESCvar.ALevent = *reinterpret_cast<volatile uint32_t*>(HPM_ESC_BASE + 0x220);
    }

    inline static constexpr auto image_ = Sii::image();
};

} // namespace librmcs::firmware::ecat

extern "C" {
void ESC_read(uint16_t address, void* buffer, uint16_t length) {
    librmcs::firmware::ecat::Hardware::read(address, buffer, length);
}
void ESC_write(uint16_t address, void* buffer, uint16_t length) {
    librmcs::firmware::ecat::Hardware::write(address, buffer, length);
}
void ESC_init(const esc_cfg_t*) { librmcs::firmware::ecat::Hardware::init(); }
void ESC_reset() {}
void evk_eeprom_handler() { librmcs::firmware::ecat::Hardware::eeprom(); }
}
