/*
** This file has been pre-processed with DynASM.
** https://luajit.org/dynasm.html
** DynASM version 1.5.0, DynASM x64 version 1.5.0
** DO NOT EDIT! The original file is in "dynasm-shape.dasc".
*/

#line 1 "dynasm-shape.dasc"
/*|// JIT-SPIKE Q2(a): THE SAME NATIVE BLOCK, EMITTED BY LuaJIT's DynASM. */
/*|// */
/*|// This file is a C file with DynASM lines in it: `minilua dynasm.lua -o dynasm-shape.h */
/*|// dynasm-shape.dasc` rewrites every `|` line into a `dasm_put(...)` call and emits the */
/*|// assembled action list at the top.  That is exactly how LuaJIT itself uses it */
/*|// (vendor/luajit/src/Makefile:675-677: `$(DASM) $(DASM_FLAGS) -o $@ $(DASM_DASC)`). */
/*|// */
/*|// The block is results/jit-spike/x64enc.c's emit_A(): the shape-A loop with the guest's */
/*|// registers in host registers, the back edge inside the block, and the HOST's callee-saved */
/*|// rbx preserved across it.  `n` is a RUNTIME operand (it arrives in edi and is moved to r8d); */
/*|// `seed` arrives in esi, exactly as in the encoder route's emit_A() -- so the two routes' */
/*|// byte streams are directly comparable, which is the whole point of doing both. */
/*|.arch x64 */
#if DASM_VERSION != 10500
#error "Version mismatch between DynASM and included encoding engine"
#endif
#line 14 "dynasm-shape.dasc"
/*|.actionlist spike_actionlist */
static const unsigned char spike_actionlist[38] = {
  83,65,137,252,248,49,192,49,219,137,252,241,49,252,255,248,1,1,216,41,217,
  49,252,247,65,252,255,200,15,133,244,1,137,252,248,91,195,255
};

#line 15 "dynasm-shape.dasc"

void spike_A_emit(Dst_DECL)
{
  /*| push rbx */
  /*| mov r8d, edi */
  /*| xor eax, eax */
  /*| xor ebx, ebx */
  /*| mov ecx, esi */
  /*| xor edi, edi */
  /*|1: */
  /*| add eax, ebx */
  /*| sub ecx, ebx */
  /*| xor edi, esi */
  /*| dec r8d */
  /*| jnz <1 */
  /*| mov eax, edi */
  /*| pop rbx */
  /*| ret */
  dasm_put(Dst, 0);
#line 33 "dynasm-shape.dasc"
}
