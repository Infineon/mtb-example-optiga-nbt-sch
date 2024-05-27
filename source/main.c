// SPDX-FileCopyrightText: 2024 Infineon Technologies AG
// SPDX-License-Identifier: MIT

/**
 * \file main.c
 * \brief Main function starting up FreeRTOS for NBT BLE connection handover usecase.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cyhal.h"
#include "cybsp.h"
#include "cy_retarget_io.h"
#include "cybsp_bt_config.h"
#include "cycfg_bt_settings.h"
#include "wiced_bt_stack.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "timers.h"

#include "infineon/ifx-logger.h"
#include "infineon/logger-printf.h"
#include "infineon/logger-cyhal-rtos.h"
#include "infineon/ifx-protocol.h"
#include "infineon/i2c-cyhal.h"
#include "infineon/ifx-t1prime.h"
#include "infineon/nbt-cmd.h"

#include "bluetooth-handling.h"
#include "data-storage.h"
#include "nbt-utilities.h"

/**
 * \brief Skeleton for BLE connection handover message.
 * \details Populated according to *NFC Forum: Bluetooth Secure Simple Pairing Using NFC* application document.
 * \details Uses simplified tag format with fields for:
 *     * BLE Device Address (required, updated via callback_mac_address_changed())
 *     * BLE Role (required)
 *     * Security Manager TK (optional but required by AOSP based Bluetooth stacks - still ignored)
 *     * LE Secure Connection Confirmation Value (optional but required by AOSP based Bluetooth stacks - updated via callback_sc_confirmation_value_changed())
 *     * LE Secure Connection Random Value (optional but required by AOSP based Bluetooth stacks - updated via callback_sc_random_value_changed())
 *     * BLE OOB flags (optional)
 *     * BLE Local Name (optional)
 *     * BLE Appeareance (optional)
 */
// clang-format off
static uint8_t CONNECTION_HANDOVER_MESSAGE[] = {
    // NDEF message length
    0x00U, 0x23U + 0x4EU,
    // NDEF Record Header
    0xD2U,
    // Record Type Length
    0x20U,
    // Payload Length
    0x4EU,
    // Record Type Name: application/vnd.bluetooth.le.oob
    0x61U, 0x70U, 0x70U, 0x6CU, 0x69U, 0x63U, 0x61U, 0x74U, 0x69U, 0x6FU, 0x6EU, 0x2FU, 0x76U, 0x6EU, 0x64U, 0x2EU,
    0x62U, 0x6CU, 0x75U, 0x65U, 0x74U, 0x6FU, 0x6FU, 0x74U, 0x68U, 0x2EU, 0x6CU, 0x65U, 0x2EU, 0x6FU, 0x6FU, 0x62U,
    // Payload
    // BLE Device Address (1B length, 1B data type, 6B address, 1B address type)
    0x08U, 0x1BU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0XFFU, 0x00U,
    // BLE Role (1B length, 1B data type, 1B role "Peripheral")
    0x02U, 0x1CU, 0x00U,
    // BLE Local Name (1B length, 1B data type, 3B name "NBT")
    0x04U, 0x09U, 0x4EU, 0x42U, 0x54U,
    // Appearance (1B length, 1B data type, 2B appearance "HID: Mouse")
    0x03U, 0x19U, 0xC2U, 0x03U,
    // Security Manager TK (1B length, 1B data type, 16B key)
    0x11U, 0x10U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    // LE Secure Connection Confirmation Value (1B length, 1B data type, 16B confirmation value)
    0x11U, 0x22U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    // LE Secure Connection Random Value (1B length, 1B data type, 16B random value)
    0x11U, 0x23U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    // LE OOB Flags (1B length, 1B data type, 1B flags LE General Discoverable Mode, BR/EDR not supported
    0x02U, 0x01U, 0x06U
};
// clang-format on

/**
 * \brief Offset of MAC address in CONNECTION_HANDOVER_MESSAGE.
 */
#define CONNECTION_HANDOVER_MESSAGE_MAC_OFFSET 39U

/**
 * \brief Offset of LE Secure Connection Confirmation value in CONNECTION_HANDOVER_MESSAGE.
 */
#define CONNECTION_HANDOVER_MESSAGE_CONFIRMATION_OFFSET 78U

/**
 * \brief Offset of LE Secure Connection Random value in CONNECTION_HANDOVER_MESSAGE.
 */
#define CONNECTION_HANDOVER_MESSAGE_RANDOM_OFFSET 96U

/**
 * \brief String used as source information for logging.
 */
#define LOG_TAG "NBT example"

/**
 * \brief NBT framework logger.
 */
static ifx_logger_t logger_implementation;

/**
 * \brief ModusToolbox CYHAL I2C driver for communication with NBT.
 */
static cyhal_i2c_t i2c_device;

/**
 * \brief Adapter between ModusToolbox CYHAL I2C driver and NBT library framework.
 */
static ifx_protocol_t driver_adapter;

/**
 * \brief Communication protocol stack for NBT library framework.
 */
static ifx_protocol_t communication_protocol;

/**
 * \brief NBT abstraction.
 */
static nbt_cmd_t nbt;

/**
 * \brief FreeRTOS mutex waiting for user button presses.
 */
static SemaphoreHandle_t btn_irq_sleeper;

/**
 * \brief Period length for time_keeper.
 * \details This is basically how often the timer should wake up and increment elapsed_periods.
 */
#define PERIOD_LENGTH_MS 100U

/**
 * \brief Simple counter value of periods elapsed since application start.
 * \details Period length defined by PERIOD_LENGTH_MS.
 */
static uint32_t elapsed_periods = 0U;

/**
 * \brief FreeRTOS timer periodically waking up and incrementing elapsed_periods.
 */
static TimerHandle_t time_keeper;

/**
 * \brief Simple callback function periodically triggered by time_keeper and incrementing elapsed_periods.
 */
static void period_elapsed()
{
    elapsed_periods++;
}

/**
 * \brief Interrupt handler for user button.
 * \details Updates btn_irq_sleeper that will in turn wake up btn_task().
 * \param[in] handler_arg ignored.
 * \param[in] event ignored.
 */
static void btn_irq(void *handler_arg, cyhal_gpio_event_t event)
{
    (void) handler_arg;
    (void) event;

    static BaseType_t xHigherPriorityTaskWoken;
    xSemaphoreGiveFromISR(btn_irq_sleeper, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/**
 * \brief Callback data for btn_irq().
 */
static cyhal_gpio_callback_data_t btn_irq_data = {.callback = btn_irq, .callback_arg = NULL};

/**
 * \brief FreeRTOS task waiting for button presses and handling user inputs accordingly.
 * \details Short click sends HID events, long click resets BLE bonding data.
 * \param[in] data Ignored.
 */
static void btn_task(void *data)
{
    (void) data;

    static uint32_t press_start = 0U;

    // Wait for button interrupt
    while (1)
    {
        if (xSemaphoreTake(btn_irq_sleeper, portMAX_DELAY) == pdPASS)
        {
            if (cyhal_gpio_read(CYBSP_USER_BTN) == CYBSP_BTN_PRESSED)
            {
                press_start = elapsed_periods;
            }
            else
            {
                uint32_t press_duration = elapsed_periods - press_start;
                if ((press_duration * PERIOD_LENGTH_MS) > 5000U)
                {
                    ble_clear_bonding_info();
                }
                else
                {
                    ble_gatt_send_hid_update();
                }
            }
        }
    }
}

/**
 * \brief Callback triggered once BLE MAC address is available / changed.
 * \details This callback is used to update the NBT NDEF file to set the MAC address for the NFC connection handover.
 * \param[in] mac BLE MAC address to write to connection handover record.
 * \return ifx_status_t \c IFX_SUCCESS if successful, any other value in case of error.
 */
ifx_status_t callback_mac_address_changed(wiced_bt_device_address_t mac)
{
    for (size_t i = 0U; i < sizeof(wiced_bt_device_address_t); i++)
    {
        CONNECTION_HANDOVER_MESSAGE[CONNECTION_HANDOVER_MESSAGE_MAC_OFFSET + i] = mac[sizeof(wiced_bt_device_address_t) - 1U - i];
    }
    return nbt_write_file(&nbt, NBT_FILEID_NDEF, CONNECTION_HANDOVER_MESSAGE_MAC_OFFSET, CONNECTION_HANDOVER_MESSAGE + CONNECTION_HANDOVER_MESSAGE_MAC_OFFSET,
                          sizeof(wiced_bt_device_address_t));
}

/**
 * \brief Callback triggered once LE Secure Connection Confirmation Value is available / changed.
 * \details This callback is used to update the NBT NDEF file to set the SC confirmation value for the NFC connection handover.
 * \param[in] confirmation LE Secure Connection Confirmation Value to write to connection handover record.
 * \return ifx_status_t \c IFX_SUCCESS if successful, any other value in case of error.
 */
ifx_status_t callback_sc_confirmation_value_changed(uint8_t confirmation[0x10U])
{
    memcpy(CONNECTION_HANDOVER_MESSAGE + CONNECTION_HANDOVER_MESSAGE_CONFIRMATION_OFFSET, confirmation, 0x10U);
    return nbt_write_file(&nbt, NBT_FILEID_NDEF, CONNECTION_HANDOVER_MESSAGE_CONFIRMATION_OFFSET,
                          CONNECTION_HANDOVER_MESSAGE + CONNECTION_HANDOVER_MESSAGE_CONFIRMATION_OFFSET, 0x10U);
}

/**
 * \brief Callback triggered once LE Secure Connection Confirmation Value is available / changed.
 * \details This callback is used to update the NBT NDEF file to set the SC confirmation value for the NFC connection handover.
 * \param[in] random LE Secure Connection Random Value to write to connection handover record.
 * \return ifx_status_t \c IFX_SUCCESS if successful, any other value in case of error.
 */
ifx_status_t callback_sc_random_value_changed(uint8_t random[0x10U])
{
    memcpy(CONNECTION_HANDOVER_MESSAGE + CONNECTION_HANDOVER_MESSAGE_RANDOM_OFFSET, random, 0x10U);
    return nbt_write_file(&nbt, NBT_FILEID_NDEF, CONNECTION_HANDOVER_MESSAGE_RANDOM_OFFSET,
                          CONNECTION_HANDOVER_MESSAGE + CONNECTION_HANDOVER_MESSAGE_RANDOM_OFFSET, 0x10U);
}

/**
 * \brief Configures NBT for BLE connection handover usecase.
 * \details Sets file access policies, configures communication interface and writes connection handover skeleton to NDEF file.
 * \param[in] nbt NBT abstraction for communication.
 * \return ifx_status_t \c IFX_SUCCESS if successful, any other value in case of error.
 */
static ifx_status_t nbt_configure_ble_connection_handover(nbt_cmd_t *nbt)
{
    if (nbt == NULL)
    {
        return IFX_ERROR(LIB_NBT_APDU, NBT_SET_CONFIGURATION, IFX_ILLEGAL_ARGUMENT);
    }
    const nbt_file_access_policy_t fap_cc = {.file_id = NBT_FILEID_CC,
                                             .i2c_read_access_condition = NBT_ACCESS_ALWAYS,
                                             .i2c_write_access_condition = NBT_ACCESS_NEVER,
                                             .nfc_read_access_condition = NBT_ACCESS_ALWAYS,
                                             .nfc_write_access_condition = NBT_ACCESS_NEVER};
    const nbt_file_access_policy_t fap_ndef = {.file_id = NBT_FILEID_NDEF,
                                               .i2c_read_access_condition = NBT_ACCESS_ALWAYS,
                                               .i2c_write_access_condition = NBT_ACCESS_ALWAYS,
                                               .nfc_read_access_condition = NBT_ACCESS_ALWAYS,
                                               .nfc_write_access_condition = NBT_ACCESS_NEVER};
    const nbt_file_access_policy_t fap_fap = {.file_id = NBT_FILEID_FAP,
                                              .i2c_read_access_condition = NBT_ACCESS_ALWAYS,
                                              .i2c_write_access_condition = NBT_ACCESS_ALWAYS,
                                              .nfc_read_access_condition = NBT_ACCESS_ALWAYS,
                                              .nfc_write_access_condition = NBT_ACCESS_ALWAYS};
    const nbt_file_access_policy_t fap_proprietary1 = {.file_id = NBT_FILEID_PROPRIETARY1,
                                                       .i2c_read_access_condition = NBT_ACCESS_NEVER,
                                                       .i2c_write_access_condition = NBT_ACCESS_NEVER,
                                                       .nfc_read_access_condition = NBT_ACCESS_NEVER,
                                                       .nfc_write_access_condition = NBT_ACCESS_NEVER};
    const nbt_file_access_policy_t fap_proprietary2 = {.file_id = NBT_FILEID_PROPRIETARY2,
                                                       .i2c_read_access_condition = NBT_ACCESS_NEVER,
                                                       .i2c_write_access_condition = NBT_ACCESS_NEVER,
                                                       .nfc_read_access_condition = NBT_ACCESS_NEVER,
                                                       .nfc_write_access_condition = NBT_ACCESS_NEVER};
    const nbt_file_access_policy_t fap_proprietary3 = {.file_id = NBT_FILEID_PROPRIETARY3,
                                                       .i2c_read_access_condition = NBT_ACCESS_NEVER,
                                                       .i2c_write_access_condition = NBT_ACCESS_NEVER,
                                                       .nfc_read_access_condition = NBT_ACCESS_NEVER,
                                                       .nfc_write_access_condition = NBT_ACCESS_NEVER};
    const nbt_file_access_policy_t fap_proprietary4 = {.file_id = NBT_FILEID_PROPRIETARY4,
                                                       .i2c_read_access_condition = NBT_ACCESS_NEVER,
                                                       .i2c_write_access_condition = NBT_ACCESS_NEVER,
                                                       .nfc_read_access_condition = NBT_ACCESS_NEVER,
                                                       .nfc_write_access_condition = NBT_ACCESS_NEVER};
    const nbt_file_access_policy_t *faps[] = {&fap_cc, &fap_ndef, &fap_fap, &fap_proprietary1, &fap_proprietary2, &fap_proprietary3, &fap_proprietary4};
    const struct nbt_configuration configuration = {.fap = (nbt_file_access_policy_t **) faps,
                                                    .fap_len = sizeof(faps) / sizeof(struct nbt_configuration *),
                                                    .communication_interface = NBT_COMM_INTF_NFC_ENABLED_I2C_ENABLED,
                                                    .irq_function = NBT_GPIO_FUNCTION_DISABLED};
    ifx_status_t status = nbt_configure(nbt, &configuration);
    if (ifx_error_check(status))
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Could not confgure NBT for connection handover usecase.");
        return status;
    }

    // Write skeleton message, later updated based on events
    if (ifx_error_check(nbt_select_nbt_application(nbt)))
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Could not re-select NBT application.");
        return status;
    }
    return nbt_write_file(nbt, NBT_FILEID_NDEF, 0x00, CONNECTION_HANDOVER_MESSAGE, sizeof(CONNECTION_HANDOVER_MESSAGE));
}

/**
 * \brief FreeRTOS task establishing communication channel to NBT and then starting all other tasks.
 * \details NBT should be configured before starting the BLE stack but requires FreeRTOS to be running.
 * \param[in] data Ignored.
 */
static void startup_task(void *arg)
{
    (void) arg;

    // Activate communication channel to NBT
    uint8_t *atpo = NULL;
    size_t atpo_len = 0U;
    ifx_status_t status = ifx_protocol_activate(&communication_protocol, &atpo, &atpo_len);
    if (ifx_error_check(status))
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Could not open communication channel to NBT");
        goto cleanup;
    }
    if (atpo != NULL)
    {
        free(atpo);
        atpo = NULL;
    }

    // Set NBT to BLE connection handover configuration
    status = nbt_configure_ble_connection_handover(&nbt);
    if (ifx_error_check(status))
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Could not set NBT to BLE connection handover configuration");
        goto cleanup;
    }

    // Start global time keeper here
    if (xTimerStart(time_keeper, 0U) != pdPASS)
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Could not start global time keeper");
        goto cleanup;
    }

    // Prepare persistent storage
    if (data_storage_initialize() != CY_RSLT_SUCCESS)
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Could not set up persistent key value storage");
        goto cleanup;
    }

    // Start BLE GATT server
    if (wiced_bt_stack_init(ble_callback, &wiced_bt_cfg_settings) != WICED_BT_SUCCESS)
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_FATAL, "Could not start BLE GATT server");
        goto cleanup;
    }

    vTaskDelete(NULL);
    return;

cleanup:
    cyhal_i2c_free(&i2c_device);
    ifx_protocol_destroy(&communication_protocol);
    nbt_destroy(&nbt);
    vTaskDelete(NULL);
}

/**
 * \brief Main function starting NBT BLE connection handover usecase via FreeRTOS tasks.
 * \details Prepares ModusToolbox and NBT framwework components and starts actual tasks required for usecase.
 */
int main(void)
{
    ///////////////////////////////////////////////////////////////////////////
    // ModusTooblbox start-up boilerplate
    ///////////////////////////////////////////////////////////////////////////
    cy_rslt_t result;
#if defined(CY_DEVICE_SECURE)
    cyhal_wdt_t wdt_obj;
    result = cyhal_wdt_init(&wdt_obj, cyhal_wdt_get_max_timeout_ms());
    CY_ASSERT(CY_RSLT_SUCCESS == result);
    cyhal_wdt_free(&wdt_obj);
#endif
    result = cybsp_init();
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }
    __enable_irq();

    ///////////////////////////////////////////////////////////////////////////
    // ModusTooblbox component configuration
    ///////////////////////////////////////////////////////////////////////////

    // RetargetIO for logging data via serial connection
    result = cy_retarget_io_init(CYBSP_DEBUG_UART_TX, CYBSP_DEBUG_UART_RX, CY_RETARGET_IO_BAUDRATE);
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }
    printf("\x1b[2J\x1b[;H");
    printf("****************** "
           "NBT: Static Connection Handover "
           "****************** \r\n\n");

    // User button to send HID events
    result = cyhal_gpio_init(CYBSP_USER_BTN, CYHAL_GPIO_DIR_INPUT, CYHAL_GPIO_DRIVE_PULLUP, CYBSP_BTN_OFF);
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }
    btn_irq_sleeper = xSemaphoreCreateBinary();
    if (btn_irq_sleeper == NULL)
    {
        CY_ASSERT(0);
    }
    cyhal_gpio_register_callback(CYBSP_USER_BTN, &btn_irq_data);
    cyhal_gpio_enable_event(CYBSP_USER_BTN, CYHAL_GPIO_IRQ_BOTH, configMAX_PRIORITIES - 1U, true);

    // I2C driver for communication with NBT
    cyhal_i2c_cfg_t i2c_cfg = {.is_slave = false, .address = 0x00U, .frequencyhal_hz = 400000U};
    result = cyhal_i2c_init(&i2c_device, CYBSP_I2C_SDA, CYBSP_I2C_SCL, NULL);
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }
    result = cyhal_i2c_configure(&i2c_device, &i2c_cfg);
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    // Utility timer keeping track of time
    time_keeper = xTimerCreate("time keeper", pdMS_TO_TICKS(PERIOD_LENGTH_MS), pdTRUE, NULL, (TimerCallbackFunction_t)period_elapsed);
    if (time_keeper == NULL)
    {
        CY_ASSERT(0);
    }

    // BLE GATT server
    cybt_platform_config_init(&cybsp_bt_platform_cfg);

    ///////////////////////////////////////////////////////////////////////////
    // NBT library configuration
    ///////////////////////////////////////////////////////////////////////////

    // Logging framework
    ifx_status_t status = logger_printf_initialize(&logger_implementation);
    if (ifx_error_check(status))
    {
        CY_ASSERT(0);
    }
    status = ifx_logger_set_level(&logger_implementation, IFX_LOG_DEBUG);
    if (ifx_error_check(status))
    {
        CY_ASSERT(0);
    }
    status = logger_cyhal_rtos_initialize(ifx_logger_default, &logger_implementation);
    if (ifx_error_check(status))
    {
        CY_ASSERT(0);
    }
    status = ifx_logger_set_level(ifx_logger_default, IFX_LOG_DEBUG);
    if (ifx_error_check(status))
    {
        CY_ASSERT(0);
    }
    status = logger_cyhal_rtos_start(ifx_logger_default, NULL);
    if (ifx_error_check(status))
    {
        CY_ASSERT(0);
    }

    // I2C driver adapter
    status = i2c_cyhal_initialize(&driver_adapter, &i2c_device, NBT_DEFAULT_I2C_ADDRESS);
    if (ifx_error_check(status))
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_ERROR, "Could not initialize I2C driver adapter");
        CY_ASSERT(0);
    }

    // Communication protocol (data link layer)
    status = ifx_t1prime_initialize(&communication_protocol, &driver_adapter);
    if (ifx_error_check(status))
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_ERROR, "Could not initialize NBT communication protocol");
        CY_ASSERT(0);
    }
    ifx_protocol_set_logger(&communication_protocol, ifx_logger_default);

    // NBT command abstraction
    status = nbt_initialize(&nbt, &communication_protocol, ifx_logger_default);
    if (ifx_error_check(status))
    {
        ifx_logger_log(ifx_logger_default, LOG_TAG, IFX_LOG_ERROR, "Could not initialize NBT abstraction");
        CY_ASSERT(0);
    }

    ///////////////////////////////////////////////////////////////////////////
    // FreeRTOS start-up
    ///////////////////////////////////////////////////////////////////////////
    xTaskCreate(btn_task, (char *) "Button", 1024U, 0U, configMAX_PRIORITIES - 4U, NULL);
    xTaskCreate(startup_task, (char *) "Start-up", 2048U, 0U, configMAX_PRIORITIES - 1U, NULL);
    vTaskStartScheduler();

    ///////////////////////////////////////////////////////////////////////////
    // Cleanup (should not be reached)
    ///////////////////////////////////////////////////////////////////////////
    vSemaphoreDelete(btn_irq_sleeper);
    cy_retarget_io_deinit();
    wiced_bt_stack_deinit();
    ifx_logger_destroy(&logger_implementation);

    CY_ASSERT(0);
    return -1;
}
