/* ARM32 probe for the firmware-specific libbsa.so management ABI. */
.syntax unified
.arm
.global _start
.text
_start:
.ifdef GATT_SEARCH
    ldr r0, [sp]
    cmp r0, #2
    blo gatt_args_invalid
    ldr r0, [sp, #8]
    bl parse_kettle_address
    cmp r0, #0
    bne gatt_args_invalid
    ldr r0, [sp]
.ifdef GATT_BOIL
    cmp r0, #2
    beq heating_args_done
    cmp r0, #3
    bne gatt_args_invalid
    ldr r0, [sp, #12]
    ldrb r1, [r0]
    ldrb r2, [r0, #1]
    ldrb r3, [r0, #2]
    cmp r3, #0
    bne gatt_args_invalid
    sub r1, r1, #48
    sub r2, r2, #48
    cmp r1, #9
    cmpls r2, #9
    bhi gatt_args_invalid
    add r1, r1, r1, lsl #2
    add r1, r2, r1, lsl #1
    cmp r1, #35
    blo gatt_args_invalid
    cmp r1, #90
    bhi gatt_args_invalid
    ldr r0, =heat_target
    strb r1, [r0]
    b heating_args_done
.else
    cmp r0, #2
    beq heating_args_done
.endif
gatt_args_invalid:
    mov r0, #2
    b exit
heating_args_done:
.endif
    mov r0, #0
    ldr r1, =deadline
    mov r2, #0
    mov r7, #104
    svc #0
    cmp r0, #0
    blt exit

    ldr r4, =open_params
    mov r0, r4
    bl BSA_MgtOpenInit
    cmp r0, #0
    bne exit
    ldr r1, =management_callback
    str r1, [r4, #100]
    mov r0, r4
    bl BSA_MgtOpen
    cmp r0, #0
    bne exit

    mov r0, #2
    ldr r1, =opened
    mov r2, #opened_length
    mov r7, #4
    svc #0

.ifdef DISCOVERY
    ldr r4, =discovery_params
    mov r0, r4
    bl BSA_DiscStartInit
    cmp r0, #0
    bne discovery_failed
    mov r1, #4
    str r1, [r4]
    ldr r1, =discovery_callback
    str r1, [r4, #4]
    mov r1, #0x10
    strb r1, [r4, #24]
    mov r0, r4
    bl BSA_DiscStart
    cmp r0, #0
    bne discovery_failed
    mov r0, #2
    ldr r1, =started
    mov r2, #started_length
    mov r7, #4
    svc #0
.endif
.ifdef GATT_REGISTER
.ifdef GATT_ENABLE
    ldr r4, =gatt_enable_params
    mov r0, r4
    bl BSA_BleEnableInit
    cmp r0, #0
    bne gatt_finished
    mov r0, r4
    bl BSA_BleEnable
    cmp r0, #0
    bne gatt_finished
    mov r0, #2
    ldr r1, =enabled
    mov r2, #enabled_length
    mov r7, #4
    svc #0
.endif
    ldr r4, =gatt_register_params
    mov r0, r4
    bl BSA_BleClAppRegisterInit
    cmp r0, #0
    bne gatt_finished
    mov r1, #2
    strh r1, [r4]
    movw r1, #0xfad7
    strh r1, [r4, #4]
.ifdef GATT_SEARCH
    ldr r1, =gatt_callback
.else
    ldr r1, =management_callback
.endif
    str r1, [r4, #24]
    mov r0, r4
    bl BSA_BleClAppRegister
    movw r1, #1601
    cmp r0, r1
    beq gatt_disabled
    cmp r0, #0
    bne gatt_finished
    mov r0, #2
    ldr r1, =registered
    mov r2, #registered_length
    mov r7, #4
    svc #0
.ifdef GATT_SEARCH
    ldrb r0, [r4, #20]
    bl gatt_search
    mov r8, r0
.endif
    ldr r6, =gatt_deregister_params
    mov r0, r6
    bl BSA_BleClAppDeregisterInit
    cmp r0, #0
    bne gatt_finished
    ldrb r1, [r4, #20]
    strb r1, [r6]
    mov r0, r6
    bl BSA_BleClAppDeregister
    cmp r0, #0
    bne gatt_finished
    mov r0, #2
    ldr r1, =deregistered
    mov r2, #deregistered_length
    mov r7, #4
    svc #0
.ifdef GATT_SEARCH
    mov r0, r8
.else
    mov r0, #0
.endif
    b gatt_finished
gatt_disabled:
    mov r5, r0
    mov r0, #2
    ldr r1, =disabled
    mov r2, #disabled_length
    mov r7, #4
    svc #0
    mov r0, r5
gatt_finished:
    mov r5, r0
    b close_management
.endif
.ifdef DISCOVERY
    ldr r0, =discovery_complete
    mov r1, #0
    ldr r2, =duration
    bl wait_for_change
.else
    ldr r0, =duration
    mov r1, #0
    mov r7, #162
    svc #0
.endif

    mov r5, #0
.ifdef DISCOVERY
    ldr r1, =discovery_complete
    ldr r1, [r1]
    cmp r1, #0
    moveq r5, #1
    b close_session
discovery_failed:
    mov r5, r0
close_session:
.endif
close_management:
    ldr r4, =close_params
    mov r0, r4
    bl BSA_MgtCloseInit
    cmp r0, #0
    bne exit
    mov r0, r4
    bl BSA_MgtClose
    cmp r0, #0
    bne exit

    mov r0, #2
    ldr r1, =closed
    mov r2, #closed_length
    mov r7, #4
    svc #0
    mov r0, r5
exit:
    mov r7, #248
    svc #0

management_callback:
    mov r0, #0
    bx lr

.ifdef DISCOVERY
discovery_callback:
    cmp r0, #0
    beq log_device
    cmp r0, #1
    bxne lr
    ldr r1, =discovery_complete
    str r0, [r1]
    push {r0, lr}
    mov r3, r1
    bl wake_result
    pop {r0, lr}
    ldr r1, =complete_event
    mov r2, #complete_event_length
log_event:
    push {r7, lr}
    mov r0, #2
    mov r7, #4
    svc #0
    pop {r7, pc}

log_device:
    cmp r1, #0
    bxeq lr
    /* Write one framed record: six address bytes and a zero-padded name field. */
    push {r4, r5, r7, lr}
    sub sp, sp, #256
    mov r4, r1
    mov r5, sp
    mov r2, #6
copy_address:
    ldrb r3, [r4], #1
    strb r3, [r5], #1
    subs r2, r2, #1
    bne copy_address
    add r4, r4, #3
    mov r2, #249
    mov r3, #0
copy_name:
    cmp r2, #149
    bls pad_name
    ldrb r3, [r4], #1
    cmp r3, #0
    beq pad_name
store_name_byte:
    strb r3, [r5], #1
    subs r2, r2, #1
    bne copy_name
    b finish_record
pad_name:
    mov r3, #0
    strb r3, [r5], #1
    subs r2, r2, #1
    bne pad_name
finish_record:
    mov r0, #1
    mov r3, #10
    strb r3, [r5]
    mov r1, sp
    mov r2, #256
    mov r7, #4
    svc #0
    add sp, sp, #256
    pop {r4, r5, r7, pc}
.endif

/* Wait while *word == expected, with a relative timeout. Recheck results
 * in the caller, including after EAGAIN if the event arrived before futex.
 */
wait_for_change:
    push {r4, r7, lr}
    mov r3, r2
    mov r2, r1
    mov r1, #128
    mov r4, #0
    mov r7, #240
    svc #0
    dmb ish
    pop {r4, r7, pc}

/* Publish before calling this helper. r3 holds the futex word address.
 * Preserve the callback's event and message registers.
 */
wake_result:
    push {r0-r3, r4, r7, lr}
    dmb ish
    mov r0, r3
    mov r1, #129
    mov r2, #1
    mov r3, #0
    mov r4, #0
    mov r7, #240
    svc #0
    pop {r0-r3, r4, r7, pc}

.ifdef GATT_SEARCH
.include "scripts/bsa-gatt-search.inc"
.endif

.section .rodata
opened:
    .ascii "BSA_MgtOpen: 0\n"
.equ opened_length, . - opened
closed:
    .ascii "BSA_MgtClose: 0\n"
.equ closed_length, . - closed
.ifdef DISCOVERY
started:
    .ascii "BSA_DiscStart: 0\n"
.equ started_length, . - started
complete_event:
    .ascii "BSA_DISC_CMPL_EVT\n"
.equ complete_event_length, . - complete_event
.endif
.ifdef GATT_REGISTER
.ifdef GATT_ENABLE
enabled:
    .ascii "BSA_BleEnable: 0\n"
.equ enabled_length, . - enabled
.endif
deregistered:
    .ascii "BSA_BleClAppDeregister: 0\n"
.equ deregistered_length, . - deregistered
disabled:
    .ascii "BSA_BleClAppRegister: 1601 (BLE_NOT_ENABLED)\n"
.equ disabled_length, . - disabled
registered:
    .ascii "BSA_BleClAppRegister: 0\n"
.equ registered_length, . - registered
.endif
.balign 4
deadline:
.ifdef GATT_TEMPERATURE
    .word 0, 0, 60, 0
.else
.ifdef GATT_CACHE_THEN_SEARCH
    .word 0, 0, 36, 0
.else
 .ifdef GATT_SEARCH
    .word 0, 0, 24, 0
.else
    .word 0, 0, 12, 0
.endif
.endif
.endif
duration:
.ifdef DISCOVERY
    .word 7, 0
.else
    .word 3, 0
.endif

.bss
.balign 4
open_params:
    .space 104
close_params:
    .space 4

.ifdef DISCOVERY
discovery_params:
    .space 32
discovery_complete:
    .space 4
.endif

.ifdef GATT_REGISTER
.ifdef GATT_ENABLE
gatt_enable_params:
    .space 4
.endif
gatt_register_params:
    .space 32
gatt_deregister_params:
    .space 4
.endif

.section .note.GNU-stack,"",%progbits
