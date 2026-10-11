#include "ec_abi.h"
.syntax unified
.thumb
.cpu cortex-m4
.global dan_observe_entry
.type dan_observe_entry,%function
.thumb_func
dan_observe_entry:
    push {r0-r4,lr}
    bl file_forward_response
    pop {r0-r4,lr}
    push.w {r4-r8,lr}
    b.w EC_DAN_DISPATCH_RESUME
