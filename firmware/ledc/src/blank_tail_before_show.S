.syntax unified
.thumb
.cpu cortex-m4
.section .text

push {r0-r4, lr}
ldr r1, =0x200025bc
cmp r0, r1
bne done
ldr r1, =0x20002650
ldrh r4, [r1]
ldrh r1, [r0, #2]
cmp r4, r1
bhs done
ldr r0, [r0, #8]
add r3, r4, r4, lsl #1
add r0, r0, r3
subs r2, r1, r4
add r2, r2, r2, lsl #1
movs r1, #0
bl 0x1577e
done:
ldmia sp!, {r0-r4, lr}
ldr r3, [r0, #8]
stmdb sp!, {r0, r1, r2, r4, r5, r6, r7, r8, r9, sl, fp, lr}
b.w 0xce3e
.ltorg

