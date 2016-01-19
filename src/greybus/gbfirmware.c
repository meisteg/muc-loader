/**
 * Copyright (c) 2015 Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "debug.h"
#include "boot_main.h"
#include "greybus.h"
#include "gbfirmware.h"
#include "datalink.h"
#include "stm32l4xx_hal.h"
#include "tftf.h"
#include "stm32l4xx_flash.h"
#include "stm32_hal_mod.h"

/* Greybus FirmWare protocol version we support */
#define GB_FIRMWARE_VERSION_MAJOR   0x00
#define GB_FIRMWARE_VERSION_MINOR   0x01

#define CPORT_POLLING_TIMEOUT       512

#define MUCLOADER_BASE_ADDR     FLASH_BASE

static bool tftf_header_received = false;
static uint8_t tftf_buff[TFTF_HEADER_SIZE];
static uint16_t id_count = 1;
static uint16_t section_index = 0;

static struct fw_flash_data {
    uint32_t fw_flash_addr;
    uint32_t fw_request_size;
    uint32_t fw_offset;
    int32_t fw_remaining_size;
    uint32_t new_payload_size;
} fw_flash_data;

extern uint32_t agreed_pl_size;
uint8_t responded_op = GB_FW_OP_INVALID;

static uint8_t _gbfw_stage = 0x00;  /* current flashing stage */

int fw_cport_handler(uint32_t cportid, void *data, size_t len);
static int gbfw_ready_to_boot(uint8_t status);

static int gbfw_get_version(uint32_t cportid, gb_operation_header *header) {
    uint8_t payload[2] = {GB_FIRMWARE_VERSION_MAJOR, GB_FIRMWARE_VERSION_MINOR};
    return greybus_op_response(cportid, header, GB_OP_SUCCESS, payload,
                               sizeof(payload));
}

static int gbfw_ap_ready(uint32_t cportid, gb_operation_header *header) {
    return greybus_op_response(cportid, header, GB_OP_SUCCESS, NULL, 0);
}

static struct gbfw_firmware_size_response firmware_size_response;

int gbfw_firmware_size(uint8_t stage) {
    int rc;
    struct gbfw_firmware_size_request req = {stage};

    _gbfw_stage = stage;

    rc = greybus_send_request(gbfw_cportid, id_count++, GB_FW_OP_FIRMWARE_SIZE,
                              (uint8_t*)&req, sizeof(req));
    if (rc) {
        return rc;
    }

    return 0;
}

static void gbfw_next_stage(void) {
    memset(&fw_flash_data, 0, sizeof(fw_flash_data));
    tftf_header_received = false;

    if (_gbfw_stage >= GBFW_STAGE_MIN && _gbfw_stage < GBFW_STAGE_MAX) {
        gbfw_firmware_size(_gbfw_stage + 1);
    } else {
        gbfw_ready_to_boot(GB_FW_BOOT_STATUS_SECURE);
    }
}


static int gbfw_firmware_size_response(gb_operation_header *head, void *data,
                                       uint32_t len) {
    size_t fw_size;

    if (head->status) {
        gbfw_next_stage();
        return 0;
    }

    responded_op = GB_FW_OP_FIRMWARE_SIZE;

    if (len < sizeof(struct gbfw_firmware_size_response)) {
        dbgprint("gbfw_firmware_size_response: bad response size\r\n");
        return GB_FW_ERR_INVALID;
    }
    memcpy(&firmware_size_response, data, sizeof(firmware_size_response));

    fw_size = firmware_size_response.size;
    dbgprinthex32(fw_size);

    if (fw_size > TFTF_HEADER_SIZE) {
        int rc;
        struct gbfw_get_firmware_request req = {0, TFTF_HEADER_SIZE};
        rc = greybus_send_request(gbfw_cportid, id_count++, GB_FW_OP_GET_FIRMWARE,
                                  (uint8_t*)&req, sizeof(req));
        if (rc) {
            return rc;
        }
        fw_flash_data.fw_request_size = TFTF_HEADER_SIZE;
    } else {
        dbgprint("skip empty firmware");
        gbfw_next_stage();
    }

    return 0;
}

static int gbfw_get_firmware(uint32_t offset, uint32_t size) {
    int rc;
    struct gbfw_get_firmware_request req = {offset, size};
    rc = greybus_send_request(gbfw_cportid, id_count++, GB_FW_OP_GET_FIRMWARE,
                              (uint8_t*)&req, sizeof(req));
    if (rc) {
        return rc;
    }

    return 0;
}


static int gbfw_get_firmware_response(gb_operation_header *header, void *data,
                                      uint32_t len) {
    uint8_t *data_ptr;
    uint32_t flash_addr;
    uint32_t flash_data_size;
    uint32_t pl_data_size;

    tftf_header *tf_header = (tftf_header *)data;
    data_ptr = (uint8_t *)data;

    if(tftf_header_received == false) {
        if(!valid_tftf_header(tf_header)) {
            dbgprint("INVALID_TFTF");
            return GB_FW_ERR_INVALID;
        }

        memcpy(tftf_buff, data_ptr, sizeof(tftf_header));
        tftf_header_received = true;

        /* flash the raw code and any attached sections*/
        section_index = get_section_index(TFTF_SECTION_RAW_CODE, &tf_header->sections[0]);
        if(section_index == TFTF_SECTION_END) {
            dbgprint("No sections to flash");
            return GB_FW_ERR_INVALID;
        }
        dbgprinthex32(section_index);

        /* erase image copy address and size */
        fw_flash_data.fw_flash_addr = tf_header->sections[section_index].section_load_address;
        fw_flash_data.fw_remaining_size = firmware_size_response.size - TFTF_HEADER_SIZE;
        fw_flash_data.fw_offset = 0;
        fw_flash_data.new_payload_size = 0;

        flash_erase(fw_flash_data.fw_flash_addr, fw_flash_data.fw_remaining_size);
        set_flashing_flag();

        /* program the first packet */
        data_ptr += sizeof(tftf_header);

        flash_addr = fw_flash_data.fw_flash_addr + fw_flash_data.fw_offset;
        flash_data_size = fw_flash_data.fw_request_size - TFTF_HEADER_SIZE;

        dbgprint("FLADR:");
        dbgprinthex32(flash_addr);
        dbgprint("\r\n");

        program_flash_data(flash_addr, flash_data_size, data_ptr);

        fw_flash_data.fw_offset += (fw_flash_data.fw_request_size - TFTF_HEADER_SIZE);
        fw_flash_data.fw_remaining_size -= (fw_flash_data.fw_request_size - TFTF_HEADER_SIZE);

        dbgprint("off:");
        dbgprinthex32(fw_flash_data.fw_offset + TFTF_HEADER_SIZE);
        dbgprint("\r\n");

        /* Calculate the size for get firmware request */
        pl_data_size = agreed_pl_size - (NW_HEADER_SIZE + GB_HEADER_SIZE);
        /* data size should be multiple of uint64_t */
        fw_flash_data.fw_request_size =
                 pl_data_size - (pl_data_size % sizeof(uint64_t));
    } else {
        tf_header = (tftf_header *)tftf_buff;

        flash_addr = fw_flash_data.fw_flash_addr + fw_flash_data.fw_offset;
        flash_data_size = fw_flash_data.new_payload_size;

        dbgprint("FLADR:");
        dbgprinthex32(flash_addr);
        dbgprint("\r\n");
        dbgprint("PLSZ:");
        dbgprinthex32(flash_data_size);
        dbgprint("\r\n");

        program_flash_data(flash_addr, flash_data_size, data_ptr);

        fw_flash_data.fw_offset += flash_data_size;
        fw_flash_data.fw_remaining_size -= flash_data_size;

        dbgprint("off:");
        dbgprinthex32(fw_flash_data.fw_offset + TFTF_HEADER_SIZE);
        dbgprint("\r\n");
        dbgprint("Rem:");
        dbgprinthex32(fw_flash_data.fw_remaining_size);
        dbgprint("\r\n");
    }

    if(fw_flash_data.fw_remaining_size > 0) {
        if(fw_flash_data.fw_remaining_size < fw_flash_data.fw_request_size) {
            fw_flash_data.new_payload_size = fw_flash_data.fw_remaining_size;
        } else {
            fw_flash_data.new_payload_size = fw_flash_data.fw_request_size;
        }

        dbgprint("Req:");
        dbgprinthex32(fw_flash_data.new_payload_size);
        dbgprint("\r\n");
        gbfw_get_firmware(fw_flash_data.fw_offset + TFTF_HEADER_SIZE,
                          fw_flash_data.new_payload_size);
    } else {
        gbfw_next_stage();
    }
    return 0;
}

static int gbfw_ready_to_boot(uint8_t status) {
    int rc;
    struct gbfw_ready_to_boot_request req = {status};
    tftf_header *tf_header = (tftf_header *)tftf_buff;

    section_index = get_section_index(TFTF_SECTION_RAW_CODE, &tf_header->sections[0]);
    if((tf_header->sections[section_index].section_load_address != MUCLOADER_BASE_ADDR)) {
            program_tftf_header(tftf_buff, sizeof(tftf_buff));
    }

    /* Erase the Flash Mode Barker */
    ErasePage((uint32_t)(FLASHMODE_FLAG_PAGE));

    responded_op = GB_FW_OP_READY_TO_BOOT;
    /* ready_to_boot currently doesn't respond so fake a response */
    rc = greybus_send_request(gbfw_cportid, id_count++, GB_FW_OP_READY_TO_BOOT,
                              (uint8_t*)&req, sizeof(req));

    return rc;
}

static int gbfw_ready_to_boot_response(gb_operation_header *header, void *data,
                                       uint32_t len) {
    if (header->status) {
        dbgprint("gbfw_ready_to_boot_response(): got error status\r\n");
        return -header->status;
    }
    return 0;
}

int fw_cport_handler(uint32_t cportid, void *data, size_t len) {

    int rc = 0;
    uint8_t *data_ptr;

    if (cportid != gbfw_cportid) {
        dbgprint("fw_cport_handler: incorrect CPort #\r\n");
        return GB_FW_ERR_INVALID;
    }

    if (len < sizeof(gb_operation_header)) {
        dbgprint("fw_cport_handler: RX data length error\r\n");
        return GB_FW_ERR_INVALID;
    }

    gb_operation_header *op_header = (gb_operation_header *)data;
    data_ptr = (uint8_t *)data;

    if(op_header->size > len) {
        dbgprint("fw_cport_handler: nonsense message.\r\n");
        return GB_FW_ERR_INVALID;
    }

    if (op_header->type & GB_TYPE_RESPONSE && op_header->status) {
        dbgprintx32("fw_cport_handler: Greybus response, status 0x",
                   op_header->status, "\r\n");
    }
    /*
     * This works here because we're actually calling this handler
     * synchronously.  We set the constant and return to the recipient rather
     * than setting the constant in parallel with the recipient.
     */
    responded_op = op_header->type;
    data_ptr += sizeof(gb_operation_header);
    len -= sizeof(gb_operation_header);
    switch (op_header->type) {
    case GB_FW_OP_PROTOCOL_VERSION:
        dbgprint("FWVER\r\n");
        rc = gbfw_get_version(cportid, op_header);
        break;
    case GB_FW_OP_FIRMWARE_SIZE | GB_TYPE_RESPONSE:
        dbgprint("FWSZRSP\r\n");
        rc = gbfw_firmware_size_response(op_header, data_ptr, len);
        break;
    case GB_FW_OP_GET_FIRMWARE | GB_TYPE_RESPONSE:
        dbgprint("GETFWRESP\r\n");
        rc = gbfw_get_firmware_response(op_header, data_ptr, len);
        break;
    case GB_FW_OP_READY_TO_BOOT | GB_TYPE_RESPONSE:
        dbgprint("FWBOOT\r\n");
        rc = gbfw_ready_to_boot_response(op_header, data_ptr, len);
        break;
    case GB_FW_OP_AP_READY:
        dbgprint("FWAPRDY\r\n");
        rc = gbfw_ap_ready(cportid, op_header);
        break;
    default:
        dbgprint("FWDFLT\r\n");
        responded_op = GB_FW_OP_INVALID;
        break;
    }

    if (rc) {
        responded_op = GB_FW_OP_INVALID;
    }

    return rc;
}

int cport_connected = 0, offset = -1;

int greybus_cport_connect(void) {
    if (cport_connected == 1) {
        /* Don't know what to do if it is already connected */
        return GB_FW_ERR_INVALID;
    }

    offset = 0;
    cport_connected = 1;
    return 0;
}

int greybus_cport_disconnect(void) {
    if (cport_connected == 0) {
        return -EINVAL;
    }
    return 0;
}