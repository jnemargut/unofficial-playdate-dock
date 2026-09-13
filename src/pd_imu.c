#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pd_imu.h"

#define IMU_SPI     spi0
#define PIN_SCK     2
#define PIN_MOSI    3
#define PIN_MISO    4
#define PIN_CS      5

// ICM-42688-P registers (user bank 0)
#define REG_DEVICE_CONFIG   0x11
#define REG_INT_CONFIG      0x14
#define REG_ACCEL_DATA_X1   0x1F
#define REG_PWR_MGMT0       0x4E
#define REG_ACCEL_CONFIG0   0x50
#define REG_WHO_AM_I        0x75
#define REG_BANK_SEL        0x76

#define WHO_AM_I_EXPECTED   0x47   // confirmed on the real board

volatile int8_t pd_imu_map[3]  = {0, 1, 2};
volatile int8_t pd_imu_sign[3] = {1, 1, 1};

static bool present = false;
static int  orientation_idx = 0;

// ±2g gives the best resolution for tilt, which is all we want. 32768 counts
// over 2g = 16384 counts per g.
#define ACCEL_COUNTS_PER_G  16384.0f

static inline void cs_low(void)  { gpio_put(PIN_CS, 0); __asm volatile("nop\nnop\nnop"); }
static inline void cs_high(void) { __asm volatile("nop\nnop\nnop"); gpio_put(PIN_CS, 1); }

static void reg_write(uint8_t reg, uint8_t val) {
	uint8_t buf[2] = { reg & 0x7f, val };   // MSB clear = write
	cs_low();
	spi_write_blocking(IMU_SPI, buf, 2);
	cs_high();
}

static uint8_t reg_read(uint8_t reg) {
	uint8_t tx[2] = { (uint8_t)(reg | 0x80), 0 };   // MSB set = read
	uint8_t rx[2] = { 0, 0 };
	cs_low();
	spi_write_read_blocking(IMU_SPI, tx, rx, 2);
	cs_high();
	return rx[1];
}

static void reg_read_burst(uint8_t reg, uint8_t *dst, size_t n) {
	uint8_t addr = (uint8_t)(reg | 0x80);
	cs_low();
	spi_write_blocking(IMU_SPI, &addr, 1);
	spi_read_blocking(IMU_SPI, 0x00, dst, n);
	cs_high();
}

bool pd_imu_init(void) {
	// Initialise SPI *after* the system clock change, or the baud divisor is
	// computed from the wrong parent frequency.
	spi_init(IMU_SPI, 1000 * 1000);
	spi_set_format(IMU_SPI, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);

	gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
	gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
	gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

	gpio_init(PIN_CS);
	gpio_set_dir(PIN_CS, GPIO_OUT);
	gpio_put(PIN_CS, 1);

	sleep_ms(10);

	// Soft reset, then wait: the datasheet asks for 1 ms; be generous.
	reg_write(REG_BANK_SEL, 0x00);
	reg_write(REG_DEVICE_CONFIG, 0x01);
	sleep_ms(20);
	reg_write(REG_BANK_SEL, 0x00);

	uint8_t who = reg_read(REG_WHO_AM_I);
	if (who != WHO_AM_I_EXPECTED) {
		printf("[imu] WHO_AM_I = 0x%02x, expected 0x%02x, not initialised\n",
		       who, WHO_AM_I_EXPECTED);
		present = false;
		return false;
	}

	// ACCEL_FS_SEL = 011 (±2g) in bits [7:5], ODR = 1000 (100 Hz) in bits [3:0].
	// We only consume one sample per video frame, so 100 Hz is already more
	// than the Playdate can use.
	reg_write(REG_ACCEL_CONFIG0, (uint8_t)((0x3 << 5) | 0x8));

	// Accelerometer into low-noise mode; gyro stays off, we don't send gyro
	// and it is the larger power draw.
	reg_write(REG_PWR_MGMT0, 0x03);
	sleep_ms(10);   // datasheet: no register writes for 200us after this

	present = true;
	printf("[imu] ICM-42688-P ready (WHO_AM_I 0x%02x), accel +/-2g @100Hz\n", who);
	return true;
}

bool pd_imu_present(void) { return present; }

void pd_imu_read_raw(int16_t *x, int16_t *y, int16_t *z) {
	uint8_t b[6] = {0};
	if (present) reg_read_burst(REG_ACCEL_DATA_X1, b, 6);
	*x = (int16_t)((b[0] << 8) | b[1]);
	*y = (int16_t)((b[2] << 8) | b[3]);
	*z = (int16_t)((b[4] << 8) | b[5]);
}

void pd_imu_read(pd_imu_sample_t *out) {
	int16_t r[3];
	pd_imu_read_raw(&r[0], &r[1], &r[2]);

	float g[3];
	for (int i = 0; i < 3; i++) {
		int8_t src = pd_imu_map[i];
		if (src < 0 || src > 2) src = (int8_t)i;
		g[i] = (r[src] / ACCEL_COUNTS_PER_G) * (float)pd_imu_sign[i];
	}
	out->x = g[0];
	out->y = g[1];
	out->z = g[2];
}

// The six orientations that actually occur when a small board is mounted in a
// cradle. Cycling beats guessing, since the right answer depends on physical
// mounting nobody has decided yet.
static const int8_t ORIENT[6][6] = {
	// map x,y,z   sign x,y,z
	{ 0, 1, 2,      1,  1,  1 },
	{ 0, 1, 2,     -1, -1,  1 },
	{ 1, 0, 2,      1, -1,  1 },
	{ 1, 0, 2,     -1,  1,  1 },
	{ 0, 2, 1,      1,  1,  1 },
	{ 2, 1, 0,      1,  1,  1 },
};

void pd_imu_cycle_orientation(void) {
	orientation_idx = (orientation_idx + 1) % 6;
	for (int i = 0; i < 3; i++) {
		pd_imu_map[i]  = ORIENT[orientation_idx][i];
		pd_imu_sign[i] = ORIENT[orientation_idx][i + 3];
	}
	pd_imu_print_config();
}

void pd_imu_print_config(void) {
	printf("[imu] orientation %d: map={%d,%d,%d} sign={%d,%d,%d}\n",
	       orientation_idx,
	       pd_imu_map[0], pd_imu_map[1], pd_imu_map[2],
	       pd_imu_sign[0], pd_imu_sign[1], pd_imu_sign[2]);
}
