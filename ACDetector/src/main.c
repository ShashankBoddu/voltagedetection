#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/sys/printk.h>

#include "adc_lib/adc_backend.h"
#include "ble_lib/ble_backend.h"
#include "range_lib/range_backend.h"
#include "storage_lib/storage_backend.h"

/* Define threads here to ensure they are created at boot */
K_THREAD_DEFINE(ble_thread_id, 1024, ble_tx_thread_fn, NULL, NULL, NULL, 9, 0,
                0);
K_THREAD_DEFINE(adc_thread, 1024, adc_thread_fn, NULL, NULL, NULL, 8, 0, 0);

K_SEM_DEFINE(boot_done_sem, 0, 10);

int main(void) {
  printk("\n\t\t>> ACDetector V1.0.3 <<\n\n");

  /* Initialize Peripherals */
  storage_init(&g_thresholds);
  adc_param_init();
  ble_param_init();
  range_param_init();

  /* Confirm the image to prevent MCUboot from reverting */
  int err = boot_write_img_confirmed();
  if (err) {
    printk("Failed to confirm image: %d\n", err);
  } else {
    printk("Image successfully confirmed.\n");
  }

  /* Boot Buzzer Notification (0.5s) */
  buzzer_set(true);
  k_sleep(K_MSEC(500));
  buzzer_set(false);

  printk("ACDetector Initialized. Releasing threads.\n");
  k_sem_give(&boot_done_sem); // Wakes ADC Thread
  k_sem_give(&boot_done_sem); // Wakes BLE Thread

  /* Main loop */
  while (1) {
    k_sleep(K_MSEC(1000));
  }
  return 0;
}
