/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Deotaland local modification: transactional VHCI resource initialization.
 * Included only by esp_nimble_hci.c after its private callback definitions.
 */
#pragma once
#include <stdbool.h>

extern void ble_transport_init(void);
extern void ble_transport_deinit(void);
extern esp_err_t ble_buf_alloc(void);
extern void ble_buf_free(void);

esp_err_t esp_nimble_hci_init(void)
{
    esp_err_t ret;
    bool transport_ready = false;
    bool callback_attempted = false;

    if (vhci_send_sem) return ESP_ERR_INVALID_STATE;
    ret = ble_buf_alloc();
    if (ret != ESP_OK) goto err;

    vhci_send_sem = xSemaphoreCreateBinary();
    if (vhci_send_sem == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }
    xSemaphoreGive(vhci_send_sem);
    ble_transport_init();
    transport_ready = true;
#if MYNEWT_VAL(BLE_QUEUE_CONG_CHECK)
    ble_adv_list_init();
#endif

    // Register last: callbacks may execute synchronously during publication.
    callback_attempted = true;
    ret = esp_vhci_host_register_callback(&vhci_host_cb);
    if (ret != ESP_OK) goto err;
    return ESP_OK;

err:
    if (callback_attempted) esp_vhci_host_register_callback(&dummy_vhci_host_cb);
    if (transport_ready) {
#if MYNEWT_VAL(BLE_QUEUE_CONG_CHECK)
        ble_adv_list_deinit();
#endif
        ble_transport_deinit();
    }
    if (vhci_send_sem) {
        vSemaphoreDelete(vhci_send_sem);
        vhci_send_sem = NULL;
    }
    ble_buf_free();
    return ret;
}

esp_err_t esp_nimble_hci_deinit(void)
{
    if (!vhci_send_sem) return ESP_OK;

    // Detach the callbacks while their resources still exist. If the
    // controller refuses, retain those resources and let the caller retry.
    esp_err_t ret = esp_vhci_host_register_callback(&dummy_vhci_host_cb);
    if (ret != ESP_OK) return ret;
    // The API requires a stopped host, so no TX producer still uses this
    // semaphore. After detaching callbacks no future ready event can give it;
    // waiting for a credit here could otherwise block teardown forever.
    vSemaphoreDelete(vhci_send_sem);
    vhci_send_sem = NULL;
#if MYNEWT_VAL(BLE_QUEUE_CONG_CHECK)
    ble_adv_list_deinit();
#endif
    ble_transport_deinit();
    ble_buf_free();
    return ESP_OK;
}
