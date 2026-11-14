; SPDX-License-Identifier: GPL-2.0-or-later
; Real DOS INT 21h regression tests, run with the included VMSMOUNT.
bits 16
org 100h
%macro phase 1
 mov dx,%1
 mov ah,9
 int 21h
%endmacro
%macro ok 0
 jc fail
%endmacro
start:
 phase msg_create
 mov dx,file
 xor cx,cx
 mov ah,5bh
 int 21h
 ok
 mov [handle],ax
 mov bx,ax
 mov dx,data
 mov cx,6
 mov ah,40h
 int 21h
 ok
 cmp ax,6
 jne fail
 ; Seek to byte 3, zero-length write truncates.
 mov bx,[handle]
 mov ax,4200h
 xor cx,cx
 mov dx,3
 int 21h
 ok
 xor cx,cx
 mov ah,40h
 int 21h
 ok
 mov ah,3eh
 int 21h
 ok
 ; AH=5Bh must fail without truncating the existing three-byte file.
 mov dx,file
 xor cx,cx
 mov ah,5bh
 int 21h
 jnc fail
 cmp ax,80
 jne fail
 phase msg_read
 mov dx,file
 mov ax,3d00h
 int 21h
 ok
 mov bx,ax
 mov [handle],ax
 mov dx,buffer
 mov cx,16
 mov ah,3fh
 int 21h
 ok
 cmp ax,3
 jne fail
 cmp word [buffer],'ab'
 jne fail
 cmp byte [buffer+2],'c'
 jne fail
 mov cx,16
 mov dx,buffer
 mov ah,3fh
 int 21h
 ok
 or ax,ax
 jnz fail
 mov ah,3eh
 int 21h
 ok
 phase msg_attr
 mov dx,file
 mov cx,27h ; readonly + hidden + system + archive
 mov ax,4301h
 int 21h
 ok
 mov ax,4300h
 int 21h
 ok
 and cx,27h
 cmp cx,27h
 jne fail
 mov ax,3d02h ; writable open must fail
 int 21h
 jnc fail
 cmp ax,5
 jne fail
 mov ax,4301h
 mov cx,20h ; clear readonly/hidden/system
 int 21h
 ok
 phase msg_date
 mov dx,file
 mov ax,3d02h
 int 21h
 ok
 mov bx,ax
 mov [handle],ax
 mov ax,5701h
 mov cx,1883h ; 03:04:06
 mov dx,5822h ; 2024-01-02
 int 21h
 ok
 mov ah,3eh
 int 21h
 ok
 mov dx,file
 mov ax,3d00h
 int 21h
 ok
 mov bx,ax
 mov ax,5700h
 int 21h
 ok
 cmp cx,1883h
 jne fail
 cmp dx,5822h
 jne fail
 mov ah,3eh
 int 21h
 ok
 phase msg_new
 mov si,file
 mov bx,2
 xor cx,cx
 mov dx,10h ; extended create: fail if exists, create if missing
 mov ax,6c00h
 int 21h
 jnc fail
 phase msg_dirs
 mov dx,dir
 mov ah,39h
 int 21h
 ok
 mov dx,file
 mov di,moved
 mov ah,56h
 int 21h
 ok
 mov dx,dir
 mov ah,3ah ; non-empty rmdir must fail
 int 21h
 jnc fail
 mov dx,moved
 mov ah,41h
 int 21h
 ok
 mov dx,dir
 mov ah,3ah
 int 21h
 ok
 phase msg_pass
 mov ax,4c00h
 int 21h
fail:
 push ax
 phase msg_fail
 pop ax
 ; DOS error (or failed comparison value), hexadecimal
 mov bx,ax
 mov cx,4
.hex:
 rol bx,4
 mov dx,bx
 and dx,15
 add dl,'0'
 cmp dl,'9'
 jbe .digit
 add dl,7
.digit:
 mov ah,2
 int 21h
 loop .hex
 mov ax,4c01h
 int 21h
file db 'D:\SHARED\CHECK.BIN',0
moved db 'D:\SHARED\SUB\MOVED.BIN',0
dir db 'D:\SHARED\SUB',0
msg_create db 'create/write/seek/truncate',13,10,'$'
msg_read db 'reopen/read/EOF',13,10,'$'
msg_attr db 'DOS readonly/hidden/system/archive roundtrip',13,10,'$'
msg_date db 'DOS timestamp set/close/reopen/get',13,10,'$'
msg_new db 'exclusive create collision',13,10,'$'
msg_dirs db 'mkdir/rename/nonempty-rmdir/delete/rmdir',13,10,'$'
msg_pass db 'FILETEST PASS',13,10,'$'
msg_fail db 'FILETEST FAIL: AX=','$'
handle dw 0
data db 'abcdef'
buffer times 16 db 0
