// SPDX-FileCopyrightText: 2024 Infineon Technologies AG
// SPDX-License-Identifier: MIT

/**
 * \file data-storage.c
 * \brief General utility for a key value data storage.
 * \details Used to store credentials persistently.
 */
#include "cyhal.h"
#include "cyhal_flash.h"
#include "mtb_kvstore.h"

#include "infineon/ifx-error.h"
#include "infineon/ifx-logger.h"

#include "data-storage.h"

/**
 * \brief String used as source information for logging.
 */
#define LOG_TAG "Storage"

/**
 * \brief Handle to PSoC flash for persistent credentials storage.
 */
static cyhal_flash_t flash;

/**
 * \brief Block device mapping flash to key value storage.
 */
static mtb_kvstore_bd_t block_device;

/**
 * \brief Global data storage (e.g. for persistent credentials cache).
 * \details Initialized using data_storage_initialize, therefore this function must be called beforehand.
 */
mtb_kvstore_t data_storage;

/**
 * \brief mtb_kvstore_bd_read_size implementation for block_device.
 */
static uint32_t data_storage_read_size(void *context, uint32_t addr)
{
    (void) context;
    (void) addr;
    return 1U;
}

/**
 * \brief mtb_kvstore_bd_program_size implementation for block_device.
 */
static uint32_t data_storage_program_size(void *context, uint32_t addr)
{
    if (context == NULL)
    {
        return CY_RSLT_TYPE_ERROR;
    }
    cyhal_flash_t *flash = (cyhal_flash_t *) context;
    cyhal_flash_info_t flash_info;
    cyhal_flash_get_info(flash, &flash_info);
    if (flash_info.block_count < 2U)
    {
        return CY_RSLT_TYPE_ERROR;
    }
    return flash_info.blocks[1].page_size;
}

/**
 * \brief mtb_kvstore_bd_erase_size implementation for block_device.
 */
static uint32_t data_storage_erase_size(void *context, uint32_t addr)
{
    if (context == NULL)
    {
        return CY_RSLT_TYPE_ERROR;
    }
    cyhal_flash_t *flash = (cyhal_flash_t *) context;
    cyhal_flash_info_t flash_info;
    cyhal_flash_get_info(flash, &flash_info);
    if (flash_info.block_count < 2U)
    {
        return CY_RSLT_TYPE_ERROR;
    }
    return flash_info.blocks[1].sector_size;
}

/**
 * \brief mtb_kvstore_bd_read implementation for block_device.
 */
static cy_rslt_t data_storage_read(void *context, uint32_t addr, uint32_t length, uint8_t *buf)
{
    if (context == NULL)
    {
        return CY_RSLT_TYPE_ERROR;
    }
    cyhal_flash_t *flash = (cyhal_flash_t *) context;
    return cyhal_flash_read(flash, addr, buf, length);
}

/**
 * \brief mtb_kvstore_bd_program implementation for block_device.
 */
static cy_rslt_t data_storage_program(void *context, uint32_t addr, uint32_t length, const uint8_t *buf)
{
    if (context == NULL)
    {
        return CY_RSLT_TYPE_ERROR;
    }
    cyhal_flash_t *flash = (cyhal_flash_t *) context;
    uint32_t program_size = data_storage_program_size(context, addr);
    if (program_size == 0U)
    {
        return CY_RSLT_TYPE_ERROR;
    }
    cy_rslt_t result = CY_RSLT_SUCCESS;
    for (uint32_t offset = 0U; offset < length; offset += program_size)
    {
        result = cyhal_flash_program(flash, addr + offset, (const uint32_t *) (buf + offset));
        if (result != CY_RSLT_SUCCESS)
        {
            return result;
        }
    }
    return result;
}

/**
 * \brief mtb_kvstore_bd_erase implementation for block_device.
 */
static cy_rslt_t data_storage_erase(void *context, uint32_t addr, uint32_t length)
{
    if (context == NULL)
    {
        return CY_RSLT_TYPE_ERROR;
    }
    cyhal_flash_t *flash = (cyhal_flash_t *) context;
    uint32_t erase_size = data_storage_erase_size(context, addr);
    if (erase_size == 0U)
    {
        return CY_RSLT_TYPE_ERROR;
    }
    cy_rslt_t result = CY_RSLT_SUCCESS;
    for (uint32_t offset = 0U; offset < length; offset += erase_size)
    {
        result = cyhal_flash_erase(flash, addr + offset);
        if (result != CY_RSLT_SUCCESS)
        {
            return result;
        }
    }
    return result;
}

/**
 * \brief Initializes and configures global data_storage.
 * \returns cy_rslt_t CR_RSLT_SUCCESS if successful, any other value in case of error.
 */
cy_rslt_t data_storage_initialize()
{
    // Flash for persistent credential storage
    cy_rslt_t result = cyhal_flash_init(&flash);
    if (result != CY_RSLT_SUCCESS)
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Could not initialize flash storage");
        return result;
    }
    cyhal_flash_info_t flash_info;
    cyhal_flash_get_info(&flash, &flash_info);
    if (flash_info.block_count < 2U)
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Persistent flash storage is too small");
        return CY_RSLT_TYPE_ERROR;
    }

    // KV store for easier access to data
    block_device.read = data_storage_read;
    block_device.program = data_storage_program;
    block_device.erase = data_storage_erase;
    block_device.read_size = data_storage_read_size;
    block_device.program_size = data_storage_program_size;
    block_device.erase_size = data_storage_erase_size;
    block_device.context = &flash;

    size_t block_length = flash_info.blocks[1].page_size * 16U;
    uint32_t start_address = flash_info.blocks[1].start_address + flash_info.blocks[1].size - block_length;
    result = mtb_kvstore_init(&data_storage, start_address, block_length, &block_device);
    if (result != CY_RSLT_SUCCESS)
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Could not set up persistent key value storage");
        return result;
    }

    return CY_RSLT_SUCCESS;
}
