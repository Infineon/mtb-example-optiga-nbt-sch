// SPDX-FileCopyrightText: 2024 Infineon Technologies AG
// SPDX-License-Identifier: MIT

/**
 * \file bluetooth-handling.h
 * \brief Bluetooth Low Energy (BLE) and Generic Attribute Profile (GATT) handler.
 */
#ifndef BLUETOOTH_HANDLING_H
#define BLUETOOTH_HANDLING_H

#include <stdint.h>

#include "mtb_kvstore.h"
#include "wiced_bt_stack.h"

#include "infineon/ifx-error.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Sends BLE GATT notification to connection device to mute/unmute.
 */
void ble_gatt_send_hid_update(void);

/**
 * \brief Clear bonding information to reset device.
 * \details This function is used by the button handler to clear bonding information on long-click.
 */
void ble_clear_bonding_info(void);

/**
 * \brief Callback for all Bluetooth (Low Energy) events.
 * \details Can be used via wiced_bt_stack_init() to start the BLE stack.
 * \param[in] event BLE event for internal state machine.
 * \param[in,out] event_data Additional input/output buffer for event data specific to `event`.
 * \returns WICED_BT_SUCCESS if successful, any other value in case of error.
 */
wiced_result_t ble_callback(wiced_bt_management_evt_t event, wiced_bt_management_evt_data_t *event_data);

/**
 * \brief Callback triggered once BLE MAC address is available / changed.
 * \details This callback is used to update the NBT NDEF file to set the MAC address for the NFC connection handover.
 * \param[in] mac BLE MAC address to write to connection handover record.
 * \return ifx_status_t \c IFX_SUCCESS if successful, any other value in case of error.
 */
ifx_status_t callback_mac_address_changed(wiced_bt_device_address_t mac);

/**
 * \brief Callback triggered once LE Secure Connection Confirmation Value is available / changed.
 * \details This callback is used to update the NBT NDEF file to set the SC confirmation value for the NFC connection handover.
 * \param[in] confirmation LE Secure Connection Confirmation Value to write to connection handover record.
 * \return ifx_status_t \c IFX_SUCCESS if successful, any other value in case of error.
 */
ifx_status_t callback_sc_confirmation_value_changed(uint8_t confirmation[0x10U]);

/**
 * \brief Callback triggered once LE Secure Connection Confirmation Value is available / changed.
 * \details This callback is used to update the NBT NDEF file to set the SC confirmation value for the NFC connection handover.
 * \param[in] random LE Secure Connection Random Value to write to connection handover record.
 * \return ifx_status_t \c IFX_SUCCESS if successful, any other value in case of error.
 */
ifx_status_t callback_sc_random_value_changed(uint8_t random[0x10U]);

#ifdef __cplusplus
}
#endif

#endif // BLUETOOTH_HANDLING_H
