/*
 * ide.c -- the ATA/ATAPI IDE controller.  See ide.h for the JS->C mapping, for what the
 * baliveau disk path needs, and for the ATAPI deferral (the device's identity is here, its
 * command set is not).
 *
 * Every function below cites the ide.js line range it transcribes.  The names are the
 * upstream prototype names with the JS receiver dropped (IDEChannel.prototype.read_status
 * -> channel_read_status), so the transcription can be read side by side with the original.
 */

#include "ide.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../cpu/cpu.h"  /* unimplemented_instruction */
#include "../cpu/memory.h" /* read8/read16/read32s/mem8/memory_size (the DMA PRD walk) */
#include "io.h"
#include "machine.h" /* device_raise_irq / device_lower_irq */
#include "pci.h"
#include "rtc.h" /* the CMOS geometry registers set_disk_buffer writes */

/* ide.js:45-48 */
#define CDROM_SECTOR_SIZE 2048
#define HD_SECTOR_SIZE 512
#define BUS_MASTER_BASE 0xB400

/* ide.js:50-97 -- the register offsets and the status/error/device/control bits, kept
 * verbatim so the transcription can be read against the original. */
#define ATA_REG_ERROR      0x01
#define ATA_REG_STATUS     0x07
#define ATA_REG_ALT_STATUS 0x00
#define ATA_REG_DATA       0x00
#define ATA_REG_SECTOR     0x02
#define ATA_REG_LBA_LOW    0x03
#define ATA_REG_LBA_MID    0x04
#define ATA_REG_LBA_HIGH   0x05
#define ATA_REG_DEVICE     0x06
#define ATA_REG_FEATURES   0x01
#define ATA_REG_COMMAND    0x07
#define ATA_REG_CONTROL    0x00

/* ide.js:68-72 -- the bus-master IDE registers (BAR4); +8 for the secondary channel */
#define BMI_REG_COMMAND 0x00
#define BMI_REG_STATUS  0x02
#define BMI_REG_PRDT    0x04

#define ATA_ER_ABRT 0x04

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DSC  0x10
#define ATA_SR_DF   0x20
#define ATA_SR_DRDY 0x40
#define ATA_SR_BSY  0x80

#define ATA_DR_DEV 0x10

#define ATA_CR_NIEN 0x02
#define ATA_CR_SRST 0x04

/* ide.js:99-134 */
#define ATA_CMD_DEVICE_RESET                  0x08
#define ATA_CMD_EXECUTE_DEVICE_DIAGNOSTIC     0x90
#define ATA_CMD_FLUSH_CACHE                   0xE7
#define ATA_CMD_FLUSH_CACHE_EXT               0xEA
#define ATA_CMD_GET_MEDIA_STATUS              0xDA
#define ATA_CMD_IDENTIFY_DEVICE               0xEC
#define ATA_CMD_IDENTIFY_PACKET_DEVICE        0xA1
#define ATA_CMD_IDLE_IMMEDIATE                0xE1
#define ATA_CMD_INITIALIZE_DEVICE_PARAMETERS  0x91
#define ATA_CMD_MEDIA_LOCK                    0xDE
#define ATA_CMD_NOP                           0x00
#define ATA_CMD_PACKET                        0xA0
#define ATA_CMD_READ_DMA                      0xC8
#define ATA_CMD_READ_DMA_EXT                  0x25
#define ATA_CMD_READ_MULTIPLE                 0x29
#define ATA_CMD_READ_MULTIPLE_EXT             0xC4
#define ATA_CMD_READ_NATIVE_MAX_ADDRESS       0xF8
#define ATA_CMD_READ_NATIVE_MAX_ADDRESS_EXT   0x27
#define ATA_CMD_READ_SECTORS                  0x20
#define ATA_CMD_READ_SECTORS_EXT              0x24
#define ATA_CMD_READ_VERIFY_SECTORS           0x40
#define ATA_CMD_SECURITY_FREEZE_LOCK          0xF5
#define ATA_CMD_SET_FEATURES                  0xEF
#define ATA_CMD_SET_MAX                       0xF9
#define ATA_CMD_SET_MULTIPLE_MODE             0xC6
#define ATA_CMD_STANDBY_IMMEDIATE             0xE0
#define ATA_CMD_WRITE_DMA                     0xCA
#define ATA_CMD_WRITE_DMA_EXT                 0x35
#define ATA_CMD_WRITE_MULTIPLE                0x39
#define ATA_CMD_WRITE_MULTIPLE_EXT            0xC5
#define ATA_CMD_WRITE_SECTORS                 0x30
#define ATA_CMD_WRITE_SECTORS_EXT             0x34
#define ATA_CMD_READ_LOG_EXT                  0x2F
#define ATA_CMD_10h                           0x10
#define ATA_CMD_F0h                           0xF0

#define ATAPI_SIGNATURE_LO 0x14
#define ATAPI_SIGNATURE_HI 0xEB

/* ide.js:253-256 */
#define MEDIUM_CHANGED_NONE 0
#define MEDIUM_CHANGED_UNIT_ATTENTION 1
#define MEDIUM_CHANGED_NOT_PRESENT 2

/* ide.js:973-975 */
#define TRANSLATION_NONE  0
#define TRANSLATION_LBA   1
#define TRANSLATION_LARGE 2

/* The hardware's own counts: two channels, two devices each. */
#define IDE_CHANNELS 2
#define IDE_INTERFACES_PER_CHANNEL 2

typedef struct ide_channel ide_channel_t;

typedef struct ide_interface {
    ide_channel_t *channel;
    const char *name; /* "ide0.0" -- ide.js:830 */
    int32_t interface_nr; /* ide.js:839 */

    const uint8_t *buffer; /* the disk image, or NULL for no media */
    uint32_t buffer_length;

    bool drive_connected; /* ide.js:847 */
    bool is_atapi;        /* ide.js:853 */

    uint32_t sector_size;        /* ide.js:850 */
    uint32_t sector_count;       /* ide.js:856 */
    uint32_t head_count;         /* ide.js:859 */
    uint32_t sectors_per_track;  /* ide.js:862 */
    uint32_t cylinder_count;     /* ide.js:865 */

    int32_t is_lba;           /* ide.js:868 */
    int32_t sector_count_reg; /* ide.js:871 */
    int32_t lba_low_reg;      /* ide.js:874 */
    int32_t features_reg;     /* ide.js:877 */
    int32_t lba_mid_reg;      /* ide.js:880 */
    int32_t lba_high_reg;     /* ide.js:883 */
    int32_t head;             /* ide.js:886 */
    int32_t device_reg;       /* ide.js:889 */
    int32_t status_reg;       /* ide.js:892 */
    int32_t sectors_per_drq;  /* ide.js:895 */
    int32_t error_reg;        /* ide.js:898 */

    uint32_t data_pointer; /* ide.js:901 */
    uint32_t data_length;  /* ide.js:908 */
    uint32_t data_end;     /* ide.js:911 */
    uint8_t *data;         /* ide.js:903-905 (data16/data32 are byte views of it) */
    uint32_t data_capacity;

    int32_t current_command; /* ide.js:914 */
    uint32_t write_dest;     /* ide.js:917 */

    int32_t medium_changed;    /* ide.js:935 (ATAPI only) */
    int32_t atapi_sense_key;   /* ide.js:929 */
    int32_t atapi_add_sense;   /* ide.js:932 */
    int32_t current_atapi_command; /* ide.js:926 */
    uint32_t overrun_notes;  /* this port's rate limit on the read-port diagnostic */
} ide_interface_t;

struct ide_channel {
    int32_t channel_nr;
    bus_connector_t *bus;
    int32_t command_base;
    int32_t control_base;
    int32_t irq;
    const char *name;

    ide_interface_t master;
    ide_interface_t slave;
    ide_interface_t *current_interface; /* ide.js:400 */

    int32_t device_control_reg; /* ide.js:403 */
    uint32_t prdt_addr;         /* ide.js:406 */
    int32_t dma_status;         /* ide.js:409 */
    int32_t dma_command;        /* ide.js:412 */
};

static ide_channel_t ide_channels[IDE_CHANNELS];
static bool ide_channel_present[IDE_CHANNELS];

/* forward declarations: the device's state machine is mutually recursive (a read of the
 * data port ends the transfer, an end of transfer arms the next one) */
static void interface_push_irq(ide_interface_t *ide);
static void interface_ata_advance(ide_interface_t *ide, int32_t cmd, uint32_t sectors);
static void interface_read_end(ide_interface_t *ide);
static void interface_write_end(ide_interface_t *ide);
static void interface_do_write(ide_interface_t *ide);
static void interface_create_identify_packet(ide_interface_t *ide);
static void interface_atapi_handle(ide_interface_t *ide);
static void interface_do_atapi_dma(ide_interface_t *ide);
static void interface_atapi_check_condition_response(ide_interface_t *ide, int32_t sense_key,
                                                    int32_t additional_sense);
static void channel_push_irq(ide_channel_t *channel);
static int32_t channel_dma_read_command8(ide_channel_t *channel);
static void channel_dma_write_command8(ide_channel_t *channel, int32_t value);

/* ------------------------------------------------------------------ *
 * helpers
 * ------------------------------------------------------------------ */

/* A diagnostic the GUEST cannot see.  The reference's PINNED RELEASE build compiles
 * dbg_assert out (results/m0-oracle.md's note), so where upstream's arm has a real effect
 * after the assert (an abort, a status), this port keeps that effect and names the case
 * here -- it never stops a machine upstream would have run. */
static void ide_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("v86c: ide: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* little-endian loads out of the transfer buffer, assembled byte by byte so the port does
 * not depend on the host's byte order (the APE also runs on aarch64). */
static int32_t data_read8(const ide_interface_t *ide, uint32_t offset)
{
    if (offset >= ide->data_capacity) return 0;
    return ide->data[offset];
}

static int32_t data_read16(const ide_interface_t *ide, uint32_t offset)
{
    if (offset + 1 >= ide->data_capacity) return 0;
    return (int32_t)((uint32_t)ide->data[offset] | ((uint32_t)ide->data[offset + 1] << 8));
}

static int32_t data_read32(const ide_interface_t *ide, uint32_t offset)
{
    if (offset + 3 >= ide->data_capacity) return 0;
    return (int32_t)((uint32_t)ide->data[offset] | ((uint32_t)ide->data[offset + 1] << 8) |
                     ((uint32_t)ide->data[offset + 2] << 16) |
                     ((uint32_t)ide->data[offset + 3] << 24));
}

static void data_write16(ide_interface_t *ide, uint32_t offset, int32_t value)
{
    ide->data[offset] = (uint8_t)value;
    ide->data[offset + 1] = (uint8_t)((uint32_t)value >> 8);
}

static void data_write32(ide_interface_t *ide, uint32_t offset, int32_t value)
{
    ide->data[offset] = (uint8_t)value;
    ide->data[offset + 1] = (uint8_t)((uint32_t)value >> 8);
    ide->data[offset + 2] = (uint8_t)((uint32_t)value >> 16);
    ide->data[offset + 3] = (uint8_t)((uint32_t)value >> 24);
}

/* ------------------------------------------------------------------ *
 * THE ACCESS TRACE (a diagnostic, like io.h's V86C_LOG_PORTS).  With V86C_LOG_IDE set
 * (any nonempty value but "0") every IDE register access prints the port, the width, the
 * direction and the value to stderr.  It DECIDES NOTHING -- no return value, device or port
 * -map byte depends on it, so a guest cannot tell whether it is on.  It exists because the
 * kernel's own ATA probe is SILENT BY DESIGN (libata's ata_port_probe sets ATA_EHI_QUIET,
 * libata-core.c:5835-5852, so a failed identify prints nothing at all) and the only way to
 * see what a real driver does to the device is to watch the device's own port.
 * ------------------------------------------------------------------ */
static bool ide_trace_on(void)
{
    static int state = -1;
    if (state < 0) {
        const char *value = getenv("V86C_LOG_IDE");
        state = value != NULL && strcmp(value, "0") != 0;
    }
    return state != 0;
}

static void ide_trace(int32_t port, unsigned width, bool write, uint32_t value)
{
    if (!ide_trace_on()) return;
    fprintf(stderr, "v86c: ide %s%u port=0x%x value=0x%x\n", write ? "write" : "read", width,
            (unsigned)port, (unsigned)(value & (width == 8 ? 0xFFu : width == 16 ? 0xFFFFu
                                                                                : 0xFFFFFFFFu)));
}

/* ------------------------------------------------------------------ *
 * the interface -- ide.js:827-2917
 * ------------------------------------------------------------------ */

/* ide.js:2779-2790 */
static void interface_data_allocate_noclear(ide_interface_t *ide, uint32_t len)
{
    if (ide->data_capacity < len) {
        /* ide.js:2781-2786: the JS array is REPLACED (`new Uint8Array(len + 3 & ~3)`), i.e.
         * grown to the requested size, never shrunk. */
        uint32_t capacity = (len + 3) & ~3u;
        uint8_t *grown = realloc(ide->data, capacity);
        if (grown == NULL) {
            fprintf(stderr, "v86c: ide: cannot grow the transfer buffer to %u bytes\n",
                    (unsigned)capacity);
            exit(1);
        }
        ide->data = grown;
        memset(ide->data + ide->data_capacity, 0, capacity - ide->data_capacity);
        ide->data_capacity = capacity;
    }
    ide->data_length = len;
    ide->data_pointer = 0;
}

/* ide.js:2773-2777 */
static void interface_data_allocate(ide_interface_t *ide, uint32_t len)
{
    interface_data_allocate_noclear(ide, len);
    memset(ide->data, 0, (len + 3) & ~3u);
}

/* ide.js:2792-2796 */
static void interface_data_set(ide_interface_t *ide, const uint8_t *bytes, uint32_t len)
{
    interface_data_allocate_noclear(ide, len);
    memcpy(ide->data, bytes, len);
}

/* ide.js:2798-2813 -- the bus messages the reference's UI reads for its disk-activity LED.
 * This port sends them on the device bus; nothing in this machine listens, and no guest
 * behaviour depends on them. */
static void interface_report_read_start(ide_interface_t *ide)
{
    bus_send(ide->channel->bus, "ide-read-start", NULL, 0);
}

static void interface_report_read_end(ide_interface_t *ide, uint32_t byte_count)
{
    BUS_SEND(ide->channel->bus, "ide-read-end", ide->channel->channel_nr, (int32_t)byte_count,
             (int32_t)(byte_count / ide->sector_size));
}

static void interface_report_write(ide_interface_t *ide, uint32_t byte_count)
{
    BUS_SEND(ide->channel->bus, "ide-write-end", ide->channel->channel_nr, (int32_t)byte_count,
             (int32_t)(byte_count / ide->sector_size));
}

/* ide.js:2836-2844.  See ide.h: with a synchronous buffer (upstream's own `async: false`,
 * which is what both gate legs use) no read is ever in flight at a cancellation point. */
static void interface_cancel_io_operations(ide_interface_t *ide) { (void)ide; }

/* ide.js:1142-1163 */
static void interface_device_reset(ide_interface_t *ide)
{
    if (ide->is_atapi) {
        ide->status_reg = 0;
        ide->sector_count_reg = 1;
        ide->error_reg = 1;
        ide->lba_low_reg = 1;
        ide->lba_mid_reg = ATAPI_SIGNATURE_LO;
        ide->lba_high_reg = ATAPI_SIGNATURE_HI;
    } else {
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_ERR;
        ide->sector_count_reg = 1;
        ide->error_reg = 1;
        ide->lba_low_reg = 1;
        ide->lba_mid_reg = 0;
        ide->lba_high_reg = 0;
    }
    interface_cancel_io_operations(ide);
}

/* ide.js:1165-1168 */
static void interface_push_irq(ide_interface_t *ide) { channel_push_irq(ide->channel); }

/* ide.js:1170-1175 */
static void interface_ata_abort_command(ide_interface_t *ide)
{
    ide->error_reg = ATA_ER_ABRT;
    ide->status_reg = ATA_SR_DRDY | ATA_SR_ERR;
    interface_push_irq(ide);
}

/* ide.js:977-1057 -- the geometry the reference derives from the disk's own MBR, with
 * qemu's fallback (upstream's comment says so, and the numbers below are its). */
static void interface_get_disk_geometry(ide_interface_t *ide, uint32_t *cylinders_out,
                                        uint32_t *heads_out, uint32_t *sectors_out,
                                        int32_t *translation_out)
{
    bool have_mbr_geometry = false;
    uint32_t mbr_cylinders = 0, mbr_heads = 0, mbr_sectors = 0;

    /* ide.js:980: `this.buffer.get_from_cache(0, 512)` -- the disk's first sector. */
    if (ide->buffer_length >= 512) {
        const uint8_t *mbr = ide->buffer;
        if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
            for (int i = 0; i < 4; i++) {
                uint32_t offset = 0x1BE + (uint32_t)i * 16;
                uint32_t end_head = mbr[offset + 5];
                uint32_t sectors = mbr[offset + 6] & 63;
                uint32_t sector_count = (uint32_t)mbr[offset + 12] |
                                        ((uint32_t)mbr[offset + 13] << 8) |
                                        ((uint32_t)mbr[offset + 14] << 16) |
                                        ((uint32_t)mbr[offset + 15] << 24);
                if (sector_count && end_head && sectors) {
                    uint32_t heads = end_head + 1;
                    uint32_t cylinders = ide->sector_count / (heads * sectors);
                    if (cylinders >= 1 && cylinders <= 16383) {
                        have_mbr_geometry = true;
                        mbr_cylinders = cylinders;
                        mbr_heads = heads;
                        mbr_sectors = sectors;
                        break;
                    }
                }
            }
        }
    }

    uint32_t cylinders, heads, sectors;
    int32_t translation;

    if (have_mbr_geometry && mbr_heads <= 16) {
        cylinders = mbr_cylinders;
        heads = mbr_heads;
        sectors = mbr_sectors;
        translation = TRANSLATION_NONE;
    } else {
        heads = 16;
        sectors = 63;
        cylinders = ide->sector_count / (heads * sectors);
        if (cylinders > 16383) {
            cylinders = 16383;
        } else if (cylinders < 2) {
            cylinders = 2;
        }
        if (!have_mbr_geometry && cylinders <= 1024) {
            translation = TRANSLATION_NONE;
        } else if (have_mbr_geometry && cylinders * 16 <= 131072) {
            translation = TRANSLATION_LARGE;
        } else {
            translation = TRANSLATION_LBA;
        }
    }

    *cylinders_out = cylinders;
    *heads_out = heads;
    *sectors_out = sectors;
    *translation_out = translation;
}

/* ide.js:1059-1140 */
static void interface_set_disk_buffer(ide_interface_t *ide)
{
    if (ide->buffer == NULL) return;

    if (ide->is_atapi) {
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ | ATA_SR_ERR; /* COND */
        ide->error_reg = ATAPI_SK_UNIT_ATTENTION << 4;
    }

    ide->sector_count = ide->buffer_length / ide->sector_size;
    if (ide->buffer_length % ide->sector_size != 0) {
        /* ide.js:1074-1078: "warning: disk size not aligned with sector size" + ceil */
        ide_note("%s: warning: disk size not aligned with sector size", ide->name);
        ide->sector_count = (ide->buffer_length + ide->sector_size - 1) / ide->sector_size;
    }

    if (ide->is_atapi) {
        /* ide.js:1080-1092: the ATAPI default geometry 1/2048 */
        ide->head_count = 1;
        ide->sectors_per_track = 2048;
        ide->cylinder_count = ide->sector_count / ide->head_count / ide->sectors_per_track;
    } else {
        uint32_t cylinders, heads, sectors;
        int32_t translation;
        interface_get_disk_geometry(ide, &cylinders, &heads, &sectors, &translation);

        ide->cylinder_count = cylinders;
        ide->head_count = heads;
        ide->sectors_per_track = sectors;

        /* ide.js:1101-1133: the CMOS registers a BIOS reads the geometry from.  Upstream
         * writes them through the CPU's RTC device; this port's rtc.c is that device. */
        int shift = (ide->channel->channel_nr * 2 + ide->interface_nr) * 2;
        rtc_cmos_write(CMOS_BIOS_DISKTRANSFLAG,
                       (int32_t)((rtc_cmos_read(CMOS_BIOS_DISKTRANSFLAG) & ~(3 << shift)) |
                                 (translation << shift)));

        if (ide->channel->channel_nr == 0) {
            bool is_drive1 = ide->interface_nr == 0;

            rtc_cmos_write(CMOS_DISK_DATA,
                           (int32_t)(rtc_cmos_read(CMOS_DISK_DATA) | (is_drive1 ? 0xF0 : 0x0F)));
            rtc_cmos_write(is_drive1 ? CMOS_DISK_DRIVE1_TYPE : CMOS_DISK_DRIVE2_TYPE, 47);

            int32_t drive_reg = is_drive1 ? CMOS_DISK_DRIVE1_CYL : CMOS_DISK_DRIVE2_CYL;
            rtc_cmos_write(drive_reg + 0, (int32_t)(ide->cylinder_count & 0xFF));
            rtc_cmos_write(drive_reg + 1, (int32_t)(ide->cylinder_count >> 8 & 0xFF));
            rtc_cmos_write(drive_reg + 2, (int32_t)(ide->head_count & 0xFF));
            rtc_cmos_write(drive_reg + 3, 0xFF);
            rtc_cmos_write(drive_reg + 4, 0xFF);
            rtc_cmos_write(drive_reg + 5, (int32_t)(0xC0 | (ide->head_count > 8 ? 1 : 0) << 3));
            rtc_cmos_write(drive_reg + 6, (int32_t)(ide->cylinder_count & 0xFF));
            rtc_cmos_write(drive_reg + 7, (int32_t)(ide->cylinder_count >> 8 & 0xFF));
            rtc_cmos_write(drive_reg + 8, (int32_t)(ide->sectors_per_track & 0xFF));
        }
    }

    /* ide.js:1136-1139: `if(this.channel.cpu) this.push_irq()`.  Upstream guards on the
     * CPU being wired to the channel; in this machine it always is (machine_init owns both),
     * so the guard has no false arm. */
    interface_push_irq(ide);
}

/* ide.js:2004-2054 */
static int32_t interface_read_data(ide_interface_t *ide, uint32_t length)
{
    if (ide->data_pointer < ide->data_end) {
        if (ide->data_pointer + length - 1 >= ide->data_end) {
            /* ide.js:2008: dbg_assert(this.data_pointer + length - 1 < this.data_end).
             * Upstream's RELEASE build has the assert compiled out and still returns the
             * buffer's bytes, so this port does the same instead of stopping a machine
             * upstream would have run -- and names it, rate-limited (a guest that did it
             * once usually does it in a loop). */
            if (ide->overrun_notes < 3) {
                ide->overrun_notes++;
                ide_note("%s: data port read of %u bytes crosses the end of the transfer"
                         " (pointer %u, end %u)", ide->name, (unsigned)length,
                         (unsigned)ide->data_pointer, (unsigned)ide->data_end);
            }
        }

        int32_t result;
        if (length == 1) {
            result = data_read8(ide, ide->data_pointer);
        } else if (length == 2) {
            result = data_read16(ide, ide->data_pointer);
        } else {
            result = data_read32(ide, ide->data_pointer);
        }

        ide->data_pointer += length;

        if (ide->data_pointer >= ide->data_end) {
            interface_read_end(ide);
        }
        return result;
    }

    /* ide.js:2045-2053: a read past the end of the transfer answers 0 (upstream's
     * `return 0` with the "empty" debug log). */
    ide->data_pointer += length;
    return 0;
}

/* ide.js:2056-2122 */
static void interface_read_end(ide_interface_t *ide)
{
    if (ide->current_command == ATA_CMD_PACKET) {
        if (ide->data_end == ide->data_length) {
            ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
            ide->sector_count_reg = (ide->sector_count_reg & ~7) | 3;
            interface_push_irq(ide);
        } else {
            ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
            ide->sector_count_reg = (ide->sector_count_reg & ~7) | 2;
            interface_push_irq(ide);
            uint32_t byte_count = (uint32_t)((ide->lba_high_reg << 8) & 0xFF00) |
                                  (uint32_t)(ide->lba_mid_reg & 0xFF);
            if (ide->data_end + byte_count > ide->data_length) {
                ide->lba_mid_reg = (int32_t)((ide->data_length - ide->data_end) & 0xFF);
                ide->lba_high_reg = (int32_t)((ide->data_length - ide->data_end) >> 8 & 0xFF);
                ide->data_end = ide->data_length;
            } else {
                ide->data_end += byte_count;
            }
        }
        return;
    }

    ide->error_reg = 0;
    if (ide->data_pointer >= ide->data_length) {
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        return;
    }

    uint32_t sector_count;
    if (ide->current_command == ATA_CMD_READ_MULTIPLE ||
        ide->current_command == ATA_CMD_READ_MULTIPLE_EXT) {
        uint32_t remaining = (ide->data_length - ide->data_end) / 512;
        uint32_t per_drq = (uint32_t)ide->sectors_per_drq;
        sector_count = per_drq < remaining ? per_drq : remaining;
    } else {
        sector_count = 1;
    }
    interface_ata_advance(ide, ide->current_command, sector_count);
    ide->data_end += 512 * sector_count;
    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    interface_push_irq(ide);
}

/* ide.js:2124-2166 */
static void interface_write_data_port(ide_interface_t *ide, int32_t data, uint32_t length)
{
    if (ide->data_pointer >= ide->data_end) {
        /* ide.js:2130's dbg_log: "redundant write to data port" */
        return;
    }

    if (length == 1) {
        ide->data[ide->data_pointer++] = (uint8_t)data;
    } else if (length == 2) {
        data_write16(ide, ide->data_pointer, data);
        ide->data_pointer += 2;
    } else {
        data_write32(ide, ide->data_pointer, data);
        ide->data_pointer += 4;
    }

    if (ide->data_pointer == ide->data_end) {
        interface_write_end(ide);
    }
}

/* ide.js:2183-2215 */
static void interface_write_end(ide_interface_t *ide)
{
    if (ide->current_command == ATA_CMD_PACKET) {
        interface_atapi_handle(ide);
        return;
    }

    if (ide->data_pointer >= ide->data_length) {
        interface_do_write(ide);
    } else {
        /* ide.js:2203-2213: "XXX: Should advance here, but do_write does all the
         * advancing" -- upstream's own comment, kept. */
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        ide->data_end += 512;
        interface_push_irq(ide);
    }
}

/* ide.js:1790-1807 */
static void interface_do_write(ide_interface_t *ide)
{
    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;

    interface_ata_advance(ide, ide->current_command, ide->data_length / 512);
    interface_push_irq(ide);

    /* ide.js:1802: `this.buffer.set(this.write_dest, data, cb)` -- a synchronous write
     * through the buffer (upstream's SyncBuffer.set). */
    if (ide->write_dest + ide->data_length <= ide->buffer_length) {
        memcpy((uint8_t *)ide->buffer + ide->write_dest, ide->data, ide->data_length);
    }
    interface_report_write(ide, ide->data_length);
}

/* ide.js:2539-2551 */
static uint32_t interface_get_chs(ide_interface_t *ide)
{
    uint32_t c = (uint32_t)(ide->lba_mid_reg & 0xFF) |
                 ((uint32_t)(ide->lba_high_reg << 8) & 0xFF00u);
    uint32_t h = (uint32_t)ide->head;
    uint32_t s = (uint32_t)(ide->lba_low_reg & 0xFF);

    return (c * ide->head_count + h) * ide->sectors_per_track + s - 1;
}

/* ide.js:2553-2559 */
static uint32_t interface_get_lba28(ide_interface_t *ide)
{
    return (uint32_t)(ide->lba_low_reg & 0xFF) | ((uint32_t)(ide->lba_mid_reg << 8) & 0xFF00u) |
           ((uint32_t)(ide->lba_high_reg << 16) & 0xFF0000u) |
           (((uint32_t)ide->head & 0xF) << 24);
}

/* ide.js:2561-2568 */
static uint32_t interface_get_lba48(ide_interface_t *ide)
{
    return (uint32_t)(ide->lba_low_reg & 0xFF) | ((uint32_t)(ide->lba_mid_reg << 8) & 0xFF00u) |
           ((uint32_t)(ide->lba_high_reg << 16) & 0xFF0000u) |
           (((uint32_t)(ide->lba_low_reg >> 8) << 24) & 0xFF000000u);
}

/* ide.js:2570-2584 */
static uint32_t interface_get_lba(ide_interface_t *ide, bool is_lba48)
{
    if (is_lba48) return interface_get_lba48(ide);
    if (ide->is_lba) return interface_get_lba28(ide);
    return interface_get_chs(ide);
}

/* ide.js:2586-2600 */
static uint32_t interface_get_count(ide_interface_t *ide, bool is_lba48)
{
    if (is_lba48) {
        uint32_t count = (uint32_t)(ide->sector_count_reg & 0xFFFF);
        return count == 0 ? 0x10000 : count;
    }
    uint32_t count = (uint32_t)(ide->sector_count_reg & 0xFF);
    return count == 0 ? 0x100 : count;
}

/* ide.js:2217-2257 */
static void interface_ata_advance(ide_interface_t *ide, int32_t cmd, uint32_t sectors)
{
    ide->sector_count_reg -= (int32_t)sectors;

    if (cmd == ATA_CMD_READ_SECTORS_EXT || cmd == ATA_CMD_READ_MULTIPLE ||
        cmd == ATA_CMD_READ_DMA_EXT || cmd == ATA_CMD_WRITE_SECTORS_EXT ||
        cmd == ATA_CMD_WRITE_MULTIPLE || cmd == ATA_CMD_WRITE_DMA_EXT) {
        uint32_t new_sector = sectors + interface_get_lba48(ide);
        ide->lba_low_reg = (int32_t)((new_sector & 0xFF) | ((new_sector >> 16) & 0xFF00));
        ide->lba_mid_reg = (int32_t)(new_sector >> 8 & 0xFF);
        ide->lba_high_reg = (int32_t)(new_sector >> 16 & 0xFF);
    } else if (ide->is_lba) {
        uint32_t new_sector = sectors + interface_get_lba28(ide);
        ide->lba_low_reg = (int32_t)(new_sector & 0xFF);
        ide->lba_mid_reg = (int32_t)(new_sector >> 8 & 0xFF);
        ide->lba_high_reg = (int32_t)(new_sector >> 16 & 0xFF);
        ide->head = (ide->head & ~0xF) | (int32_t)(new_sector & 0xF);
    } else {
        uint32_t new_sector = sectors + interface_get_chs(ide);
        uint32_t c = new_sector / (ide->head_count * ide->sectors_per_track);
        ide->lba_mid_reg = (int32_t)(c & 0xFF);
        ide->lba_high_reg = (int32_t)(c >> 8 & 0xFF);
        ide->head = (int32_t)(((new_sector / ide->sectors_per_track) % ide->head_count) & 0xF);
        ide->lba_low_reg = (int32_t)((new_sector % ide->sectors_per_track + 1) & 0xFF);
    }
}

/* ide.js:2259-2307.
 *
 * THE ASYNCHRONOUS READ IS INLINED.  Upstream calls `read_buffer(start, byte_count, cb)`
 * and the callback does `data_set(data)`: with a synchronous buffer (upstream's own
 * `async: false`, and this port's only kind) the callback runs before read_buffer returns,
 * so the sequence below is the same sequence of effects. */
static void interface_ata_read_sectors(ide_interface_t *ide, int32_t cmd)
{
    bool is_lba48 = cmd == ATA_CMD_READ_SECTORS_EXT || cmd == ATA_CMD_READ_MULTIPLE;
    uint32_t count = interface_get_count(ide, is_lba48);
    uint32_t lba = interface_get_lba(ide, is_lba48);
    bool is_single = cmd == ATA_CMD_READ_SECTORS || cmd == ATA_CMD_READ_SECTORS_EXT;

    uint64_t byte_count = (uint64_t)count * ide->sector_size;
    uint64_t start = (uint64_t)lba * ide->sector_size;

    if (start + byte_count > ide->buffer_length) {
        /* ide.js:2281-2284: dbg_assert(false, "<name>: ATA read: Outside of disk"), then
         * status_reg = 0xFF and an IRQ.  The status is what a guest sees; kept. */
        ide_note("%s: ATA read: Outside of disk (lba %u count %u)", ide->name, (unsigned)lba,
                 (unsigned)count);
        ide->status_reg = 0xFF;
        interface_push_irq(ide);
        return;
    }

    ide->status_reg = ATA_SR_DRDY | ATA_SR_BSY;
    interface_report_read_start(ide);

    interface_data_set(ide, ide->buffer + start, (uint32_t)byte_count);
    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    ide->data_end = is_single ? 512 : (uint32_t)(byte_count < (uint64_t)ide->sectors_per_drq * 512
                                                     ? byte_count
                                                     : (uint64_t)ide->sectors_per_drq * 512);
    interface_ata_advance(ide, cmd, is_single ? 1 : (count < ide->sectors_per_track
                                                         ? count
                                                         : ide->sectors_per_track));
    interface_push_irq(ide);
    interface_report_read_end(ide, (uint32_t)byte_count);
}

/* ide.js:2309-2336 */
static void interface_ata_read_sectors_dma(ide_interface_t *ide, int32_t cmd)
{
    bool is_lba48 = cmd == ATA_CMD_READ_DMA_EXT;
    uint32_t count = interface_get_count(ide, is_lba48);
    uint32_t lba = interface_get_lba(ide, is_lba48);

    uint64_t byte_count = (uint64_t)count * ide->sector_size;
    uint64_t start = (uint64_t)lba * ide->sector_size;

    if (start + byte_count > ide->buffer_length) {
        ide_note("%s: ATA read: Outside of disk (lba %u count %u)", ide->name, (unsigned)lba,
                 (unsigned)count);
        ide->status_reg = 0xFF;
        interface_push_irq(ide);
        return;
    }

    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    ide->channel->dma_status |= 1;
}

/* The PRD table walk -- ide.js:2366-2390 (reads) and 2490-2519 (writes), shared here
 * because the two bodies differ only in the direction of the copy.
 *
 * Returns false if the table describes more than the transfer asked for, which upstream's
 * `write_blob(data.subarray(offset, offset + prd_count), prd_addr)` would silently
 * under-fill: refusing is the honest answer and no gate reaches it (a kernel's own PRD
 * table is exactly the transfer size). */
static bool interface_dma_walk_prdt(ide_interface_t *ide, uint32_t byte_count, bool to_guest,
                                    const uint8_t *guest_data, uint32_t *bytes_done)
{
    uint32_t prdt_start = ide->channel->prdt_addr;
    uint32_t offset = 0;

    for (;;) {
        uint32_t prd_addr = (uint32_t)read32s(prdt_start);
        uint32_t prd_count = (uint32_t)(read16(prdt_start + 4) & 0xFFFF);
        int32_t end = read8(prdt_start + 7) & 0x80;

        if (prd_count == 0) prd_count = 0x10000;
        if (offset + prd_count > byte_count) {
            ide_note("%s: DMA PRD table describes more than the transfer (%u > %u)", ide->name,
                     (unsigned)(offset + prd_count), (unsigned)byte_count);
            break;
        }
        if ((uint64_t)prd_addr + prd_count > cpu->memory_size) {
            /* Upstream reads/writes mem8 directly (`this.cpu.write_blob(...)`,
             * `this.cpu.mem8.subarray(...)`), where an address past the end of the wasm
             * memory traps.  This is the same structural failure, named. */
            char name[160];
            snprintf(name, sizeof(name),
                     "ide_dma(prd=0x%x len=%u) outside guest RAM (memory_size=0x%x)",
                     (unsigned)prd_addr, (unsigned)prd_count, (unsigned)cpu->memory_size);
            unimplemented_instruction(name);
            break;
        }

        if (to_guest) {
            write_blob(guest_data + offset, prd_count, prd_addr);
        } else {
            for (uint32_t i = 0; i < prd_count; i++) {
                ide->data[offset + i] = (uint8_t)read8_no_mmap_check(prd_addr + i);
            }
        }

        offset += prd_count;
        prdt_start += 8;
        if (end) break;
    }

    *bytes_done = offset;
    return offset == byte_count;
}

/* ide.js:2338-2403 */
static void interface_do_ata_read_sectors_dma(ide_interface_t *ide)
{
    int32_t cmd = ide->current_command;

    bool is_lba48 = cmd == ATA_CMD_READ_DMA_EXT;
    uint32_t count = interface_get_count(ide, is_lba48);
    uint32_t lba = interface_get_lba(ide, is_lba48);

    uint64_t byte_count = (uint64_t)count * ide->sector_size;
    uint64_t start = (uint64_t)lba * ide->sector_size;

    interface_report_read_start(ide);

    uint32_t moved = 0;
    interface_dma_walk_prdt(ide, (uint32_t)byte_count, true, ide->buffer + start, &moved);

    interface_ata_advance(ide, ide->current_command, count);
    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
    ide->channel->dma_status &= ~1;
    ide->current_command = -1;

    interface_report_read_end(ide, (uint32_t)byte_count);
    interface_push_irq(ide);
}

/* ide.js:2405-2438 */
static void interface_ata_write_sectors(ide_interface_t *ide, int32_t cmd)
{
    bool is_lba48 = cmd == ATA_CMD_WRITE_SECTORS_EXT || cmd == ATA_CMD_WRITE_MULTIPLE;
    uint32_t count = interface_get_count(ide, is_lba48);
    uint32_t lba = interface_get_lba(ide, is_lba48);
    bool is_single = cmd == ATA_CMD_WRITE_SECTORS || cmd == ATA_CMD_WRITE_SECTORS_EXT;

    uint64_t byte_count = (uint64_t)count * ide->sector_size;
    uint64_t start = (uint64_t)lba * ide->sector_size;

    if (start + byte_count > ide->buffer_length) {
        ide_note("%s: ATA write: Outside of disk (lba %u count %u)", ide->name, (unsigned)lba,
                 (unsigned)count);
        ide->status_reg = 0xFF;
        interface_push_irq(ide);
        return;
    }

    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    interface_data_allocate_noclear(ide, (uint32_t)byte_count);
    ide->data_end = is_single ? 512 : (uint32_t)(byte_count < (uint64_t)ide->sectors_per_drq * 512
                                                     ? byte_count
                                                     : (uint64_t)ide->sectors_per_drq * 512);
    ide->write_dest = (uint32_t)start;
}

/* ide.js:2440-2467 */
static void interface_ata_write_sectors_dma(ide_interface_t *ide, int32_t cmd)
{
    bool is_lba48 = cmd == ATA_CMD_WRITE_DMA_EXT;
    uint32_t count = interface_get_count(ide, is_lba48);
    uint32_t lba = interface_get_lba(ide, is_lba48);

    uint64_t byte_count = (uint64_t)count * ide->sector_size;
    uint64_t start = (uint64_t)lba * ide->sector_size;

    if (start + byte_count > ide->buffer_length) {
        ide_note("%s: ATA DMA write: Outside of disk (lba %u count %u)", ide->name,
                 (unsigned)lba, (unsigned)count);
        ide->status_reg = 0xFF;
        interface_push_irq(ide);
        return;
    }

    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    ide->channel->dma_status |= 1;
}

/* ide.js:2469-2537 */
static void interface_do_ata_write_sectors_dma(ide_interface_t *ide)
{
    int32_t cmd = ide->current_command;

    bool is_lba48 = cmd == ATA_CMD_WRITE_DMA_EXT;
    uint32_t count = interface_get_count(ide, is_lba48);
    uint32_t lba = interface_get_lba(ide, is_lba48);

    uint64_t byte_count = (uint64_t)count * ide->sector_size;
    uint64_t start = (uint64_t)lba * ide->sector_size;

    /* ide.js:2488-2519: the transfer is assembled FROM the PRD table into a staging buffer
     * and then written to the disk in one set().  The staging area is the interface's own
     * data buffer (which ata_write_sectors_dma did not touch). */
    interface_data_allocate_noclear(ide, (uint32_t)byte_count);

    uint32_t moved = 0;
    interface_dma_walk_prdt(ide, (uint32_t)byte_count, false, NULL, &moved);

    if (start + byte_count <= ide->buffer_length) {
        memcpy((uint8_t *)ide->buffer + start, ide->data, (uint32_t)byte_count);
    }

    interface_ata_advance(ide, ide->current_command, count);
    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
    interface_push_irq(ide);
    ide->channel->dma_status &= ~1;
    ide->current_command = -1;
    interface_report_write(ide, (uint32_t)byte_count);
}

/* ide.js:2605-2771 -- the IDENTIFY DEVICE / IDENTIFY PACKET DEVICE packet, byte for byte.
 *
 * THE FILL QUIRK IN strcpy_be16 IS REPRODUCED: upstream's
 *     out_buffer.fill(32, ofs8, len8)
 * passes len8 (the LENGTH) as the END offset instead of ofs8 + len8.  MEASURED for all
 * three call sites -- (10,10) -> fill(32,20,20), (23,4) -> fill(32,46,8), (27,20) ->
 * fill(32,54,40) -- every one has ofs8 >= len8, so the fill is a NO-OP and the strings end
 * up NUL-padded by the `fill(0, 0, 512)` above them.  Written the way upstream writes it,
 * so the bytes match whoever is right. */
static void identify_strcpy_be16(uint8_t *out, uint32_t ofs16, uint32_t len16, const char *str)
{
    uint32_t ofs8 = ofs16 << 1;
    uint32_t len8 = len16 << 1;
    uint32_t end8 = ofs8 + len8;

    for (uint32_t i = ofs8; i < len8 && i < 512; i++) out[i] = 32;

    size_t length = strlen(str);
    for (size_t i_str = 0; i_str < length && ofs8 < end8; i_str++) {
        if (i_str & 1) {
            out[ofs8] = (uint8_t)str[i_str];
            ofs8 += 2;
        } else {
            out[ofs8 + 1] = (uint8_t)str[i_str];
        }
    }
}

static void interface_create_identify_packet(ide_interface_t *ide)
{
    uint32_t cylinder_count = ide->cylinder_count < 16383 ? ide->cylinder_count : 16383;

    /* ide.js:2631-2642 */
    uint32_t general_cfg = ide->is_atapi ? 0x8540 : 0x0040;
    uint32_t multiword_dma_mode = ide->current_command == ATA_CMD_PACKET ? 0 : 0x0407;
    uint32_t major_version = 0x0000;
    uint32_t feat_82 = ide->is_atapi ? (1u << 14 | 1u << 9 | 1u << 5) : (1u << 14);
    uint32_t feat_83 =
        ide->is_atapi ? (1u << 14 | 1u << 12) : (1u << 14 | 1u << 13 | 1u << 12 | 1u << 10);
    uint32_t feat_84 = 1u << 14;

    interface_data_allocate_noclear(ide, 512);
    memset(ide->data, 0, 512);

    /* ide.js:2645-2760, in its order: each W16 is one 16-bit word, little-endian on the
     * wire (upstream's byte list is (low, high) per word).  The word numbers in the
     * comments are the ATA ones. */
    uint8_t *d = ide->data;
    int at = 0;
#define W16(v)                                                                            \
    do {                                                                                  \
        d[at++] = (uint8_t)((uint32_t)(v) & 0xFF);                                        \
        d[at++] = (uint8_t)(((uint32_t)(v) >> 8) & 0xFF);                                 \
    } while (0)

    W16(general_cfg);                        /* 0 */
    W16(cylinder_count);                     /* 1 */
    W16(0);                                  /* 2 reserved */
    W16(ide->head_count);                    /* 3 */
    W16(ide->sectors_per_track / 512);       /* 4 */
    W16(512 >> 8);                           /* 5 */
    W16(ide->sectors_per_track);             /* 6 */
    W16(0); W16(0); W16(0);                  /* 7-9 vendor unique */
    W16(0); W16(0); W16(0); W16(0); W16(0);  /* 10-19 serial number (filled below) */
    W16(0); W16(0); W16(0); W16(0); W16(0);
    W16(0x0003);                             /* 20 buffer type */
    W16(0x0200);                             /* 21 buffer size in 512-byte increments */
    W16(0x0004);                             /* 22 ECC bytes */
    W16(0); W16(0); W16(0); W16(0);          /* 23-26 firmware revision (filled below) */
    for (int i = 0; i < 20; i++) W16(0);     /* 27-46 model number: 20 words (filled below) */
    W16(0x0080);                             /* 47 max sectors per interrupt */
    W16(0x0001);                             /* 48 doubleword I/O */
    W16(0x0200);                             /* 49 capabilities: LBA */
    W16(0);                                  /* 50 reserved */
    W16(0x0200);                             /* 51 PIO data transfer cycle timing mode */
    W16(0x0200);                             /* 52 DMA data transfer cycle timing mode */
    W16(0x0007);                             /* 53 words 54-58 are valid */
    W16(cylinder_count);                     /* 54 current cylinders */
    W16(ide->head_count);                    /* 55 current heads */
    W16(ide->sectors_per_track);             /* 56 current sectors per track */
    W16(ide->sector_count);                  /* 57-58 current capacity in sectors */
    W16(ide->sector_count >> 16);
    W16(0);                                  /* 59 multiple sector setting */
    W16(ide->sector_count);                  /* 60-61 total addressable sectors */
    W16(ide->sector_count >> 16);
    W16(0);                                  /* 62 single-word DMA transfer mode */
    W16(multiword_dma_mode);                 /* 63 multiword DMA transfer mode */
    W16(0);                                  /* 64 PIO modes supported */
    W16(30); W16(30); W16(30); W16(30);      /* 65-68 cycle times */
    W16(0); W16(0); W16(0); W16(0); W16(0);  /* 69-73 reserved */
    W16(0);                                  /* 74 reserved */
    W16(0);                                  /* 75 queue depth */
    W16(0); W16(0); W16(0); W16(0);          /* 76-79 reserved */
    W16(major_version);                      /* 80 major version number */
    W16(0);                                  /* 81 minor version number */
    W16(feat_82);                            /* 82 command set supported */
    W16(feat_83);                            /* 83 */
    W16(feat_84);                            /* 84 */
    W16(feat_82);                            /* 85 command set enabled (copy of 82) */
    W16(feat_83);                            /* 86 (copy of 83) */
    W16(feat_84);                            /* 87 (copy of 84) */
    W16(0);                                  /* 88 DMA related field */
    W16(0);                                  /* 89 security erase time */
    W16(0);                                  /* 90 enhanced security erase time */
    W16(0);                                  /* 91 advanced power management value */
    W16(0);                                  /* 92 master password revision code */
    W16(0x6001);                             /* 93 hardware reset result */
    W16(0);                                  /* 94 acoustic management value */
    W16(0); W16(0); W16(0); W16(0); W16(0);  /* 95-99 reserved */
    W16(ide->sector_count);                  /* 100-101 max user LBA (48-bit) */
    W16(ide->sector_count >> 16);
#undef W16

    /* THE LENGTH IS STRUCTURAL: ide.js:2645-2760's byte list is exactly 204 entries (102
     * words), and every word above it is addressed by its index, so a miscount here would
     * silently shift every field.  MEASURED: this check is what found the missing words. */
    if (at != 204) {
        fprintf(stderr, "v86c: ide: %s: the identify packet wrote %d bytes, not 204\n",
                ide->name, at);
        exit(1);
    }

    char serial[16];
    snprintf(serial, sizeof(serial), "8086-86%d%d", (int)ide->channel->channel_nr,
             (int)ide->interface_nr);
    identify_strcpy_be16(ide->data, 10, 10, serial);
    identify_strcpy_be16(ide->data, 23, 4, "1.00");
    identify_strcpy_be16(ide->data, 27, 20, ide->is_atapi ? "v86 ATAPI CD-ROM" : "v86 ATA HD");

    ide->data_length = 512;
    ide->data_end = 512;
}

/* ide.js:1774-1788 */
static void interface_atapi_check_condition_response(ide_interface_t *ide, int32_t sense_key,
                                                    int32_t additional_sense)
{
    /* "The sense state must be requested by the host using REQUEST SENSE immediately
     * following a CHECK CONDITION response or else it will be lost" (upstream's comment) */
    interface_data_allocate(ide, 0);
    ide->data_end = ide->data_length;
    ide->status_reg = ATA_SR_DRDY | ATA_SR_ERR; /* COND shares the ERR bit */
    ide->error_reg = sense_key << 4;
    ide->sector_count_reg = (ide->sector_count_reg & ~7) | 2 | 1;
    ide->atapi_sense_key = sense_key;
    ide->atapi_add_sense = additional_sense;
}

/* ide.js:205-229's ATAPI_CMD table, as the one thing ata_command needs from it (its flags) */
static int32_t atapi_command_flags(int32_t cmd)
{
    switch (cmd) {
    case ATAPI_CMD_TEST_UNIT_READY:
    case ATAPI_CMD_PAUSE:
    case ATAPI_CMD_READ_10:
    case ATAPI_CMD_READ_12:
    case ATAPI_CMD_READ_CAPACITY:
    case ATAPI_CMD_READ_FORMAT_CAPACITIES:
    case ATAPI_CMD_READ_CD:
    case ATAPI_CMD_READ_DISK_INFORMATION:
    case ATAPI_CMD_READ_SUBCHANNEL:
    case ATAPI_CMD_READ_TOC_PMA_ATIP:
    case ATAPI_CMD_READ_TRACK_INFORMATION:
        return ATAPI_CF_NEEDS_DISK;
    default:
        return ATAPI_CF_NONE;
    }
}

/* ide.js:1809-1893 -- READ (10)/(12) through the PIO path.  The asynchronous
 * read_buffer callback is inlined the way ata_read_sectors' is. */
static void interface_atapi_read(ide_interface_t *ide, const uint8_t *cmd)
{
    /* "Note: Big Endian" (upstream's comment): the CDB's fields are big-endian */
    uint32_t lba = ((uint32_t)cmd[2] << 24) | ((uint32_t)cmd[3] << 16) | ((uint32_t)cmd[4] << 8) |
                   (uint32_t)cmd[5];
    uint32_t count = cmd[0] == ATAPI_CMD_READ_12
                         ? (((uint32_t)cmd[6] << 24) | ((uint32_t)cmd[7] << 16) |
                            ((uint32_t)cmd[8] << 8) | (uint32_t)cmd[9])
                         : (((uint32_t)cmd[7] << 8) | (uint32_t)cmd[8]);

    uint32_t byte_count = count * ide->sector_size;
    uint64_t start = (uint64_t)lba * ide->sector_size;

    ide->data_length = 0;
    uint32_t req_length =
        (uint32_t)(ide->lba_high_reg << 8 & 0xFF00) | (uint32_t)(ide->lba_mid_reg & 0xFF);
    ide->lba_mid_reg = 0;
    ide->lba_high_reg = 0; /* "oak technology driver (windows 3.0)" */

    if (req_length == 0xFFFF) req_length--;
    if (req_length > byte_count) req_length = byte_count;

    if (ide->buffer == NULL) {
        ide_note("%s: CD read: no buffer", ide->name);
        ide->status_reg = 0xFF;
        ide->error_reg = 0x41;
        interface_push_irq(ide);
    } else if (start >= ide->buffer_length) {
        ide_note("%s: CD read: Outside of disk (end=%u size=%u)", ide->name,
                 (unsigned)(start + byte_count), (unsigned)ide->buffer_length);
        ide->status_reg = 0xFF;
        interface_push_irq(ide);
    } else if (byte_count == 0) {
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        ide->data_pointer = 0;
    } else {
        uint64_t available = ide->buffer_length - start;
        if ((uint64_t)byte_count > available) byte_count = (uint32_t)available;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_BSY;
        interface_report_read_start(ide);

        interface_data_set(ide, ide->buffer + start, byte_count);
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        ide->sector_count_reg = (ide->sector_count_reg & ~7) | 2;
        interface_push_irq(ide);

        req_length &= ~3u;
        ide->data_end = req_length;
        if (ide->data_end > ide->data_length) ide->data_end = ide->data_length;
        ide->lba_mid_reg = (int32_t)(ide->data_end & 0xFF);
        ide->lba_high_reg = (int32_t)(ide->data_end >> 8 & 0xFF);
        interface_report_read_end(ide, byte_count);
    }
}

/* ide.js:1895-1934 */
static void interface_atapi_read_dma(ide_interface_t *ide, const uint8_t *cmd)
{
    uint32_t lba = ((uint32_t)cmd[2] << 24) | ((uint32_t)cmd[3] << 16) | ((uint32_t)cmd[4] << 8) |
                   (uint32_t)cmd[5];
    uint32_t count = cmd[0] == ATAPI_CMD_READ_12
                         ? (((uint32_t)cmd[6] << 24) | ((uint32_t)cmd[7] << 16) |
                            ((uint32_t)cmd[8] << 8) | (uint32_t)cmd[9])
                         : (((uint32_t)cmd[7] << 8) | (uint32_t)cmd[8]);
    uint32_t byte_count = count * ide->sector_size;
    uint64_t start = (uint64_t)lba * ide->sector_size;

    if (ide->buffer == NULL || start >= ide->buffer_length) {
        ide_note("%s: CD read: Outside of disk (end=%u size=%u)", ide->name,
                 (unsigned)(start + byte_count), (unsigned)ide->buffer_length);
        ide->status_reg = 0xFF;
        interface_push_irq(ide);
        return;
    }

    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_BSY;
    interface_report_read_start(ide);
    interface_report_read_end(ide, byte_count);
    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    ide->sector_count_reg = (ide->sector_count_reg & ~7) | 2;
    interface_data_set(ide, ide->buffer + start, byte_count);
    interface_do_atapi_dma(ide);
}

/* ide.js:1936-2002.  The PRD walk is the shared one (interface_dma_walk_prdt); upstream's
 * atapi path writes `this.data` through the same table the ATA DMA paths use. */
static void interface_do_atapi_dma(ide_interface_t *ide)
{
    if ((ide->channel->dma_status & 1) == 0) {
        /* ide.js:1940's dbg_log: "Status not set" */
        return;
    }
    if ((ide->status_reg & ATA_SR_DRQ) == 0) {
        /* ide.js:1946's dbg_log: "DRQ not set" */
        return;
    }

    uint32_t moved = 0;
    interface_dma_walk_prdt(ide, ide->data_length, true, ide->data, &moved);

    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
    ide->channel->dma_status &= ~1;
    ide->current_command = -1;
}

/* ide.js:1449-1772 -- THE ATAPI COMMAND HANDLER.  Ported in full: the measurement in the
 * unit report shows a real Linux guest REACHES it (libata probes the ATAPI device with a
 * PACKET/INQUIRY before any media exists), so "the baliveau disk path does not need it" was
 * FALSE and a stub here would have stopped the gate's machine.  ide.h carries the
 * correction and what is still out of scope (iso9660.js, which builds an image rather than
 * driving the device). */
static void interface_atapi_handle(ide_interface_t *ide)
{
    int32_t cmd = ide->data[0];
    int32_t cmd_flags = atapi_command_flags(cmd);

    ide->data_pointer = 0;
    ide->current_atapi_command = cmd;

    /* ide.js:1462-1480: report a media change over two commands, each pending until
     * REQUEST SENSE (NOT PRESENT then UNIT ATTENTION) */
    if (ide->medium_changed && ide->buffer != NULL && cmd != ATAPI_CMD_REQUEST_SENSE &&
        cmd != ATAPI_CMD_INQUIRY) {
        if (ide->medium_changed == MEDIUM_CHANGED_NOT_PRESENT) {
            ide->medium_changed = MEDIUM_CHANGED_UNIT_ATTENTION;
            interface_atapi_check_condition_response(ide, ATAPI_SK_NOT_READY,
                                                     ATAPI_ASC_MEDIUM_NOT_PRESENT);
        } else {
            ide->medium_changed = MEDIUM_CHANGED_NONE;
            interface_atapi_check_condition_response(ide, ATAPI_SK_UNIT_ATTENTION,
                                                     ATAPI_ASC_MEDIUM_MAY_HAVE_CHANGED);
        }
        interface_push_irq(ide);
        return;
    }

    if (cmd != ATAPI_CMD_REQUEST_SENSE) { /* upstream's TODO, kept */
        ide->atapi_sense_key = 0;
        ide->atapi_add_sense = 0;
    }

    if (ide->buffer == NULL && (cmd_flags & ATAPI_CF_NEEDS_DISK)) {
        interface_atapi_check_condition_response(ide, ATAPI_SK_NOT_READY,
                                                 ATAPI_ASC_MEDIUM_NOT_PRESENT);
        interface_push_irq(ide);
        return;
    }

    switch (cmd) {
    case ATAPI_CMD_TEST_UNIT_READY:
        if (ide->buffer != NULL) {
            interface_data_allocate(ide, 0);
            ide->data_end = ide->data_length;
            ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        } else {
            interface_atapi_check_condition_response(ide, ATAPI_SK_NOT_READY,
                                                     ATAPI_ASC_MEDIUM_NOT_PRESENT);
        }
        break;

    case ATAPI_CMD_REQUEST_SENSE:
        interface_data_allocate(ide, (uint32_t)(ide->data[4] & 0xFF));
        ide->data_end = ide->data_length;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        ide->data[0] = 0x80 | 0x70; /* valid | SCSI error code */
        ide->data[2] = (uint8_t)ide->atapi_sense_key;
        ide->data[7] = 8; /* additional sense length, fixed 8 for error code 0x70 */
        if (ide->data_length > 12) ide->data[12] = (uint8_t)ide->atapi_add_sense;
        ide->atapi_sense_key = 0;
        ide->atapi_add_sense = 0;
        break;

    case ATAPI_CMD_INQUIRY: {
        uint32_t length = (uint32_t)(ide->data[4] & 0xFF);
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        /* [CD-SCSI-2] "INQUIRY Command": device type 5 (CD-ROM), removable, ANSI-1,
         * response format 0x31; vendor "SONY    ", product "CD-ROM CDU-1000 ", revision
         * "1.1a" */
        static const uint8_t inquiry[36] = {
            0x05, 0x80, 0x01, 0x31, 31, 0, 0, 0,
            0x53, 0x4F, 0x4E, 0x59, 0x20, 0x20, 0x20, 0x20,
            0x43, 0x44, 0x2D, 0x52, 0x4F, 0x4D, 0x20, 0x43,
            0x44, 0x55, 0x2D, 0x31, 0x30, 0x30, 0x30, 0x20,
            0x31, 0x2E, 0x31, 0x61,
        };
        interface_data_allocate_noclear(ide, 36);
        memcpy(ide->data, inquiry, 36);
        uint32_t len = length < 36 ? length : 36;
        ide->data_end = ide->data_length = len;
        break;
    }

    case ATAPI_CMD_MODE_SENSE_6:
        interface_data_allocate(ide, (uint32_t)(ide->data[4] & 0xFF));
        ide->data_end = ide->data_length;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        break;

    case ATAPI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        interface_data_allocate(ide, 0);
        ide->data_end = ide->data_length;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        break;

    case ATAPI_CMD_READ_CAPACITY: {
        uint32_t count = ide->sector_count - 1;
        uint8_t payload[8] = { (uint8_t)(count >> 24), (uint8_t)(count >> 16),
                               (uint8_t)(count >> 8), (uint8_t)count, 0, 0,
                               (uint8_t)(ide->sector_size >> 8 & 0xFF),
                               (uint8_t)(ide->sector_size & 0xFF) };
        interface_data_set(ide, payload, 8);
        ide->data_end = ide->data_length;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        break;
    }

    case ATAPI_CMD_READ_FORMAT_CAPACITIES: {
        uint32_t count = ide->sector_count - 1;
        uint8_t payload[12] = { 0, 0, 0, 8,
                                (uint8_t)(count >> 24), (uint8_t)(count >> 16),
                                (uint8_t)(count >> 8), (uint8_t)count, 2,
                                (uint8_t)(ide->sector_size >> 16 & 0xFF),
                                (uint8_t)(ide->sector_size >> 8 & 0xFF),
                                (uint8_t)(ide->sector_size & 0xFF) };
        interface_data_set(ide, payload, 12);
        ide->data_end = ide->data_length;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        break;
    }

    case ATAPI_CMD_READ_10:
    case ATAPI_CMD_READ_12:
        if (ide->features_reg & 1) {
            interface_atapi_read_dma(ide, ide->data);
        } else {
            interface_atapi_read(ide, ide->data);
        }
        break;

    case ATAPI_CMD_READ_SUBCHANNEL: {
        uint32_t length = (uint32_t)(ide->data[8] & 0xFF);
        interface_data_allocate(ide, length < 8 ? length : 8);
        ide->data_end = ide->data_length;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        break;
    }

    case ATAPI_CMD_READ_TOC_PMA_ATIP: {
        uint32_t length = (uint32_t)((ide->data[8] & 0xFF) | ((ide->data[7] & 0xFF) << 8));
        uint32_t format = (uint32_t)(ide->data[9] >> 6);
        /* the MSF flag is read BEFORE data_allocate overwrites the CDB (upstream's own
         * comment, ide.js:1622) */
        bool toc_msf = (ide->data[1] & 2) != 0;

        interface_data_allocate(ide, length);
        ide->data_end = ide->data_length;
        if (format == 0) {
            /* track 1 start and lead-out, in MSF when requested (Win9x sizes the disc from
             * the lead-out MSF) */
            uint32_t start[4], leadout[4];
            uint32_t leadout_lba = ide->sector_count;
            if (toc_msf) {
                uint32_t a = 150;
                start[0] = 0;
                start[1] = (uint32_t)((0 + a) / (75 * 60));
                start[2] = (uint32_t)(((0 + a) / 75) % 60);
                start[3] = (uint32_t)((0 + a) % 75);
                leadout[0] = 0;
                leadout[1] = (uint32_t)((leadout_lba + a) / (75 * 60));
                leadout[2] = (uint32_t)(((leadout_lba + a) / 75) % 60);
                leadout[3] = (uint32_t)((leadout_lba + a) % 75);
            } else {
                uint32_t lbas[2] = { 0, leadout_lba };
                uint32_t *dsts[2] = { start, leadout };
                for (int k = 0; k < 2; k++) {
                    dsts[k][0] = (uint32_t)(lbas[k] >> 24 & 0xFF);
                    dsts[k][1] = (uint32_t)(lbas[k] >> 16 & 0xFF);
                    dsts[k][2] = (uint32_t)(lbas[k] >> 8 & 0xFF);
                    dsts[k][3] = (uint32_t)(lbas[k] & 0xFF);
                }
            }
            uint8_t toc[20] = { 0, 18, 1, 1, 0, 0x14, 1, 0,
                                (uint8_t)start[0], (uint8_t)start[1], (uint8_t)start[2],
                                (uint8_t)start[3], 0, 0x16, 0xAA, 0,
                                (uint8_t)leadout[0], (uint8_t)leadout[1],
                                (uint8_t)leadout[2], (uint8_t)leadout[3] };
            uint32_t n = ide->data_capacity < 20 ? ide->data_capacity : 20;
            memcpy(ide->data, toc, n);
        } else if (format == 1) {
            static const uint8_t raw_toc[12] = { 0, 10, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
            uint32_t n = ide->data_capacity < 12 ? ide->data_capacity : 12;
            memcpy(ide->data, raw_toc, n);
        } else {
            /* ide.js:1667-1668: dbg_assert(false, "unimplemented format"), i.e. in the
             * release build an empty response; this port names it. */
            ide_note("%s: error: unimplemented format: %u", ide->name, (unsigned)format);
        }
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        break;
    }

    case ATAPI_CMD_GET_CONFIGURATION: {
        uint32_t length = (uint32_t)((ide->data[8] & 0xFF) | ((ide->data[7] & 0xFF) << 8));
        if (length > 32) length = 32;
        interface_data_allocate(ide, length);
        ide->data_end = ide->data_length;
        if (ide->data_length >= 11) {
            uint32_t payload = length - 4;
            ide->data[0] = (uint8_t)(payload >> 24 & 0xFF);
            ide->data[1] = (uint8_t)(payload >> 16 & 0xFF);
            ide->data[2] = (uint8_t)(payload >> 8 & 0xFF);
            ide->data[3] = (uint8_t)(payload & 0xFF);
            ide->data[6] = 0x08;
            ide->data[10] = 3;
        }
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        break;
    }

    case ATAPI_CMD_READ_DISK_INFORMATION:
        interface_data_allocate(ide, 0);
        ide->data_end = ide->data_length;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        break;

    case ATAPI_CMD_READ_TRACK_INFORMATION:
        interface_atapi_check_condition_response(ide, ATAPI_SK_ILLEGAL_REQUEST,
                                                 ATAPI_ASC_INV_FIELD_IN_CMD_PACKET);
        break;

    case ATAPI_CMD_MODE_SENSE_10: {
        uint32_t length = (uint32_t)((ide->data[8] & 0xFF) | ((ide->data[7] & 0xFF) << 8));
        uint32_t page_code = (uint32_t)(ide->data[2] & 0xFF);
        if (page_code == 0x2A) {
            interface_data_allocate(ide, length < 30 ? length : 30);
        }
        ide->data_end = ide->data_length;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        break;
    }

    case ATAPI_CMD_MECHANISM_STATUS: {
        uint32_t length = (uint32_t)((ide->data[9] & 0xFF) | ((ide->data[8] & 0xFF) << 8));
        interface_data_allocate(ide, length);
        ide->data_end = ide->data_length;
        if (ide->data_length > 5) ide->data[5] = 1;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
        break;
    }

    case ATAPI_CMD_START_STOP_UNIT: {
        int32_t loej_start = ide->data[4] & 0x3;
        if (ide->buffer != NULL && loej_start == 0x2) {
            ide->medium_changed = MEDIUM_CHANGED_NOT_PRESENT;
            /* ide.js:1724 nulls the CD's buffer -- the EJECT path.  This port's disk image
             * pointers are the machine's (a const buffer it does not own), so the media is
             * dropped by clearing the pointer the interface reads. */
            ide->buffer = NULL;
            ide->buffer_length = 0;
        }
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_data_allocate(ide, 0);
        ide->data_end = ide->data_length;
        break;
    }

    case ATAPI_CMD_PAUSE:
    case ATAPI_CMD_GET_EVENT_STATUS_NOTIFICATION:
    case ATAPI_CMD_REPORT_KEY:
    case ATAPI_CMD_READ_DISC_STRUCTURE:
        /* ide.js:1731-1737: "unimplemented" -- these four answer ILLEGAL REQUEST with
         * INVALID FIELD IN CDB, as upstream does */
        interface_atapi_check_condition_response(ide, ATAPI_SK_ILLEGAL_REQUEST,
                                                 ATAPI_ASC_INV_FIELD_IN_CMD_PACKET);
        break;

    case ATAPI_CMD_READ_CD:
        /* ide.js:1739-1744: "unimplemented" -- an empty, successful response */
        interface_data_allocate(ide, 0);
        ide->data_end = ide->data_length;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        break;

    default:
        /* ide.js:1746-1749: dbg_assert(false, ...) then ILLEGAL REQUEST */
        ide_note("%s: error: unimplemented ATAPI command 0x%02x", ide->name, (unsigned)cmd);
        interface_atapi_check_condition_response(ide, ATAPI_SK_ILLEGAL_REQUEST,
                                                 ATAPI_ASC_INV_FIELD_IN_CMD_PACKET);
        break;
    }

    ide->sector_count_reg = (ide->sector_count_reg & ~7) | 2;

    if ((ide->status_reg & ATA_SR_BSY) == 0) {
        interface_push_irq(ide);
    }

    if ((ide->status_reg & ATA_SR_BSY) == 0 && ide->data_length == 0) {
        ide->sector_count_reg |= 1;
        ide->status_reg &= ~ATA_SR_DRQ;
    }
}

/* ide.js:1185-1447 */
static void interface_ata_command(ide_interface_t *ide, int32_t cmd)
{
    if (!ide->drive_connected && cmd != ATA_CMD_EXECUTE_DEVICE_DIAGNOSTIC) {
        /* ide.js:1187-1191 */
        ide_note("%s: ATA command 0x%02x ignored: no drive connected", ide->name,
                 (unsigned)cmd);
        return;
    }

    ide->current_command = cmd;
    ide->error_reg = 0;

    switch (cmd) {
    case ATA_CMD_DEVICE_RESET:
        ide->data_pointer = 0;
        ide->data_end = 0;
        ide->data_length = 0;
        interface_device_reset(ide);
        interface_push_irq(ide);
        break;

    case ATA_CMD_10h:
        ide->lba_mid_reg = 0;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_push_irq(ide);
        break;

    case ATA_CMD_READ_NATIVE_MAX_ADDRESS: {
        uint32_t last_sector = ide->sector_count - 1;
        ide->lba_low_reg = (int32_t)(last_sector & 0xFF);
        ide->lba_mid_reg = (int32_t)(last_sector >> 8 & 0xFF);
        ide->lba_high_reg = (int32_t)(last_sector >> 16 & 0xFF);
        ide->device_reg =
            (int32_t)((ide->device_reg & 0xF0) | (int32_t)(last_sector >> 24 & 0x0F));
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_push_irq(ide);
        break;
    }

    case ATA_CMD_READ_NATIVE_MAX_ADDRESS_EXT: {
        uint32_t last_sector = ide->sector_count - 1;
        ide->lba_low_reg = (int32_t)(last_sector & 0xFF);
        ide->lba_mid_reg = (int32_t)(last_sector >> 8 & 0xFF);
        ide->lba_high_reg = (int32_t)(last_sector >> 16 & 0xFF);
        ide->lba_low_reg |= (int32_t)((last_sector >> 24) << 8 & 0xFF00);
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_push_irq(ide);
        break;
    }

    case ATA_CMD_READ_SECTORS:
        if (ide->is_atapi) {
            ide->lba_mid_reg = ATAPI_SIGNATURE_LO; /* see [ATA8-ACS] 4.3 */
            ide->lba_high_reg = ATAPI_SIGNATURE_HI;
            interface_ata_abort_command(ide);
        } else {
            interface_ata_read_sectors(ide, cmd);
        }
        break;

    case ATA_CMD_READ_SECTORS_EXT:
    case ATA_CMD_READ_MULTIPLE:
    case ATA_CMD_READ_MULTIPLE_EXT:
        if (ide->is_atapi) {
            interface_ata_abort_command(ide);
        } else {
            interface_ata_read_sectors(ide, cmd);
        }
        break;

    case ATA_CMD_WRITE_SECTORS:
    case ATA_CMD_WRITE_SECTORS_EXT:
    case ATA_CMD_WRITE_MULTIPLE:
    case ATA_CMD_WRITE_MULTIPLE_EXT:
        if (ide->is_atapi) {
            interface_ata_abort_command(ide);
        } else {
            interface_ata_write_sectors(ide, cmd);
        }
        break;

    case ATA_CMD_EXECUTE_DEVICE_DIAGNOSTIC:
        /* ide.js:1278-1289: "the behaviour of this command is independent of the selected
         * device" */
        ide->channel->master.status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        ide->channel->master.error_reg = 0x01;
        interface_push_irq(&ide->channel->master);
        if (ide->channel->slave.drive_connected) {
            ide->channel->slave.status_reg = ATA_SR_DRDY | ATA_SR_DSC;
            ide->channel->slave.error_reg = 0x01;
            interface_push_irq(&ide->channel->slave);
        }
        break;

    case ATA_CMD_INITIALIZE_DEVICE_PARAMETERS:
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_push_irq(ide);
        break;

    case ATA_CMD_PACKET:
        if (ide->is_atapi) {
            interface_data_allocate(ide, 12);
            ide->data_end = 12;
            ide->sector_count_reg = 0x01; /* 0x01: transfer of a command packet (C/D) */
            ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
            /* The 12-byte CDB is written through the data port; write_end dispatches into
             * the (unported) ATAPI handler -- interface_atapi_stop. */
        } else {
            interface_ata_abort_command(ide);
        }
        break;

    case ATA_CMD_IDENTIFY_PACKET_DEVICE:
        if (ide->is_atapi) {
            interface_create_identify_packet(ide);
            ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
            interface_push_irq(ide);
        } else {
            interface_ata_abort_command(ide);
        }
        break;

    case ATA_CMD_SET_MULTIPLE_MODE:
        /* ide.js:1324-1330: logical sectors per DRQ block in word 1 */
        ide->sectors_per_drq = ide->sector_count_reg & 0xFF;
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_push_irq(ide);
        break;

    case ATA_CMD_READ_LOG_EXT:
        interface_ata_abort_command(ide);
        break;

    case ATA_CMD_READ_DMA:
    case ATA_CMD_READ_DMA_EXT:
        interface_ata_read_sectors_dma(ide, cmd);
        break;

    case ATA_CMD_WRITE_DMA:
    case ATA_CMD_WRITE_DMA_EXT:
        interface_ata_write_sectors_dma(ide, cmd);
        break;

    case ATA_CMD_READ_VERIFY_SECTORS:
        /* ide.js:1348-1352's TODO ("check that lba and sector count are within the disk's
         * size") is upstream's, kept as the comment it is. */
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_push_irq(ide);
        break;

    case ATA_CMD_GET_MEDIA_STATUS:
        if (ide->is_atapi) {
            if (ide->buffer == NULL) ide->error_reg |= 0x02; /* NM: no media */
            if (ide->medium_changed) {
                ide->error_reg |= 0x20; /* MC: media change */
                ide->medium_changed = MEDIUM_CHANGED_NONE;
            }
            ide->error_reg |= 0x40; /* WP: write protect */
        }
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_push_irq(ide);
        break;

    case ATA_CMD_STANDBY_IMMEDIATE:
    case ATA_CMD_IDLE_IMMEDIATE:
    case ATA_CMD_FLUSH_CACHE:
    case ATA_CMD_FLUSH_CACHE_EXT:
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_push_irq(ide);
        break;

    case ATA_CMD_IDENTIFY_DEVICE:
        if (ide->is_atapi) {
            ide->lba_mid_reg = ATAPI_SIGNATURE_LO;
            ide->lba_high_reg = ATAPI_SIGNATURE_HI;
            interface_ata_abort_command(ide);
        } else {
            interface_create_identify_packet(ide);
            ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC | ATA_SR_DRQ;
            interface_push_irq(ide);
        }
        break;

    case ATA_CMD_SET_FEATURES:
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_push_irq(ide);
        break;

    case ATA_CMD_MEDIA_LOCK:
        ide->status_reg = ATA_SR_DRDY;
        interface_push_irq(ide);
        break;

    case ATA_CMD_SECURITY_FREEZE_LOCK:
        ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
        interface_push_irq(ide);
        break;

    case ATA_CMD_SET_MAX:
    case ATA_CMD_NOP:
        interface_ata_abort_command(ide);
        break;

    case ATA_CMD_F0h:
        /* ide.js:1430-1433: "unimplemented vendor-specific ATA command: ABORT" */
        ide_note("%s: unimplemented vendor-specific ATA command 0x%02x: ABORT", ide->name,
                 (unsigned)cmd);
        interface_ata_abort_command(ide);
        break;

    default:
        /* ide.js:1435-1438: dbg_assert(false, "unimplemented ATA command") then an abort.
         * In the pinned release build the assert is compiled out, so what a guest sees is
         * the ABORT -- which is what this port does, with the command named here. */
        ide_note("%s: unimplemented ATA command 0x%02x: ABORT", ide->name, (unsigned)cmd);
        interface_ata_abort_command(ide);
        break;
    }
}

/* ------------------------------------------------------------------ *
 * the channel -- ide.js:384-819
 * ------------------------------------------------------------------ */

/* ide.js:643-646 */
static int32_t channel_read_status(ide_channel_t *channel)
{
    return channel->current_interface->drive_connected ? channel->current_interface->status_reg
                                                       : 0;
}

/* ide.js:648-663 */
static void channel_write_control(ide_channel_t *channel, int32_t data)
{
    if (data & ATA_CR_SRST) {
        device_lower_irq((uint8_t)channel->irq);
        interface_device_reset(&channel->master);
        interface_device_reset(&channel->slave);
    }
    channel->device_control_reg = data;
}

/* ide.js:665-681 */
static uint32_t channel_dma_read_addr(ide_channel_t *channel) { return channel->prdt_addr; }

static void channel_dma_set_addr(ide_channel_t *channel, int32_t data)
{
    channel->prdt_addr = (uint32_t)data;
}

/* ide.js:683-699 */
static int32_t channel_dma_read_status(ide_channel_t *channel) { return channel->dma_status; }

static void channel_dma_write_status(ide_channel_t *channel, int32_t value)
{
    channel->dma_status &= ~(value & 6);
}

/* ide.js:701-713 */
static int32_t channel_dma_read_command(ide_channel_t *channel)
{
    return channel_dma_read_command8(channel) | channel_dma_read_status(channel) << 16;
}

static int32_t channel_dma_read_command8(ide_channel_t *channel) { return channel->dma_command; }

/* ide.js:715-770 */
static void channel_dma_write_command(ide_channel_t *channel, int32_t value)
{
    channel_dma_write_command8(channel, value & 0xFF);
    channel_dma_write_status(channel, value >> 16 & 0xFF);
}

static void channel_dma_write_command8(ide_channel_t *channel, int32_t value)
{
    int32_t old_command = channel->dma_command;
    channel->dma_command = value & 0x09;

    if ((old_command & 1) == (value & 1)) return;

    if ((value & 1) == 0) {
        channel->dma_status &= ~1;
        return;
    }

    channel->dma_status |= 1;

    switch (channel->current_interface->current_command) {
    case ATA_CMD_READ_DMA:
    case ATA_CMD_READ_DMA_EXT:
        interface_do_ata_read_sectors_dma(channel->current_interface);
        break;
    case ATA_CMD_WRITE_DMA:
    case ATA_CMD_WRITE_DMA_EXT:
        interface_do_ata_write_sectors_dma(channel->current_interface);
        break;
    case ATA_CMD_PACKET:
        interface_do_atapi_dma(channel->current_interface);
        break;
    default:
        /* ide.js:761-768: a spurious DMA command write clears status bit 0, sets bit 1 and
         * raises the IRQ. */
        channel->dma_status &= ~1;
        channel->dma_status |= 2;
        channel_push_irq(channel);
        break;
    }
}

/* ide.js:772-783 */
static void channel_push_irq(ide_channel_t *channel)
{
    if ((channel->device_control_reg & ATA_CR_NIEN) == 0) {
        channel->dma_status |= 4;
        device_raise_irq((uint8_t)channel->irq);
    }
}

/* ------------------------------------------------------------------ *
 * the port-map callbacks.  io.h's callbacks take the port (D3); reads take it too, so
 * upstream's per-width closures become one function per width per register.
 * ------------------------------------------------------------------ */

static int32_t port_data_read8(void *device, int32_t port)
{
    (void)port;
    int32_t value = interface_read_data(((ide_channel_t *)device)->current_interface, 1);
    ide_trace(port, 8, false, (uint32_t)value);
    return value;
}

static int32_t port_data_read16(void *device, int32_t port)
{
    (void)port;
    int32_t value = interface_read_data(((ide_channel_t *)device)->current_interface, 2);
    ide_trace(port, 16, false, (uint32_t)value);
    return value;
}

static int32_t port_data_read32(void *device, int32_t port)
{
    (void)port;
    int32_t value = interface_read_data(((ide_channel_t *)device)->current_interface, 4);
    ide_trace(port, 32, false, (uint32_t)value);
    return value;
}

static void port_data_write8(void *device, int32_t port, int32_t value)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)value);
    interface_write_data_port(((ide_channel_t *)device)->current_interface, value, 1);
}

static void port_data_write16(void *device, int32_t port, int32_t value)
{
    (void)port;
    ide_trace(port, 16, true, (uint32_t)value);
    interface_write_data_port(((ide_channel_t *)device)->current_interface, value, 2);
}

static void port_data_write32(void *device, int32_t port, int32_t value)
{
    (void)port;
    ide_trace(port, 32, true, (uint32_t)value);
    interface_write_data_port(((ide_channel_t *)device)->current_interface, value, 4);
}

static int32_t port_error_read8(void *device, int32_t port)
{
    (void)port;
    int32_t value = ((ide_channel_t *)device)->current_interface->error_reg & 0xFF;
    ide_trace(port, 8, false, (uint32_t)value);
    return value;
}

static int32_t port_sector_read8(void *device, int32_t port)
{
    (void)port;
    int32_t value = ((ide_channel_t *)device)->current_interface->sector_count_reg & 0xFF;
    ide_trace(port, 8, false, (uint32_t)value);
    return value;
}

static int32_t port_lba_low_read8(void *device, int32_t port)
{
    (void)port;
    int32_t value = ((ide_channel_t *)device)->current_interface->lba_low_reg & 0xFF;
    ide_trace(port, 8, false, (uint32_t)value);
    return value;
}

static int32_t port_lba_mid_read8(void *device, int32_t port)
{
    (void)port;
    int32_t value = ((ide_channel_t *)device)->current_interface->lba_mid_reg & 0xFF;
    ide_trace(port, 8, false, (uint32_t)value);
    return value;
}

static int32_t port_lba_high_read8(void *device, int32_t port)
{
    (void)port;
    int32_t value = ((ide_channel_t *)device)->current_interface->lba_high_reg & 0xFF;
    ide_trace(port, 8, false, (uint32_t)value);
    return value;
}

static int32_t port_device_read8(void *device, int32_t port)
{
    (void)port;
    int32_t value = ((ide_channel_t *)device)->current_interface->device_reg & 0xFF;
    ide_trace(port, 8, false, (uint32_t)value);
    return value;
}

/* ide.js:490-499: a read of the STATUS register also lowers the IRQ line. */
static int32_t port_status_read8(void *device, int32_t port)
{
    (void)port;
    ide_channel_t *channel = device;
    int32_t status = channel_read_status(channel);
    device_lower_irq((uint8_t)channel->irq);
    ide_trace(port, 8, false, (uint32_t)status);
    return status;
}

/* ide.js:610: the alternate status register does NOT lower the IRQ (that is the whole
 * point of the register, and upstream's read is the bare read_status). */
static int32_t port_alt_status_read8(void *device, int32_t port)
{
    (void)port;
    int32_t value = channel_read_status(device);
    ide_trace(port, 8, false, (uint32_t)value);
    return value;
}

/* ide.js:512-560 -- the FEATURES/SECTOR/LBA_* writes reach BOTH devices; upstream's own
 * asymmetry with the reads below, which go to the SELECTED one. */
static void port_wide_write8(ide_channel_t *channel, int32_t data, int field)
{
    int32_t *master;
    int32_t *slave;
    switch (field) {
    case 0: master = &channel->master.features_reg; slave = &channel->slave.features_reg; break;
    case 1: master = &channel->master.sector_count_reg; slave = &channel->slave.sector_count_reg; break;
    case 2: master = &channel->master.lba_low_reg; slave = &channel->slave.lba_low_reg; break;
    case 3: master = &channel->master.lba_mid_reg; slave = &channel->slave.lba_mid_reg; break;
    default: master = &channel->master.lba_high_reg; slave = &channel->slave.lba_high_reg; break;
    }
    *master = (*master << 8 | data) & 0xFFFF;
    *slave = (*slave << 8 | data) & 0xFFFF;
}

static void port_features_write8(void *device, int32_t port, int32_t data)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)data);
    port_wide_write8(device, data, 0);
}

static void port_sector_write8(void *device, int32_t port, int32_t data)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)data);
    port_wide_write8(device, data, 1);
}

static void port_lba_low_write8(void *device, int32_t port, int32_t data)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)data);
    port_wide_write8(device, data, 2);
}

static void port_lba_mid_write8(void *device, int32_t port, int32_t data)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)data);
    port_wide_write8(device, data, 3);
}

static void port_lba_high_write8(void *device, int32_t port, int32_t data)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)data);
    port_wide_write8(device, data, 4);
}

/* ide.js:562-588 */
static void port_device_write8(void *device, int32_t port, int32_t data)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)data);
    ide_channel_t *channel = device;
    int32_t select_slave = data & ATA_DR_DEV;

    if ((select_slave && channel->current_interface == &channel->master) ||
        (!select_slave && channel->current_interface == &channel->slave)) {
        channel->current_interface = select_slave ? &channel->slave : &channel->master;
    }
    channel->master.device_reg = data;
    channel->slave.device_reg = data;
    /* ide.js:584-587: bit 6 and the low nibble -- upstream's own "TODO: where does this
     * definition come from? not in [ATA-6] or [ATA-4]!", kept as it is. */
    channel->master.is_lba = data >> 6 & 1;
    channel->slave.is_lba = data >> 6 & 1;
    channel->master.head = data & 0xF;
    channel->slave.head = data & 0xF;
}

/* ide.js:590-603 */
static void port_command_write8(void *device, int32_t port, int32_t data)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)data);
    ide_channel_t *channel = device;
    channel->current_interface->status_reg &= ~(ATA_SR_ERR | ATA_SR_DF);
    interface_ata_command(channel->current_interface, data);
    device_lower_irq((uint8_t)channel->irq);
}

static void port_control_write8(void *device, int32_t port, int32_t data)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)data);
    channel_write_control(device, data);
}

static int32_t port_bmi_read8(void *device, int32_t port)
{
    (void)port;
    int32_t value = channel_dma_read_command8(device);
    ide_trace(port, 8, false, (uint32_t)value);
    return value;
}

static int32_t port_bmi_read32(void *device, int32_t port)
{
    (void)port;
    int32_t value = channel_dma_read_command(device);
    ide_trace(port, 32, false, (uint32_t)value);
    return value;
}

static void port_bmi_write8(void *device, int32_t port, int32_t value)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)value);
    channel_dma_write_command8(device, value);
}

static void port_bmi_write32(void *device, int32_t port, int32_t value)
{
    (void)port;
    ide_trace(port, 32, true, (uint32_t)value);
    channel_dma_write_command(device, value);
}

static int32_t port_bmi_status_read8(void *device, int32_t port)
{
    (void)port;
    int32_t value = channel_dma_read_status(device);
    ide_trace(port, 8, false, (uint32_t)value);
    return value;
}

static void port_bmi_status_write8(void *device, int32_t port, int32_t value)
{
    (void)port;
    ide_trace(port, 8, true, (uint32_t)value);
    channel_dma_write_status(device, value);
}

static int32_t port_bmi_prdt_read32(void *device, int32_t port)
{
    (void)port;
    int32_t value = (int32_t)channel_dma_read_addr(device);
    ide_trace(port, 32, false, (uint32_t)value);
    return value;
}

static void port_bmi_prdt_write32(void *device, int32_t port, int32_t value)
{
    (void)port;
    ide_trace(port, 32, true, (uint32_t)value);
    channel_dma_set_addr(device, value);
}

/* ------------------------------------------------------------------ *
 * the interfaces' shared constructor -- ide.js:827-945
 * ------------------------------------------------------------------ */
static void interface_init(ide_channel_t *channel, ide_interface_t *ide, int32_t interface_nr,
                           const char *name, ide_disk_t config)
{
    memset(ide, 0, sizeof(*ide));
    ide->channel = channel;
    ide->interface_nr = interface_nr;
    ide->name = name;

    ide->buffer = config.buffer;
    ide->buffer_length = config.buffer != NULL ? config.length : 0;

    /* ide.js:847: `this.drive_connected = is_cd || !!buffer` */
    ide->drive_connected = config.is_cdrom || config.buffer != NULL;
    ide->sector_size = config.is_cdrom ? CDROM_SECTOR_SIZE : HD_SECTOR_SIZE;
    ide->is_atapi = config.is_cdrom;
    ide->head_count = ide->is_atapi ? 1 : 0;
    ide->status_reg = ATA_SR_DRDY | ATA_SR_DSC;
    ide->sectors_per_drq = 0x80;
    ide->current_command = -1;

    /* ide.js:903: `new Uint8Array(64 * 1024)` -- the transfer buffer every command shares */
    interface_data_allocate_noclear(ide, 64 * 1024);

    /* ide.js:937 */
    interface_set_disk_buffer(ide);
}

/* ------------------------------------------------------------------ *
 * the channel's constructor -- ide.js:384-641
 * ------------------------------------------------------------------ */
static void channel_init(ide_channel_t *channel, bus_connector_t *bus, int32_t channel_nr,
                         int32_t command_base, int32_t control_base, int32_t irq,
                         const char *name, ide_disk_t master_cfg, ide_disk_t slave_cfg)
{
    static const char *interface_names[IDE_CHANNELS][IDE_INTERFACES_PER_CHANNEL] = {
        { "ide0.0", "ide0.1" },
        { "ide1.0", "ide1.1" },
    };

    memset(channel, 0, sizeof(*channel));
    channel->channel_nr = channel_nr;
    channel->bus = bus;
    channel->command_base = command_base;
    channel->control_base = control_base;
    channel->irq = irq;
    channel->name = name;

    interface_init(channel, &channel->master, 0, interface_names[channel_nr][0], master_cfg);
    interface_init(channel, &channel->slave, 1, interface_names[channel_nr][1], slave_cfg);

    channel->current_interface = &channel->master;   /* ide.js:400 */
    channel->device_control_reg = ATA_CR_NIEN;       /* ide.js:403 */

    /* ide.js:420-603 -- the command block registers */
    io_register_read(command_base | ATA_REG_DATA, channel, port_data_read8, port_data_read16,
                     port_data_read32);
    io_register_read(command_base | ATA_REG_ERROR, channel, port_error_read8, NULL, NULL);
    io_register_read(command_base | ATA_REG_SECTOR, channel, port_sector_read8, NULL, NULL);
    io_register_read(command_base | ATA_REG_LBA_LOW, channel, port_lba_low_read8, NULL, NULL);
    io_register_read(command_base | ATA_REG_LBA_MID, channel, port_lba_mid_read8, NULL, NULL);
    io_register_read(command_base | ATA_REG_LBA_HIGH, channel, port_lba_high_read8, NULL, NULL);
    io_register_read(command_base | ATA_REG_DEVICE, channel, port_device_read8, NULL, NULL);
    io_register_read(command_base | ATA_REG_STATUS, channel, port_status_read8, NULL, NULL);

    io_register_write(command_base | ATA_REG_DATA, channel, port_data_write8, port_data_write16,
                      port_data_write32);
    io_register_write(command_base | ATA_REG_FEATURES, channel, port_features_write8, NULL, NULL);
    io_register_write(command_base | ATA_REG_SECTOR, channel, port_sector_write8, NULL, NULL);
    io_register_write(command_base | ATA_REG_LBA_LOW, channel, port_lba_low_write8, NULL, NULL);
    io_register_write(command_base | ATA_REG_LBA_MID, channel, port_lba_mid_write8, NULL, NULL);
    io_register_write(command_base | ATA_REG_LBA_HIGH, channel, port_lba_high_write8, NULL, NULL);
    io_register_write(command_base | ATA_REG_DEVICE, channel, port_device_write8, NULL, NULL);
    io_register_write(command_base | ATA_REG_COMMAND, channel, port_command_write8, NULL, NULL);

    /* ide.js:609-613 -- the control block register */
    io_register_read(control_base | ATA_REG_ALT_STATUS, channel, port_alt_status_read8, NULL,
                     NULL);
    io_register_write(control_base | ATA_REG_CONTROL, channel, port_control_write8, NULL, NULL);

    /* ide.js:620-638 -- the bus-master registers (BAR4, +8 for the secondary channel) */
    int32_t bus_master_base = BUS_MASTER_BASE + channel_nr * 8;
    io_register_read(bus_master_base | BMI_REG_COMMAND, channel, port_bmi_read8, NULL,
                     port_bmi_read32);
    io_register_write(bus_master_base | BMI_REG_COMMAND, channel, port_bmi_write8, NULL,
                      port_bmi_write32);
    io_register_read(bus_master_base | BMI_REG_STATUS, channel, port_bmi_status_read8, NULL, NULL);
    io_register_write(bus_master_base | BMI_REG_STATUS, channel, port_bmi_status_write8, NULL,
                      NULL);
    io_register_read(bus_master_base | BMI_REG_PRDT, channel, NULL, NULL, port_bmi_prdt_read32);
    io_register_write(bus_master_base | BMI_REG_PRDT, channel, NULL, NULL, port_bmi_prdt_write32);
}

/* ------------------------------------------------------------------ *
 * the controller -- ide.js:290-363
 * ------------------------------------------------------------------ */

void ide_init(bus_connector_t *bus, const ide_disk_t *primary_master,
              const ide_disk_t *primary_slave, const ide_disk_t *secondary_master)
{
    /* ide.js:298-299: `has_primary = ide_config && ide_config[0][0]`.  A NULL slot here is
     * upstream's `undefined`. */
    bool has_primary = primary_master != NULL;
    bool has_secondary = secondary_master != NULL;

    memset(ide_channels, 0, sizeof(ide_channels));
    memset(ide_channel_present, 0, sizeof(ide_channel_present));

    if (!has_primary && !has_secondary) return;

    ide_disk_t missing = { NULL, 0, false };

    if (has_primary) {
        /* ide.js:302-305.  A slave needs a master: upstream builds the slave's config from
         * the same channel entry, and a slave with no media is a missing device. */
        channel_init(&ide_channels[0], bus, 0, 0x1F0, 0x3F6, 14, "ide0", *primary_master,
                     primary_slave != NULL ? *primary_slave : missing);
        ide_channel_present[0] = true;
    }
    if (has_secondary) {
        channel_init(&ide_channels[1], bus, 1, 0x170, 0x376, 15, "ide1", *secondary_master,
                     missing);
        ide_channel_present[1] = true;
    }

    /* ide.js:311-358 -- the PCI function (00:1e.0, the PIIX3 IDE).  Its config space is
     * BUILT from the two channels' bases (upstream builds the array at construction), so it
     * is built here rather than being a table. */
    uint32_t command_base0 = has_primary ? (uint32_t)ide_channels[0].command_base : 0;
    uint32_t control_base0 = has_primary ? (uint32_t)ide_channels[0].control_base : 0;
    uint32_t command_base1 = has_secondary ? (uint32_t)ide_channels[1].command_base : 0;
    uint32_t control_base1 = has_secondary ? (uint32_t)ide_channels[1].control_base : 0;

    static uint8_t ide_pci_space[160];
    memset(ide_pci_space, 0, sizeof(ide_pci_space));

    ide_pci_space[0] = 0x86;
    ide_pci_space[1] = 0x80; /* vendor 8086: Intel */
    ide_pci_space[2] = 0x10;
    ide_pci_space[3] = 0x70; /* device 7010: 82371SB PIIX3 IDE */
    ide_pci_space[4] = 0x05;
    ide_pci_space[5] = 0x00;
    ide_pci_space[6] = 0xA0;
    ide_pci_space[7] = 0x02;
    ide_pci_space[8] = 0x00;
    ide_pci_space[9] = 0x80;  /* prog_if: ISA compatibility mode only, supports bus mastering */
    ide_pci_space[10] = 0x01; /* subclass: IDE controller */
    ide_pci_space[11] = 0x01; /* class: mass storage controller */
    /* 16/20: BAR0/BAR1, the primary channel's command and control blocks */
    ide_pci_space[16] = (uint8_t)((command_base0 & 0xFF) | 1);
    ide_pci_space[17] = (uint8_t)(command_base0 >> 8);
    ide_pci_space[20] = (uint8_t)((control_base0 & 0xFF) | 1);
    ide_pci_space[21] = (uint8_t)(control_base0 >> 8);
    /* 24/28: BAR2/BAR3, the secondary channel's */
    ide_pci_space[24] = (uint8_t)((command_base1 & 0xFF) | 1);
    ide_pci_space[25] = (uint8_t)(command_base1 >> 8);
    ide_pci_space[28] = (uint8_t)((control_base1 & 0xFF) | 1);
    ide_pci_space[29] = (uint8_t)(control_base1 >> 8);
    /* 32: BAR4, the bus-master registers of both channels */
    ide_pci_space[32] = (uint8_t)((BUS_MASTER_BASE & 0xFF) | 1);
    ide_pci_space[33] = (uint8_t)(BUS_MASTER_BASE >> 8);
    /* 44-47: subsystem vendor/device (upstream's own values) */
    ide_pci_space[44] = 0x43;
    ide_pci_space[45] = 0x10;
    ide_pci_space[46] = 0xD4;
    ide_pci_space[47] = 0x82;
    ide_pci_space[60] = 0x00; /* interrupt LINE: IRQs 14/15 are predefined in compatibility
                               * mode, and upstream's array has 0 here */
    ide_pci_space[61] = 0x01; /* interrupt PIN: INTA (upstream's array byte 0x3D) */
    /* Everything else is zero, as upstream's array is. */

    static pci_bar_t ide_bars[5];
    memset(ide_bars, 0, sizeof(ide_bars));
    ide_bars[0].size = has_primary ? 8 : 0;   /* BAR0: primary command block */
    ide_bars[1].size = has_primary ? 1 : 0;   /* BAR1: primary control block */
    ide_bars[2].size = has_secondary ? 8 : 0; /* BAR2: secondary command block */
    ide_bars[3].size = has_secondary ? 1 : 0; /* BAR3: secondary control block */
    ide_bars[4].size = 16;                    /* BAR4: bus master, both channels */

    static pci_device_t ide_device;
    memset(&ide_device, 0, sizeof(ide_device));
    ide_device.pci_id = IDE_PCI_ID;
    ide_device.pci_space = ide_pci_space;
    ide_device.pci_space_length = (int32_t)sizeof(ide_pci_space);
    ide_device.pci_bars = ide_bars;
    ide_device.pci_bars_count = 5;
    ide_device.name = "ide";

    pci_register_device(&ide_device);
}

/* The selftest's view of the machine's device set (diagnostics only; a guest learns the
 * same thing from the port map). */
bool ide_channel_is_present(int32_t channel_nr);
bool ide_channel_is_present(int32_t channel_nr)
{
    return channel_nr >= 0 && channel_nr < IDE_CHANNELS && ide_channel_present[channel_nr];
}
