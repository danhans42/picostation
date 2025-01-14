#include "subq.h"

#include <stdint.h>
#include <stdio.h>

#include "disc_image.h"
#include "hardware/pio.h"
#include "logging.h"
#include "main.pio.h"
#include "pico/stdlib.h"
#include "picostation.h"
#include "utils.h"
#include "values.h"

#if DEBUG_SUBQ
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) while (0)
#endif

void picostation::SubQ::printf_subq(const uint8_t *data) {
    for (int i = 0; i < 12; i++) {
        DEBUG_PRINT("%02X ", data[i]);
    }
}

void picostation::SubQ::start_subq(const int sector) {
    const SubQ::Data tracksubq = m_discImage->generateSubQ(sector);
    subq_program_init(PIOInstance::SUBQ, SM::SUBQ, g_subqOffset, Pin::SQSO, Pin::SQCK);
    pio_sm_set_enabled(PIOInstance::SUBQ, SM::SUBQ, true);

    const uint sub[3] = {
        (uint)((tracksubq.raw[3] << 24) | (tracksubq.raw[2] << 16) | (tracksubq.raw[1] << 8) | (tracksubq.raw[0])),
        (uint)((tracksubq.raw[7] << 24) | (tracksubq.raw[6] << 16) | (tracksubq.raw[5] << 8) | (tracksubq.raw[4])),
        (uint)((tracksubq.raw[11] << 24) | (tracksubq.raw[10] << 16) | (tracksubq.raw[9] << 8) | (tracksubq.raw[8]))};
    pio_sm_put_blocking(PIOInstance::SUBQ, SM::SUBQ, sub[0]);
    pio_sm_put_blocking(PIOInstance::SUBQ, SM::SUBQ, sub[1]);
    pio_sm_put_blocking(PIOInstance::SUBQ, SM::SUBQ, sub[2]);

#if DEBUG_SUBQ
    if (sector % 50 == 0) {
        printf_subq(tracksubq.raw);
        DEBUG_PRINT("%d\n", sector);
    }
#endif
}
