/*
 * ide.h -- the ATA/ATAPI IDE controller (the primary and secondary channels).
 *
 * Port of v86/src/ide.js (2917 lines): the controller and its two channels, the four
 * device slots (primary/secondary x master/slave), the ATA task file, PIO and bus-master
 * DMA, IDENTIFY DEVICE, the READ/WRITE commands, the status/error state machine, and the
 * two IRQ lines.
 *
 * WHAT THE BALIVEAU LANE NEEDS (the consumer, /home/jaye/futaie/baliveau/todo.md ss7:
 * "`/dev/hda` in fx-init") is a hard disk the guest kernel can read: the ATA side.  That
 * is what this unit ports in full.
 *
 * ATAPI/CD-ROM IS DEFERRED, AND THIS IS WHAT A GUEST OBSERVES (the brief's requirement to
 * say so loudly rather than half-port it):
 *
 *   - The DEVICE is here, because the reference machine always has one: cpu.js:1226 builds
 *     `ide_config[1][0] = { is_cdrom: true, buffer: settings.cdrom }` unconditionally, so
 *     the reference registers the IDE controller (and its PCI function 00:1e.0, which the
 *     Linux kernel's own dmesg shows in results/m2-2-js-serial.txt) in EVERY configuration,
 *     including one with no disk at all.  A port that omitted the secondary channel would
 *     change the machine's PCI layout and the kernel's probe of the secondary channel, i.e.
 *     it would be a different machine from the one the gate compares against.  So the
 *     secondary ATAPI device exists, is `drive_connected`, resets to the ATAPI signature
 *     and answers IDENTIFY PACKET DEVICE with the identify packet -- its IDENTITY.
 *   - The COMMAND SET is not: `atapi_handle` (ide.js:1449-1772: PACKET and the SCSI-2/MMC-2
 *     CDBs, READ CD, READ TOC, MODE SENSE, the sense/medium-change state) and the ISO9660
 *     side (v86/src/iso9660.js) are NOT ported.  A guest that writes a PACKET command and
 *     then a CDB STOPS with the command named (ide.c's atapi_handle) instead of getting a
 *     fabricated sense; a guest that never sends one -- the baliveau disk path, and any
 *     machine whose CD-ROM is never opened -- cannot tell the difference, because without
 *     media every CDB with ATAPI_CF_NEEDS_DISK answers the same CHECK CONDITION anyway.
 *     The unit report states which of those two cases the gate's kernel actually reaches.
 *
 * NOT PORTED, with the reason:
 *
 *   - `get_state`/`set_state` (ide.js:365-377, 785-819, 2846-2917): this port has no
 *     state-save machinery at all (uart.h says the same for the UART).
 *   - every `dbg_log`: there is no log channel in this port (the M1b-1 rule).  The text is
 *     kept in comments where it carries information.
 *   - `eject`/`set_cdrom` (the host's media-change path, ide.js:952-971): they are driven by
 *     the reference's UI/starter, not by a guest; a machine built with a CD image gets it at
 *     construction, and nothing changes media at run time here.
 *   - the `AbortController`/io-id cancellation bookkeeping (ide.js:2815-2844): it exists to
 *     cancel an ASYNCHRONOUS buffer read.  This port's disk image is in host memory and its
 *     `read_buffer` completes synchronously -- which is upstream's own SyncBuffer behaviour
 *     (`async: false`, which is what both legs of the gate use), where nothing is ever in
 *     flight at a cancellation point.  `cancel_io_operations` is kept as a named no-op.
 */

#ifndef V86C_HW_IDE_H
#define V86C_HW_IDE_H

#include <stdbool.h>
#include <stdint.h>

#include "bus.h"

/* cpu.js:1219-1228: one entry of `ide_config`, i.e. one device slot.  `buffer == NULL`
 * with `is_cdrom == false` is the reference's MISSING device; `buffer == NULL` with
 * `is_cdrom == true` is an ATAPI drive with no media; a buffer with `is_cdrom == false` is
 * a hard disk. */
typedef struct ide_disk {
    const uint8_t *buffer;
    uint32_t length;
    bool is_cdrom;
} ide_disk_t;

/* cpu.js:1225-1228: `new IDEController(this, device_bus, ide_config)`.  The PRIMARY channel
 * exists iff a primary master was given (ide.js:298-304: `has_primary = ide_config[0][0]`);
 * a primary slave needs a master (the constructor's own rule); the SECONDARY channel exists
 * iff a secondary master was given, which the reference's ide_config always provides. */
void ide_init(bus_connector_t *bus, const ide_disk_t *primary_master,
              const ide_disk_t *primary_slave, const ide_disk_t *secondary_master);

/* The controller's name ("ide") and pci function, for the machine's diagnostics and the
 * selftest's config-space case. */
#define IDE_PCI_ID (0x1E << 3)

/* Whether the machine built a channel (0 = primary, 1 = secondary).  Diagnostics and the
 * selftest's "the machine's device set is what the config says" case; a guest learns the
 * same fact from the port map. */
bool ide_channel_is_present(int32_t channel_nr);

/* ide.js:175-197 -- the ATAPI (SCSI-2/MMC-2) command set */
#define ATAPI_CMD_TEST_UNIT_READY       0x00
#define ATAPI_CMD_REQUEST_SENSE         0x03
#define ATAPI_CMD_INQUIRY               0x12
#define ATAPI_CMD_MODE_SENSE_6          0x1A
#define ATAPI_CMD_START_STOP_UNIT       0x1B
#define ATAPI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1E
#define ATAPI_CMD_READ_FORMAT_CAPACITIES 0x23
#define ATAPI_CMD_READ_CAPACITY         0x25
#define ATAPI_CMD_READ_10               0x28
#define ATAPI_CMD_READ_SUBCHANNEL       0x42
#define ATAPI_CMD_READ_TOC_PMA_ATIP     0x43
#define ATAPI_CMD_PAUSE                 0x45
#define ATAPI_CMD_GET_CONFIGURATION     0x46
#define ATAPI_CMD_GET_EVENT_STATUS_NOTIFICATION 0x4A
#define ATAPI_CMD_READ_DISK_INFORMATION 0x51
#define ATAPI_CMD_READ_TRACK_INFORMATION 0x52
#define ATAPI_CMD_MODE_SENSE_10         0x5A
#define ATAPI_CMD_READ_12               0xA8
#define ATAPI_CMD_REPORT_KEY            0xA4
#define ATAPI_CMD_READ_DISC_STRUCTURE   0xAD
#define ATAPI_CMD_MECHANISM_STATUS      0xBD
#define ATAPI_CMD_READ_CD               0xBE

/* ide.js:199-202 -- the per-command flags.  MEASURED against ide.js:205-229: every entry
 * uses either NONE or NEEDS_DISK; ATAPI_CF_UNIT_ATTN is defined upstream and USED BY NO
 * COMMAND, so it has no arm here. */
#define ATAPI_CF_NONE       0x00
#define ATAPI_CF_NEEDS_DISK 0x01

/* ide.js:236-251 -- the sense keys and additional sense codes */
#define ATAPI_SK_NO_SENSE           0
#define ATAPI_SK_RECOVERED_ERROR    1
#define ATAPI_SK_NOT_READY          2
#define ATAPI_SK_MEDIUM_ERROR       3
#define ATAPI_SK_HARDWARE_ERROR     4
#define ATAPI_SK_ILLEGAL_REQUEST    5
#define ATAPI_SK_UNIT_ATTENTION     6
#define ATAPI_SK_DATA_PROTECT       7
#define ATAPI_SK_BLANK_CHECK        8
#define ATAPI_SK_ABORTED_COMMAND    11
#define ATAPI_ASC_INV_FIELD_IN_CMD_PACKET 0x24
#define ATAPI_ASC_MEDIUM_MAY_HAVE_CHANGED 0x28
#define ATAPI_ASC_MEDIUM_NOT_PRESENT 0x3A

#endif /* V86C_HW_IDE_H */
