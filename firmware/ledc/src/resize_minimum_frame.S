.syntax unified
.thumb
.cpu cortex-m4
.section .text

ldr r3, =0x20002650
strh r0, [r3]
uxth r1, r0
cmp r1, #152
bhs physical
movs r1, #152
physical:
ldr r0, =0x200025bc
b.w 0xcdb6
.ltorg

