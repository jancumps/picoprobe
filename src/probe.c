/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <pico/stdlib.h>
#include <stdio.h>
#include <string.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>

#include "led.h"
#include "picoprobe_config.h"
#include "probe.pio.h"
#include "tusb.h"

#define DIV_ROUND_UP(m, n)	(((m) + (n) - 1) / (n))

// Only want to set / clear one gpio per event so go up in powers of 2
enum _dbg_pins {
    DBG_PIN_WRITE = 1,
    DBG_PIN_WRITE_WAIT = 2,
    DBG_PIN_READ = 4,
    DBG_PIN_PKT = 8,
};

CU_REGISTER_DEBUG_PINS(probe_timing)

// Uncomment to enable debug
//CU_SELECT_DEBUG_PINS(probe_timing)

#define PROBE_BUF_SIZE 8192
struct _probe {
    // PIO offset
    uint offset;
    uint initted;
};

static struct _probe probe;

void probe_set_swclk_freq(uint freq_khz) {
        uint clk_sys_freq_khz = clock_get_hz(clk_sys) / 1000;
        picoprobe_info("Set swclk freq %dKHz sysclk %dkHz\n", freq_khz, clk_sys_freq_khz);
        // Worked out with saleae
        uint32_t divider = clk_sys_freq_khz / freq_khz / 2;
        pio_sm_set_clkdiv_int_frac(pio0, PROBE_SM, divider, 0);
}

void probe_assert_reset(bool state)
{
#if defined(PROBE_PIN_RESET)
    /* Change the direction to out to drive pin to 0 or to in to emulate open drain */
    gpio_set_dir(PROBE_PIN_RESET, state);
#endif
}

void probe_write_bits(uint bit_count, uint32_t data_byte) {
    DEBUG_PINS_SET(probe_timing, DBG_PIN_WRITE);
    pio_sm_put_blocking(pio0, PROBE_SM, bit_count - 1);
    pio_sm_put_blocking(pio0, PROBE_SM, data_byte);
    DEBUG_PINS_SET(probe_timing, DBG_PIN_WRITE_WAIT);
    picoprobe_dump("Write %d bits 0x%x\n", bit_count, data_byte);
    // Wait for pio to push garbage to rx fifo so we know it has finished sending
    pio_sm_get_blocking(pio0, PROBE_SM);
    DEBUG_PINS_CLR(probe_timing, DBG_PIN_WRITE_WAIT);
    DEBUG_PINS_CLR(probe_timing, DBG_PIN_WRITE);
}

uint32_t probe_read_bits(uint bit_count) {
    DEBUG_PINS_SET(probe_timing, DBG_PIN_READ);
    pio_sm_put_blocking(pio0, PROBE_SM, bit_count - 1);
    uint32_t data = pio_sm_get_blocking(pio0, PROBE_SM);
    uint32_t data_shifted = data;
    if (bit_count < 32) {
        data_shifted = data >> (32 - bit_count);
    }

    picoprobe_dump("Read %d bits 0x%x (shifted 0x%x)\n", bit_count, data, data_shifted);
    DEBUG_PINS_CLR(probe_timing, DBG_PIN_READ);
    return data_shifted;
}

void probe_read_mode(void) {
    pio_sm_exec(pio0, PROBE_SM, pio_encode_jmp(probe.offset + probe_offset_in_posedge));
    while(pio_sm_get_pc(pio0, PROBE_SM) != probe.offset + probe_offset_in_idle);
}

void probe_write_mode(void) {
    pio_sm_exec(pio0, PROBE_SM, pio_encode_jmp(probe.offset + probe_offset_out_negedge));
    while(pio_sm_get_pc(pio0, PROBE_SM) != probe.offset + probe_offset_out_idle);
}

void probe_gpio_init()
{
#if defined(PROBE_PIN_RESET)
    // Target reset pin: pull up, input to emulate open drain pin
    gpio_pull_up(PROBE_PIN_RESET);
    // gpio_init will leave the pin cleared and set as input
    gpio_init(PROBE_PIN_RESET);
#endif
    // Funcsel pins
    pio_gpio_init(pio0, PROBE_PIN_SWCLK);
    pio_gpio_init(pio0, PROBE_PIN_SWDIO);
    // Make sure SWDIO has a pullup on it. Idle state is high
    gpio_pull_up(PROBE_PIN_SWDIO);
}

void probe_init() {
    if (!probe.initted) {
        uint offset = pio_add_program(pio0, &probe_program);
        probe.offset = offset;

        pio_sm_config sm_config = probe_program_get_default_config(offset);

        // Set SWCLK as a sideset pin
        sm_config_set_sideset_pins(&sm_config, PROBE_PIN_SWCLK);

        // Set SWDIO offset
        sm_config_set_out_pins(&sm_config, PROBE_PIN_SWDIO, 1);
        sm_config_set_set_pins(&sm_config, PROBE_PIN_SWDIO, 1);
#ifdef PROBE_PIN_SWDI
        sm_config_set_in_pins(&sm_config, PROBE_PIN_SWDI);
#else
        sm_config_set_in_pins(&sm_config, PROBE_PIN_SWDIO);
#endif

        // Set SWD and SWDIO pins as output to start. This will be set in the sm
        pio_sm_set_consecutive_pindirs(pio0, PROBE_SM, PROBE_PIN_OFFSET, 2, true);

        // shift output right, autopull off, autopull threshold
        sm_config_set_out_shift(&sm_config, true, false, 0);
        // shift input right as swd data is lsb first, autopush off
        sm_config_set_in_shift(&sm_config, true, false, 0);

        // Init SM with config
        pio_sm_init(pio0, PROBE_SM, offset, &sm_config);

        // Set up divisor
        probe_set_swclk_freq(1000);

        // Enable SM
        pio_sm_set_enabled(pio0, PROBE_SM, 1);
        probe.initted = 1;
    }

    // Jump to write program
    probe_write_mode();
}

void probe_deinit(void)
{
  probe_read_mode();
  if (probe.initted) {
    pio_sm_set_enabled(pio0, PROBE_SM, 0);
    pio_remove_program(pio0, &probe_program, probe.offset);
    probe.initted = 0;
  }
}
