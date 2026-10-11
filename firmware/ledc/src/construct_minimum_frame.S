.syntax unified
.thumb
.cpu cortex-m4
.section .text

ldr r1, =0x1c000
ldrh r1, [r1]
cmp r1, #78
beq valid
cmp r1, #100
beq valid
cmp r1, #152
beq valid
movw ip, #309
cmp r1, ip
beq valid
movw ip, #338
cmp r1, ip
beq valid
movs r1, #152
valid:
ldr ip, =0x20002650
strh.w r1, [ip]
// Keep enough pixels to clear the old tail when selecting a shorter map.
cmp r1, #152
bhs physical
movs r1, #152
physical:
b.w 0xcf50
.ltorg

