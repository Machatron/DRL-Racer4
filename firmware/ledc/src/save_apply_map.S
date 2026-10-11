.syntax unified
.thumb
.cpu cortex-m4
.section .text

push {r4, lr}
mov r4, r0
bl 0x9f2c
mov r0, r4
bl 0x55b4
pop {r4, pc}

