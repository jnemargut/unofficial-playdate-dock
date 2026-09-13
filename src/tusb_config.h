#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Board / RTOS
// ---------------------------------------------------------------------------
#define CFG_TUSB_MCU              OPT_MCU_RP2040
#define CFG_TUSB_OS               OPT_OS_PICO
#define CFG_TUSB_DEBUG            0

// We are HOST only. The one USB controller on the RP2040 cannot be both.
// NOTE: this is why stdio moves to a UART on GP16/17, there is no USB serial.
#define CFG_TUH_ENABLED           1
#define CFG_TUD_ENABLED           0

// Use the RP2040's NATIVE USB controller, not Pico-PIO-USB.
// PIO-USB requires a system clock of exactly 120 or 240 MHz; PicoDVI needs 252.
// They cannot coexist. The native controller runs from PLL_USB and does not
// care what clk_sys is doing, which is why this combination works at all.
#define CFG_TUH_RPI_PIO_USB       0
#define BOARD_TUH_RHPORT          0
#define BOARD_TUH_MAX_SPEED       OPT_MODE_FULL_SPEED

// Put USB data in its own section; the SDK places it in USB DPRAM.
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__((aligned(4)))

// ---------------------------------------------------------------------------
// Host stack sizing
// ---------------------------------------------------------------------------
// Exactly one device (the Playdate) hangs off the root port. No hub: TinyUSB's
// RP2040 native host + hub path is the flaky one, and we don't need it.
#define CFG_TUH_DEVICE_MAX        1
#define CFG_TUH_HUB               0

// Default is 256 bytes. A device whose configuration descriptor exceeds this
// does not fail cleanly: it wedges the host stack permanently, because the
// assert returns without clearing `enumerating_daddr` (tinyusb issue #2177).
// The Playdate's descriptor is small, but the failure mode is bad enough that
// paying 1 KB of RAM to make it impossible is obviously correct.
#define CFG_TUH_ENUMERATION_BUFSIZE 1024

// Bulk completions arrive fast once the ping-pong EPX path is doing its job.
#define CFG_TUH_TASK_QUEUE_SZ     64

// ---------------------------------------------------------------------------
// CDC host (the Playdate is a bog-standard CDC-ACM device, VID 1331 PID 5740)
// ---------------------------------------------------------------------------
#define CFG_TUH_CDC               1
#define CFG_TUH_CDC_FTDI          0
#define CFG_TUH_CDC_CP210X        0
#define CFG_TUH_CDC_CH34X         0

// A single screen row message is 4 (header) + 52 (payload) = 56 bytes, and at
// 30 fps with a busy screen they arrive in bursts. The default RX buffer is one
// 64-byte bulk packet, which would thrash constantly.
#define CFG_TUH_CDC_RX_BUFSIZE    8192
#define CFG_TUH_CDC_TX_BUFSIZE    512

// Assert DTR/RTS and set a line coding at enumeration so the Playdate's console
// starts talking without us having to do a control transfer dance by hand.
#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM \
	(CDC_CONTROL_LINE_STATE_DTR | CDC_CONTROL_LINE_STATE_RTS)

#define CFG_TUH_CDC_LINE_CODING_ON_ENUM \
	{ 115200, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }

// Everything else off: we are not a general-purpose host.
#define CFG_TUH_HID               0
#define CFG_TUH_MSC               0
#define CFG_TUH_VENDOR            0

#ifdef __cplusplus
}
#endif

#endif // _TUSB_CONFIG_H_
