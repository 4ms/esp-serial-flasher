/* Copyright 2020-2023 Espressif Systems (Shanghai) CO LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "protocol.h"
#include "esp_loader_io.h"
#include "slip.h"
#include <stddef.h>
#include <string.h>

//#define LOG_MESSAGES

#ifdef LOG_MESSAGES
#include <stdio.h>
#endif

#define CMD_SIZE(cmd) ( sizeof(cmd) - sizeof(command_common_t) )

static uint32_t s_sequence_number = 0;

static esp_loader_error_t check_response(command_t cmd, uint32_t *reg_value, void* resp, uint32_t resp_size);

static uint8_t compute_checksum(const uint8_t *data, uint32_t size)
{
    uint8_t checksum = 0xEF;

    while (size--) {
        checksum ^= *data++;
    }

    return checksum;
}

static esp_loader_error_t send_cmd(const void *cmd_data, uint32_t size, uint32_t *reg_value)
{
    response_t response;
    command_t command = ((const command_common_t *)cmd_data)->command;

    #ifdef LOG_MESSAGES
    printf("Command op=0x%02x data len %u: ", (uint8_t)command, size);
    for (int i=0; i<size; i++)
    {
        printf("%02x", ((const uint8_t *)cmd_data)[i]);
    }
    printf("\n");
    #endif

    RETURN_ON_ERROR( SLIP_send_delimiter() );
    RETURN_ON_ERROR( SLIP_send((const uint8_t *)cmd_data, size) );
    RETURN_ON_ERROR( SLIP_send_delimiter() );

    const uint8_t response_cnt = command == SYNC ? 8 : 1;
    
    for (uint8_t recv_cnt = 0; recv_cnt < response_cnt; recv_cnt++) {
        RETURN_ON_ERROR(check_response(command, reg_value, &response, sizeof(response)));
    }

    return ESP_LOADER_SUCCESS;
}


static esp_loader_error_t send_cmd_with_data(const void *cmd_data, size_t cmd_size,
                                             const void *data, size_t data_size)
{
    response_t response;
    command_t command = ((const command_common_t *)cmd_data)->command;

    #ifdef LOG_MESSAGES
    printf("Command op=0x%02x data len %u + %u: ", (uint8_t)command, cmd_size, data_size);
    for (int i=0; i<cmd_size; i++)
    {
        printf("%02x", ((const uint8_t *)cmd_data)[i]);
    }
    printf(" ");
    for (int i=0; i<data_size; i++)
    {
        printf("%02x", ((const uint8_t *)data)[i]);
    }
    printf("\n");
    #endif

    RETURN_ON_ERROR( SLIP_send_delimiter() );
    RETURN_ON_ERROR( SLIP_send((const uint8_t *)cmd_data, cmd_size) );
    RETURN_ON_ERROR( SLIP_send(data, data_size) );
    RETURN_ON_ERROR( SLIP_send_delimiter() );

    return check_response(command, NULL, &response, sizeof(response));
}


static esp_loader_error_t send_cmd_md5(const void *cmd_data, size_t cmd_size, uint8_t md5_out[MD5_SIZE])
{
    rom_md5_response_t response;
    command_t command = ((const command_common_t *)cmd_data)->command;

    RETURN_ON_ERROR( SLIP_send_delimiter() );
    RETURN_ON_ERROR( SLIP_send((const uint8_t *)cmd_data, cmd_size) );
    RETURN_ON_ERROR( SLIP_send_delimiter() );

    RETURN_ON_ERROR( check_response(command, NULL, &response, sizeof(response)) );

    memcpy(md5_out, response.md5, MD5_SIZE);

    return ESP_LOADER_SUCCESS;
}


static void log_loader_internal_error(error_code_t error)
{
    loader_port_debug_print("Error: ");

    switch (error) {
        case INVALID_CRC:     loader_port_debug_print("INVALID_CRC"); break;
        case INVALID_COMMAND: loader_port_debug_print("INVALID_COMMAND"); break;
        case COMMAND_FAILED:  loader_port_debug_print("COMMAND_FAILED"); break;
        case FLASH_WRITE_ERR: loader_port_debug_print("FLASH_WRITE_ERR"); break;
        case FLASH_READ_ERR:  loader_port_debug_print("FLASH_READ_ERR"); break;
        case READ_LENGTH_ERR: loader_port_debug_print("READ_LENGTH_ERR"); break;
        case DEFLATE_ERROR:   loader_port_debug_print("DEFLATE_ERROR"); break;
        default:              loader_port_debug_print("UNKNOWN ERROR"); break;
    }

    loader_port_debug_print("\n");
}


static esp_loader_error_t check_response(command_t cmd, uint32_t *reg_value, void* resp, uint32_t resp_size)
{
    esp_loader_error_t err;
    common_response_t *response = (common_response_t *)resp;

    do {
        err = SLIP_receive_packet(resp, resp_size);
        if (err != ESP_LOADER_SUCCESS) {
            return err;
        }
    } while ((response->direction != READ_DIRECTION) || (response->command != cmd));

    response_status_t *status = (response_status_t *)((uint8_t *)resp + resp_size - sizeof(response_status_t));

    #ifdef LOG_MESSAGES
    printf("Check response cmd 0x%02x: ", (uint8_t)cmd);
    printf("Direction %u, command 0x%02x, size %u, failed 0x%02x, error 0x%02x\n", response->direction, response->command, response->size, status->failed, status->error);
    #endif

    if (status->failed) {
        log_loader_internal_error(status->error);
        return ESP_LOADER_ERROR_INVALID_RESPONSE;
    }

    if (reg_value != NULL) {
        *reg_value = response->value;
    }

    return ESP_LOADER_SUCCESS;
}

esp_loader_error_t loader_flash_begin_cmd(uint32_t offset,
                                          uint32_t erase_size,
                                          uint32_t block_size,
                                          uint32_t blocks_to_write,
                                          bool encryption)
{
    uint32_t encryption_size = encryption ? sizeof(uint32_t) : 0;

    flash_begin_command_t flash_begin_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = FLASH_BEGIN,
            .size = CMD_SIZE(flash_begin_cmd) - encryption_size,
            .checksum = 0
        },
        .erase_size = erase_size,
        .packet_count = blocks_to_write,
        .packet_size = block_size,
        .offset = offset,
        .encrypted = 0
    };

    s_sequence_number = 0;

    return send_cmd(&flash_begin_cmd, sizeof(flash_begin_cmd) - encryption_size, NULL);
}

esp_loader_error_t loader_flash_defl_begin_cmd(uint32_t offset,
                                          uint32_t uncompressed_size,
                                          uint32_t block_size,
                                          uint32_t blocks_to_write,
                                          bool encryption)
{
    uint32_t encryption_size = encryption ? sizeof(uint32_t) : 0;

    flash_defl_begin_command_t flash_begin_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = FLASH_DEFL_BEGIN,
            .size = CMD_SIZE(flash_begin_cmd) - encryption_size,
            .checksum = 0
        },
        .uncompressed_size = uncompressed_size,
        .packet_count = blocks_to_write,
        .packet_size = block_size,
        .offset = offset,
        .encrypted = 0
    };

    s_sequence_number = 0;

    return send_cmd(&flash_begin_cmd, sizeof(flash_begin_cmd) - encryption_size, NULL);
}


esp_loader_error_t loader_flash_data_cmd(const uint8_t *data, uint32_t size)
{
    data_command_t data_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = FLASH_DATA,
            .size = CMD_SIZE(data_cmd) + size,
            .checksum = compute_checksum(data, size)
        },
        .data_size = size,
        .sequence_number = s_sequence_number++,
    };

    return send_cmd_with_data(&data_cmd, sizeof(data_cmd), data, size);
}

esp_loader_error_t loader_flash_defl_data_cmd(const uint8_t *data, uint32_t size)
{
    data_command_t data_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = FLASH_DEFL_DATA,
            .size = CMD_SIZE(data_cmd) + size,
            .checksum = compute_checksum(data, size)
        },
        .data_size = size,
        .sequence_number = s_sequence_number++,
    };

    return send_cmd_with_data(&data_cmd, sizeof(data_cmd), data, size);
}


esp_loader_error_t loader_flash_end_cmd(bool stay_in_loader)
{
    flash_end_command_t end_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = FLASH_END,
            .size = CMD_SIZE(end_cmd),
            .checksum = 0
        },
        .stay_in_loader = stay_in_loader
    };

    return send_cmd(&end_cmd, sizeof(end_cmd), NULL);
}

esp_loader_error_t loader_flash_defl_end_cmd(bool stay_in_loader)
{
    flash_end_command_t end_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = FLASH_DEFL_END,
            .size = CMD_SIZE(end_cmd),
            .checksum = 0
        },
        .stay_in_loader = stay_in_loader
    };

    return send_cmd(&end_cmd, sizeof(end_cmd), NULL);
}


esp_loader_error_t loader_mem_begin_cmd(uint32_t offset, uint32_t size, uint32_t blocks_to_write, uint32_t block_size)
{

    mem_begin_command_t mem_begin_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = MEM_BEGIN,
            .size = CMD_SIZE(mem_begin_cmd),
            .checksum = 0
        },
        .total_size = size,
        .blocks = blocks_to_write,
        .block_size = block_size,
        .offset = offset
    };

    s_sequence_number = 0;

    return send_cmd(&mem_begin_cmd, sizeof(mem_begin_cmd), NULL);
}


esp_loader_error_t loader_mem_data_cmd(const uint8_t *data, uint32_t size)
{
    data_command_t data_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = MEM_DATA,
            .size = CMD_SIZE(data_cmd) + size,
            .checksum = compute_checksum(data, size)
        },
        .data_size = size,
        .sequence_number = s_sequence_number++,
    };
    return send_cmd_with_data(&data_cmd, sizeof(data_cmd), data, size);
}

esp_loader_error_t loader_mem_end_cmd(uint32_t entrypoint)
{
    mem_end_command_t end_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = MEM_END,
            .size = CMD_SIZE(end_cmd),
        },
        .stay_in_loader = (entrypoint == 0),
        .entry_point_address = entrypoint
    };

    return send_cmd(&end_cmd, sizeof(end_cmd), NULL);
}


esp_loader_error_t loader_sync_cmd(void)
{
    sync_command_t sync_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = SYNC,
            .size = CMD_SIZE(sync_cmd),
            .checksum = 0
        },
        .sync_sequence = {
            0x07, 0x07, 0x12, 0x20,
            0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
            0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
            0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
            0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
        }
    };

    return send_cmd(&sync_cmd, sizeof(sync_cmd), NULL);
}


esp_loader_error_t loader_write_reg_cmd(uint32_t address, uint32_t value,
                                        uint32_t mask, uint32_t delay_us)
{
    write_reg_command_t write_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = WRITE_REG,
            .size = CMD_SIZE(write_cmd),
            .checksum = 0
        },
        .address = address,
        .value = value,
        .mask = mask,
        .delay_us = delay_us
    };

    return send_cmd(&write_cmd, sizeof(write_cmd), NULL);
}


esp_loader_error_t loader_read_reg_cmd(uint32_t address, uint32_t *reg)
{
    read_reg_command_t read_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = READ_REG,
            .size = CMD_SIZE(read_cmd),
            .checksum = 0
        },
        .address = address,
    };

    return send_cmd(&read_cmd, sizeof(read_cmd), reg);
}


esp_loader_error_t loader_spi_attach_cmd(uint32_t config)
{
    spi_attach_command_t attach_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = SPI_ATTACH,
            .size = CMD_SIZE(attach_cmd),
            .checksum = 0
        },
        .configuration = config,
        .zero = 0
    };

    return send_cmd(&attach_cmd, sizeof(attach_cmd), NULL);
}

esp_loader_error_t loader_change_baudrate_cmd(uint32_t baudrate)
{
    change_baudrate_command_t baudrate_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = CHANGE_BAUDRATE,
            .size = CMD_SIZE(baudrate_cmd),
            .checksum = 0
        },
        .new_baudrate = baudrate,
        .old_baudrate = 0 // ESP32 ROM only
    };

    return send_cmd(&baudrate_cmd, sizeof(baudrate_cmd), NULL);
}

esp_loader_error_t loader_md5_cmd(uint32_t address, uint32_t size, uint8_t *md5_out)
{
    spi_flash_md5_command_t md5_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = SPI_FLASH_MD5,
            .size = CMD_SIZE(md5_cmd),
            .checksum = 0
        },
        .address = address,
        .size = size,
        .reserved_0 = 0,
        .reserved_1 = 0
    };

    return send_cmd_md5(&md5_cmd, sizeof(md5_cmd), md5_out);
}

esp_loader_error_t loader_spi_parameters(uint32_t total_size)
{
    write_spi_command_t spi_cmd = {
        .common = {
            .direction = WRITE_DIRECTION,
            .command = SPI_SET_PARAMS,
            .size = 24,
            .checksum = 0
        },
        .id = 0,
        .total_size = total_size,
        .block_size = 64 * 1024,
        .sector_size = 4 * 1024,
        .page_size = 0x100,
        .status_mask = 0xFFFF,
    };

    return send_cmd(&spi_cmd, sizeof(spi_cmd), NULL);
}

__attribute__ ((weak)) void loader_port_debug_print(const char *str)
{
    (void)str;
}
