// SPDX-FileCopyrightText: 2024 Infineon Technologies AG
// SPDX-License-Identifier: MIT

/**
 * \file mbedtls-config.h
 * \brief Configuration to enable (hardware-accelerated) mbedTLS features.
 */

// AES-CMAC functionality required for OOB connection handover
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CMAC_C
#define MBEDTLS_AES_ALT
