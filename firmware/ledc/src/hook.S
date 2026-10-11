.syntax unified
.thumb
.cpu cortex-m4
.section .text.entry,"ax",%progbits
.global entry
.thumb_func
entry:
    push {r0-r4,lr}
    bl file_service
    cmp r0,#0
    pop {r0-r4,lr}
    bne handled

    push.w {r4-r11,lr}
    b.w 0x86e4
handled:
    bx lr

.global receive_guard_entry
.thumb_func
receive_guard_entry:
    push {r0,r1,r3,lr}
    bl receive_render_guard
    mov r2,r0
    pop {r0,r1,r3,pc}

.global complete_guard_entry
.thumb_func
complete_guard_entry:
    push {r0-r4,lr}
    bl complete_render_guard
    pop {r0-r4,lr}
    movs r2,#0
    bx lr

.global gif_count_entry
.thumb_func
gif_count_entry:
    push {r0-r4,lr}
    bl custom_gif_count
    cmp r0,#0
    blt count_fallback
    add sp,#20
    pop {pc}
count_fallback:
    pop {r0-r4,lr}
    push {r4-r7,lr}
    ldr r6,=0x200017fc
    b.w 0x4378

.global gif_name_entry
.thumb_func
gif_name_entry:
    push {r0-r4,lr}
    bl custom_gif_name
    cmp r0,#0
    beq name_fallback
    add sp,#20
    pop {pc}
name_fallback:
    pop {r0-r4,lr}
    push.w {r4-r8,lr}
    b.w 0x450c

.global gif_show_entry
.thumb_func
gif_show_entry:
    push {r0-r3,r12,lr}
    bl gif_show_allowed
    cmp r0,#0
    pop {r0-r3,r12,lr}
    beq show_held
    // The stock branch at 0x5b6c consumes these comparison flags.
    cmp r0,#1
    push {r3-r5,lr}
    b.w 0x5b68
show_held:
    bx lr
