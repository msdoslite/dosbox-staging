; SPDX-License-Identifier: GPL-2.0-or-later
bits 16
org 100h
 mov dx,file
 mov ax,3d00h
 int 21h
 jc fail
 mov bx,ax
 mov dx,buf
 mov cx,3
 mov ah,3fh
 int 21h
 jc fail
 cmp ax,3
 jne fail
 cmp word [buf],'AB'
 jne fail
 cmp byte [buf+2],'C'
 jne fail
 mov ah,3eh
 int 21h
 jc fail
 ; Leave an unfinished search intentionally: session teardown must release it.
 mov dx,pattern
 xor cx,cx
 mov ah,4eh
 int 21h
 jc fail
 mov ax,4c00h
 int 21h
fail:
 mov ax,4c01h
 int 21h
file db 'D:\HELLO.TXT',0
pattern db 'D:\*.*',0
buf times 4 db 0
