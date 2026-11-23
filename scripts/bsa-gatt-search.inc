/* Firmware-specific GATT probes and read-only kettle status transaction. */
.text
/* Parse the explicitly selected address before opening BSA. */
parse_kettle_address:
    push {r4, r5, r6, lr}
    mov r4, r0
    ldr r5, =kettle_address
    mov r6, #6
address_byte:
    ldrb r0, [r4], #1
    bl address_nibble
    cmp r0, #15
    bhi address_invalid
    mov r3, r0, lsl #4
    ldrb r0, [r4], #1
    bl address_nibble
    cmp r0, #15
    bhi address_invalid
    orr r0, r0, r3
    strb r0, [r5], #1
    ldrb r0, [r4], #1
    subs r6, r6, #1
    beq address_end
    cmp r0, #58
    beq address_byte
address_invalid:
    mov r0, #1
    pop {r4, r5, r6, pc}
address_end:
    cmp r0, #0
    bne address_invalid
    pop {r4, r5, r6, pc}

address_nibble:
    sub r1, r0, #48
    cmp r1, #9
    movls r0, r1
    bxls lr
    orr r0, r0, #32
    sub r0, r0, #87
    cmp r0, #10
    movlo r0, #255
    bx lr

gatt_search:
    push {r4, r5, r6, lr}
    mov r5, r0
    ldr r4, =connect_params
    mov r0, r4
    bl BSA_BleClConnectInit
    cmp r0, #0
    bne gatt_search_return
    strb r5, [r4]
    ldr r1, =kettle_address
    add r2, r4, #1
    mov r3, #6
copy_kettle_address:
    ldrb r0, [r1], #1
    strb r0, [r2], #1
    subs r3, r3, #1
    bne copy_kettle_address
    mov r1, #1
    strb r1, [r4, #7]
    mov r0, r4
    bl BSA_BleClConnect
    cmp r0, #0
    bne gatt_open_failed
    ldr r0, =open_result
    bl wait_for_gatt_result
    ldr r1, =open_result
    ldr r0, [r1]
    cmp r0, #0
    moveq r0, #1
    beq gatt_open_failed
    subs r0, r0, #1
    bne gatt_open_failed
    ldrh r5, [r1, #4]
.ifdef GATT_CONNECT_ONLY
    b close_gatt_link
.else
.ifdef GATT_REPLY_CACHE
    ldr r0, =cache_request
    bl wait_for_gatt_result
    ldr r1, =cache_request
    ldr r0, [r1]
    cmp r0, #0
    .ifdef GATT_CACHE_ONLY
    moveq r0, #4
    beq close_gatt_link
    .else
    beq start_service_search
    .endif
    subs r0, r0, #1
    cmp r0, r5
    movne r0, #3
    bne close_gatt_link
    ldr r4, =cache_params
    mov r0, r4
    bl BSA_BleClCacheLoadInit
    cmp r0, #0
    bne close_gatt_link
    strh r5, [r4, #6]
    ldr r1, =kettle_address
    add r2, r4, #8
    mov r3, #6
copy_cache_address:
    ldrb r0, [r1], #1
    strb r0, [r2], #1
    subs r3, r3, #1
    bne copy_cache_address
.ifdef GATT_CACHE_IMAGE
    mov r1, #8
    strh r1, [r4, #2]
    ldr r1, =kettle_cache_attrs
    add r2, r4, #16
    mov r3, #224
copy_cache_attrs:
    ldrb r0, [r1], #1
    strb r0, [r2], #1
    subs r3, r3, #1
    bne copy_cache_attrs
.endif
    mov r0, r4
    bl BSA_BleClCacheLoad
    cmp r0, #0
    bne close_gatt_link

start_service_search:
.endif

.ifdef GATT_CACHE_ONLY
    mov r0, #2
    ldr r1, =cache_reply_accepted
    mov r2, #cache_reply_accepted_length
    mov r7, #4
    svc #0
.ifdef GATT_CACHE_THEN_SEARCH
    ldr r1, =search_result
    mov r0, #0
    str r0, [r1]
    ldr r4, =search_params
    mov r0, r4
    bl BSA_BleClSearchInit
    cmp r0, #0
    bne close_gatt_link
    strh r5, [r4]
    mov r0, r4
    bl BSA_BleClSearch
    cmp r0, #0
    bne close_gatt_link
    mov r0, #2
    ldr r1, =search_started
    mov r2, #search_started_length
    mov r7, #4
    svc #0
    ldr r0, =search_result
    bl wait_for_gatt_result
    ldr r1, =search_result
    ldr r0, [r1]
    cmp r0, #0
    moveq r0, #2
    beq close_gatt_link
    sub r0, r0, #1
    cmp r0, #0
.ifdef GATT_TEMPERATURE
    bne close_gatt_link
    bl kettle_status
.endif
.else
    bl wait_for_event
.ifdef GATT_CACHE_IMAGE
    ldr r1, =search_result
    ldr r0, [r1]
    cmp r0, #0
    moveq r0, #2
    subne r0, r0, #1
.else
    mov r0, #0
.endif
.endif
.else
    ldr r4, =search_params
    mov r0, r4
    bl BSA_BleClSearchInit
    cmp r0, #0
    bne close_gatt_link
    strh r5, [r4]
    mov r0, r4
    bl BSA_BleClSearch
    cmp r0, #0
    bne close_gatt_link
    ldr r0, =search_result
    bl wait_for_gatt_result
    ldr r1, =search_result
    ldr r0, [r1]
    cmp r0, #0
    subne r0, r0, #1
    moveq r0, #2
.endif
.endif
close_gatt_link:
    mov r6, r0
    ldr r4, =link_close_params
    mov r0, r4
    bl BSA_BleClCloseInit
    cmp r0, #0
    bne gatt_search_return
    strh r5, [r4]
    mov r0, r4
    bl BSA_BleClClose
.ifdef GATT_TEMPERATURE
    mov r4, r0
    mov r0, r4
    ldr r1, =close_api_label
    mov r2, #close_api_label_length
    bl print_hex16
    mov r0, r6
.else
    cmp r0, #0
    moveq r0, r6
.endif
    b gatt_search_return
gatt_open_failed:
.ifdef GATT_TEMPERATURE
    mov r6, r0
    ldr r1, =open_failed_label
    mov r2, #open_failed_label_length
    bl print_hex16
    mov r0, r6
.endif
gatt_search_return:
    pop {r4, r5, r6, pc}

wait_for_gatt_result:
    mov r1, #0
    ldr r2, =event_wait
    b wait_for_change

wait_for_event:
    push {r7, lr}
    ldr r0, =event_wait
    mov r1, #0
    mov r7, #162
    svc #0
    pop {r7, pc}

/* Output a one-byte event and payload. Service reports carry 24 bytes.
 * Open, close and search completion carry status and connection ID.
 */
gatt_callback:
    cmp r1, #0
    bxeq lr
    cmp r0, #1
    beq gatt_log_open
    cmp r0, #5
    beq gatt_log_completion
    cmp r0, #4
    moveq r2, #4
    beq log_gatt_record
    cmp r0, #10
    beq gatt_log_cache_request
.ifdef GATT_TEMPERATURE
    cmp r0, #3
    beq gatt_log_write
    cmp r0, #7
    beq gatt_log_notification
.endif
    cmp r0, #6
    movne r2, #8
    moveq r2, #24
.ifdef GATT_CACHE_IMAGE
    b log_gatt_record
.else
    bxne lr
    b log_gatt_record
.endif
gatt_log_cache_request:
    ldr r2, =cache_request
    ldrh r3, [r1]
    add r3, r3, #1
    str r3, [r2]
    mov r3, r2
    push {r4, lr}
    bl wake_result
    pop {r4, lr}
    mov r2, #4
    b log_gatt_record
gatt_log_open:
    ldr r2, =open_result
    ldrh r3, [r1, #2]
    strh r3, [r2, #4]
    b save_gatt_result
gatt_log_completion:
    ldr r2, =search_result
save_gatt_result:
    ldrh r3, [r1]
    add r3, r3, #1
    dmb ish
    str r3, [r2]
    mov r3, r2
    push {r4, lr}
    bl wake_result
    pop {r4, lr}
    mov r2, #4
log_gatt_record:
    push {r4, r7, lr}
    sub sp, sp, #28
    strb r0, [sp]
    add r4, sp, #1
    add r0, r2, #1
copy_gatt_record:
    ldrb r3, [r1], #1
    strb r3, [r4], #1
    subs r2, r2, #1
    bne copy_gatt_record
    mov r2, r0
    mov r0, #1
    mov r1, sp
    mov r7, #4
    svc #0
    add sp, sp, #28
    pop {r4, r7, pc}

.ifdef GATT_TEMPERATURE
/* Register for Nordic UART indications, enable its CCCD, authenticate,
 * and send the read-only status command. r5 is the open connection id.
 */
kettle_status:
    push {r4, r5, r6, r7, r8, lr}
    ldr r4, =link_conn
    strh r5, [r4]
    mov r0, r5
    ldr r1, =conn_label
    mov r2, #conn_label_length
    bl print_hex16
    ldr r4, =notif_count
    mov r0, #0
    str r0, [r4]
    ldr r0, =notif_params
    bl BSA_BleClNotifRegisterInit
    cmp r0, #0
    movne r0, #6
    bne kettle_status_return
    ldr r0, =notif_params
    ldr r1, =notif_template
    mov r2, #64
    bl copy_bytes
    ldr r4, =notif_params
    ldr r3, =gatt_register_params
    ldrb r3, [r3, #20]
    strb r3, [r4]
    add r0, r4, #56
    ldr r1, =kettle_address
    mov r2, #6
    bl copy_bytes
    ldr r0, =notif_params
    bl BSA_BleClNotifRegister
    cmp r0, #0
    movne r0, #6
    bne kettle_status_return
    mov r0, #2
    ldr r1, =notify_ready
    mov r2, #notify_ready_length
    mov r7, #4
    svc #0
    ldr r1, =cccd_template
    bl prepare_write
    cmp r0, #0
    movne r0, #7
    bne kettle_status_return
    bl send_write
    cmp r0, #0
    movne r0, #8
    bne kettle_status_return
    ldr r1, =auth_template
    bl prepare_write
    cmp r0, #0
    movne r0, #10
    bne kettle_status_return
    bl load_status_key
    cmp r0, #0
    bne kettle_status_return
    bl send_write
    mov r4, r0
    bl clear_status_key
    mov r0, r4
    cmp r0, #0
    movne r0, #10
    bne kettle_status_return
    mov r0, #0xff
    mov r1, #1
    bl wait_for_frame
    cmp r0, #0
    moveq r0, #11
    beq kettle_status_return
    ldrb r3, [r0, #3]
    cmp r3, #0
    moveq r0, #11
    beq kettle_status_return
    mov r0, #2
    ldr r1, =auth_ready
    mov r2, #auth_ready_length
    mov r7, #4
    svc #0
    ldr r1, =status_template
    bl prepare_write
    cmp r0, #0
    movne r0, #12
    bne kettle_status_return
    bl send_write
    cmp r0, #0
    movne r0, #12
    bne kettle_status_return
    mov r0, #6
    mov r1, #2
    bl wait_for_frame
    cmp r0, #0
    moveq r0, #13
    beq kettle_status_return
.ifdef GATT_STOP
    bl stop_if_active
.else
.ifdef GATT_BOIL
    bl boil_if_idle
.else
    ldrb r8, [r0, #8]
    ldrb r5, [r0, #5]
    ldrb r6, [r0, #3]
    cmp r6, #0
    moveq r5, #100
    bl heating_active
    mov r6, r0
    cmp r0, #0
    ldreq r1, =status_idle
    ldrne r1, =status_heating
    mov r0, #2
    mov r2, #status_heating_length
    mov r7, #4
    svc #0
    mov r0, r8
    ldr r1, =temperature_text
    bl print_temperature
    cmp r6, #0
    beq print_status_done
    cmp r5, #0
    beq print_status_done
    mov r0, r5
    ldr r1, =target_temperature_text
    bl print_temperature
print_status_done:
    mov r0, #0
.endif
.endif
kettle_status_return:
    pop {r4, r5, r6, r7, r8, pc}

/* Ready for Sky reports whether the selected mode is running. Lamp
 * and game modes are not heating even when their running flag is set.
 */
heating_active:
    ldrb r1, [r0, #3]
    ldrb r2, [r0, #11]
    mov r0, #0
    cmp r1, #2
    bxhi lr
    cmp r2, #0
    movne r0, #1
    bx lr

.ifdef GATT_STOP
/* The initial status was authenticated. Use a new sequence for each
 * operation so that the final state cannot be confused with the initial one.
 */
stop_if_active:
    push {r4, r7, r8, lr}
    bl heating_active
    cmp r0, #0
    beq stop_idle
    ldr r1, =stop_template
    bl prepare_write
    cmp r0, #0
    bne stop_unconfirmed
    bl send_write
    cmp r0, #0
    bne stop_unconfirmed
    mov r0, #4
    mov r1, #3
    bl wait_for_frame
    cmp r0, #0
    beq stop_unconfirmed
    ldrb r1, [r0, #3]
    cmp r1, #1
    bne stop_unconfirmed
    ldr r1, =stop_status_template
    bl prepare_write
    cmp r0, #0
    bne stop_unconfirmed
    bl send_write
    cmp r0, #0
    bne stop_unconfirmed
    mov r0, #6
    mov r1, #4
    bl wait_for_frame
    cmp r0, #0
    beq stop_unconfirmed
    ldrb r1, [r0, #11]
    cmp r1, #0
    bne stop_unconfirmed
    ldr r1, =heating_stopped
    mov r2, #heating_stopped_length
    b log_stop_result
stop_idle:
    ldr r1, =heating_idle
    mov r2, #heating_idle_length
log_stop_result:
    mov r0, #2
    mov r7, #4
    svc #0
    mov r0, #0
    pop {r4, r7, r8, pc}
stop_unconfirmed:
    mov r0, #14
    pop {r4, r7, r8, pc}
.endif

.ifdef GATT_BOIL
/* Operate only on an idle kettle with no reported error. Every response
 * is matched by sequence number and opcode. Never resend START.
 */
boil_if_idle:
    push {r4, r6, r7, lr}
    ldrb r1, [r0, #12]
    cmp r1, #0
    bne boil_unconfirmed
    ldrb r1, [r0, #11]
    cmp r1, #0
    bne boil_already_running
    ldr r7, =heat_target
    ldrb r7, [r7]
    cmp r7, #0
    movne r4, #1
    moveq r4, #0
    beq boil_prepare_mode
    ldrb r1, [r0, #8]
    cmp r1, r7
    bhs heat_already_reached
boil_prepare_mode:
    ldrb r6, [r0, #16]
    ldr r1, =boil_mode_template
    bl prepare_write
    cmp r0, #0
    bne boil_unconfirmed
    /* Retain the kettle's current boil-time adjustment. */
    ldr r0, =write_params
    strb r4, [r0, #63]
    strb r7, [r0, #65]
    strb r6, [r0, #76]
    mov r6, r7
    bl send_write
    cmp r0, #0
    bne boil_unconfirmed
    mov r0, #5
    mov r1, #3
    bl wait_for_frame
    cmp r0, #0
    beq boil_unconfirmed
    ldrb r1, [r0, #3]
    cmp r1, #1
    bne boil_unconfirmed

    ldr r1, =boil_check_template
    mov r0, #4
    bl boil_read_status
    cmp r0, #0
    beq boil_unconfirmed
    ldrb r1, [r0, #3]
    cmp r1, r4
    bne boil_unconfirmed
    cmp r6, #0
    ldrbne r1, [r0, #5]
    cmpne r1, r6
    bne boil_unconfirmed
    ldrb r2, [r0, #11]
    ldrb r3, [r0, #12]
    orrs r2, r2, r3
    bne boil_unconfirmed

    ldr r1, =boil_start_template
    bl prepare_write
    cmp r0, #0
    bne boil_unconfirmed
    bl send_write
    cmp r0, #0
    bne boil_confirm_status
    mov r0, #3
    mov r1, #5
    bl wait_for_frame
boil_confirm_status:
    /* A write or acknowledgement timeout leaves the result uncertain.
     * Read state once even in that case.
     */
    ldr r1, =boil_status_template
    mov r0, #6
    bl boil_read_status
    cmp r0, #0
    beq boil_unconfirmed
    ldrb r1, [r0, #3]
    cmp r1, r4
    bne boil_unconfirmed
    cmp r6, #0
    ldrbne r1, [r0, #5]
    cmpne r1, r6
    bne boil_unconfirmed
    ldrb r2, [r0, #11]
    ldrb r3, [r0, #12]
    cmp r3, #0
    bne boil_unconfirmed
    cmp r2, #0
    beq boil_unconfirmed
    ldr r1, =boil_started
    mov r2, #boil_started_length
    b log_boil_result

heat_already_reached:
    ldr r1, =heat_reached
    mov r2, #heat_reached_length
    b log_boil_result
boil_already_running:
    ldr r1, =boil_busy
    mov r2, #boil_busy_length
log_boil_result:
    mov r0, #2
    mov r7, #4
    svc #0
    mov r0, #0
    pop {r4, r6, r7, pc}
boil_unconfirmed:
    mov r0, #15
    pop {r4, r6, r7, pc}

boil_read_status:
    push {r4, lr}
    mov r4, r0
    bl prepare_write
    cmp r0, #0
    bne boil_read_failed
    bl send_write
    cmp r0, #0
    bne boil_read_failed
    mov r0, #6
    mov r1, r4
    bl wait_for_frame
    pop {r4, pc}
boil_read_failed:
    mov r0, #0
    pop {r4, pc}
.endif

prepare_write:
    push {r4, r5, r6, lr}
    mov r5, r1
    ldr r4, =write_params
    mov r0, r4
    bl BSA_BleClWriteInit
    cmp r0, #0
    bne prepare_write_return
    mov r0, r4
    mov r1, r5
    mov r2, #244
    bl copy_bytes
    ldr r4, =write_params
    ldr r3, =link_conn
    ldrh r3, [r3]
    strh r3, [r4]
    mov r0, #0
prepare_write_return:
    pop {r4, r5, r6, pc}

send_write:
    push {r4, r5, r7, lr}
    ldr r4, =write_result
    mov r0, #0
    str r0, [r4]
    ldr r0, =write_params
    bl BSA_BleClWrite
    mov r5, r0
    mov r0, r5
    ldr r1, =write_api_label
    mov r2, #write_api_label_length
    bl print_hex16
    mov r0, r5
    cmp r0, #0
    bne send_write_return
    ldr r0, =write_result
    bl wait_for_gatt_result
    ldr r4, =write_result
    ldr r0, [r4]
    mov r5, r0
    ldr r1, =write_evt_label
    mov r2, #write_evt_label_length
    bl print_hex16
    mov r0, r5
    cmp r0, #1
    movne r0, #1
    moveq r0, #0
send_write_return:
    pop {r4, r5, r7, pc}

load_status_key:
    push {r4, r5, r7, lr}
    ldr r0, =key_path
    mov r1, #0
    mov r7, #5
    svc #0
    cmp r0, #0
    movlt r0, #9
    blt load_status_key_return
    mov r4, r0
    ldr r1, =write_params
    add r1, r1, #63
    mov r2, #8
    mov r7, #3
    svc #0
    mov r5, r0
    mov r0, r4
    mov r7, #6
    svc #0
    cmp r5, #8
    moveq r0, #0
    movne r0, #9
load_status_key_return:
    pop {r4, r5, r7, pc}

clear_status_key:
    push {r1, r2, r3, lr}
    ldr r1, =write_params
    add r1, r1, #63
    mov r2, #8
    mov r3, #0
clear_status_key_byte:
    strb r3, [r1], #1
    subs r2, r2, #1
    bne clear_status_key_byte
    pop {r1, r2, r3, pc}

wait_for_frame:
    push {r4-r8, lr}
    mov r4, r0
    mov r8, r1
    ldr r5, =notif_count
    mov r6, #4
wait_for_frame_next:
    ldr r1, [r5]
    push {r1, r2}
    dmb ish
    mov r0, r4
    mov r1, r8
    bl find_frame
    pop {r1, r2}
    cmp r0, #0
    bne wait_for_frame_return
    cmp r6, #0
    beq wait_for_frame_return
    mov r0, r5
    ldr r2, =event_wait
    bl wait_for_change
    cmn r0, #110
    moveq r6, #0
    subne r6, r6, #1
    b wait_for_frame_next
wait_for_frame_return:
    pop {r4-r8, pc}

find_frame:
    push {r4, r5, lr}
    mov r2, r0
    cmp r2, #6
    moveq r0, #19
    movne r0, #4
    ldr r4, =notif_frames
    ldr r5, =notif_count
    ldr r5, [r5]
    cmp r5, #0
    beq find_frame_none
find_frame_next:
    ldrb r3, [r4]
    cmp r3, #0x55
    bne find_frame_advance
    ldrb r3, [r4, #1]
    cmp r3, r1
    bne find_frame_advance
    ldrb r3, [r4, #2]
    cmp r3, r2
    bne find_frame_advance
    ldrb r3, [r4, r0]
    cmp r3, #0xaa
    beq find_frame_found
find_frame_advance:
    add r4, r4, #24
    subs r5, r5, #1
    bne find_frame_next
find_frame_none:
    mov r0, #0
    pop {r4, r5, pc}
find_frame_found:
    mov r0, r4
    pop {r4, r5, pc}

print_temperature:
    push {r4, r5, r6, r7, lr}
    ldr r4, =temperature_line
    mov r2, #18
    mov r6, r0
    cmp r0, #100
    bhs print_temperature_hundreds
print_temperature_copy:
    ldrb r3, [r1], #1
    strb r3, [r4], #1
    subs r2, r2, #1
    bne print_temperature_copy
    mov r5, r6
    mov r3, #0
print_temperature_tens:
    cmp r5, #10
    blo print_temperature_store
    sub r5, r5, #10
    add r3, r3, #1
    b print_temperature_tens
print_temperature_store:
    add r3, r3, #0x30
    add r5, r5, #0x30
    ldr r4, =temperature_line
    strb r3, [r4, #15]
    strb r5, [r4, #16]
    mov r0, #2
    mov r1, r4
    mov r2, #18
    mov r7, #4
    svc #0
    pop {r4, r5, r6, r7, pc}

print_temperature_hundreds:
    cmp r6, #100
    bne print_temperature_invalid
    mov r0, #2
    add r1, r1, #18
    mov r2, #temperature_hundreds_length
    mov r7, #4
    svc #0
    b print_temperature_return
print_temperature_invalid:
    mov r0, #2
    ldr r1, =temperature_invalid
    mov r2, #temperature_invalid_length
    mov r7, #4
    svc #0
print_temperature_return:
    pop {r4, r5, r6, r7, pc}

print_hex16:
    push {r4, r6, r7, lr}
    mov r4, r0
    mov r0, #2
    mov r7, #4
    svc #0
    ldr r6, =hex_line
    mov r3, #12
print_hex16_digit:
    mov r0, r4, lsr r3
    and r0, r0, #0xf
    cmp r0, #10
    addlo r0, r0, #0x30
    addhs r0, r0, #0x57
    strb r0, [r6], #1
    subs r3, r3, #4
    bpl print_hex16_digit
    mov r0, #0x0a
    strb r0, [r6]
    ldr r1, =hex_line
    mov r0, #2
    mov r2, #5
    mov r7, #4
    svc #0
    pop {r4, r6, r7, pc}

copy_bytes:
    push {r3, lr}
copy_bytes_next:
    ldrb r3, [r1], #1
    strb r3, [r0], #1
    subs r2, r2, #1
    bne copy_bytes_next
    pop {r3, pc}

gatt_log_write:
    ldr r2, =write_result
    ldrh r3, [r1, #2]
    add r3, r3, #1
    dmb ish
    str r3, [r2]
    mov r3, r2
    push {r4, lr}
    bl wake_result
    pop {r4, lr}
    mov r2, #4
    b log_gatt_record

gatt_log_notification:
    push {r4, r5, r6, r7, r8, lr}
    mov r6, r1
    mov r4, r1
    /* The event window is 120 bytes and a stored frame is 24 bytes, so only
     * 97 start offsets can be tested without reading past its end. */
    mov r5, #97
gatt_notification_scan:
    ldrb r3, [r4]
    cmp r3, #0x55
    bne gatt_notification_advance
    mov r3, #23
gatt_notification_end:
    ldrb r2, [r4, r3]
    cmp r2, #0xaa
    beq gatt_notification_store
    subs r3, r3, #1
    bne gatt_notification_end
gatt_notification_advance:
    add r4, r4, #1
    subs r5, r5, #1
    bne gatt_notification_scan
    mov r0, #7
    mov r1, r6
    mov r2, #8
    bl log_gatt_record
    pop {r4, r5, r6, r7, r8, pc}
gatt_notification_store:
    ldr r2, =notif_count
    ldr r3, [r2]
    cmp r3, #8
    bhs gatt_notification_logged
    add r5, r3, r3, lsl #1
    lsl r5, r5, #3
    ldr r0, =notif_frames
    add r0, r0, r5
    mov r1, r4
    mov r5, #24
gatt_notification_copy:
    ldrb r7, [r1], #1
    strb r7, [r0], #1
    subs r5, r5, #1
    bne gatt_notification_copy
    add r3, r3, #1
    dmb ish
    str r3, [r2]
    mov r3, r2
    bl wake_result
gatt_notification_logged:
    mov r0, #7
    mov r1, r4
    mov r2, #24
    bl log_gatt_record
    pop {r4, r5, r6, r7, r8, pc}
.endif

.section .rodata
.ifdef GATT_CACHE_ONLY
cache_reply_accepted:
.ifdef GATT_CACHE_IMAGE
    .ascii "BSA_BleClCacheLoad: image\n"
.else
    .ascii "BSA_BleClCacheLoad: 0\n"
.endif
.equ cache_reply_accepted_length, . - cache_reply_accepted
.endif
.ifdef GATT_CACHE_THEN_SEARCH
search_started:
    .ascii "BSA_BleClSearch: 0\n"
.equ search_started_length, . - search_started
.endif
.ifdef GATT_CACHE_IMAGE
.balign 4
kettle_cache_attrs:
.include "scripts/kettle-cache-attrs.inc"
.endif
.ifdef GATT_TEMPERATURE
open_failed_label:
    .ascii "GATT open failed: "
.equ open_failed_label_length, . - open_failed_label
notify_ready:
    .ascii "BSA_BleClNotifRegister: 0\n"
.equ notify_ready_length, . - notify_ready
conn_label:
    .ascii "conn: "
.equ conn_label_length, . - conn_label
write_api_label:
    .ascii "write_api: "
.equ write_api_label_length, . - write_api_label
write_evt_label:
    .ascii "write_evt: "
.equ write_evt_label_length, . - write_evt_label
close_api_label:
    .ascii "close_api: "
.equ close_api_label_length, . - close_api_label
auth_ready:
    .ascii "auth: 0\n"
.equ auth_ready_length, . - auth_ready
.ifdef GATT_STOP
heating_stopped:
    .ascii "heating_result: stopped\n"
.equ heating_stopped_length, . - heating_stopped
heating_idle:
    .ascii "heating_result: idle\n"
.equ heating_idle_length, . - heating_idle
.endif
.ifdef GATT_BOIL
heat_reached:
    .ascii "boil_result: reached\n"
.equ heat_reached_length, . - heat_reached
boil_started:
    .ascii "boil_result: started\n"
.equ boil_started_length, . - boil_started
boil_busy:
    .ascii "boil_result: busy\n"
.equ boil_busy_length, . - boil_busy
.endif
status_idle:
    .ascii "heating_on: 0\n"
status_heating:
    .ascii "heating_on: 1\n"
.equ status_heating_length, . - status_heating
temperature_text:
    .ascii "temperature_c: 00\n"
temperature_hundreds:
    .ascii "temperature_c: 100\n"
.equ temperature_hundreds_length, . - temperature_hundreds
temperature_invalid:
    .ascii "temperature out of range\n"
.equ temperature_invalid_length, . - temperature_invalid
target_temperature_text:
    .ascii "target_temp_c: 00\n"
    .ascii "target_temp_c: 100\n"
key_path:
    .asciz "/tmp/ble-temp/key"
.balign 4
.include "build/phase1/kettle-status-blobs.inc"
.endif
.balign 4
event_wait:
    .word 6, 0
.bss
.balign 4
kettle_address:
    .space 8
.ifdef GATT_BOIL
heat_target:
    .space 4
.endif
connect_params:
    .space 8
search_params:
    .space 24
link_close_params:
    .space 4
open_result:
    .space 8
search_result:
    .space 4
cache_request:
    .space 4
cache_params:
    .space 296
.ifdef GATT_TEMPERATURE
.balign 4
write_params:
    .space 244
notif_params:
    .space 64
write_result:
    .space 4
hex_line:
    .space 8
temperature_line:
    .space 20
notif_count:
    .space 4
notif_frames:
    .space 192
link_conn:
    .space 2
.endif
