#include "storage_backend.h"
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#define STORAGE_KEY "app/thresholds"

static thresholds_t *loaded_thresholds;

// Default values for a single channel (Calibrated: 35mV BLC, 25mV ALC)
static const channel_thresholds_t default_channel = {.blc_mean_min = 1100,
                                                     .blc_mean_max = 2200,
                                                     .blc_rms_min = 35,
                                                     .blc_rms_max = 800,

                                                     .alc_mean_min = 1100,
                                                     .alc_mean_max = 2200,
                                                     .alc_rms_min = 25,
                                                     .alc_rms_max = 500};

static bool g_thresholds_loaded_from_flash = false;

static int threshold_handle_set(const char *name, size_t len,
                                settings_read_cb read_cb, void *cb_arg) {
  const char *next;
  if (settings_name_steq(name, "thresholds", &next) && !next) {
    if (len != sizeof(thresholds_t)) {
      return -EINVAL;
    }
    if (loaded_thresholds) {
      if (read_cb(cb_arg, loaded_thresholds, sizeof(thresholds_t)) < 0) {
        return -EIO;
      }
      g_thresholds_loaded_from_flash = true;
    }
    return 0;
  }
  return -ENOENT;
}

static struct settings_handler my_conf = {.name = "app",
                                          .h_set = threshold_handle_set};

int storage_init(thresholds_t *thresholds) {
  if (!thresholds) {
    return -EINVAL;
  }

  loaded_thresholds = thresholds;

  // Set defaults for all channels
  for (int i = 0; i < 16; i++) {
    thresholds->channels[i] = default_channel;
  }
  // =========================================================================
  // Factory Calibrated Defaults for All 12 Voltage Ranges (IEC 61243-1 / MUX)
  // Derived from: 230V 10-report empirical baseline, Voltrack safe clearance
  // distances, and CD74HC4067 resistor divider schematic network (10MΩ to 10kΩ).
  // =========================================================================

  // Channel 0 (230V Range, 0.05m safe distance): 10 MΩ Shunt (R6)
  thresholds->channels[0].blc_mean_min = 1100;
  thresholds->channels[0].blc_mean_max = 2200;
  thresholds->channels[0].blc_rms_min = 21;
  thresholds->channels[0].blc_rms_max = 800;
  thresholds->channels[0].alc_mean_min = 1100;
  thresholds->channels[0].alc_mean_max = 2200;
  thresholds->channels[0].alc_rms_min = 15;
  thresholds->channels[0].alc_rms_max = 500;

  // Channel 1 (1.1kV Range, 0.10m safe distance): 5.1 MΩ Shunt (R10)
  // Calibrated for Substation Induction (> 500V alarm threshold):
  // 100V (2 earth rods) -> ALC ~36 mV, BLC ~44 mV (Quiet / Safe to Work)
  // 650V (1 earth rod)  -> ALC ~161 mV, BLC ~196 mV (ALARM: Hazardous Induction)
  // 500V Alarm Trigger  -> ALC Min = 125 mV, BLC Min = 150 mV
  thresholds->channels[1].blc_mean_min = 1100;
  thresholds->channels[1].blc_mean_max = 2200;
  thresholds->channels[1].blc_rms_min = 150;
  thresholds->channels[1].blc_rms_max = 800;
  thresholds->channels[1].alc_mean_min = 1100;
  thresholds->channels[1].alc_mean_max = 2200;
  thresholds->channels[1].alc_rms_min = 125;
  thresholds->channels[1].alc_rms_max = 500;

  // Channel 2 (3.3kV Range, 0.15m safe distance): 2.0 MΩ Shunt (R15)
  thresholds->channels[2].blc_mean_min = 1100;
  thresholds->channels[2].blc_mean_max = 2200;
  thresholds->channels[2].blc_rms_min = 30;
  thresholds->channels[2].blc_rms_max = 800;
  thresholds->channels[2].alc_mean_min = 1100;
  thresholds->channels[2].alc_mean_max = 2200;
  thresholds->channels[2].alc_rms_min = 22;
  thresholds->channels[2].alc_rms_max = 500;

  // Channel 3 (6.6kV Range, 0.20m safe distance): 1.5 MΩ Shunt (R19)
  thresholds->channels[3].blc_mean_min = 1100;
  thresholds->channels[3].blc_mean_max = 2200;
  thresholds->channels[3].blc_rms_min = 32;
  thresholds->channels[3].blc_rms_max = 800;
  thresholds->channels[3].alc_mean_min = 1100;
  thresholds->channels[3].alc_mean_max = 2200;
  thresholds->channels[3].alc_rms_min = 24;
  thresholds->channels[3].alc_rms_max = 500;

  // Channel 4 (11kV Range, 0.25m safe distance): 1.0 MΩ Shunt (R23)
  thresholds->channels[4].blc_mean_min = 1100;
  thresholds->channels[4].blc_mean_max = 2200;
  thresholds->channels[4].blc_rms_min = 35;
  thresholds->channels[4].blc_rms_max = 800;
  thresholds->channels[4].alc_mean_min = 1100;
  thresholds->channels[4].alc_mean_max = 2200;
  thresholds->channels[4].alc_rms_min = 25;
  thresholds->channels[4].alc_rms_max = 500;

  // Channel 5 (25kV Range, 0.40m safe distance): 560 kΩ Shunt (R27)
  thresholds->channels[5].blc_mean_min = 1100;
  thresholds->channels[5].blc_mean_max = 2200;
  thresholds->channels[5].blc_rms_min = 35;
  thresholds->channels[5].blc_rms_max = 800;
  thresholds->channels[5].alc_mean_min = 1100;
  thresholds->channels[5].alc_mean_max = 2200;
  thresholds->channels[5].alc_rms_min = 25;
  thresholds->channels[5].alc_rms_max = 500;

  // Channel 6 (33kV Range, 0.50m safe distance): 220 kΩ Shunt (R31)
  // Calibrated from HAL Substation live 33kV testing (ALC measured 24-37 mV)
  thresholds->channels[6].blc_mean_min = 1100;
  thresholds->channels[6].blc_mean_max = 2200;
  thresholds->channels[6].blc_rms_min = 24;
  thresholds->channels[6].blc_rms_max = 800;
  thresholds->channels[6].alc_mean_min = 1100;
  thresholds->channels[6].alc_mean_max = 2200;
  thresholds->channels[6].alc_rms_min = 18;
  thresholds->channels[6].alc_rms_max = 500;

  // Channel 7 (66kV Range, 0.80m safe distance): 120 kΩ Shunt (R35)
  thresholds->channels[7].blc_mean_min = 1100;
  thresholds->channels[7].blc_mean_max = 2200;
  thresholds->channels[7].blc_rms_min = 35;
  thresholds->channels[7].blc_rms_max = 800;
  thresholds->channels[7].alc_mean_min = 1100;
  thresholds->channels[7].alc_mean_max = 2200;
  thresholds->channels[7].alc_rms_min = 25;
  thresholds->channels[7].alc_rms_max = 500;

  // Channel 8 (132kV Range, 1.00m safe distance): 94 kΩ Shunt Rework
  thresholds->channels[8].blc_mean_min = 1100;
  thresholds->channels[8].blc_mean_max = 2200;
  thresholds->channels[8].blc_rms_min = 24;
  thresholds->channels[8].blc_rms_max = 800;
  thresholds->channels[8].alc_mean_min = 1100;
  thresholds->channels[8].alc_mean_max = 2200;
  thresholds->channels[8].alc_rms_min = 18;
  thresholds->channels[8].alc_rms_max = 500;

  // Channel 9 (220kV Range, 2.00m safe distance): 67 kΩ Shunt Rework
  thresholds->channels[9].blc_mean_min = 1100;
  thresholds->channels[9].blc_mean_max = 2200;
  thresholds->channels[9].blc_rms_min = 24;
  thresholds->channels[9].blc_rms_max = 800;
  thresholds->channels[9].alc_mean_min = 1100;
  thresholds->channels[9].alc_mean_max = 2200;
  thresholds->channels[9].alc_rms_min = 18;
  thresholds->channels[9].alc_rms_max = 500;

  // Channel 10 (400kV Range, 3.00m safe distance): 51 kΩ Shunt Rework (Fixes 6 mV drop)
  thresholds->channels[10].blc_mean_min = 1100;
  thresholds->channels[10].blc_mean_max = 2200;
  thresholds->channels[10].blc_rms_min = 14;
  thresholds->channels[10].blc_rms_max = 800;
  thresholds->channels[10].alc_mean_min = 1100;
  thresholds->channels[10].alc_mean_max = 2200;
  thresholds->channels[10].alc_rms_min = 11;
  thresholds->channels[10].alc_rms_max = 500;

  // Channel 11 (765kV Range, 5.00m safe distance): 47 kΩ Shunt Rework (Prevents collapse)
  thresholds->channels[11].blc_mean_min = 1100;
  thresholds->channels[11].blc_mean_max = 2200;
  thresholds->channels[11].blc_rms_min = 14;
  thresholds->channels[11].blc_rms_max = 800;
  thresholds->channels[11].alc_mean_min = 1100;
  thresholds->channels[11].alc_mean_max = 2200;
  thresholds->channels[11].alc_rms_min = 11;
  thresholds->channels[11].alc_rms_max = 500;

  int err = settings_subsys_init();
  if (err) {
    printk("Settings subsystem init failed (err %d)\n", err);
    return err;
  }

  err = settings_register(&my_conf);
  if (err) {
    printk("Settings register failed (err %d)\n", err);
    return err;
  }

  err = settings_load_subtree("app");
  if (err) {
    printk("Settings load subtree 'app' failed (err %d)\n", err);
    return err;
  }

  // If flash was empty (uninitialized), commit calibrated defaults to flash
  if (!g_thresholds_loaded_from_flash) {
    printk("Flash uninitialized: saving calibrated defaults into flash...\n");
    storage_save_thresholds(thresholds);
  } else {
    printk("Storage initialized: Custom thresholds loaded from flash.\n");
  }
  return 0;
}

int storage_save_thresholds(const thresholds_t *thresholds) {
  if (!thresholds) {
    return -EINVAL;
  }

  int err = settings_save_one(STORAGE_KEY, thresholds, sizeof(thresholds_t));
  if (err) {
    printk("Settings save failed (err %d)\n", err);
  } else {
    printk("Settings saved successfully\n");
  }
  return err;
}
