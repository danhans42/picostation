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
#include "disc_image.h"
#include "i2s.h"
#include "logging.h"
#include "main.pio.h"
#include "subq.h"
#include "utils.h"
#include "values.h"

#if DEBUG_MISC
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) while (0)
#endif

uint latched = 0;           // Mechacon command latch
volatile bool soct = false; // Serial Read Out Circuit
uint countTrack = 0;
uint track = 0;
uint originalTrack = 0;

uint sledMoveDirection = SledMove::STOP;

volatile int sector = 0;
int sectorForTrackUpdate = 0;
volatile int sectorSending = -1;

volatile bool subqDelay = false;

static int prevMode = 1;
int mode = 1;

mutex_t mechaconMutex;
volatile bool coreReady[2] = {false, false};

static uint mechachonOffset;
uint soctOffset;
uint subqOffset;

// PWM Config
static pwm_config cfg_CLOCK;
static pwm_config cfg_DA15;
static pwm_config cfg_LRCK;
static uint clockSliceNum;
static uint da15SliceNum;
static uint lrckSliceNum;

volatile uint currentSens;
volatile bool sensData[16] = {
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

picostation::DiscImage discImage;

void clampSectorTrackLimits();
void initialize();
void maybeChangeMode();
void maybeReset();
void setSens(uint what, bool new_value);
void __time_critical_func(updateMechSens)();

void clampSectorTrackLimits()
{
    // static constexpr uint c_trackMax = 24000; // 90:14:40
    // static constexpr int c_sectorMax = 440000; // 98:47:50

    static constexpr uint c_trackMax = 20892;  // 73:59:58
    static constexpr int c_sectorMax = 333000; // 74:00:00

    if (track < 0 || sector < 0)
    {
        DEBUG_PRINT("Clamping sector/track, below 0\n");
        track = 0;
        sector = 0;
        sectorForTrackUpdate = 0;
    }

    if (track > c_trackMax || sector > c_sectorMax)
    {
        DEBUG_PRINT("Clamping sector/track, above max\n");
        track = c_trackMax;
        sector = trackToSector(track);
        sectorForTrackUpdate = sector;
    }
}

void initialize()
{
#if DEBUG_LOGGING_ENABLED
    stdio_init_all();
    stdio_set_chars_available_callback(NULL, NULL);
    sleep_ms(1250);
#endif
    DEBUG_PRINT("Initializing...\n");

    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(100);

    srand(time(NULL));
    mutex_init(&mechaconMutex);

    gpio_init(Pin::SCEX_DATA);
    gpio_init(Pin::SENS);
    gpio_init(Pin::LMTSW);
    gpio_init(Pin::XLAT);
    gpio_init(Pin::DOOR);
    gpio_init(Pin::RESET);
    gpio_init(Pin::SQCK);
    gpio_init(Pin::SQSO);
    gpio_init(Pin::CMD_CK);

    gpio_init(Pin::LRCK);
    gpio_init(Pin::DA15);
    gpio_init(Pin::CLK);

    gpio_set_dir(Pin::SCEX_DATA, GPIO_OUT);
    gpio_put(Pin::SCEX_DATA, 1);
    gpio_set_dir(Pin::SENS, GPIO_OUT);
    gpio_set_dir(Pin::LMTSW, GPIO_OUT);
    gpio_set_dir(Pin::XLAT, GPIO_IN);
    gpio_set_dir(Pin::DOOR, GPIO_IN);
    gpio_set_dir(Pin::RESET, GPIO_IN);
    gpio_set_dir(Pin::SQCK, GPIO_IN);
    gpio_set_dir(Pin::SQSO, GPIO_OUT);
    gpio_set_dir(Pin::CMD_CK, GPIO_IN);

    gpio_set_dir(Pin::LRCK, GPIO_OUT);
    gpio_set_dir(Pin::DA15, GPIO_OUT);
    gpio_set_dir(Pin::CLK, GPIO_OUT);

    // Main clock
    gpio_set_function(Pin::CLK, GPIO_FUNC_PWM);
    clockSliceNum = pwm_gpio_to_slice_num(Pin::CLK);
    cfg_CLOCK = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&cfg_CLOCK, PWM_DIV_FREE_RUNNING);
    pwm_config_set_wrap(&cfg_CLOCK, 1);
    pwm_config_set_clkdiv_int(&cfg_CLOCK, 2);
    pwm_init(clockSliceNum, &cfg_CLOCK, false);
    pwm_set_both_levels(clockSliceNum, 1, 1);

    // Data clock
    gpio_set_function(Pin::DA15, GPIO_FUNC_PWM);
    da15SliceNum = pwm_gpio_to_slice_num(Pin::DA15);
    cfg_DA15 = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&cfg_DA15, PWM_DIV_FREE_RUNNING);
    pwm_config_set_wrap(&cfg_DA15, (1 * 32) - 1);
    pwm_config_set_clkdiv_int(&cfg_DA15, 4);
    pwm_config_set_output_polarity(&cfg_DA15, true, true);
    pwm_init(da15SliceNum, &cfg_DA15, false);
    pwm_set_both_levels(da15SliceNum, 16, 16);

    // Left/right clock
    gpio_set_function(Pin::LRCK, GPIO_FUNC_PWM);
    lrckSliceNum = pwm_gpio_to_slice_num(Pin::LRCK);
    cfg_LRCK = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&cfg_LRCK, PWM_DIV_FREE_RUNNING);
    pwm_config_set_wrap(&cfg_LRCK, (48 * 32) - 1);
    pwm_config_set_clkdiv_int(&cfg_LRCK, 4);
    pwm_init(lrckSliceNum, &cfg_LRCK, false);
    pwm_set_both_levels(lrckSliceNum, (48 * 16), (48 * 16));

    gpio_put(Pin::SQSO, 0);
    gpio_put(Pin::SCOR, 0);

    gpio_set_input_hysteresis_enabled(Pin::RESET, true);
    gpio_set_input_hysteresis_enabled(Pin::SQCK, true);
    gpio_set_input_hysteresis_enabled(Pin::XLAT, true);
    gpio_set_input_hysteresis_enabled(Pin::CMD_CK, true);

    uint i2s_pio_offset = pio_add_program(pio0, &i2s_data_program);
    subqOffset = pio_add_program(pio0, &subq_program);
    i2s_data_program_init(pio0, SM::c_i2sData, i2s_pio_offset, Pin::DA15, Pin::DA16);

    uint scor_offset = pio_add_program(pio1, &scor_program);
    mechachonOffset = pio_add_program(pio1, &mechacon_program);
    soctOffset = pio_add_program(pio1, &soct_program);
    scor_program_init(pio1, SM::c_scor, scor_offset, Pin::SCOR);
    mechacon_program_init(pio1, SM::c_mechacon, mechachonOffset, Pin::CMD_DATA);

    uint64_t startTime = time_us_64();

    pio_sm_set_enabled(pio0, SM::c_i2sData, true);
    pwm_set_mask_enabled((1 << lrckSliceNum) | (1 << da15SliceNum) | (1 << clockSliceNum));

    gpio_set_dir(Pin::RESET, GPIO_OUT);
    gpio_put(Pin::RESET, 0);
    sleep_ms(300);
    gpio_set_dir(Pin::RESET, GPIO_IN);

    while ((time_us_64() - startTime) < 30000)
    {
        if (gpio_get(Pin::RESET) == 0)
        {
            startTime = time_us_64();
        }
    }

    while ((time_us_64() - startTime) < 30000)
    {
        if (gpio_get(Pin::CMD_CK) == 0)
        {
            startTime = time_us_64();
        }
    }

    DEBUG_PRINT("ON!\n");
    multicore_launch_core1(i2sDataThread);
    gpio_set_irq_enabled_with_callback(Pin::XLAT, GPIO_IRQ_EDGE_FALL, true, &interrupt_xlat);
    pio_enable_sm_mask_in_sync(pio1, (1u << SM::c_scor) | (1u << SM::c_mechacon));
}

void maybeChangeMode()
{
    if (prevMode == 1 && mode == 2)
    {
        pwm_set_mask_enabled(0);
        pwm_config_set_clkdiv_int(&cfg_DA15, 2);
        pwm_config_set_clkdiv_int(&cfg_LRCK, 2);
        pwm_hw->slice[da15SliceNum].div = cfg_DA15.div;
        pwm_hw->slice[lrckSliceNum].div = cfg_LRCK.div;
        pwm_set_mask_enabled((1 << lrckSliceNum) | (1 << da15SliceNum) | (1 << clockSliceNum));
        prevMode = 2;
        DEBUG_PRINT("x2\n");
    }
    else if (prevMode == 2 && mode == 1)
    {
        pwm_set_mask_enabled(0);
        pwm_config_set_clkdiv_int(&cfg_DA15, 4);
        pwm_config_set_clkdiv_int(&cfg_LRCK, 4);
        pwm_hw->slice[da15SliceNum].div = cfg_DA15.div;
        pwm_hw->slice[lrckSliceNum].div = cfg_LRCK.div;
        pwm_set_mask_enabled((1 << lrckSliceNum) | (1 << da15SliceNum) | (1 << clockSliceNum));
        prevMode = 1;
        DEBUG_PRINT("x1\n");
    }
}

void maybeReset()
{
    if (gpio_get(Pin::RESET) == 0)
    {
        DEBUG_PRINT("RESET!\n");
        pio_sm_set_enabled(pio0, SM::c_subq, false);
        pio_sm_set_enabled(pio1, SM::c_soct, false);
        pio_sm_set_enabled(pio1, SM::c_mechacon, false);
        pio_enable_sm_mask_in_sync(pio1, (1u << SM::c_scor) | (1u << SM::c_mechacon));

        mechacon_program_init(pio1, SM::c_mechacon, mechachonOffset, Pin::CMD_DATA);
        subqDelay = false;
        soct = false;

        gpio_init(Pin::SQSO);
        gpio_set_dir(Pin::SQSO, GPIO_OUT);
        gpio_put(Pin::SQSO, 0);

        uint64_t startTime = time_us_64();

        while ((time_us_64() - startTime) < 30000)
        {
            if (gpio_get(Pin::RESET) == 0)
            {
                startTime = time_us_64();
            }
        }

        while ((time_us_64() - startTime) < 30000)
        {
            if (gpio_get(Pin::CMD_CK) == 0)
            {
                startTime = time_us_64();
            }
        }

        pio_sm_set_enabled(pio1, SM::c_mechacon, true);
    }
}

void __time_critical_func(setSens)(uint what, bool new_value)
{
    sensData[what] = new_value;
    if (what == currentSens)
    {
        gpio_put(Pin::SENS, new_value);
    }
}

void __time_critical_func(updateMechSens)()
{
    while (!pio_sm_is_rx_fifo_empty(pio1, SM::c_mechacon))
    {
        uint c = pio_sm_get_blocking(pio1, SM::c_mechacon) >> 24;
        latched >>= 8;
        latched |= c << 16;
        currentSens = c >> 4;
        gpio_put(Pin::SENS, sensData[c >> 4]);
    }
}

int main()
{
    constexpr uint c_TrackMoveTime = 15; // uS

    uint64_t sledTimer = 0;
    uint64_t subqDelayTime = 0;

    int sectorPerTrackI = sectorsPerTrack(0);

    set_sys_clock_khz(271200, true);
    sleep_ms(5);

    initialize();
    coreReady[0] = true;

    while (!coreReady[1])
    {
        sleep_ms(1);
    }

    while (true)
    {
        // Limit Switch
        gpio_put(Pin::LMTSW, sector > 3000);

        // Update latching, output SENS
        if (mutex_try_enter(&mechaconMutex, 0))
        {
            updateMechSens();
            mutex_exit(&mechaconMutex);
        }

        // X1/X2 mode/speed
        maybeChangeMode();

        clampSectorTrackLimits();

        // Check for reset signal
        maybeReset();

        // Soct/Sled/seek/autoseq
        if (soct)
        {
            uint interrupts = save_and_disable_interrupts();
            // waiting for RX FIFO entry does not work.
            sleep_us(300);
            soct = false;
            pio_sm_set_enabled(pio1, SM::c_soct, false);
            restore_interrupts(interrupts);
        }
        else if (sledMoveDirection == SledMove::FORWARD)
        {
            if ((time_us_64() - sledTimer) > c_TrackMoveTime)
            {
                sledTimer = time_us_64();
                track++;
                sector = trackToSector(track);
                sectorForTrackUpdate = sector;

                if ((track - originalTrack) >= countTrack)
                {
                    originalTrack = track;
                    setSens(SENS::COUT, !sensData[SENS::COUT]);
                }
            }
        }
        else if (sledMoveDirection == SledMove::REVERSE)
        {
            if ((time_us_64() - sledTimer) > c_TrackMoveTime)
            {
                sledTimer = time_us_64();
                track--;
                sector = trackToSector(track);
                sectorForTrackUpdate = sector;
                if ((originalTrack - track) >= countTrack)
                {
                    originalTrack = track;
                    setSens(SENS::COUT, !sensData[SENS::COUT]);
                }
            }
        }
        else if (sensData[SENS::GFS])
        {
            if (sectorSending == sector && !subqDelay)
            {
                sector++;
                if ((sector - sectorForTrackUpdate) >= sectorPerTrackI)
                {
                    sectorForTrackUpdate = sector;
                    track++;
                    sectorPerTrackI = sectorsPerTrack(track);
                }
                subqDelay = true;
                subqDelayTime = time_us_64();
            }

            if (subqDelay && (time_us_64() - subqDelayTime) > 3333)
            {
                setSens(SENS::XBUSY, 0);
                subqDelay = false;
                start_subq(sector);
            }
        }
    }
}
