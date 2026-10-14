#!/bin/sh
# results/m2-1-mut-rig.sh -- M2-1's ASYMMETRY RIG (the brief's G2).
#
# For each single-line mutation below: apply it, rebuild build/hw_selftest, run it, and
# REQUIRE a non-zero exit (the mutation must be visible); then restore the source and
# re-require a zero exit (the restored tree must be green again).  The asymmetry -- fails
# before, passes after -- is the proof that the selftest's assertions are load-bearing.
#
# A mutation that does not compile, or that the selftest cannot see, is NOT a pass: it is
# printed as such and must be replaced.  (m1c's rig records the same rule; two of M1a-2's
# first-attempt mutations were invalid for exactly these two reasons.)
#
# Usage: sh results/m2-1-mut-rig.sh          (from the project root)

set -u
cd "$(dirname "$0")/.." || exit 2

rig_fail=0

run_case() {
    label="$1"; file="$2"; old="$3"; new="$4"

    cp "$file" /tmp/m2-1-mut-backup.c
    if ! python3 - "$file" "$old" "$new" <<'PY'
import sys
path, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(path).read()
if s.count(old) != 1:
    sys.exit("MUTATION TARGET NOT UNIQUE (%d matches): %s" % (s.count(old), old))
open(path, 'w').write(s.replace(old, new))
PY
    then
        echo "CASE $label: INVALID (could not apply the mutation)"
        cp /tmp/m2-1-mut-backup.c "$file"
        rig_fail=1
        return
    fi

    if make build/hw_selftest >/tmp/m2-1-mut-build.log 2>&1; then
        if ./build/hw_selftest >/tmp/m2-1-mut-run.log 2>&1; then
            echo "CASE $label: INVALID (the mutation COMPILED and the selftest still PASSED)"
            rig_fail=1
        else
            echo "CASE $label: PASS (mutated -> hw_selftest FAILS, $(grep -c '^FAIL' /tmp/m2-1-mut-run.log) failing assertion(s))"
            grep '^FAIL' /tmp/m2-1-mut-run.log | head -2 | sed 's/^/    /'
        fi
    else
        echo "CASE $label: INVALID (the mutation did not compile)"
        rig_fail=1
    fi

    cp /tmp/m2-1-mut-backup.c "$file"
    if make build/hw_selftest >/tmp/m2-1-mut-build.log 2>&1 && ./build/hw_selftest >/tmp/m2-1-mut-run.log 2>&1; then
        echo "    restored -> hw_selftest PASSES again ($(grep -o '[0-9]* checks, [0-9]* failures' /tmp/m2-1-mut-run.log))"
    else
        echo "    restored -> hw_selftest STILL FAILS (the restore is broken!)"
        rig_fail=1
    fi
}

echo "=== M2-1 asymmetry (each mutation alone, then restored) ==="
date -u

# 1. the RTC's status-C read must clear only the TOP nibble
run_case "M1 rtc status-C clear mask" src/hw/rtc.c \
    "rtc->cmos_c &= ~0xF0;" "rtc->cmos_c &= ~0x0F;"

# 2. fill_cmos's extended-memory low byte
run_case "M2 rtc fill_cmos lsb/msb" src/hw/rtc.c \
    "rtc_cmos_write(CMOS_MEM_EXTMEM_LOW, memory_above_1m & 0xFF);" \
    "rtc_cmos_write(CMOS_MEM_EXTMEM_LOW, memory_above_1m >> 8 & 0xFF);"

# 3. the PIT's start value must follow the reload
run_case "M3 pit start value follows reload" src/hw/pit.c \
    "pit->counter_start_value[i] = pit->counter_reload[i];" \
    "pit->counter_start_value[i] = 0;"

# 4. the PIT latch returns the value MINUS ONE (upstream's quirk)
run_case "M4 pit latch value-1" src/hw/pit.c \
    "pit->counter_latch_value[i] = (uint16_t)(value ? value - 1 : 0);" \
    "pit->counter_latch_value[i] = (uint16_t)value;"

# 5. the DMA's autoinit must restore the init address (not clear it)
run_case "M5 dma autoinit restore" src/hw/dma.c \
    "        dma->channel_addr[channel] = dma->channel_addr_init[channel];" \
    "        dma->channel_addr[channel] = 0;"

# 6. the DMA's 16-bit channel doubling
run_case "M6 dma count_get_8bit doubling" src/hw/dma.c \
    "    if (channel >= 5) {
        count *= 2;
    }" \
    "    if (channel >= 2) {
        count *= 2;
    }"

# 7. the PS/2 status port's always-set bit 4
run_case "M7 ps2 status bit 4" src/hw/ps2.c \
    "    int32_t status_byte = 0x10;" "    int32_t status_byte = 0x00;"

# 8. the PS/2 mouse packet's always-set bit 3
run_case "M8 ps2 mouse info bit 3" src/hw/ps2.c \
    "(dy < 0) << 5 | (dx < 0) << 4 | 1 << 3 | ps2->mouse_clicks" \
    "(dy < 0) << 5 | (dx < 0) << 4 | ps2->mouse_clicks"

# 9. the PCI query's status register
run_case "M9 pci query status" src/hw/pci.c \
    "pci_set_status32((int32_t)0x80000000u);" "pci_set_status32(0);"

# 10. the PCI absent-device read's response
run_case "M10 pci absent device response" src/hw/pci.c \
    "        pci_set_response32(-1);
        pci_set_status32(0);" \
    "        pci_set_response32(0);
        pci_set_status32(0);"

# 11. the ACPI PM1 event's status bit
run_case "M11 acpi pm1_status bit" src/hw/acpi.c \
    "        acpi->pm1_status |= 1;" "        acpi->pm1_status = 0;"

# 12. the I/O bridge: the PIC's port 0x21 in FRONT of the map
run_case "M12 io bridge pic port 0x21" src/hw/io.c \
    "    case 0x21: port21_write((uint8_t)value); break;" \
    "    case 0x21: break;"

# 12b. the DMA's channel-advance amount, as a single-line edit inside dma_do_read
run_case "M12b dma do_read advance" src/hw/dma.c \
    "    dma->channel_addr[channel] = (uint16_t)(dma->channel_addr[channel] + read_count);

    write_blob" \
    "    dma->channel_addr[channel] = (uint16_t)(dma->channel_addr[channel] + read_count - 1);

    write_blob"

# 13. the mmap block lookup (the same expression appears in the 32-bit shim, so the target
# carries the function's own line above it)
run_case "M13 io mmap block shift" src/hw/io.c \
    "static int32_t io_mmap_read8(uint32_t addr)
{
    int32_t (*fn)(uint32_t) = io_mmap[addr >> MMAP_BLOCK_BITS].read8;" \
    "static int32_t io_mmap_read8(uint32_t addr)
{
    int32_t (*fn)(uint32_t) = io_mmap[0].read8;"

# 14. the device bus's name match
run_case "M14 bus name match" src/hw/bus.c \
    "        if (strcmp(listener->name, name) == 0) {" \
    "        if (strcmp(listener->name, name) != 0) {"

# 15. the timer poll: the devices must be driven at all
run_case "M15 machine poll drives the devices" src/hw/machine.c \
    "    if (!machine_ready) return t;" "    return t;"

echo
if [ "$rig_fail" -eq 0 ]; then
    echo "=== all 15 mutations were VISIBLE (and the restore is green) ==="
else
    echo "=== AT LEAST ONE MUTATION WAS INVALID (see INVALID above) ==="
fi
exit "$rig_fail"
