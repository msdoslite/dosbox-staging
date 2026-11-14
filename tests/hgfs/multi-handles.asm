bits 16
org 100h
start:
 mov ax,cs
 mov ds,ax
 mov es,ax
 mov ss,ax
 mov sp,stack_end
 mov bx,(stack_end-$$+100h+15)/16
 mov ah,4ah
 int 21h
 jc fail
 mov ax,cs
 mov [params+4],ax
 mov [params+8],ax
 mov [params+12],ax
 mov dx,dfile
 mov ax,3d00h
 int 21h
 jc fail
 mov [dhnd],ax
 mov dx,efile
 mov ax,3d00h
 int 21h
 jc fail
 mov [ehnd],ax
 mov bx,[dhnd]
 call readbyte
 cmp byte [buf],'g'
 jne fail
 mov bx,[ehnd]
 call readbyte
 cmp byte [buf],'w'
 jne fail
 ; Busy D: must not be detached while this DOS handle exists.
 mov byte [stage],'1'
 call unload
 mov ah,4dh
 int 21h
 cmp al,245
 jne fail
 mov byte [stage],'2'
 mov bx,[dhnd]
 call readbyte
 cmp byte [buf],'a'
 jne fail
 mov bx,[ehnd]
 call readbyte
 cmp byte [buf],'o'
 jne fail
 mov byte [stage],'3'
 mov bx,[dhnd]
 mov ah,3eh
 int 21h
 jc fail
 mov byte [stage],'4'
 call unload
 mov byte [stage],'5'
 ; Verify the actual effect instead of relying on TSR exit status.
 mov dx,dfile
 mov ax,3d00h
 int 21h
 jnc fail
 mov bx,[ehnd]
 call readbyte
 cmp byte [buf],'r'
 jne fail
 mov bx,[ehnd]
 mov ah,3eh
 int 21h
 jc fail
 mov dx,passmsg
 mov ah,09h
 int 21h
 mov ax,4c00h
 int 21h
readbyte:
 mov dx,buf
 mov cx,1
 mov ah,3fh
 int 21h
 jc fail
 cmp ax,1
 jne fail
 ret
unload:
 push ds
 push es
 mov dx,exe
 mov bx,params
 mov ax,4b00h
 int 21h
 pop es
 pop ds
 jc fail
 ret
fail:
 mov dx,failmsg
 mov ah,09h
 int 21h
 mov ax,4c01h
 int 21h
dfile db 'D:\HELLO.TXT',0
efile db 'E:\HELLO.TXT',0
exe db 'C:\VMSMOUNT\VMSMOUNT.EXE',0
tail db 8,' /U /L:D',13
params dw 0,tail,0,5ch,0,6ch,0
dhnd dw 0
ehnd dw 0
buf db 0
passmsg db 'PASS: interleaved handles, busy unmount, E survives D detach',13,10,'$'
failmsg db 'FAIL: multi handles stage '
stage db '0',13,10,'$'
times 512 db 0
stack_end:
