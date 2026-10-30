/* Exercise the public register API; no ROM, BIOS, SDL or GPU is needed. */
#include "hal/dc_hardware.h"
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
    DCHardware *hw = dc_hw_init();
    const uint32_t revision = 0x005F689C;
    CHECK(hw != NULL);
    CHECK(dc_hw_read32(hw, revision) == 0x0B);
    CHECK(dc_hw_read32(hw, revision | 0x80000000) == 0x0B);
    CHECK(dc_hw_read32(hw, revision | 0xA0000000) == 0x0B);
    dc_hw_write32(hw, revision, 0xFFFFFFFF);
    CHECK(dc_hw_read32(hw, revision) == 0x0B);
    dc_hw_write32(hw, revision | 0xA0000000, 0);
    CHECK(dc_hw_read32(hw, revision | 0x80000000) == 0x0B);
    CHECK(dc_hw_read32(hw, revision - 4) == 0);
    CHECK(dc_hw_read32(hw, PVR_ID) == 0x17FD11DB);
    dc_hw_destroy(hw);
    puts("Holly revision: physical/P1/P2 reads, read-only behavior, adjacent register passed");
    return 0;
}
