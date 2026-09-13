cmake_minimum_required(VERSION 3.13)

# ---- Toolchain / SDK ----------------------------------------------------
set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk)
set(PICODVI_PATH  ${CMAKE_CURRENT_LIST_DIR}/lib/PicoDVI-audio/software)

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(playdate_dock C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ---- PicoDVI ------------------------------------------------------------
# libdvi is an INTERFACE library; it needs PicoDVI's include dir on the path.
include_directories(${PICODVI_PATH}/include)
add_subdirectory(${PICODVI_PATH}/libdvi ${CMAKE_BINARY_DIR}/libdvi)

# ---- Our firmware -------------------------------------------------------
add_executable(playdate_dock
	src/main.c
	src/pd_frame.c
	src/pd_stream.c
	src/pd_font.c
	src/pd_imu.c
	src/pd_ui.c
	src/pd_audio.c
)

target_include_directories(playdate_dock PRIVATE src)

target_compile_definitions(playdate_dock PRIVATE
	# The Flipper Video Game Module's DVI wiring:
	#   TMDS pairs on GPIO 10/12/14, clock on GPIO 8, differential pairs inverted.
	DVI_DEFAULT_SERIAL_CONFIG=picodvi_dvi_cfg

	# The default for libdvi's runtime vertical repeat, which this firmware
	# patches in and then changes on the fly. 1 means every scanline is encoded;
	# the streaming path switches to 2 at 800x480, where the picture is an exact
	# 2x and the hardware can duplicate lines instead. See NOTES.md.
	DVI_VERTICAL_REPEAT=1
	DVI_N_TMDS_BUFFERS=3
	DVI_MONOCHROME_TMDS

	# Keep the DVI + USB hot paths out of flash (XIP stalls wreck the signal).
	PICO_DEFAULT_UART_TX_PIN=16
	PICO_DEFAULT_UART_RX_PIN=17
)

target_compile_options(playdate_dock PRIVATE -Wall -Wno-unused-function)

target_link_libraries(playdate_dock
	pico_stdlib
	pico_multicore
	pico_util
	hardware_spi
	hardware_dma
	hardware_pio
	hardware_vreg
	hardware_clocks
	libdvi
	tinyusb_host
)

# Host mode uses the one USB controller, so stdio goes out the UART on GP16/17.
pico_enable_stdio_usb(playdate_dock 0)
pico_enable_stdio_uart(playdate_dock 1)

# The QSPI flash runs at clk_sys / PICO_FLASH_SPI_CLKDIV (default 2). This
# binary can boot at 295.2 MHz for the 800x480 mode, where /2 would be 147.6 MHz
# past the W25Q128's ~133 MHz ceiling, so execute-in-place dies on the first
# fetch after the clock change and the board hangs before it can light the LED.
#
# That value is baked into the SECOND-STAGE BOOTLOADER, which the SDK builds as
# its own shared target, defining it on this executable does nothing at all.
# It needs a boot2 of its own.
pico_define_boot_stage2(boot2_dock ${PICO_SDK_PATH}/src/rp2040/boot_stage2/compile_time_choice.S)
target_compile_definitions(boot2_dock PRIVATE PICO_FLASH_SPI_CLKDIV=4)
pico_set_boot_stage2(playdate_dock boot2_dock)

pico_add_extra_outputs(playdate_dock)
