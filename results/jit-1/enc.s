.intel_syntax noprefix
.text
f:
  push rbp
  push rbx
  push r12
  push r13
  push r14
  push r15
  sub rsp, 24
  movabs rbp, 0x1122334455667788
  mov eax, [rbp+0]
  mov ecx, [rbp+4]
  mov edx, [rbp+8]
  mov ebx, [rbp+12]
  mov r12d, [rbp+16]
  mov r13d, [rbp+20]
  mov r14d, [rbp+24]
  mov r15d, [rbp+28]
  mov [rbp+0], eax
  mov [rbp+28], r15d
  mov dword ptr [rbp+68], 0x12345678
  mov byte ptr [rbp+148], 0x3e
  mov byte ptr [rbp+672], 0
  mov dword ptr [rsp+0], 0
  mov dword ptr [rsp+4], 0
  mov dword ptr [rsp+8], 3
  mov eax, [rsp+8]
  add dword ptr [rsp], 1
  cmp byte ptr [r8], 0
  mov r11d, [r11]
  mov [r11], r9d
  add qword ptr [rdi], r11
  sub edi, [r11]
  test dil, 4
  cmp dword ptr [rbp+68], esi
  xor r11d, r10d
  test r11d, 0xFFFFF000
  cmp r9d, 100001
  jae f
  call r11
  mov esi, 0x1000
  mov r10d, 0x2000
  mov edi, 5
  inc r9d
  mov dword ptr [rdi], 1

  add rsp, 24
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbx
  pop rbp
  ret
