// Deotaland: compile the bundled host only for the selected ESP32 BLE backend.
#include "vendor/nimble-arduino/DeotalandNimblePlatform.h"
#if DEOTALAND_NIMBLE_ACTIVE
#include "vendor/nimble-arduino/nimconfig.h"
/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <vendor/nimble-arduino/nimble/porting/nimble/include/syscfg/syscfg.h>
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)

#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <vendor/nimble-arduino/nimble/porting/nimble/include/sysinit/sysinit.h>
#include "vendor/nimble-arduino/nimble/nimble/transport/include/nimble/transport.h"
#include "vendor/nimble-arduino/nimble/esp_port/esp-hci/include/esp_nimble_hci.h"

/* This file is only used by ESP32, ESP32C3 and ESP32S3. */
int
ble_transport_to_ll_cmd_impl(void *buf)
{
    return ble_hci_trans_hs_cmd_tx(buf);
}

int
ble_transport_to_ll_acl_impl(struct os_mbuf *om)
{
    return ble_hci_trans_hs_acl_tx(om);
}

void
ble_transport_ll_init(void)
{

}

void
ble_transport_ll_deinit(void)
{

}

#endif /* CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3 */

#endif // DEOTALAND_NIMBLE_ACTIVE
