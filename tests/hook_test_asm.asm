; ============================================================================
;  tests/hook_test.cpp 里那几段 GNU asm 的 MSVC 版：同样的指令字节，用 ml64 汇编。
;  GCC/MinGW 走 hook_test.cpp 里的 asm(R"( ... )")，MSVC 走这个文件，
;  两边摆出来的字节必须一模一样（挂钩引擎就是按字节工作的）。
;  由 build.ps1 一起交给 cl，cl 会自动调用 ml64。
; ============================================================================
_TEXT SEGMENT ALIGN(16) 'CODE'

EXTERN t3_target:PROC

PUBLIC t1, t1_site
PUBLIC t2, t2_site
PUBLIC t3, t3_site
PUBLIC t4_with_rsi, t4_func

; ---- 测试 1：mov rcx,rbx ; call [rax+70] ; mov ecx,[rbx+58]（ApplyResetAndEquip 的样子）----
; int t1(void* obj, void** vtbl, double* io)  返回 [obj'+58] + vfunc 的返回值；xmm 原样带出来
; rcx=obj  rdx=vtbl  r8=io(double*)
t1:
    push rbx
    push rdi
    push rsi
    sub  rsp, 20h
    mov  rbx, rcx
    mov  rax, rdx
    movsd xmm0, QWORD PTR [r8]
    mov  rdi, r8
    mov  rsi, 1122334455667788h
t1_site:
    db 48h,8Bh,0CBh, 0FFh,50h,70h            ; mov rcx,rbx ; call [rax+0x70]
    db 8Bh,4Bh,58h                           ; mov ecx,[rbx+0x58]
    add  eax, ecx
    movsd QWORD PTR [rdi], xmm0
    mov  rcx, 1122334455667788h
    cmp  rsi, rcx
    je   t1_same
    mov  eax, -999
t1_same:
    add  rsp, 20h
    pop  rsi
    pop  rdi
    pop  rbx
    ret

; ---- 测试 2：test dword [rcx+2298],80000011 ; mov rax,rcx ; je（QuestLoad 的样子）----
t2:
t2_site:
    db 0F7h,81h,98h,22h,00h,00h,11h,00h,00h,80h   ; test dword [rcx+2298],80000011
    db 48h,89h,0C8h                               ; mov rax,rcx（不动标志位）
    je   t2_zero
    mov  eax, 1
    ret
t2_zero:
    xor  eax, eax
    ret

; ---- 测试 3：call rel32（BaseLoad 的样子）----
t3:
    sub  rsp, 28h
t3_site:
    call t3_target
    mov  eax, 5
    add  rsp, 28h
    ret

; ---- 测试 4：函数开头 push rbx ; sub rsp,40（MantleLoad 的样子），rsi 由调用方给 ----
; rcx = 要放进 rsi 的值
t4_with_rsi:
    push rsi
    sub  rsp, 20h
    mov  rsi, rcx
    mov  eax, -1
    call t4_func
    add  rsp, 20h
    pop  rsi
    ret
t4_func:
    db 40h,53h, 48h,83h,0ECh,40h                 ; push rbx ; sub rsp,0x40
    mov  eax, 7
    add  rsp, 40h
    pop  rbx
    ret

_TEXT ENDS
END
