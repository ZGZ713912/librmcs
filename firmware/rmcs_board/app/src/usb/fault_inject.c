#include <stdbool.h>
#include <stdint.h>

#ifdef LIBRMCS_USB_FAULT_INJECT

# include <board.h>
# include <device/usbd.h>
# include <hpm_common.h>
# include <hpm_soc.h>
# include <hpm_usb_drv.h>

/* Test-only hook for the unfixed firmware. Press the user key to arm the injection and drop
 * the USB pull-up; the reconnect 100 ms later makes the host issue a bus reset, which is then
 * forced to hang inside the ISR, exactly like the pre-fix unbounded ENDPTPRIME wait. */
# define KEY_SAMPLE_TICKS (40000U)  /* 10 ms at 4 MHz */
# define RECONNECT_TICKS  (400000U) /* 100 ms at 4 MHz */

extern void __real_usb_dcd_bus_reset(USB_Type* ptr, uint16_t ep0_max_packet_size);

static bool key_initialized;
static bool key_was_pressed;
static bool armed;
static bool reconnect_pending;
static uint32_t last_sample_ticks;
static uint32_t disconnect_ticks;

static uint32_t mchtmr_ticks(void) {
    const volatile uint32_t* const mtime = (const volatile uint32_t*)HPM_MCHTMR_BASE;
    return mtime[0];
}

void usb_fault_inject_poll(void) {
    const uint32_t now = mchtmr_ticks();

    if (!key_initialized) {
        board_init_user_key();
        key_initialized = true;
        last_sample_ticks = now;
    }

    if (reconnect_pending && (uint32_t)(now - disconnect_ticks) >= RECONNECT_TICKS) {
        reconnect_pending = false;
        (void)tud_connect();
    }

    if ((uint32_t)(now - last_sample_ticks) < KEY_SAMPLE_TICKS)
        return;
    last_sample_ticks = now;

    const bool pressed = board_user_key_pressed();
    if (pressed && !key_was_pressed) {
        armed = true;
        reconnect_pending = tud_disconnect();
        disconnect_ticks = now;
    }
    key_was_pressed = pressed;
}

void __wrap_usb_dcd_bus_reset(USB_Type* ptr, uint16_t ep0_max_packet_size) {
    if (armed) {
        armed = false;
        /* Emulate the pre-fix wedge: the bus-reset ISR never returns. */
        for (;;) {
            __asm__ volatile("" ::: "memory");
        }
    }

    __real_usb_dcd_bus_reset(ptr, ep0_max_packet_size);
}

#endif
