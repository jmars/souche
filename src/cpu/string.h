/*
 * string.h -- the string instructions: rep/no-rep movs/stos/lods/cmps/scas/ins/outs.
 *
 * Port of v86/src/rust/cpu/string.rs (701 lines).  The whole file funnels into one
 * `string_instruction(is_asize_32, ds_or_prefix, instruction, size, rep)` (string.rs:60);
 * the 48 exported wrappers are one-line calls into it, exactly as upstream, so that the
 * fast/slow-path structure and the iteration-limit behaviour stay in ONE place.
 *
 * FAULT BEHAVIOUR (cpu_state.h's contract): every one of these SETS cpu->pagefault and
 * keeps the partial effects -- there is no return value to check because the call is the
 * whole body of the instruction (instructions.c's instr_A4 etc.).
 *
 * WHAT THE EMITTER CALLS: the 48 names below, verbatim from string.rs:516-701 -- the
 * emitter's C output calls them by these names (they are not opcode functions, so the
 * `instr*` naming scheme of gen/generate_interpreter.js:87-101 does not apply to them;
 * they are reached from instructions.c's instr_A4/instr_A5/... one-liners).
 */

#ifndef V86C_STRING_H
#define V86C_STRING_H

#include "cpu_state.h"

/* string.rs:517-536 -- movs */
/* THE 8-BYTE (REX.W) FORMS: 0xA5/0xA7/0xAB/0xAD/0xAF with REX.W, i.e. MOVSQ/CMPSQ/STOSQ/
 * LODSQ/SCASQ.  string.rs has no counterpart (upstream implements no long mode); they share
 * string_instruction's 64-bit path with the byte/word/dword forms. */
void movsq_rep(bool is_asize_32, int32_t seg);
void movsq_no_rep(bool is_asize_32, int32_t seg);
void cmpsq_repz(bool is_asize_32, int32_t seg);
void cmpsq_repnz(bool is_asize_32, int32_t seg);
void cmpsq_no_rep(bool is_asize_32, int32_t seg);
void stosq_rep(bool is_asize_32);
void stosq_no_rep(bool is_asize_32);
void lodsq_rep(bool is_asize_32, int32_t seg);
void lodsq_no_rep(bool is_asize_32, int32_t seg);
void scasq_repz(bool is_asize_32);
void scasq_repnz(bool is_asize_32);
void scasq_no_rep(bool is_asize_32);

void movsb_rep(bool is_asize_32, int32_t seg);
void movsw_rep(bool is_asize_32, int32_t seg);
void movsd_rep(bool is_asize_32, int32_t seg);
void movsb_no_rep(bool is_asize_32, int32_t seg);
void movsw_no_rep(bool is_asize_32, int32_t seg);
void movsd_no_rep(bool is_asize_32, int32_t seg);

/* string.rs:539-558 -- lods */
void lodsb_rep(bool is_asize_32, int32_t seg);
void lodsw_rep(bool is_asize_32, int32_t seg);
void lodsd_rep(bool is_asize_32, int32_t seg);
void lodsb_no_rep(bool is_asize_32, int32_t seg);
void lodsw_no_rep(bool is_asize_32, int32_t seg);
void lodsd_no_rep(bool is_asize_32, int32_t seg);

/* string.rs:561-580 -- stos (no source operand: no segment argument) */
void stosb_rep(bool is_asize_32);
void stosw_rep(bool is_asize_32);
void stosd_rep(bool is_asize_32);
void stosb_no_rep(bool is_asize_32);
void stosw_no_rep(bool is_asize_32);
void stosd_no_rep(bool is_asize_32);

/* string.rs:583-617 -- cmps */
void cmpsb_repz(bool is_asize_32, int32_t seg);
void cmpsw_repz(bool is_asize_32, int32_t seg);
void cmpsd_repz(bool is_asize_32, int32_t seg);
void cmpsb_repnz(bool is_asize_32, int32_t seg);
void cmpsw_repnz(bool is_asize_32, int32_t seg);
void cmpsd_repnz(bool is_asize_32, int32_t seg);
void cmpsb_no_rep(bool is_asize_32, int32_t seg);
void cmpsw_no_rep(bool is_asize_32, int32_t seg);
void cmpsd_no_rep(bool is_asize_32, int32_t seg);

/* string.rs:620-651 -- scas */
void scasb_repz(bool is_asize_32);
void scasw_repz(bool is_asize_32);
void scasd_repz(bool is_asize_32);
void scasb_repnz(bool is_asize_32);
void scasw_repnz(bool is_asize_32);
void scasd_repnz(bool is_asize_32);
void scasb_no_rep(bool is_asize_32);
void scasw_no_rep(bool is_asize_32);
void scasd_no_rep(bool is_asize_32);

/* string.rs:654-676 -- outs */
void outsb_rep(bool is_asize_32, int32_t seg);
void outsw_rep(bool is_asize_32, int32_t seg);
void outsd_rep(bool is_asize_32, int32_t seg);
void outsb_no_rep(bool is_asize_32, int32_t seg);
void outsw_no_rep(bool is_asize_32, int32_t seg);
void outsd_no_rep(bool is_asize_32, int32_t seg);

/* string.rs:679-701 -- ins */
void insb_rep(bool is_asize_32);
void insw_rep(bool is_asize_32);
void insd_rep(bool is_asize_32);
void insb_no_rep(bool is_asize_32);
void insw_no_rep(bool is_asize_32);
void insd_no_rep(bool is_asize_32);

/* ------------------------------------------------------------------ *
 * PORT I/O BOUNDARY.
 *
 * cpu.rs:4613-4649 (io_port_read8/16/32, io_port_write8/16/32) and cpu.rs:3009-3055
 * (test_privileges_for_io).  Upstream's io_port_* are the wasm imports `js::io_port_*`
 * (cpu.rs:44-50) -- the device layer's bridge into io.js -- with the PIC's six ports
 * intercepted in front of it (pic.rs, M1c's pic.c, plan ss7 step 18).  test_privileges_for_io
 * reads the TSS I/O-permission bitmap and raises #GP.
 *
 * The device layer (M1c/M2) OWNS these definitions; they are declared here because
 * string.c is their first caller (ins/outs) and instructions.c's instr_E4..EF are their
 * second.  They are NOT stubs: nothing in this unit defines them, and the selftest
 * supplies recording definitions so the file links (the same arrangement M1a-1 made for
 * call_interrupt_vector and M1a-2 for switch_seg/trigger_*).  See the unit report's
 * deferred-symbol linkage table.
 * ------------------------------------------------------------------ */
int32_t io_port_read8(int32_t port);
int32_t io_port_read16(int32_t port);
int32_t io_port_read32(int32_t port);
void io_port_write8(int32_t port, int32_t value);
void io_port_write16(int32_t port, int32_t value);
void io_port_write32(int32_t port, int32_t value);
bool test_privileges_for_io(int32_t port, int32_t size);

#endif /* V86C_STRING_H */
