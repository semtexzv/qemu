/*
 * ST M25P80 emulator. Emulate all SPI flash devices based on the m25p80 command
 * set. Known devices table current as of Jun/2012 and taken from linux.
 * See drivers/mtd/devices/m25p80.c.
 *
 * Copyright (C) 2011 Edgar E. Iglesias <edgar.iglesias@gmail.com>
 * Copyright (C) 2012 Peter A. G. Crosthwaite <peter.crosthwaite@petalogix.com>
 * Copyright (C) 2012 PetaLogix
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) a later version of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "hw/ssi/ssi.h"
#include "qemu/bitops.h"
#include "qemu/log.h"

#ifndef M25P80_ERR_DEBUG
#define M25P80_ERR_DEBUG 0
#endif

#define DB_PRINT_L(level, ...) do { \
    if (M25P80_ERR_DEBUG > (level)) { \
        fprintf(stderr,  ": %s: ", __func__); \
        fprintf(stderr, ## __VA_ARGS__); \
    } \
} while (0);

/* Fields for FlashPartInfo->flags */

/* erase capabilities */
#define ER_4K 1
#define ER_32K 2
/* set to allow the page program command to write 0s back to 1. Useful for
 * modelling EEPROM with SPI flash command set
 */
#define EEPROM 0x100

/* 16 MiB max in 3 byte address mode */
#define MAX_3BYTES_SIZE 0x1000000

#define SPI_NOR_MAX_ID_LEN 6

typedef struct FlashPartInfo {
    const char *part_name;
    /*
     * This array stores the ID bytes.
     * The first three bytes are the JEDIC ID.
     * JEDEC ID zero means "no ID" (mostly older chips).
     */
    uint8_t id[SPI_NOR_MAX_ID_LEN];
    uint8_t id_len;
    /* there is confusion between manufacturers as to what a sector is. In this
     * device model, a "sector" is the size that is erased by the ERASE_SECTOR
     * command (opcode 0xd8).
     */
    uint32_t sector_size;
    uint32_t n_sectors;
    uint32_t page_size;
    uint16_t flags;
} FlashPartInfo;

/* adapted from linux */
/* Used when the "_ext_id" is two bytes at most */
#define INFO(_part_name, _jedec_id, _ext_id, _sector_size, _n_sectors, _flags)\
    .part_name = _part_name,\
    .id = {\
        ((_jedec_id) >> 16) & 0xff,\
        ((_jedec_id) >> 8) & 0xff,\
        (_jedec_id) & 0xff,\
        ((_ext_id) >> 8) & 0xff,\
        (_ext_id) & 0xff,\
          },\
    .id_len = (!(_jedec_id) ? 0 : (3 + ((_ext_id) ? 2 : 0))),\
    .sector_size = (_sector_size),\
    .n_sectors = (_n_sectors),\
    .page_size = 256,\
    .flags = (_flags),

#define INFO6(_part_name, _jedec_id, _ext_id, _sector_size, _n_sectors, _flags)\
    .part_name = _part_name,\
    .id = {\
        ((_jedec_id) >> 16) & 0xff,\
        ((_jedec_id) >> 8) & 0xff,\
        (_jedec_id) & 0xff,\
        ((_ext_id) >> 16) & 0xff,\
        ((_ext_id) >> 8) & 0xff,\
        (_ext_id) & 0xff,\
          },\
    .id_len = 6,\
    .sector_size = (_sector_size),\
    .n_sectors = (_n_sectors),\
    .page_size = 256,\
    .flags = (_flags),\

#define JEDEC_NUMONYX 0x20
#define JEDEC_WINBOND 0xEF
#define JEDEC_SPANSION 0x01

/* Numonyx (Micron) Configuration register macros */
#define VCFG_DUMMY 0x1
#define VCFG_WRAP_SEQUENTIAL 0x2
#define NVCFG_XIP_MODE_DISABLED (7 << 9)
#define NVCFG_XIP_MODE_MASK (7 << 9)
#define VCFG_XIP_MODE_ENABLED (1 << 3)
#define CFG_DUMMY_CLK_LEN 4
#define NVCFG_DUMMY_CLK_POS 12
#define VCFG_DUMMY_CLK_POS 4
#define EVCFG_OUT_DRIVER_STRENGHT_DEF 7
#define EVCFG_VPP_ACCELERATOR (1 << 3)
#define EVCFG_RESET_HOLD_ENABLED (1 << 4)
#define NVCFG_DUAL_IO_MASK (1 << 2)
#define EVCFG_DUAL_IO_ENABLED (1 << 6)
#define NVCFG_QUAD_IO_MASK (1 << 3)
#define EVCFG_QUAD_IO_ENABLED (1 << 7)
#define NVCFG_4BYTE_ADDR_MASK (1 << 0)
#define NVCFG_LOWER_SEGMENT_MASK (1 << 1)

/* Numonyx (Micron) Flag Status Register macros */
#define FSR_4BYTE_ADDR_MODE_ENABLED 0x1
#define FSR_FLASH_READY (1 << 7)

static const FlashPartInfo known_devices[] = {
    /* Atmel -- some are (confusingly) marketed as "DataFlash" */
    { INFO("at25fs010",   0x1f6601,      0,  32 << 10,   4, ER_4K) },
    { INFO("at25fs040",   0x1f6604,      0,  64 << 10,   8, ER_4K) },

    { INFO("at25df041a",  0x1f4401,      0,  64 << 10,   8, ER_4K) },
    { INFO("at25df321a",  0x1f4701,      0,  64 << 10,  64, ER_4K) },
    { INFO("at25df641",   0x1f4800,      0,  64 << 10, 128, ER_4K) },

    { INFO("at26f004",    0x1f0400,      0,  64 << 10,   8, ER_4K) },
    { INFO("at26df081a",  0x1f4501,      0,  64 << 10,  16, ER_4K) },
    { INFO("at26df161a",  0x1f4601,      0,  64 << 10,  32, ER_4K) },
    { INFO("at26df321",   0x1f4700,      0,  64 << 10,  64, ER_4K) },

    { INFO("at45db081d",  0x1f2500,      0,  64 << 10,  16, ER_4K) },

    /* Atmel EEPROMS - it is assumed, that don't care bit in command
     * is set to 0. Block protection is not supported.
     */
    { INFO("at25128a-nonjedec", 0x0,     0,         1, 131072, EEPROM) },
    { INFO("at25256a-nonjedec", 0x0,     0,         1, 262144, EEPROM) },

    /* EON -- en25xxx */
    { INFO("en25f32",     0x1c3116,      0,  64 << 10,  64, ER_4K) },
    { INFO("en25p32",     0x1c2016,      0,  64 << 10,  64, 0) },
    { INFO("en25q32b",    0x1c3016,      0,  64 << 10,  64, 0) },
    { INFO("en25p64",     0x1c2017,      0,  64 << 10, 128, 0) },
    { INFO("en25q64",     0x1c3017,      0,  64 << 10, 128, ER_4K) },

    /* GigaDevice */
    { INFO("gd25q32",     0xc84016,      0,  64 << 10,  64, ER_4K) },
    { INFO("gd25q64",     0xc84017,      0,  64 << 10, 128, ER_4K) },

    /* Intel/Numonyx -- xxxs33b */
    { INFO("160s33b",     0x898911,      0,  64 << 10,  32, 0) },
    { INFO("320s33b",     0x898912,      0,  64 << 10,  64, 0) },
    { INFO("640s33b",     0x898913,      0,  64 << 10, 128, 0) },
    { INFO("n25q064",     0x20ba17,      0,  64 << 10, 128, 0) },

    /* Macronix */
    { INFO("mx25l2005a",  0xc22012,      0,  64 << 10,   4, ER_4K) },
    { INFO("mx25l4005a",  0xc22013,      0,  64 << 10,   8, ER_4K) },
    { INFO("mx25l8005",   0xc22014,      0,  64 << 10,  16, 0) },
    { INFO("mx25l1606e",  0xc22015,      0,  64 << 10,  32, ER_4K) },
    { INFO("mx25l3205d",  0xc22016,      0,  64 << 10,  64, 0) },
    { INFO("mx25l6405d",  0xc22017,      0,  64 << 10, 128, 0) },
    { INFO("mx25l12805d", 0xc22018,      0,  64 << 10, 256, 0) },
    { INFO("mx25l12855e", 0xc22618,      0,  64 << 10, 256, 0) },
    { INFO("mx25l25635e", 0xc22019,      0,  64 << 10, 512, 0) },
    { INFO("mx25l25655e", 0xc22619,      0,  64 << 10, 512, 0) },

    /* Micron */
    { INFO("n25q032a11",  0x20bb16,      0,  64 << 10,  64, ER_4K) },
    { INFO("n25q032a13",  0x20ba16,      0,  64 << 10,  64, ER_4K) },
    { INFO("n25q064a11",  0x20bb17,      0,  64 << 10, 128, ER_4K) },
    { INFO("n25q064a13",  0x20ba17,      0,  64 << 10, 128, ER_4K) },
    { INFO("n25q128a11",  0x20bb18,      0,  64 << 10, 256, ER_4K) },
    { INFO("n25q128a13",  0x20ba18,      0,  64 << 10, 256, ER_4K) },
    { INFO("n25q256a11",  0x20bb19,      0,  64 << 10, 512, ER_4K) },
    { INFO("n25q256a13",  0x20ba19,      0,  64 << 10, 512, ER_4K) },

    /* Spansion -- single (large) sector size only, at least
     * for the chips listed here (without boot sectors).
     */
    { INFO("s25sl032p",   0x010215, 0x4d00,  64 << 10,  64, ER_4K) },
    { INFO("s25sl064p",   0x010216, 0x4d00,  64 << 10, 128, ER_4K) },
    { INFO("s25fl256s0",  0x010219, 0x4d00, 256 << 10, 128, 0) },
    { INFO("s25fl256s1",  0x010219, 0x4d01,  64 << 10, 512, 0) },
    { INFO("s25fl512s",   0x010220, 0x4d00, 256 << 10, 256, 0) },
    { INFO("s70fl01gs",   0x010221, 0x4d00, 256 << 10, 256, 0) },
    { INFO("s25sl12800",  0x012018, 0x0300, 256 << 10,  64, 0) },
    { INFO("s25sl12801",  0x012018, 0x0301,  64 << 10, 256, 0) },
    { INFO("s25fl129p0",  0x012018, 0x4d00, 256 << 10,  64, 0) },
    { INFO("s25fl129p1",  0x012018, 0x4d01,  64 << 10, 256, 0) },
    { INFO("s25sl004a",   0x010212,      0,  64 << 10,   8, 0) },
    { INFO("s25sl008a",   0x010213,      0,  64 << 10,  16, 0) },
    { INFO("s25sl016a",   0x010214,      0,  64 << 10,  32, 0) },
    { INFO("s25sl032a",   0x010215,      0,  64 << 10,  64, 0) },
    { INFO("s25sl064a",   0x010216,      0,  64 << 10, 128, 0) },
    { INFO("s25fl016k",   0xef4015,      0,  64 << 10,  32, ER_4K | ER_32K) },
    { INFO("s25fl064k",   0xef4017,      0,  64 << 10, 128, ER_4K | ER_32K) },

    /* SST -- large erase sizes are "overlays", "sectors" are 4<< 10 */
    { INFO("sst25vf040b", 0xbf258d,      0,  64 << 10,   8, ER_4K) },
    { INFO("sst25vf080b", 0xbf258e,      0,  64 << 10,  16, ER_4K) },
    { INFO("sst25vf016b", 0xbf2541,      0,  64 << 10,  32, ER_4K) },
    { INFO("sst25vf032b", 0xbf254a,      0,  64 << 10,  64, ER_4K) },
    { INFO("sst25wf512",  0xbf2501,      0,  64 << 10,   1, ER_4K) },
    { INFO("sst25wf010",  0xbf2502,      0,  64 << 10,   2, ER_4K) },
    { INFO("sst25wf020",  0xbf2503,      0,  64 << 10,   4, ER_4K) },
    { INFO("sst25wf040",  0xbf2504,      0,  64 << 10,   8, ER_4K) },
    { INFO("sst25wf080",  0xbf2505,      0,  64 << 10,  16, ER_4K) },

    /* ST Microelectronics -- newer production may have feature updates */
    { INFO("m25p05",      0x202010,      0,  32 << 10,   2, 0) },
    { INFO("m25p10",      0x202011,      0,  32 << 10,   4, 0) },
    { INFO("m25p20",      0x202012,      0,  64 << 10,   4, 0) },
    { INFO("m25p40",      0x202013,      0,  64 << 10,   8, 0) },
    { INFO("m25p80",      0x202014,      0,  64 << 10,  16, 0) },
    { INFO("m25p16",      0x202015,      0,  64 << 10,  32, 0) },
    { INFO("m25p32",      0x202016,      0,  64 << 10,  64, 0) },
    { INFO("m25p64",      0x202017,      0,  64 << 10, 128, 0) },
    { INFO("m25p128",     0x202018,      0, 256 << 10,  64, 0) },
    { INFO("n25q032",     0x20ba16,      0,  64 << 10,  64, 0) },

    { INFO("m45pe10",     0x204011,      0,  64 << 10,   2, 0) },
    { INFO("m45pe80",     0x204014,      0,  64 << 10,  16, 0) },
    { INFO("m45pe16",     0x204015,      0,  64 << 10,  32, 0) },

    { INFO("m25pe20",     0x208012,      0,  64 << 10,   4, 0) },
    { INFO("m25pe80",     0x208014,      0,  64 << 10,  16, 0) },
    { INFO("m25pe16",     0x208015,      0,  64 << 10,  32, ER_4K) },

    { INFO("m25px32",     0x207116,      0,  64 << 10,  64, ER_4K) },
    { INFO("m25px32-s0",  0x207316,      0,  64 << 10,  64, ER_4K) },
    { INFO("m25px32-s1",  0x206316,      0,  64 << 10,  64, ER_4K) },
    { INFO("m25px64",     0x207117,      0,  64 << 10, 128, 0) },

    /* Winbond -- w25x "blocks" are 64k, "sectors" are 4KiB */
    { INFO("w25x10",      0xef3011,      0,  64 << 10,   2, ER_4K) },
    { INFO("w25x20",      0xef3012,      0,  64 << 10,   4, ER_4K) },
    { INFO("w25x40",      0xef3013,      0,  64 << 10,   8, ER_4K) },
    { INFO("w25x80",      0xef3014,      0,  64 << 10,  16, ER_4K) },
    { INFO("w25x16",      0xef3015,      0,  64 << 10,  32, ER_4K) },
    { INFO("w25x32",      0xef3016,      0,  64 << 10,  64, ER_4K) },
    { INFO("w25q32",      0xef4016,      0,  64 << 10,  64, ER_4K) },
    { INFO("w25q32dw",    0xef6016,      0,  64 << 10,  64, ER_4K) },
    { INFO("w25x64",      0xef3017,      0,  64 << 10, 128, ER_4K) },
    { INFO("w25q64",      0xef4017,      0,  64 << 10, 128, ER_4K) },
    { INFO("w25q80",      0xef5014,      0,  64 << 10,  16, ER_4K) },
    { INFO("w25q80bl",    0xef4014,      0,  64 << 10,  16, ER_4K) },
    { INFO("w25q256",     0xef4019,      0,  64 << 10, 512, ER_4K) },

    { INFO("n25q128",      0x20ba18,      0,  64 << 10, 256, 0) },
    { INFO("n25q256a",     0x20ba19,      0,  64 << 10, 512, ER_4K) },
    { INFO("n25q512a",     0x20ba20,      0,  64 << 10, 1024, ER_4K) },
};

typedef enum {
    NOP = 0,
    WRSR = 0x1,
    WRDI = 0x4,
    RDSR = 0x5,
    WREN = 0x6,
    JEDEC_READ = 0x9f,
    BULK_ERASE = 0xc7,
    READ_FSR = 0x70,

    READ = 0x03,
    READ4 = 0x13,
    FAST_READ = 0x0b,
    FAST_READ4 = 0x0c,
    DOR = 0x3b,
    DOR4 = 0x3c,
    QOR = 0x6b,
    QOR4 = 0x6c,
    DIOR = 0xbb,
    DIOR4 = 0xbc,
    QIOR = 0xeb,
    QIOR4 = 0xec,

    PP = 0x02,
    PP4 = 0x12,
    PP4_4 = 0x3e,
    DPP = 0xa2,
    QPP = 0x32,

    ERASE_4K = 0x20,
    ERASE4_4K = 0x21,
    ERASE_32K = 0x52,
    ERASE4_32K = 0x5c,
    ERASE_SECTOR = 0xd8,
    ERASE4_SECTOR = 0xdc,

    EN_4BYTE_ADDR = 0xB7,
    EX_4BYTE_ADDR = 0xE9,

    EXTEND_ADDR_READ = 0xC8,
    EXTEND_ADDR_WRITE = 0xC5,

    RESET_ENABLE = 0x66,
    RESET_MEMORY = 0x99,

    RNVCR = 0xB5,
    WNVCR = 0xB1,

    RVCR = 0x85,
    WVCR = 0x81,

    REVCR = 0x65,
    WEVCR = 0x61,
} FlashCMD;

typedef enum {
    STATE_IDLE,
    STATE_PAGE_PROGRAM,
    STATE_READ,
    STATE_COLLECTING_DATA,
    STATE_COLLECTING_VAR_LEN_DATA,
    STATE_READING_DATA,
} CMDState;

typedef enum {
    MAN_SPANSION,
    MAN_MACRONIX,
    MAN_NUMONYX,
    MAN_WINBOND,
    MAN_GENERIC,
} Manufacturer;

typedef struct Flash {
    SSISlave parent_obj;

    BlockBackend *blk;

    uint8_t *storage;
    uint32_t size;
    int page_size;

    uint8_t state;
    uint8_t data[16];
    uint32_t len;
    uint32_t pos;
    uint8_t needed_bytes;
    uint8_t cmd_in_progress;
    uint64_t cur_addr;
    uint32_t nonvolatile_cfg;
    uint32_t volatile_cfg;
    uint32_t enh_volatile_cfg;
    bool write_enable;
    bool four_bytes_address_mode;
    bool reset_enable;
    uint8_t ear;

    int64_t dirty_page;

    const FlashPartInfo *pi;

} Flash;

typedef struct M25P80Class {
    SSISlaveClass parent_class;
    FlashPartInfo *pi;
} M25P80Class;

#define TYPE_M25P80 "m25p80-generic"
#define M25P80(obj) \
     OBJECT_CHECK(Flash, (obj), TYPE_M25P80)
#define M25P80_CLASS(klass) \
     OBJECT_CLASS_CHECK(M25P80Class, (klass), TYPE_M25P80)
#define M25P80_GET_CLASS(obj) \
     OBJECT_GET_CLASS(M25P80Class, (obj), TYPE_M25P80)

static inline Manufacturer get_man(Flash *s)
{
    switch (s->pi->id[0]) {
    case 0x20:
        return MAN_NUMONYX;
    case 0xEF:
        return MAN_WINBOND;
    case 0x01:
        return MAN_SPANSION;
    case 0xC2:
        return MAN_MACRONIX;
    default:
        return MAN_GENERIC;
    }
}

static void blk_sync_complete(void *opaque, int ret)
{
    /* do nothing. Masters do not directly interact with the backing store,
     * only the working copy so no mutexing required.
     */
}

static void flash_sync_page(Flash *s, int page)
{
    QEMUIOVector iov;

    if (!s->blk || blk_is_read_only(s->blk)) {
        return;
    }

    qemu_iovec_init(&iov, 1);
    qemu_iovec_add(&iov, s->storage + page * s->pi->page_size,
                   s->pi->page_size);
    blk_aio_pwritev(s->blk, page * s->pi->page_size, &iov, 0,
                    blk_sync_complete, NULL);
}

static inline void flash_sync_area(Flash *s, int64_t off, int64_t len)
{
    QEMUIOVector iov;

    if (!s->blk || blk_is_read_only(s->blk)) {
        return;
    }

    assert(!(len % BDRV_SECTOR_SIZE));
    qemu_iovec_init(&iov, 1);
    qemu_iovec_add(&iov, s->storage + off, len);
    blk_aio_pwritev(s->blk, off, &iov, 0, blk_sync_complete, NULL);
}

static void flash_erase(Flash *s, int offset, FlashCMD cmd)
{
    uint32_t len;
    uint8_t capa_to_assert = 0;

    switch (cmd) {
    case ERASE_4K:
    case ERASE4_4K:
        len = 4 << 10;
        capa_to_assert = ER_4K;
        break;
    case ERASE_32K:
    case ERASE4_32K:
        len = 32 << 10;
        capa_to_assert = ER_32K;
        break;
    case ERASE_SECTOR:
    case ERASE4_SECTOR:
        len = s->pi->sector_size;
        break;
    case BULK_ERASE:
        len = s->size;
        break;
    default:
        abort();
    }

    DB_PRINT_L(0, "offset = %#x, len = %d\n", offset, len);
    if ((s->pi->flags & capa_to_assert) != capa_to_assert) {
        qemu_log_mask(LOG_GUEST_ERROR, "M25P80: %d erase size not supported by"
                      " device\n", len);
    }

    if (!s->write_enable) {
        qemu_log_mask(LOG_GUEST_ERROR, "M25P80: erase with write protect!\n");
        return;
    }
    memset(s->storage + offset, 0xff, len);
    flash_sync_area(s, offset, len);
}

static inline void flash_sync_dirty(Flash *s, int64_t newpage)
{
    if (s->dirty_page >= 0 && s->dirty_page != newpage) {
        flash_sync_page(s, s->dirty_page);
        s->dirty_page = newpage;
    }
}

static inline
void flash_write8(Flash *s, uint64_t addr, uint8_t data)
{
    int64_t page = addr / s->pi->page_size;
    uint8_t prev = s->storage[s->cur_addr];

    if (!s->write_enable) {
        qemu_log_mask(LOG_GUEST_ERROR, "M25P80: write with write protect!\n");
    }

    if ((prev ^ data) & data) {
        DB_PRINT_L(1, "programming zero to one! addr=%" PRIx64 "  %" PRIx8
                   " -> %" PRIx8 "\n", addr, prev, data);
    }

    if (s->pi->flags & EEPROM) {
        s->storage[s->cur_addr] = data;
    } else {
        s->storage[s->cur_addr] &= data;
    }

    flash_sync_dirty(s, page);
    s->dirty_page = page;
}

static inline int get_addr_length(Flash *s)
{
   /* check if eeprom is in use */
    if (s->pi->flags == EEPROM) {
        return 2;
    }

   switch (s->cmd_in_progress) {
   case PP4:
   case PP4_4:
   case READ4:
   case QIOR4:
   case ERASE4_4K:
   case ERASE4_32K:
   case ERASE4_SECTOR:
   case FAST_READ4:
   case DOR4:
   case QOR4:
   case DIOR4:
       return 4;
   default:
       return s->four_bytes_address_mode ? 4 : 3;
   }
}

static void complete_collecting_data(Flash *s)
{
    int i;

    s->cur_addr = 0;

    for (i = 0; i < get_addr_length(s); ++i) {
        s->cur_addr <<= 8;
        s->cur_addr |= s->data[i];
    }

    if (get_addr_length(s) == 3) {
        s->cur_addr += s->ear * MAX_3BYTES_SIZE;
    }

    s->state = STATE_IDLE;

    switch (s->cmd_in_progress) {
    case DPP:
    case QPP:
    case PP:
    case PP4:
    case PP4_4:
        s->state = STATE_PAGE_PROGRAM;
        break;
    case READ:
    case READ4:
    case FAST_READ:
    case FAST_READ4:
    case DOR:
    case DOR4:
    case QOR:
    case QOR4:
    case DIOR:
    case DIOR4:
    case QIOR:
    case QIOR4:
        s->state = STATE_READ;
        break;
    case ERASE_4K:
    case ERASE4_4K:
    case ERASE_32K:
    case ERASE4_32K:
    case ERASE_SECTOR:
    case ERASE4_SECTOR:
        flash_erase(s, s->cur_addr, s->cmd_in_progress);
        break;
    case WRSR:
        if (s->write_enable) {
            s->write_enable = false;
        }
        break;
    case EXTEND_ADDR_WRITE:
        s->ear = s->data[0];
        break;
    case WNVCR:
        s->nonvolatile_cfg = s->data[0] | (s->data[1] << 8);
        break;
    case WVCR:
        s->volatile_cfg = s->data[0];
        break;
    case WEVCR:
        s->enh_volatile_cfg = s->data[0];
        break;
    default:
        break;
    }
}

static void reset_memory(Flash *s)
{
    s->cmd_in_progress = NOP;
    s->cur_addr = 0;
    s->ear = 0;
    s->four_bytes_address_mode = false;
    s->len = 0;
    s->needed_bytes = 0;
    s->pos = 0;
    s->state = STATE_IDLE;
    s->write_enable = false;
    s->reset_enable = false;

    switch (get_man(s)) {
    case MAN_NUMONYX:
        s->volatile_cfg = 0;
        s->volatile_cfg |= VCFG_DUMMY;
        s->volatile_cfg |= VCFG_WRAP_SEQUENTIAL;
        if ((s->nonvolatile_cfg & NVCFG_XIP_MODE_MASK)
                                != NVCFG_XIP_MODE_DISABLED) {
            s->volatile_cfg |= VCFG_XIP_MODE_ENABLED;
        }
        s->volatile_cfg |= deposit32(s->volatile_cfg,
                            VCFG_DUMMY_CLK_POS,
                            CFG_DUMMY_CLK_LEN,
                            extract32(s->nonvolatile_cfg,
                                        NVCFG_DUMMY_CLK_POS,
                                        CFG_DUMMY_CLK_LEN)
                            );

        s->enh_volatile_cfg = 0;
        s->enh_volatile_cfg |= EVCFG_OUT_DRIVER_STRENGHT_DEF;
        s->enh_volatile_cfg |= EVCFG_VPP_ACCELERATOR;
        s->enh_volatile_cfg |= EVCFG_RESET_HOLD_ENABLED;
        if (s->nonvolatile_cfg & NVCFG_DUAL_IO_MASK) {
            s->enh_volatile_cfg |= EVCFG_DUAL_IO_ENABLED;
        }
        if (s->nonvolatile_cfg & NVCFG_QUAD_IO_MASK) {
            s->enh_volatile_cfg |= EVCFG_QUAD_IO_ENABLED;
        }
        if (!(s->nonvolatile_cfg & NVCFG_4BYTE_ADDR_MASK)) {
            s->four_bytes_address_mode = true;
        }
        if (!(s->nonvolatile_cfg & NVCFG_LOWER_SEGMENT_MASK)) {
            s->ear = s->size / MAX_3BYTES_SIZE - 1;
        }
        break;
    default:
        break;
    }

    DB_PRINT_L(0, "Reset done.\n");
}

static void decode_new_cmd(Flash *s, uint32_t value)
{
    s->cmd_in_progress = value;
    int i;
    DB_PRINT_L(0, "decoded new command:%x\n", value);

    if (value != RESET_MEMORY) {
        s->reset_enable = false;
    }

    switch (value) {

    case ERASE_4K:
    case ERASE4_4K:
    case ERASE_32K:
    case ERASE4_32K:
    case ERASE_SECTOR:
    case ERASE4_SECTOR:
    case READ:
    case READ4:
    case DPP:
    case QPP:
    case PP:
    case PP4:
    case PP4_4:
        s->needed_bytes = get_addr_length(s);
        s->pos = 0;
        s->len = 0;
        s->state = STATE_COLLECTING_DATA;
        break;

    case FAST_READ:
    case FAST_READ4:
    case DOR:
    case DOR4:
    case QOR:
    case QOR4:
        s->needed_bytes = get_addr_length(s);
        switch (get_man(s)) {
        case MAN_NUMONYX:
            s->needed_bytes += extract32(s->volatile_cfg, 4, 4);
            break;
        default:
            break;
        }
        s->pos = 0;
        s->len = 0;
        s->state = STATE_COLLECTING_DATA;
        break;

    case DIOR:
    case DIOR4:
        switch (get_man(s)) {
        case MAN_WINBOND:
        case MAN_SPANSION:
            s->needed_bytes = 4;
            break;
        default:
            s->needed_bytes = get_addr_length(s);
            /* Dummy cycles modeled with bytes writes instead of bits */
            s->needed_bytes += extract32(s->volatile_cfg, 4, 4);
        }
        s->pos = 0;
        s->len = 0;
        s->state = STATE_COLLECTING_DATA;
        break;

    case QIOR:
    case QIOR4:
        switch (get_man(s)) {
        case MAN_WINBOND:
        case MAN_SPANSION:
            s->needed_bytes = 6;
            break;
        default:
            s->needed_bytes = get_addr_length(s);
            /* Dummy cycles modeled with bytes writes instead of bits */
            s->needed_bytes += extract32(s->volatile_cfg, 4, 4);
        }
        s->pos = 0;
        s->len = 0;
        s->state = STATE_COLLECTING_DATA;
        break;

    case WRSR:
        if (s->write_enable) {
            s->needed_bytes = 1;
            s->pos = 0;
            s->len = 0;
            s->state = STATE_COLLECTING_DATA;
        }
        break;

    case WRDI:
        s->write_enable = false;
        break;
    case WREN:
        s->write_enable = true;
        break;

    case RDSR:
        s->data[0] = (!!s->write_enable) << 1;
        s->pos = 0;
        s->len = 1;
        s->state = STATE_READING_DATA;
        break;

    case READ_FSR:
        s->data[0] = FSR_FLASH_READY;
        if (s->four_bytes_address_mode) {
            s->data[0] |= FSR_4BYTE_ADDR_MODE_ENABLED;
        }
        s->pos = 0;
        s->len = 1;
        s->state = STATE_READING_DATA;
        break;

    case JEDEC_READ:
        DB_PRINT_L(0, "populated jedec code\n");
        for (i = 0; i < s->pi->id_len; i++) {
            s->data[i] = s->pi->id[i];
        }

        s->len = s->pi->id_len;
        s->pos = 0;
        s->state = STATE_READING_DATA;
        break;

    case BULK_ERASE:
        if (s->write_enable) {
            DB_PRINT_L(0, "chip erase\n");
            flash_erase(s, 0, BULK_ERASE);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "M25P80: chip erase with write "
                          "protect!\n");
        }
        break;
    case NOP:
        break;
    case EN_4BYTE_ADDR:
        s->four_bytes_address_mode = true;
        break;
    case EX_4BYTE_ADDR:
        s->four_bytes_address_mode = false;
        break;
    case EXTEND_ADDR_READ:
        s->data[0] = s->ear;
        s->pos = 0;
        s->len = 1;
        s->state = STATE_READING_DATA;
        break;
    case EXTEND_ADDR_WRITE:
        if (s->write_enable) {
            s->needed_bytes = 1;
            s->pos = 0;
            s->len = 0;
            s->state = STATE_COLLECTING_DATA;
        }
        break;
    case RNVCR:
        s->data[0] = s->nonvolatile_cfg & 0xFF;
        s->data[1] = (s->nonvolatile_cfg >> 8) & 0xFF;
        s->pos = 0;
        s->len = 2;
        s->state = STATE_READING_DATA;
        break;
    case WNVCR:
        if (s->write_enable) {
            s->needed_bytes = 2;
            s->pos = 0;
            s->len = 0;
            s->state = STATE_COLLECTING_DATA;
        }
        break;
    case RVCR:
        s->data[0] = s->volatile_cfg & 0xFF;
        s->pos = 0;
        s->len = 1;
        s->state = STATE_READING_DATA;
        break;
    case WVCR:
        if (s->write_enable) {
            s->needed_bytes = 1;
            s->pos = 0;
            s->len = 0;
            s->state = STATE_COLLECTING_DATA;
        }
        break;
    case REVCR:
        s->data[0] = s->enh_volatile_cfg & 0xFF;
        s->pos = 0;
        s->len = 1;
        s->state = STATE_READING_DATA;
        break;
    case WEVCR:
        if (s->write_enable) {
            s->needed_bytes = 1;
            s->pos = 0;
            s->len = 0;
            s->state = STATE_COLLECTING_DATA;
        }
        break;
    case RESET_ENABLE:
        s->reset_enable = true;
        break;
    case RESET_MEMORY:
        if (s->reset_enable) {
            reset_memory(s);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "M25P80: Unknown cmd %x\n", value);
        break;
    }
}

static int m25p80_cs(SSISlave *ss, bool select)
{
    Flash *s = M25P80(ss);

    if (select) {
        if (s->state == STATE_COLLECTING_VAR_LEN_DATA) {
            complete_collecting_data(s);
        }
        s->len = 0;
        s->pos = 0;
        s->state = STATE_IDLE;
        flash_sync_dirty(s, -1);
    }

    DB_PRINT_L(0, "%sselect\n", select ? "de" : "");

    return 0;
}

static uint32_t m25p80_transfer8(SSISlave *ss, uint32_t tx)
{
    Flash *s = M25P80(ss);
    uint32_t r = 0;

    switch (s->state) {

    case STATE_PAGE_PROGRAM:
        DB_PRINT_L(1, "page program cur_addr=%#" PRIx64 " data=%" PRIx8 "\n",
                   s->cur_addr, (uint8_t)tx);
        flash_write8(s, s->cur_addr, (uint8_t)tx);
        s->cur_addr++;
        break;

    case STATE_READ:
        r = s->storage[s->cur_addr];
        DB_PRINT_L(1, "READ 0x%" PRIx64 "=%" PRIx8 "\n", s->cur_addr,
                   (uint8_t)r);
        s->cur_addr = (s->cur_addr + 1) % s->size;
        break;

    case STATE_COLLECTING_DATA:
    case STATE_COLLECTING_VAR_LEN_DATA:
        s->data[s->len] = (uint8_t)tx;
        s->len++;

        if (s->len == s->needed_bytes) {
            complete_collecting_data(s);
        }
        break;

    case STATE_READING_DATA:
        r = s->data[s->pos];
        s->pos++;
        if (s->pos == s->len) {
            s->pos = 0;
            s->state = STATE_IDLE;
        }
        break;

    default:
    case STATE_IDLE:
        decode_new_cmd(s, (uint8_t)tx);
        break;
    }

    return r;
}

static int m25p80_init(SSISlave *ss)
{
    DriveInfo *dinfo;
    Flash *s = M25P80(ss);
    M25P80Class *mc = M25P80_GET_CLASS(s);

    s->pi = mc->pi;

    s->size = s->pi->sector_size * s->pi->n_sectors;
    s->dirty_page = -1;

    /* FIXME use a qdev drive property instead of drive_get_next() */
    dinfo = drive_get_next(IF_MTD);

    if (dinfo) {
        DB_PRINT_L(0, "Binding to IF_MTD drive\n");
        s->blk = blk_by_legacy_dinfo(dinfo);
        blk_attach_dev_nofail(s->blk, s);

        s->storage = blk_blockalign(s->blk, s->size);

        /* FIXME: Move to late init */
        if (blk_pread(s->blk, 0, s->storage, s->size) != s->size) {
            fprintf(stderr, "Failed to initialize SPI flash!\n");
            return 1;
        }
    } else {
        DB_PRINT_L(0, "No BDRV - binding to RAM\n");
        s->storage = blk_blockalign(NULL, s->size);
        memset(s->storage, 0xFF, s->size);
    }

    return 0;
}

static void m25p80_reset(DeviceState *d)
{
    Flash *s = M25P80(d);

    reset_memory(s);
}

static void m25p80_pre_save(void *opaque)
{
    flash_sync_dirty((Flash *)opaque, -1);
}

static Property m25p80_properties[] = {
    DEFINE_PROP_UINT32("nonvolatile-cfg", Flash, nonvolatile_cfg, 0x8FFF),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_m25p80 = {
    .name = "xilinx_spi",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = m25p80_pre_save,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(state, Flash),
        VMSTATE_UINT8_ARRAY(data, Flash, 16),
        VMSTATE_UINT32(len, Flash),
        VMSTATE_UINT32(pos, Flash),
        VMSTATE_UINT8(needed_bytes, Flash),
        VMSTATE_UINT8(cmd_in_progress, Flash),
        VMSTATE_UINT64(cur_addr, Flash),
        VMSTATE_BOOL(write_enable, Flash),
        VMSTATE_BOOL_V(reset_enable, Flash, 2),
        VMSTATE_UINT8_V(ear, Flash, 2),
        VMSTATE_BOOL_V(four_bytes_address_mode, Flash, 2),
        VMSTATE_UINT32_V(nonvolatile_cfg, Flash, 2),
        VMSTATE_UINT32_V(volatile_cfg, Flash, 2),
        VMSTATE_UINT32_V(enh_volatile_cfg, Flash, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void m25p80_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSISlaveClass *k = SSI_SLAVE_CLASS(klass);
    M25P80Class *mc = M25P80_CLASS(klass);

    k->init = m25p80_init;
    k->transfer = m25p80_transfer8;
    k->set_cs = m25p80_cs;
    k->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &vmstate_m25p80;
    dc->props = m25p80_properties;
    dc->reset = m25p80_reset;
    mc->pi = data;
}

static const TypeInfo m25p80_info = {
    .name           = TYPE_M25P80,
    .parent         = TYPE_SSI_SLAVE,
    .instance_size  = sizeof(Flash),
    .class_size     = sizeof(M25P80Class),
    .abstract       = true,
};

static void m25p80_register_types(void)
{
    int i;

    type_register_static(&m25p80_info);
    for (i = 0; i < ARRAY_SIZE(known_devices); ++i) {
        TypeInfo ti = {
            .name       = known_devices[i].part_name,
            .parent     = TYPE_M25P80,
            .class_init = m25p80_class_init,
            .class_data = (void *)&known_devices[i],
        };
        type_register(&ti);
    }
}

type_init(m25p80_register_types)
