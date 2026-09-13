// Host-build shim. Lets pd_stream.c and pd_frame.c compile and run natively on
// a Mac so the protocol state machine can be exercised before it goes anywhere
// near hardware with no debugger.
#ifndef HOST_PICO_STDLIB_SHIM_H
#define HOST_PICO_STDLIB_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define __not_in_flash_func(x) x
#define __not_in_flash(g)

typedef int64_t absolute_time_t;

static inline absolute_time_t get_absolute_time(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (absolute_time_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static inline uint32_t to_ms_since_boot(absolute_time_t t) {
	return (uint32_t)(t / 1000);
}

static inline absolute_time_t make_timeout_time_ms(uint32_t ms) {
	return get_absolute_time() + (absolute_time_t)ms * 1000;
}

static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) {
	return to - from;
}

#include <unistd.h>
// Real sleep on the host: the connect handshake is now a timed state machine
// driven from the main loop, so the tests have to let wall-clock time pass.
static inline void sleep_ms(uint32_t ms) { usleep(ms * 1000); }

#endif
