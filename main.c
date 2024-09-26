#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pll.h"
#include "hardware/pwm.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "cmd.h"
#include "i2s.h"
#include "main.pio.h"
#include "subq.h"
#include "utils.h"
#include "values.h"

// globals
mutex_t mechacon_mutex;
volatile uint latched = 0;
volatile uint mechachon_sm_offset;
volatile uint soct_offset;
volatile uint subq_offset;
volatile bool soct = 0;
volatile bool hasData = 0;
volatile uint sled_move_direction = SLED_MOVE_STOP;
volatile uint count_track = 0;
volatile uint track = 0;
volatile uint original_track = 0;
volatile uint sector = 0;
volatile uint sector_sending = -1;
volatile uint sector_for_track_update = 0;
volatile uint64_t subq_start_time = 0;
volatile uint64_t sled_timer = 0;
volatile uint64_t autoseq_timer = 0;
volatile uint64_t autoseq_direction = SLED_MOVE_STOP;
volatile uint autoseq_track = 0;
volatile uint8_t current_sens;
volatile int num_logical_tracks = 0;
int *logical_track_to_sector;
bool *is_data_track;
volatile int current_logical_track = 0;
volatile int mode = 1;
volatile bool core_ready[2] = {false, false};

pwm_config cfg_CLOCK;
pwm_config cfg_LRCK;
pwm_config cfg_DA15;
uint slice_num_CLOCK;
uint slice_num_DA15;
uint slice_num_LRCK;

volatile bool SENS_data[16] = {
    0, // $0X - FZC
    0, // $1X - AS
    0, // $2X - TZC
    0, // $3X - Misc.
    0, // $4X - XBUSY
    1, // $5X - FOK
    0, // $6X - 0
    0, // $7X - 0
    0, // $8X - 0
    0, // $9X - 0
    1, // $AX - GFS
    0, // $BX - COMP
    0, // $CX - COUT
    0, // $DX - 0
    0, // $EX - OV64
    0  // $FX - 0
};

void select_sens(uint8_t new_sens)
{
    current_sens = new_sens;
}

void set_sens(uint8_t what, bool new_value)
{
    SENS_data[what] = new_value;
    if (what == current_sens)
    {
        gpio_put(SENS, new_value);
    }
}

void initialize()
{
    srand(time(NULL));
    mutex_init(&mechacon_mutex);

    gpio_init(SCEX_DATA);
    gpio_init(SENS);
    gpio_init(LMTSW);
    gpio_init(XLAT);
    gpio_init(DOOR);
    gpio_init(RESET);
    gpio_init(SQCK);
    gpio_init(SQSO);
    gpio_init(CMD_CK);

    gpio_init(LRCK);
    gpio_init(DA15);
    gpio_init(CLK);

    gpio_set_dir(SCEX_DATA, GPIO_OUT);
    gpio_put(SCEX_DATA, 1);
    gpio_set_dir(SENS, GPIO_OUT);
    gpio_set_dir(LMTSW, GPIO_OUT);
    gpio_set_dir(XLAT, GPIO_IN);
    gpio_set_dir(DOOR, GPIO_IN);
    gpio_set_dir(RESET, GPIO_IN);
    gpio_set_dir(SQCK, GPIO_IN);
    gpio_set_dir(SQSO, GPIO_OUT);
    gpio_set_dir(CMD_CK, GPIO_IN);

    gpio_set_dir(LRCK, GPIO_OUT);
    gpio_set_dir(DA15, GPIO_OUT);
    gpio_set_dir(CLK, GPIO_OUT);

    gpio_set_function(CLK, GPIO_FUNC_PWM);
    slice_num_CLOCK = pwm_gpio_to_slice_num(CLK);
    cfg_CLOCK = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&cfg_CLOCK, PWM_DIV_FREE_RUNNING);
    pwm_config_set_wrap(&cfg_CLOCK, 1);
    pwm_config_set_clkdiv_int(&cfg_CLOCK, 2);
    pwm_init(slice_num_CLOCK, &cfg_CLOCK, false);
    pwm_set_both_levels(slice_num_CLOCK, 1, 1);

    gpio_set_function(DA15, GPIO_FUNC_PWM);
    slice_num_DA15 = pwm_gpio_to_slice_num(DA15);
    cfg_DA15 = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&cfg_DA15, PWM_DIV_FREE_RUNNING);
    pwm_config_set_wrap(&cfg_DA15, (1 * 32) - 1);
    pwm_config_set_clkdiv_int(&cfg_DA15, 4);
    pwm_config_set_output_polarity(&cfg_DA15, true, true);
    pwm_init(slice_num_DA15, &cfg_DA15, false);
    pwm_set_both_levels(slice_num_DA15, 16, 16);

    gpio_set_function(LRCK, GPIO_FUNC_PWM);
    slice_num_LRCK = pwm_gpio_to_slice_num(LRCK);
    cfg_LRCK = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&cfg_LRCK, PWM_DIV_FREE_RUNNING);
    pwm_config_set_wrap(&cfg_LRCK, (48 * 32) - 1);
    pwm_config_set_clkdiv_int(&cfg_LRCK, 4);
    pwm_init(slice_num_LRCK, &cfg_LRCK, false);
    pwm_set_both_levels(slice_num_LRCK, (48 * 16), (48 * 16));

    gpio_put(SQSO, 0);
    gpio_put(SCOR, 0);

    gpio_set_input_hysteresis_enabled(RESET, true);
    gpio_set_input_hysteresis_enabled(SQCK, true);
    gpio_set_input_hysteresis_enabled(XLAT, true);
    gpio_set_input_hysteresis_enabled(CMD_CK, true);
    gpio_set_drive_strength(LRCK, GPIO_DRIVE_STRENGTH_12MA);
    uint i2s_pio_offset = pio_add_program(pio0, &i2s_data_program);
    i2s_data_program_init(pio0, I2S_DATA_SM, i2s_pio_offset, DA15);

    mechachon_sm_offset = pio_add_program(pio1, &mechacon_program);
    mechacon_program_init(pio1, MECHACON_SM, mechachon_sm_offset, CMD_DATA);

    uint offset5 = pio_add_program(pio1, &scor_program);
    scor_program_init(pio1, SCOR_SM, offset5, SCOR);

    soct_offset = pio_add_program(pio1, &soct_program);

    subq_offset = pio_add_program(pio1, &subq_program);

    uint64_t startTime = time_us_64();

    pio_enable_sm_mask_in_sync(pio0, (1u << I2S_DATA_SM));
    pwm_set_mask_enabled((1 << slice_num_LRCK) | (1 << slice_num_DA15) | (1 << slice_num_CLOCK));

    gpio_set_dir(RESET, GPIO_OUT);
    gpio_put(RESET, 0);
    sleep_ms(300);
    gpio_set_dir(RESET, GPIO_IN);

    while ((time_us_64() - startTime) < 30000)
    {
        if (gpio_get(RESET) == 0)
        {
            startTime = time_us_64();
        }
    }

    while ((time_us_64() - startTime) < 30000)
    {
        if (gpio_get(CMD_CK) == 0)
        {
            startTime = time_us_64();
        }
    }

    printf("ON!\n");
    multicore_launch_core1(i2s_data_thread);
    gpio_set_irq_enabled_with_callback(XLAT, GPIO_IRQ_EDGE_FALL, true, &interrupt_xlat);
    pio_enable_sm_mask_in_sync(pio1, (1u << SCOR_SM) | (1u << MECHACON_SM));
}

int main()
{
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(100);

    set_sys_clock_khz(271200, true);
    sleep_ms(5);

    stdio_init_all();

    stdio_set_chars_available_callback(NULL, NULL);
    sleep_ms(2500);
    printf("Initializing...\n");
    initialize();
    int prevMode = 1;
    int sectors_per_track_i = sectors_per_track(0);
    bool subq_delay = 0;
    uint64_t subq_delay_time = 0;

    core_ready[0] = true;

    while (!core_ready[1])
    {
        sleep_ms(1);
    }

    while (true)
    {
        // Limit Switch
        gpio_put(LMTSW, sector > 3000);

        // Mechacon + SENS
        if (mutex_try_enter(&mechacon_mutex, 0))
        {
            while (!pio_sm_is_rx_fifo_empty(pio1, MECHACON_SM))
            {
                uint c = pio_sm_get_blocking(pio1, MECHACON_SM) >> 24;
                latched >>= 8;
                latched |= c << 16;
            }
            select_sens(latched >> 20);
            gpio_put(SENS, SENS_data[latched >> 20]);
            mutex_exit(&mechacon_mutex);
        }

        // Speed change
        if (prevMode == 1 && mode == 2)
        {
            pwm_set_mask_enabled(0);
            pwm_config_set_clkdiv_int(&cfg_DA15, 2);
            pwm_config_set_clkdiv_int(&cfg_LRCK, 2);
            pwm_hw->slice[slice_num_DA15].div = cfg_DA15.div;
            pwm_hw->slice[slice_num_LRCK].div = cfg_LRCK.div;
            pwm_set_mask_enabled((1 << slice_num_LRCK) | (1 << slice_num_DA15) | (1 << slice_num_CLOCK));
            prevMode = 2;
            printf("x2\n");
        }
        else if (prevMode == 2 && mode == 1)
        {
            pwm_set_mask_enabled(0);
            pwm_config_set_clkdiv_int(&cfg_DA15, 4);
            pwm_config_set_clkdiv_int(&cfg_LRCK, 4);
            pwm_hw->slice[slice_num_DA15].div = cfg_DA15.div;
            pwm_hw->slice[slice_num_LRCK].div = cfg_LRCK.div;
            pwm_set_mask_enabled((1 << slice_num_LRCK) | (1 << slice_num_DA15) | (1 << slice_num_CLOCK));
            prevMode = 1;
            printf("x1\n");
        }

        // Track limits
        if (track < 0 || sector < 0)
        {
            track = 0;
            sector = 0;
            sector_for_track_update = 0;
        }

        if (track > 24000 || sector > 440000)
        {
            track = 24000;
            sector = track_to_sector(track);
            sector_for_track_update = sector;
        }

        // Reset
        if (gpio_get(RESET) == 0)
        {
            printf("RESET!\n");
            pio_sm_set_enabled(pio1, SUBQ_SM, false);
            pio_sm_set_enabled(pio1, SOCT_SM, false);
            mechacon_program_init(pio1, MECHACON_SM, mechachon_sm_offset, CMD_DATA);
            subq_delay = 0;
            soct = 0;

            gpio_init(SQSO);
            gpio_set_dir(SQSO, GPIO_OUT);
            gpio_put(SQSO, 0);

            uint64_t startTime = time_us_64();

            while ((time_us_64() - startTime) < 30000)
            {
                if (gpio_get(RESET) == 0)
                {
                    startTime = time_us_64();
                }
            }

            while ((time_us_64() - startTime) < 30000)
            {
                if (gpio_get(CMD_CK) == 0)
                {
                    startTime = time_us_64();
                }
            }

            pio_sm_set_enabled(pio1, MECHACON_SM, true);
        }

        // Soct/Sled/seek/autoseq
        if (soct)
        {
            uint interrupts = save_and_disable_interrupts();
            // waiting for RX FIFO entry does not work.
            sleep_us(300);
            soct = 0;
            pio_sm_set_enabled(pio1, SOCT_SM, false);
            subq_start_time = time_us_64();
            restore_interrupts(interrupts);
        }
        else if (sled_move_direction == SLED_MOVE_FORWARD)
        {
            if ((time_us_64() - sled_timer) > TRACK_MOVE_TIME_US)
            {
                sled_timer = time_us_64();
                track++;
                sector = track_to_sector(track);
                sector_for_track_update = sector;

                if ((track - original_track) >= count_track)
                {
                    original_track = track;
                    set_sens(SENS_COUT, !SENS_data[SENS_COUT]);
                }
            }
        }
        else if (sled_move_direction == SLED_MOVE_REVERSE)
        {
            if ((time_us_64() - sled_timer) > TRACK_MOVE_TIME_US)
            {
                sled_timer = time_us_64();
                track--;
                sector = track_to_sector(track);
                sector_for_track_update = sector;
                if ((original_track - track) >= count_track)
                {
                    original_track = track;
                    set_sens(SENS_COUT, !SENS_data[SENS_COUT]);
                }
            }
        }
        else if (autoseq_direction == SLED_MOVE_FORWARD)
        {
            if ((time_us_64() - autoseq_timer) >= (TRACK_MOVE_TIME_US * (autoseq_track - track)))
            {
                autoseq_timer = time_us_64();
                track = autoseq_track;
                sector = track_to_sector(track);
                sector_for_track_update = sector;
                autoseq_direction = SLED_MOVE_STOP;
                set_sens(SENS_AUTOSEQ, 0);
                set_sens(SENS_COUT, !SENS_data[SENS_COUT]);
            }
        }
        else if (autoseq_direction == SLED_MOVE_REVERSE)
        {
            if ((time_us_64() - autoseq_timer) >= (TRACK_MOVE_TIME_US * (track - autoseq_track)))
            {
                autoseq_timer = time_us_64();
                track = autoseq_track;
                sector = track_to_sector(track);
                sector_for_track_update = sector;
                autoseq_direction = SLED_MOVE_STOP;
                set_sens(SENS_AUTOSEQ, 0);
                set_sens(SENS_COUT, !SENS_data[SENS_COUT]);
            }
        }
        else if (SENS_data[SENS_GFS])
        {
            if (sector < 4650 && (time_us_64() - subq_start_time) > 13333)
            {
                subq_start_time = time_us_64();
                start_subq();
                sector++;
                if ((sector - sector_for_track_update) >= sectors_per_track_i)
                {
                    sector_for_track_update = sector;
                    track++;
                    sectors_per_track_i = sectors_per_track(track);
                }
            }
            else
            {
                if (sector_sending == sector)
                {
                    if (!subq_delay)
                    {
                        sector++;
                        if ((sector - sector_for_track_update) >= sectors_per_track_i)
                        {
                            sector_for_track_update = sector;
                            track++;
                            sectors_per_track_i = sectors_per_track(track);
                        }
                        subq_delay = 1;
                        subq_delay_time = time_us_64();
                    }
                }

                if (subq_delay && (sector >= 4650 && (time_us_64() - subq_delay_time) > 3333))
                {
                    subq_delay = 0;
                    start_subq();
                }
            }
        }
        else
        {
            subq_delay = 0;
        }
    }
}
