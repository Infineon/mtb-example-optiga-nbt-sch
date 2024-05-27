// SPDX-FileCopyrightText: 2024 Infineon Technologies AG
// SPDX-License-Identifier: MIT

/**
 * \file data-storage.h
 * \brief General utility for a key value data storage.
 * \details Used to store credentials persistently.
 */
#ifndef DATA_STORAGE_H
#define DATA_STORAGE_H

#include "cyhal.h"
#include "mtb_kvstore.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Global data storage (e.g. for persistent credentials cache).
 * \details Initialized using data_storage_initialize, therefore this function must be called beforehand.
 */
extern mtb_kvstore_t data_storage;

/**
 * \brief Initializes and configures global data_storage.
 * \returns cy_rslt_t CR_RSLT_SUCCESS if successful, any other value in case of error.
 */
cy_rslt_t data_storage_initialize();

#ifdef __cplusplus
}
#endif

#endif // DATA_STORAGE_H
