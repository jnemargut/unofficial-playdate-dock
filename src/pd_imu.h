#ifndef PD_IMU_H
#define PD_IMU_H

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// TDK InvenSense ICM-42688-P, the 6-axis IMU on the Video Game Module.
//
// Wiring is fixed by the board and confirmed against the stock Flipper firmware
// (its `imu_test` command reports Chip ID 0x47 and passes both self-tests):
//
//   SPI0  SCK = GPIO2   MOSI = GPIO3   MISO = GPIO4   CS = GPIO5
//         INT1 = GPIO6  INT2 = GPIO7
//
// Mode 3 (CPOL=1, CPHA=1), MSB first, 8-bit. Stock firmware clocks it at 1 MHz;
// the part will do 24 MHz but we need ~30 samples a second, so slow is fine and
// slow is quiet.
//
// Note these pins sit on the Flipper connector behind 270R series resistors and
// are shared with the Flipper itself, Flipper's docs are explicit that the
// sensor cannot be driven by both at once.
// ---------------------------------------------------------------------------

typedef struct {
	float x, y, z;      // in g
} pd_imu_sample_t;

// Returns false if the part does not answer with the expected WHO_AM_I.
bool pd_imu_init(void);

bool pd_imu_present(void);

// Latest reading, already scaled to g and with the orientation mapping applied.
void pd_imu_read(pd_imu_sample_t *out);

// Raw counts, for calibration over the debug UART.
void pd_imu_read_raw(int16_t *x, int16_t *y, int16_t *z);

// --- Orientation -----------------------------------------------------------
// How the module is physically mounted relative to how a Playdate is held is
// not knowable from a datasheet, so the mapping is runtime-adjustable. `axis`
// selects which IMU axis feeds each Playdate axis (0..2) and `sign` flips it.
extern volatile int8_t pd_imu_map[3];    // default {0, 1, 2}
extern volatile int8_t pd_imu_sign[3];   // default {1, 1, 1}

void pd_imu_cycle_orientation(void);     // step through the 6 sensible mappings
void pd_imu_print_config(void);

#endif // PD_IMU_H
