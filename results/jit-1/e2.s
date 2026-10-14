.intel_syntax noprefix
.text
f:
  mov r12d, r11d
  mov ecx, [rbp+4]
  mov dword ptr [rbp+28], r15d
  mov edi, 0x1234
  movabs r11, 0x1122334455667788
  add r11d, r9d
  add edi, 0x10
  add dword ptr [rdi], r11d
  sub r11d, r10d
  and eax, ecx
  or edx, esi
  cmp r9d, r10d
  cmp dword ptr [rbp+68], 0x12345678
  test eax, eax
  xchg eax, ecx
  dec edx
  not ecx
  neg ecx
  ror eax, 3
  shl eax, 4
  shr edx, 5
  sar eax, 7
  add rsp, 8
  jne 0x100
  jmp 0x200
  call 0x300
