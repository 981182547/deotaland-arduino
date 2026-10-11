#pragma once

#include "vendor/nimble-arduino/DeotalandNimblePlatform.h"

#if DEOTALAND_NIMBLE_ACTIVE
#include "vendor/nimble-arduino/nimconfig.h"

#ifdef __cplusplus
extern "C" {
#endif
#include "vendor/nimble-arduino/nimble/porting/nimble/include/nimble/nimble_port.h"
#include "vendor/nimble-arduino/nimble/porting/npl/freertos/include/nimble/nimble_port_freertos.h"
#include "vendor/nimble-arduino/nimble/esp_port/esp-hci/include/esp_nimble_hci.h"
#include "vendor/nimble-arduino/nimble/nimble/host/include/host/ble_hs.h"
#include "vendor/nimble-arduino/nimble/nimble/host/include/host/ble_gatt.h"
#include "vendor/nimble-arduino/nimble/nimble/host/include/host/ble_att.h"
#include "vendor/nimble-arduino/nimble/nimble/host/include/host/ble_l2cap.h"
#include "vendor/nimble-arduino/nimble/nimble/host/include/host/ble_sm.h"
#include "vendor/nimble-arduino/nimble/nimble/host/include/host/ble_store.h"
#include "vendor/nimble-arduino/nimble/nimble/host/util/include/host/util/util.h"
#include "vendor/nimble-arduino/nimble/nimble/host/services/gap/include/services/gap/ble_svc_gap.h"
#include "vendor/nimble-arduino/nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h"
#include "vendor/nimble-arduino/nimble/porting/nimble/include/os/os_mbuf.h"
void ble_store_config_init(void);
#ifdef __cplusplus
}
#endif

#if CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM != 2 || MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM) != 2
#error "Deotaland bundled NimBLE must compile with two CoC channels"
#endif
#endif // DEOTALAND_NIMBLE_ACTIVE
