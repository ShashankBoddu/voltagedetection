/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hc4067.h"
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void) {
  int ret;

  printk("HC4067 Control + UART Example\n");

  ret = hc4067_init();
  if (ret < 0) {
    printk("Failed to initialize HC4067 library (err %d)\n", ret);
    return 0;
  }

  printk("HC4067 Initialized\n");

  while (1) {
    for (uint8_t i = 0; i < 16; i++) {
      hc4067_set_channel(i);
      printk("Channel set to %d\n", i);
      k_msleep(10000);
    }
  }
  return 0;
}
