/* Synthetic Maple DMA requests through the production controller path. */
#include "hal/dc_hardware.h"
#include "recompiler/sh4_cpu.h"
#include <string.h>
#include <stdio.h>
#ifndef _WIN32
/* Upstream defines the crash-handler installer only on Windows. */
void sh4_install_crash_handler(void) {}
#endif
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    return 1; \
} } while (0)

int main(void) {
    SH4CPU cpu;
    sh4_init(&cpu);
    sh4_set_cpu_ref(&cpu);
    DCHardware *hw = dc_hw_init();
    CHECK(hw != NULL);
    uint32_t *cmd = (uint32_t *)sh4_get_ram_ptr();
    uint32_t *resp = (uint32_t *)(sh4_get_ram_ptr() + 256);
    CHECK(cmd != NULL);
    /* One final DMA descriptor: DeviceRequest to port A, reply at RAM+256. */
    cmd[0] = 0x80000001; cmd[1] = 256; cmd[2] = 0x2001;
    resp[29] = 0xDEADBEEF;
    dc_maple_dma(hw, 0);
    CHECK((resp[0] & 0xFF) == 5 && (resp[0] >> 24) == 28);
    CHECK(resp[1] == 0x01000000 && resp[2] == 0xFE060F00);
    CHECK(resp[3] == 0 && resp[4] == 0);
    const unsigned char *info = (const unsigned char *)&resp[1];
    CHECK(info[16] == 0xFF && info[17] == 0);
    CHECK(!memcmp(info + 18, "Dreamcast Controller", 20));
    CHECK(!memcmp(info + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 52));
    CHECK(info[108] == 0xAE && info[109] == 1);
    CHECK(info[110] == 0xF4 && info[111] == 1);
    CHECK(resp[29] == 0xDEADBEEF); /* 28-word payload must not overrun. */
    MapleController *controller = dc_maple_get_controller(hw, 0);
    CHECK(controller != NULL);
    controller->buttons = 0xFFFD;
    controller->rtrig = 128; controller->ltrig = 64;
    controller->joyx = -64; controller->joyy = 32;
    cmd[2] = 0x2009; cmd[3] = 0x01000000; /* GetCondition */
    dc_maple_dma(hw, 0);
    CHECK(resp[2] == 0x4080FFFD && resp[3] == 0x8080A040);
    dc_hw_destroy(hw); sh4_set_cpu_ref(NULL); sh4_destroy(&cpu);
    puts("Maple controller: capability layout, payload bounds and analog condition passed");
    return 0;
}
