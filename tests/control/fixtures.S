/* Synthetic IA-32 control-transfer corpus. Native code is emitted by recomp-kit,
 * not handwritten as host exception handlers. Original CxxThrowException is
 * separately extracted from Sacred.exe at its original VA. */
.intel_syntax noprefix
.section .guest,"ax"
.equ S, 0x20000000
.macro function name
.balign 256
.global \name
.type \name,@function
\name:
.endm
.macro end name
.size \name,.-\name
.endm
.macro establish handler
push ebp
push offset \handler
push dword ptr fs:[0]
mov dword ptr fs:[0],esp
.endm
.macro finish
mov edx,esp
mov eax,[edx]
mov ebp,[edx+8]
add esp,12
mov dword ptr fs:[0],eax
.endm

function stack_probe
mov ecx,esp
mov eax,0x3d000
1:
sub ecx,0x1000
test [ecx],eax
sub eax,0x1000
jnz 1b
mov eax,85
ret
end stack_probe

function cdecl_sum
mov eax,[esp+4]
add eax,[esp+8]
ret
end cdecl_sum
function stdcall_sum
mov eax,[esp+4]
add eax,[esp+8]
ret 8
end stdcall_sum
function thiscall_sum
mov eax,ecx
add eax,[esp+4]
ret 4
end thiscall_sum
function fastcall_sum
mov eax,ecx
add eax,edx
add eax,[esp+4]
ret 4
end fastcall_sum
function wide_result
mov eax,[esp+8]
mov edx,[esp+12]
add eax,[esp+4]
adc edx,0
ret 12
end wide_result
function float_result
mov eax,[esp+4]
fld tbyte ptr [eax]
ret 4
end float_result
function out_result
mov eax,[esp+4]
mov dword ptr [eax],0x12345678
ret 4
end out_result
function indirect_call
push 17
push 29
mov eax,offset stdcall_sum
call eax
ret
end indirect_call
function tail_jump
mov eax,offset stdcall_sum
jmp eax
end tail_jump
function adjust_this
sub ecx,4
jmp thiscall_sum
end adjust_this
function vtable_call
push 7
mov ecx,100
mov eax,[S+0x50]
call dword ptr [eax]
ret
end vtable_call
function nested_callback
push 13
push 11
call dword ptr [S+0x48]
add eax,1
ret
end nested_callback
function cpp_outer
push esi
push edi
establish cpp_handler
call cpp_middle
int3
end cpp_outer
function cpp_middle
establish cleanup_handler
call cpp_throw
int3
end cpp_middle
function cpp_throw
push S+0x80
push S+0x120
mov eax,0x849ea5
call eax
int3
end cpp_throw
function cleanup_handler
mov eax,[esp+4]
test dword ptr [eax+4],2
jz cleanup_search
mov eax,[S+0x80+4]
mov ecx,S+0x120
call eax
cleanup_search:
mov eax,1
ret
end cleanup_handler
function guest_destructor
imul eax,[S],10
add eax,1
mov [S],eax
inc dword ptr [ecx]
ret
end guest_destructor
function guest_finally
imul eax,[S],10
add eax,2
mov [S],eax
ret
end guest_finally
function cpp_handler
mov eax,[esp+4]
cmp dword ptr [eax],0xe06d7363
jne continue_search
test dword ptr [eax+4],2
jnz continue_search
cmp dword ptr [eax+16],3
jne continue_search
cmp dword ptr [eax+20],0x19930520
jne continue_search
mov edx,[eax+28] /* ThrowInfo -> CatchableTypeArray -> CatchableType */
mov edx,[edx+12]
cmp dword ptr [edx],1
jne continue_search
mov edx,[edx+4]
mov edx,[edx+4]
cmp edx,S+0x100
jne continue_search
mov edx,[esp+8]
push 0xabcdef
push eax
push 0
push edx
call dword ptr [S+0x44]
/* RtlUnwind has run all inner cleanup handlers. The handler selects the catch. */
mov eax,[esp+8]
mov esp,eax
mov edx,offset cpp_catch
jmp edx
continue_search:
mov eax,1
ret
end cpp_handler
function cpp_catch
call guest_finally
imul eax,[S],10
add eax,3
mov [S],eax
finish
pop edi
pop esi
mov eax,0xabcdef
ret
end cpp_catch
function escaping_callback
push esi
push edi
establish cpp_handler
call dword ptr [S+0x4c]
int3
end escaping_callback
function continuable
establish continuation_handler
push 0
push 0
push 0
push 0x12345678
call dword ptr [S+0x40]
int3
end continuable
function continuation_handler
mov eax,[esp+12]
mov edx,[esp+8]
mov [eax+0xc4],edx
mov dword ptr [eax+0xb8],offset finally_continue
mov dword ptr [eax+0xb0],85
xor eax,eax
ret
end continuation_handler
function finally_continue
call guest_finally
finish
mov eax,85
ret
end finally_continue
function setjmp_caller
push S+0x200
call dword ptr [S+0x54]
add esp,4
test eax,eax
jnz jumped
push 0
push S+0x200
call dword ptr [S+0x58]
int3
jumped:
ret
end setjmp_caller
function mismatch_callee
add esp,4
push offset mismatch_raise
ret
end mismatch_callee
function mismatch_raise
push 0
push 0
push 0
push 0x12345678
call dword ptr [S+0x40]
int3
end mismatch_raise
function setjmp_intrinsic
nop
ret
end setjmp_intrinsic
function longjmp_intrinsic
nop
ret
end longjmp_intrinsic
function bad_return
push 0x03000001
ret
end bad_return
function foreign_return
push 0xf1000000
ret
end foreign_return
function search_filter
inc dword ptr [S+0x10]
xor eax,eax
ret 4
end search_filter
function continue_filter
mov eax,[esp+4]
mov eax,[eax+4]
mov dword ptr [eax+0xb8],offset top_resume
mov edx,[eax+0xc4]
add edx,20
mov [eax+0xc4],edx
mov eax,-1
ret 4
end continue_filter
function top_raise
push 0
push 0
push 0
push 0x12345678
call dword ptr [S+0x40]
int3
end top_raise
function top_resume
mov eax,77
ret
end top_resume
function noncontinuable
establish continuation_handler
push 0
push 0
push 1
push 0x12345678
call dword ptr [S+0x40]
int3
end noncontinuable
