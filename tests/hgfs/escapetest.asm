; SPDX-License-Identifier: GPL-2.0-or-later
; Host boundary checks through real DOS INT 21h with unmodified VMSMOUNT.
bits 16
org 100h
 mov si,paths
.next:
 lodsw
 or ax,ax
 jz passed
 mov dx,ax
 mov [current],ax
 push si
 mov ax,3d00h
 int 21h
 jnc failed
 mov dx,[current]
 xor cx,cx
 mov ah,3ch
 int 21h
 jnc failed
 mov dx,[current]
 mov ah,41h
 int 21h
 jnc failed
 pop si
 jmp .next
passed:
 mov dx,good
 mov ah,9
 int 21h
 mov ax,4c00h
 int 21h
failed:
 mov dx,bad
 mov ah,9
 int 21h
 mov ax,4c01h
 int 21h
current dw 0
paths dw p1,p2,p3,p4,p5,p6,p7,0
p1 db 'D:\..\outside\SECRET.TXT',0
p2 db 'D:\NEST\DEEP\..\..\..\outside\SECRET.TXT',0
p3 db 'D:\LINK\SECRET.TXT',0
p4 db 'D:\FILELNK.TXT',0
p5 db 'D:\HARD.TXT',0
p6 db 'D:\NEST\DEEP\..\..\**..\**SECRET.TXT',0
p7 db 'D:\..\..\outside\SECRET.TXT',0
good db 'ESCAPE TEST PASS: read/create/delete all denied',13,10,'$'
bad db 'ESCAPE TEST FAIL: unexpected access',13,10,'$'
