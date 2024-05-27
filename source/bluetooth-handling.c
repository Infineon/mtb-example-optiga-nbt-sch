// SPDX-FileCopyrightText: 2024 Infineon Technologies AG
// SPDX-License-Identifier: MIT

/**
 * \file bluetooth-handling.c
 * \brief Bluetooth Low Energy (BLE) and Generic Attribute Profile (GATT) handler.
 * \details Most callbacks are not of interest for the NBT BLE connection handover usecase.
 * \details All NBT specifics are handled via callbacks defined in *nbt-usecase-ch.h*.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cyhal.h"
#include "cybsp_bt_config.h"
#include "cycfg_bt_settings.h"
#include "cycfg_gap.h"
#include "cycfg_gatt_db.h"
#include "wiced_bt_stack.h"
#include "wiced_bt_gatt.h"
#include "wiced_bt_ble.h"

#include "FreeRTOS.h"
#include "task.h"

#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"

#include "infineon/ifx-logger.h"

#include "data-storage.h"
#include "bluetooth-handling.h"

/**
 * \brief String used as source information for logging.
 */
#define LOG_TAG "NBT example"

/**
 * \brief Bonding information being kept both in RAM as well as persistent storage.
 */
struct
{
    /**
     * \brief Device link keys of bonded device.
     */
    wiced_bt_device_link_keys_t device_link_keys;

    /**
     * \brief Simple flag if a device is currently bonded.
     */
    bool bonded;
} bonding_info;

/**
 * \brief Current value of *Client Characteristic Configuration Descriptor*.
 * \details Kept both in RAM as well as persistent storage to have same CCCD value after reboot.
 */
static uint16_t cccd;

/**
 * \brief Current WICED BLE connection ID in use.
 */
static uint16_t connection_id = 0x0000U;

/**
 * \brief Local identity keys currently in use.
 * \details Kept both in RAM as well as persistent storage to have same keys available after reboot.
 */
static wiced_bt_local_identity_keys_t local_identity_keys;

/**
 * \brief Utility performing lookup from BLE GATT attribute handle to actual gatt_db_lookup_table_t object.
 * \param[in] handle GATT attribute handle to get attribute object for.
 * \returns gatt_db_lookup_table_t object matching handle or `NULL` if not found.
 */
static gatt_db_lookup_table_t *handle2attr(uint16_t handle)
{
    for (uint16_t i = 0U; i < app_gatt_db_ext_attr_tbl_size; i++)
    {
        if (handle == app_gatt_db_ext_attr_tbl[i].handle)
        {
            return &app_gatt_db_ext_attr_tbl[i];
        }
    }
    return NULL;
}

/**
 * \brief Sends BLE GATT notification to connection device to mute/unmute.
 */
void ble_gatt_send_hid_update(void)
{
    if ((app_hids_report_client_char_config[0] & GATT_CLIENT_CONFIG_NOTIFICATION) != 0)
    {
        app_hids_report[0] = 0x01U;
        wiced_bt_gatt_server_send_notification(connection_id, HDLC_HIDS_REPORT_VALUE, app_hids_report_len, app_hids_report, NULL);
        vTaskDelay(pdMS_TO_TICKS(30U));
        app_hids_report[0] = 0x00U;
        wiced_bt_gatt_server_send_notification(connection_id, HDLC_HIDS_REPORT_VALUE, app_hids_report_len, app_hids_report, NULL);
    }
}

/**
 * \brief Clear bonding information to reset device.
 * \details This function is used by the button handler to clear bonding information on long-click.
 */
void ble_clear_bonding_info(void)
{
    if (wiced_bt_start_advertisements(BTM_BLE_ADVERT_OFF, 0U, NULL) != WICED_BT_SUCCESS)
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_ERROR, "Could not stop Bluetooth advertisement");
        return;
    }
    if (bonding_info.bonded)
    {
        if (wiced_bt_dev_delete_bonded_device(bonding_info.device_link_keys.bd_addr) != WICED_BT_SUCCESS)
        {
            ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_WARN, "Could not clear bond data for Bluetooth stack");
        }
        memset(&bonding_info.device_link_keys, 0x00, sizeof(wiced_bt_device_link_keys_t));
        bonding_info.bonded = false;
        if (mtb_kvstore_write(&data_storage, "bonding", (uint8_t *) (&bonding_info), sizeof(bonding_info)) != CY_RSLT_SUCCESS)
        {
            ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_WARN, "Could not clear bond data for Bluetooth stack in persistent storage");
        }
    }
    if (wiced_bt_ble_address_resolution_list_clear_and_disable() != WICED_BT_SUCCESS)
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_WARN, "Could clear local Bluetooth resolution list");
    }
    if (wiced_bt_start_advertisements(BTM_BLE_ADVERT_UNDIRECTED_HIGH, BLE_ADDR_PUBLIC, NULL) != WICED_BT_SUCCESS)
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_WARN, "Could re-enable Bluetooth advertisement");
    }
}

/**
 * \brief Callback for all BLE GATT events.
 * \details No specifics for NBT connection handover usecase, can just be used as is.
 * \param[in] event BLE GATT event for internal state machine.
 * \param[in,out] event_data Additional input/output buffer for event data specific to `event`.
 * \returns WICED_BT_GATT_SUCCESS if successful, any other value in case of error.
 */
static wiced_bt_gatt_status_t gatt_callback(wiced_bt_gatt_evt_t event, wiced_bt_gatt_event_data_t *event_data)
{
    switch (event)
    {
    case GATT_CONNECTION_STATUS_EVT: {
        if (event_data->connection_status.connected)
        {
            connection_id = event_data->connection_status.conn_id;
        }
        else
        {
            connection_id = 0x0000U;
            return wiced_bt_start_advertisements(BTM_BLE_ADVERT_UNDIRECTED_HIGH, BLE_ADDR_PUBLIC, NULL);
        }
        return WICED_BT_GATT_SUCCESS;
    }

    case GATT_ATTRIBUTE_REQUEST_EVT: {
        switch (event_data->attribute_request.opcode)
        {
        case GATT_REQ_READ:
        case GATT_REQ_READ_BLOB: {
            gatt_db_lookup_table_t *attribute = handle2attr(event_data->attribute_request.data.read_req.handle);
            if (attribute == NULL)
            {
                wiced_bt_gatt_server_send_error_rsp(event_data->attribute_request.conn_id, event_data->attribute_request.opcode,
                                                    event_data->attribute_request.data.read_req.handle, WICED_BT_GATT_INVALID_HANDLE);
                return WICED_BT_GATT_INVALID_HANDLE;
            }

            if (event_data->attribute_request.data.read_req.offset >= attribute->cur_len)
            {
                wiced_bt_gatt_server_send_error_rsp(event_data->attribute_request.conn_id, event_data->attribute_request.opcode,
                                                    event_data->attribute_request.data.read_req.handle, WICED_BT_GATT_INVALID_OFFSET);
                return WICED_BT_GATT_INVALID_OFFSET;
            }

            uint16_t len = MIN(event_data->attribute_request.len_requested, attribute->cur_len - event_data->attribute_request.data.read_req.offset);
            uint8_t *response = ((uint8_t *) attribute->p_data) + event_data->attribute_request.data.read_req.offset;
            return wiced_bt_gatt_server_send_read_handle_rsp(event_data->attribute_request.conn_id, event_data->attribute_request.opcode, len, response, NULL);
        }

        case GATT_REQ_WRITE:
        case GATT_CMD_WRITE: {
            gatt_db_lookup_table_t *attribute = handle2attr(event_data->attribute_request.data.write_req.handle);
            if (attribute == NULL)
            {
                wiced_bt_gatt_server_send_error_rsp(event_data->attribute_request.conn_id, event_data->attribute_request.opcode,
                                                    event_data->attribute_request.data.write_req.handle, WICED_BT_GATT_INVALID_HANDLE);
                return WICED_BT_GATT_INVALID_HANDLE;
            }
            if (event_data->attribute_request.data.write_req.val_len > attribute->max_len)
            {
                return WICED_BT_GATT_INVALID_ATTR_LEN;
            }
            attribute->cur_len = event_data->attribute_request.data.write_req.val_len;
            memcpy(attribute->p_data, event_data->attribute_request.data.write_req.p_val, event_data->attribute_request.data.write_req.val_len);
            if ((attribute->handle == HDLD_HIDS_REPORT_CLIENT_CHAR_CONFIG) && (attribute->cur_len >= 2U))
            {
                cccd = (attribute->p_data[1] << 8) | attribute->p_data[0];
                if (mtb_kvstore_write(&data_storage, "cccd", (uint8_t *) (&cccd), sizeof(cccd)) != CY_RSLT_SUCCESS)
                {
                    ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_WARN, "Could not update CCCD value in persistent storage - ignored");
                }
            }

            return wiced_bt_gatt_server_send_write_rsp(event_data->attribute_request.conn_id, event_data->attribute_request.opcode,
                                                       event_data->attribute_request.data.write_req.handle);
        }

        case GATT_REQ_READ_BY_TYPE: {
            uint8_t *response = pvPortMalloc(event_data->attribute_request.len_requested);
            if (response == NULL)
            {
                return WICED_BT_GATT_INSUF_RESOURCE;
            }
            uint16_t data_length = 0U;
            uint8_t type_length = 0U;
            while (1)
            {
                static uint16_t attribute_handle = 0U;
                attribute_handle = wiced_bt_gatt_find_handle_by_type(attribute_handle, event_data->attribute_request.data.read_by_type.e_handle,
                                                                     &event_data->attribute_request.data.read_by_type.uuid);
                if (attribute_handle == 0U)
                {
                    break;
                }
                gatt_db_lookup_table_t *attribute = handle2attr(attribute_handle);
                if (attribute == NULL)
                {
                    vPortFree(response);
                    return WICED_BT_GATT_INVALID_HANDLE;
                }
                int update_length =
                    wiced_bt_gatt_put_read_by_type_rsp_in_stream(response + data_length, event_data->attribute_request.len_requested - data_length,
                                                                 &type_length, attribute_handle, attribute->cur_len, attribute->p_data);
                if ((update_length == 0) || ((update_length + data_length) > 0xffffU))
                {
                    break;
                }
                data_length += update_length;
                attribute_handle++;
            }
            if (data_length == 0)
            {
                vPortFree(response);
                return WICED_BT_GATT_INVALID_HANDLE;
            }
            return wiced_bt_gatt_server_send_read_by_type_rsp(event_data->attribute_request.conn_id, event_data->attribute_request.opcode, type_length,
                                                              data_length, response, (void *) vPortFree);
        }

        case GATT_REQ_MTU: {
            return wiced_bt_gatt_server_send_mtu_rsp(event_data->attribute_request.conn_id, event_data->attribute_request.data.remote_mtu, CY_BT_MTU_SIZE);
        }

        case GATT_HANDLE_VALUE_NOTIF:
        case GATT_HANDLE_VALUE_CONF: {
            return WICED_BT_GATT_SUCCESS;
        }

        default: {
            return WICED_BT_GATT_ERROR;
        }
        }
    }

    case GATT_GET_RESPONSE_BUFFER_EVT: {
        event_data->buffer_request.buffer.p_app_rsp_buffer = pvPortMalloc(event_data->buffer_request.len_requested);
        event_data->buffer_request.buffer.p_app_ctxt = (void *) vPortFree;
        return WICED_BT_GATT_SUCCESS;
    }

    case GATT_APP_BUFFER_TRANSMITTED_EVT: {
        void (*freer)(void *) = (void (*)(void *)) event_data->buffer_xmitted.p_app_ctxt;
        if (freer != NULL)
        {
            freer(event_data->buffer_xmitted.p_app_data);
        }
        return WICED_BT_GATT_SUCCESS;
    }

    default: {
        return WICED_BT_GATT_SUCCESS;
    }
    }
}

/**
 * \brief Callback for all Bluetooth (Low Energy) events.
 * \details Events of interest for the NBT connection handover usecase are:
 *     * BTM_ENABLED_EVT: Update MAC address and start generating OOB data.
 *     * BTM_SMP_SC_LOCAL_OOB_DATA_NOTIFICATION_EVT: Generate OOB data and write to NBT.
 * \details GATT events are handled by gatt_callback().
 * \param[in] event BLE event for internal state machine.
 * \param[in,out] event_data Additional input/output buffer for event data specific to `event`.
 * \returns WICED_BT_SUCCESS if successful, any other value in case of error.
 */
wiced_result_t ble_callback(wiced_bt_management_evt_t event, wiced_bt_management_evt_data_t *event_data)
{
    switch (event)
    {
    case BTM_ENABLED_EVT: {
        if (event_data->enabled.status != WICED_BT_SUCCESS)
        {
            return WICED_BT_ERROR;
        }

        // NBT: Update BLE MAC with unique device ID
        wiced_bt_device_address_t mac_address;
        memcpy(mac_address, cy_bt_device_address, sizeof(wiced_bt_device_address_t));
        uint64_t id = Cy_SysLib_GetUniqueId();
        mac_address[3] = (id >> CY_UNIQUE_ID_DIE_WAFER_Pos) & 0xFFU;
        mac_address[4] = (id >> CY_UNIQUE_ID_DIE_X_Pos) & 0xFFU;
        mac_address[5] = (id >> CY_UNIQUE_ID_DIE_Y_Pos) & 0xFFU;
        if (wiced_bt_set_local_bdaddr(mac_address, BLE_ADDR_PUBLIC) != WICED_BT_SUCCESS)
        {
            return WICED_BT_ERROR;
        }

        // Write BLE connection record to NBT
        if (ifx_error_check(callback_mac_address_changed(mac_address)))
        {
            ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Could not write BLE device address to NBT");
            return WICED_BT_ERROR;
        }

        // Load previous bonding information
        uint32_t data_storage_read_size = sizeof(bonding_info);
        if (mtb_kvstore_read(&data_storage, "bonding", (uint8_t *) (&bonding_info), &data_storage_read_size) == CY_RSLT_SUCCESS)
        {
            if (bonding_info.bonded)
            {
                wiced_bt_dev_add_device_to_address_resolution_db(&bonding_info.device_link_keys);
                data_storage_read_size = sizeof(cccd);
                mtb_kvstore_read(&data_storage, "cccd", (uint8_t *) (&cccd), &data_storage_read_size);
            }
        }

        // Configure BLE, GAP and GATT server
        wiced_bt_set_pairable_mode(WICED_TRUE, WICED_FALSE);
        if (wiced_bt_ble_set_raw_advertisement_data(CY_BT_ADV_PACKET_DATA_SIZE, cy_bt_adv_packet_data) != WICED_BT_SUCCESS)
        {
            return WICED_BT_ERROR;
        }
        if (wiced_bt_gatt_register(gatt_callback) != WICED_BT_GATT_SUCCESS)
        {
            return WICED_BT_ERROR;
        }
        if (wiced_bt_gatt_db_init(gatt_database, gatt_database_len, NULL) != WICED_BT_SUCCESS)
        {
            return WICED_BT_ERROR;
        }

        // NBT: Generate OOB data for connection handover
        if (wiced_bt_smp_create_local_sc_oob_data(mac_address, BLE_ADDR_PUBLIC) != WICED_TRUE)
        {
            CY_ASSERT(0);
        }

        return wiced_bt_start_advertisements(BTM_BLE_ADVERT_UNDIRECTED_HIGH, BLE_ADDR_PUBLIC, NULL);
    }

    case BTM_PAIRING_IO_CAPABILITIES_BLE_REQUEST_EVT: {
        event_data->pairing_io_capabilities_ble_request.local_io_cap = BTM_IO_CAPABILITIES_NONE;
        event_data->pairing_io_capabilities_ble_request.oob_data = BTM_OOB_NONE;
        event_data->pairing_io_capabilities_ble_request.auth_req = BTM_LE_AUTH_REQ_SC_BOND;
        event_data->pairing_io_capabilities_ble_request.max_key_size = 0x10;
        event_data->pairing_io_capabilities_ble_request.init_keys = BTM_LE_KEY_PENC | BTM_LE_KEY_PID | BTM_LE_KEY_PCSRK | BTM_LE_KEY_LENC;
        event_data->pairing_io_capabilities_ble_request.resp_keys = BTM_LE_KEY_PENC | BTM_LE_KEY_PID | BTM_LE_KEY_PCSRK | BTM_LE_KEY_LENC;
        return WICED_BT_SUCCESS;
    }

    case BTM_PAIRING_COMPLETE_EVT: {
        bonding_info.bonded = true;
        if (mtb_kvstore_write(&data_storage, "bonding", (uint8_t *) (&bonding_info), sizeof(bonding_info)) != CY_RSLT_SUCCESS)
        {
            ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_ERROR, "Could not persistently store bonding information");
            return WICED_BT_ERROR;
        }
        return WICED_BT_SUCCESS;
    }

    case BTM_BLE_ADVERT_STATE_CHANGED_EVT: {
        if (event_data->ble_advert_state_changed == BTM_BLE_ADVERT_OFF)
        {
            if (connection_id == 0x0000U)
            {
                return wiced_bt_start_advertisements(BTM_BLE_ADVERT_UNDIRECTED_HIGH, BLE_ADDR_PUBLIC, NULL);
            }
        }
        return WICED_BT_SUCCESS;
    }

    case BTM_PAIRED_DEVICE_LINK_KEYS_UPDATE_EVT: {
        memcpy(&bonding_info.device_link_keys, &event_data->paired_device_link_keys_update, sizeof(wiced_bt_device_link_keys_t));
        return WICED_BT_SUCCESS;
    }

    case BTM_PAIRED_DEVICE_LINK_KEYS_REQUEST_EVT: {
        if (!bonding_info.bonded)
        {
            return WICED_BT_ERROR;
        }
        memcpy(&event_data->paired_device_link_keys_request, &bonding_info.device_link_keys, sizeof(wiced_bt_device_link_keys_t));
        return WICED_BT_SUCCESS;
    }

    case BTM_LOCAL_IDENTITY_KEYS_UPDATE_EVT: {
        memcpy(&local_identity_keys, &event_data->local_identity_keys_update, sizeof(wiced_bt_local_identity_keys_t));
        if (mtb_kvstore_write(&data_storage, "identity_keys", (uint8_t *) (&local_identity_keys), sizeof(wiced_bt_local_identity_keys_t)) != CY_RSLT_SUCCESS)
        {
            ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_ERROR, "Could not persistently store local identity keys");
            return WICED_BT_ERROR;
        }
        return WICED_BT_SUCCESS;
    }

    case BTM_LOCAL_IDENTITY_KEYS_REQUEST_EVT: {
        uint32_t read_size = sizeof(wiced_bt_local_identity_keys_t);
        if (mtb_kvstore_read(&data_storage, "identity_keys", NULL, &read_size) == CY_RSLT_SUCCESS)
        {
            if (mtb_kvstore_read(&data_storage, "identity_keys", (uint8_t *) (&local_identity_keys), &read_size) == CY_RSLT_SUCCESS)
            {
                return WICED_BT_SUCCESS;
            }
        }
        return WICED_BT_ERROR;
    }

    case BTM_ENCRYPTION_STATUS_EVT: {
        if (bonding_info.bonded &&
            (memcmp(event_data->encryption_status.bd_addr, bonding_info.device_link_keys.bd_addr, sizeof(wiced_bt_device_address_t)) == 0))
        {
            app_hids_report_client_char_config[0] = cccd;
        }
        return WICED_BT_SUCCESS;
    }

    case BTM_SMP_SC_LOCAL_OOB_DATA_NOTIFICATION_EVT: {
        // NBT: Update connection handover message

        // OOB random value typically dynamically generated but here {0x00} based to match wiced BLE stack
        uint8_t random_value[0x10U] = {0x00U};
        if (ifx_error_check(callback_sc_random_value_changed(random_value)))
        {
            ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_ERROR, "Could not update BLE SC random value on NBT");
            return WICED_BT_ERROR;
        }

        // OOB confirmation value is generated using AES-CMAC over public_key.x || public_key.x || 0x00 using random value as key
        uint8_t m[0x20U + 0x20U + 1U];
        memcpy(m, event_data->p_smp_sc_local_oob_data->public_key_used.x, 0x20U);
        memcpy(m + 0x20U, event_data->p_smp_sc_local_oob_data->public_key_used.x, 0x20U);
        mbedtls_cipher_context_t crypto_context;
        mbedtls_cipher_init(&crypto_context);
        if ((mbedtls_cipher_setup(&crypto_context, mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB)) != 0) ||
            (mbedtls_cipher_cmac_starts(&crypto_context, random_value, sizeof(random_value) * 8U) != 0) ||
            (mbedtls_cipher_cmac_update(&crypto_context, m, sizeof(m)) != 0))
        {
            mbedtls_cipher_free(&crypto_context);
            ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_ERROR, "Could not set up required MBED TLS cipher to calculate OOB confirmation value.");
            return WICED_BT_ERROR;
        }
        uint8_t confirmation_value[0x10U];
        int crypto_status = mbedtls_cipher_cmac_finish(&crypto_context, confirmation_value);
        mbedtls_cipher_free(&crypto_context);
        if (crypto_status != 0)
        {
            ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_ERROR, "Could not calculate required OOB confirmation value.");
            return WICED_BT_ERROR;
        }
        if (ifx_error_check(callback_sc_confirmation_value_changed(confirmation_value)))
        {
            ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_ERROR, "Could not update BLE SC confirmation value on NBT");
            return WICED_BT_ERROR;
        }

        return WICED_BT_SUCCESS;
    }

    case BTM_SECURITY_REQUEST_EVT: {
        wiced_bt_ble_security_grant(event_data->security_request.bd_addr, WICED_BT_SUCCESS);
        return WICED_BT_SUCCESS;
    }

    default: {
        return WICED_BT_SUCCESS;
    }
    }
}
