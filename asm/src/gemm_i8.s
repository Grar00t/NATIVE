.intel_syntax noprefix
.section .text
.global nx_i8_scalar
.type nx_i8_scalar,@function
# C16x16 i32 = A16x64 signed i8 * B64x16 signed i8. No aliasing.
nx_i8_scalar:
 push rbx
 push r12
 xor r8d,r8d
.scalar_i:
 xor r9d,r9d
.scalar_j:
 lea r10,[rdi+r8]
 lea r11,[rsi+r9]
 xor eax,eax
 xor ecx,ecx
.scalar_k:
 movsx ebx,byte ptr [r10+rcx]
 movsx r12d,byte ptr [r11]
 imul ebx,r12d
 add eax,ebx
 add r11,16
 inc ecx
 cmp ecx,64
 jb .scalar_k
 lea r10,[rdx+r8]
 mov [r10+r9*4],eax
 inc r9d
 cmp r9d,16
 jb .scalar_j
 add r8,64
 cmp r8,1024
 jb .scalar_i
 pop r12
 pop rbx
 ret

.size nx_i8_scalar,.-nx_i8_scalar
.section .note.GNU-stack,"",@progbits
