#ifndef BLE_BACKEND_H
#define BLE_BACKEND_H

#include "../common.h"
#include <zephyr/kernel.h>

void ble_param_init(void);
void ble_tx_thread_fn(void *arg1, void *arg2, void *arg3);
bool ble_is_connected(void);

#endif
