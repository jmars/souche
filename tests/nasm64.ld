/* tests/nasm64.ld -- the link geometry of the x86-64 corpus (S4).
 *
 * THREE FIXED ADDRESSES AND NOTHING ELSE MOVES (results/x86-64-plan.md ss2.1.1/ss2.1.2):
 *
 *   0x80000  .stub   the 32-bit machine setup: the multiboot address table, the page
 *                    tables, CR4.PAE / EFER.LME / CR0.PG, the GDT, the far jump to the
 *                    body.  ONLY the emulated leg runs it -- the native harness
 *                    (tests/mkfixture64.c) mmaps the flat image and jumps STRAIGHT to
 *                    the body, so the harness never needs a 32-bit user-mode setup.
 *   0x90000  .body   the test itself, .code64.
 *   0x100000 .bss    the compared 8 KiB window (0x100000..0x102000), NOLOAD, so the
 *                    flat image STOPS before it.  Its top is the stack (rsp = 0x102000).
 *
 * WHY .bss MUST NOT REACH THE FILE: the emulated leg loads the flat image through the
 * port's multiboot ADDRESS-TABLE branch (src/main.c:284-341, zero new loader code) and
 * that loader refuses a blob that overlaps the memory-mapped hole at 0xA0000..0x100000
 * (`in_mapped_range`, main.c:232-236).  A file that carried the .bss gap would span
 * 0x80000..0x102000 and be refused BY NAME -- so .bss is NOLOAD and the native harness
 * maps that region itself (MAP_END in tests/mkfixture64.c).  The image therefore ends
 * around 0x901xx, and `load_end_addr = 0` ("the whole file") is a valid header field.
 *
 * `-z noseparate-code` is the 32-bit lane's measured deviation for the same reason it
 * was there: a separate-code layout inserts page-aligned PT_LOADs and pushes what a
 * loader scans out of reach.  Here the script makes the addresses explicit, so the flag
 * is defensive only.
 */
ENTRY(stub_start)
SECTIONS
{
  . = 0x80000;
  .stub : { *(.stub) }

  . = 0x90000;
  .body : { *(.body) *(.body.*) }

  . = 0x100000;
  .bss (NOLOAD) : { *(.bss) *(COMMON) }

  /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.rela*) }
}
