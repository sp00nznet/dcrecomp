/**
 * Dreamcast Hardware Abstraction Layer
 *
 * Provides interfaces for all Dreamcast hardware subsystems that the
 * statically recompiled game code interacts with.
 */

#ifndef DC_HARDWARE_H
#define DC_HARDWARE_H

#include <stdint.h>
#include <stdbool.h>

/* ========== PowerVR2 (HOLLY) GPU ========== */

/* PVR registers base: 0x005F8000 (mapped at 0xA05F8000 uncached) */
#define PVR_REG_BASE    0x005F8000

/* Key PVR registers */
#define PVR_ID          0x005F8000
#define PVR_REVISION    0x005F8004
#define PVR_SOFTRESET   0x005F8008
#define PVR_STARTRENDER 0x005F8014
#define PVR_PARAM_BASE  0x005F8020
#define PVR_REGION_BASE 0x005F802C
#define PVR_SPAN_SORT   0x005F8030
/* Scanout size we present at. The DC can do other modes; these cover
 * everything these games use. */
#define DC_SCREEN_W 640
#define DC_SCREEN_H 480

#define PVR_FB_R_CTRL   0x005F8044  /* scanout enable + pixel format */
#define PVR_FB_ADDR1    0x005F8050
#define PVR_FB_ADDR2    0x005F8054
#define PVR_FB_SIZE     0x005F805C
#define PVR_FB_RENDER   0x005F8060
#define PVR_VRAM_CFG    0x005F80A0
#define PVR_FOG_TABLE_BASE 0x005F8200
#define PVR_PALETTE_BASE   0x005F9000

/* Tile Accelerator (TA) registers */
#define TA_OL_BASE      0x005F8124
#define TA_ISP_BASE     0x005F8128
#define TA_OL_LIMIT     0x005F812C
#define TA_ISP_LIMIT    0x005F8130
#define TA_NEXT_OPB     0x005F8134
#define TA_ITP_CURRENT  0x005F8138
#define TA_GLOB_TILE_CLIP 0x005F813C
#define TA_ALLOC_CTRL   0x005F8140
#define TA_LIST_INIT    0x005F8144
#define TA_LIST_CONT    0x005F8160
#define TA_NEXT_OPB_INIT 0x005F8164

/* PVR polygon types */
typedef enum {
    PVR_LIST_OPAQUE = 0,
    PVR_LIST_OPAQUE_MOD = 1,
    PVR_LIST_TRANS = 2,
    PVR_LIST_TRANS_MOD = 3,
    PVR_LIST_PUNCHTHRU = 4,
} PVRListType;

/* PVR vertex format */
typedef struct {
    float x, y, z;
    float u, v;
    uint32_t base_color;
    uint32_t offset_color;
} PVRVertex;

/* ========== AICA Sound Processor ========== */

#define AICA_REG_BASE   0x00800000
#define AICA_CHAN_BASE   0x00800000
#define AICA_COMMON      0x00802800
#define AICA_ARM_RESET   0x00802C00

/* ========== Maple Bus (Controllers) ========== */

#define MAPLE_REG_BASE  0x005F6C00
#define MAPLE_DMA_ADDR  0x005F6C04  /* SB_MDSTAR - DMA command table address */
#define MAPLE_MDTSEL    0x005F6C10  /* SB_MDTSEL - DMA trigger select */
#define MAPLE_MDEN      0x005F6C14  /* SB_MDEN - DMA enable */
#define MAPLE_MDST      0x005F6C18  /* SB_MDST - DMA start/status */
#define MAPLE_MSYS      0x005F6C80  /* SB_MSYS - Maple system control */
#define MAPLE_MMSEL     0x005F6C8C  /* SB_MMSEL - Maple multi-select */

/* Controller button masks */
#define CONT_C          (1 << 0)
#define CONT_B          (1 << 1)
#define CONT_A          (1 << 2)
#define CONT_START      (1 << 3)
#define CONT_DPAD_UP    (1 << 4)
#define CONT_DPAD_DOWN  (1 << 5)
#define CONT_DPAD_LEFT  (1 << 6)
#define CONT_DPAD_RIGHT (1 << 7)
#define CONT_Z          (1 << 8)
#define CONT_Y          (1 << 9)
#define CONT_X          (1 << 10)
#define CONT_D          (1 << 11)

/* Controller state */
typedef struct {
    uint32_t buttons;   /* Button state (active LOW) */
    uint8_t ltrig;      /* Left trigger (0-255) */
    uint8_t rtrig;      /* Right trigger (0-255) */
    int8_t joyx;        /* Joystick X (-128 to 127) */
    int8_t joyy;        /* Joystick Y (-128 to 127) */
    int8_t joy2x;       /* Second joystick X (if present) */
    int8_t joy2y;       /* Second joystick Y (if present) */
} MapleController;

/* ========== GD-ROM Drive ========== */

#define GDROM_REG_BASE  0x005F7000

/* ========== System Control ========== */

#define SB_REG_BASE     0x005F6800

/* System Board registers */
#define SB_C2DSTAT      0x005F6800
#define SB_C2DLEN       0x005F6804
#define SB_C2DST        0x005F6808
#define SB_SDSTAG       0x005F6810
#define SB_SDSTAR       0x005F6814
#define SB_SDLEN        0x005F6818
#define SB_SDDIR        0x005F681C
#define SB_SDST         0x005F6820
#define SB_ISTNRM       0x005F6900  /* Normal interrupt status */
#define SB_ISTEXT       0x005F6904  /* External interrupt status */
#define SB_ISTERR       0x005F6908  /* Error interrupt status */
#define SB_IML2NRM      0x005F6910
#define SB_IML2EXT      0x005F6914
#define SB_IML2ERR      0x005F6918
#define SB_IML4NRM      0x005F6920
#define SB_IML4EXT      0x005F6924
#define SB_IML4ERR      0x005F6928
#define SB_IML6NRM      0x005F6930
#define SB_IML6EXT      0x005F6934
#define SB_IML6ERR      0x005F6938
#define SB_PDTNRM       0x005F6940
#define SB_PDTEXT       0x005F6944

/* PVR-DMA (System Bus DMA to/from PVR/TA) */
#define SB_PDSTAP       0x005F7C00  /* PVR DMA PVR-side start addr */
#define SB_PDSTAR       0x005F7C04  /* PVR DMA system memory start addr */
#define SB_PDLEN        0x005F7C08  /* PVR DMA length */
#define SB_PDDIR        0x005F7C0C  /* PVR DMA direction (0=to PVR, 1=from PVR) */
#define SB_PDTSEL       0x005F7C10  /* PVR DMA trigger select */
#define SB_PDEN         0x005F7C14  /* PVR DMA enable */
#define SB_PDST         0x005F7C18  /* PVR DMA start trigger */

/* Sort-DMA registers already defined above: SB_SDSTAG..SB_SDST */

/* ========== Hardware Init/Access Functions ========== */

typedef struct DCHardware DCHardware;

/* Initialize all hardware subsystems */
DCHardware* dc_hw_init(void);

/* Destroy hardware state */
void dc_hw_destroy(DCHardware *hw);

/* Hardware register read/write */
uint32_t dc_hw_read32(DCHardware *hw, uint32_t addr);
void dc_hw_write32(DCHardware *hw, uint32_t addr, uint32_t val);

/* GPU operations */
void dc_pvr_init(DCHardware *hw);
void dc_pvr_start_render(DCHardware *hw);
void dc_pvr_submit_vertex(DCHardware *hw, const PVRVertex *vtx);
void dc_pvr_begin_list(DCHardware *hw, PVRListType type);
void dc_pvr_end_list(DCHardware *hw);
void dc_pvr_wait_vblank(DCHardware *hw);

/* Controller operations */
void dc_maple_init(DCHardware *hw);
void dc_maple_poll(DCHardware *hw);
void dc_maple_dma(DCHardware *hw, uint32_t mdstar);
MapleController* dc_maple_get_controller(DCHardware *hw, int port);

/* Sound operations */
void dc_aica_init(DCHardware *hw);
void dc_aica_write_channel(DCHardware *hw, int ch, uint32_t offset, uint32_t val);
void dc_aica_update(DCHardware *hw);

/* GD-ROM operations */
void dc_gdrom_init(DCHardware *hw);
int dc_gdrom_read_sectors(DCHardware *hw, uint32_t lba, uint32_t count, void *buf);

#endif /* DC_HARDWARE_H */
