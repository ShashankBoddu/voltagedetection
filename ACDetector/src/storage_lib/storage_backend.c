#include "storage_backend.h"
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#define STORAGE_KEY "app/thresholds"

static thresholds_t *loaded_thresholds;

// Default values for a single channel
static const channel_thresholds_t default_channel = {.blc_mean_min = 100,
                                                     .blc_mean_max = 2000,
                                                     .blc_rms_min = 0,
                                                     .blc_rms_max = 500,

                                                     .alc_mean_min = 100,
                                                     .alc_mean_max = 2000,
                                                     .alc_rms_min = 45,
                                                     .alc_rms_max = 3000};

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

  // Set defaults first
  for (int i = 0; i < 16; i++) {
    thresholds->channels[i] = default_channel;
  }

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

  printk("Storage initialized. Thresholds loaded.\n");
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
