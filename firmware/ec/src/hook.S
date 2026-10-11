#include "ec_abi.h"
.syntax unified
.cpu cortex-m4
.thumb

.global bridge_entry
.type bridge_entry, %function
.thumb_func
bridge_entry:

    mov r4, r0
    cmp r1, #0xb6
    bne original_command
    bl bridge_process
    b.w EC_MSP_DISPATCH_SUCCESS

original_command:
    cmp r1, #0x96
    b.w EC_MSP_DISPATCH_RESUME
