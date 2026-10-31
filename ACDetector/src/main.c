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
K_THREAD_DEFINE(ble_thread_id, 2048, ble_tx_thread_fn, NULL, NULL, NULL, 9, 0,
                0);
K_THREAD_DEFINE(adc_thread, 2048, adc_thread_fn, NULL, NULL, NULL, 8, 0, 0);

K_SEM_DEFINE(boot_done_sem, 0, 10);

int main(void) {
  /* Confirm the image immediately to prevent MCUboot from reverting */
  int err = boot_write_img_confirmed();
  if (err) {
    printk("Failed to confirm image: %d\n", err);
  } else {
    printk("Image successfully confirmed.\n");
  }

  printk("\n\t\t>> ACDetector V1.1.6 <<\n\n");

  /* Initialize Peripherals */
  storage_init(&g_thresholds);
  adc_param_init();
  ble_param_init();
  range_param_init();

  /* Automatic Power-On Kelvin Hardware Electrode Continuity Self-Test (IEC 61243-1) */
  printk("Executing Power-On Kelvin Hardware Self-Test (P0.07 Step Drive)...\n");
  bool kelvin_ok = hardware_kelvin_selftest(NULL);
  if (kelvin_ok) {
    printk("Power-On Self-Test: PASSED (Electrode wire & sensing plate intact).\n");
    /* Comprehensive Boot Annunciation Verification (500 ms Buzzer, Red LED U5, Blue LED U8) */
    annunciation_boot_selftest();
  } else {
    printk("Power-On Self-Test: FAILED (Electrode wire disconnected / broken loop)!\n");
    // Hardware failure latched: Detection state machine will sound STATUS_FAULT continuous alarm
  }

  printk("ACDetector Initialized. Releasing threads.\n");
  k_sem_give(&boot_done_sem); // Wakes ADC Thread
  k_sem_give(&boot_done_sem); // Wakes BLE Thread

  /* Main loop */
  while (1) {
    k_sleep(K_MSEC(1000));
  }
  return 0;
}
