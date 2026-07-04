#include "range_backend.h"
#include <stdlib.h>
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* The devicetree node identifier for the "range_selIC" alias. */
/* Assuming the same DT nodes as the reference project */
#define RANGE_SEL_IC0_NODE DT_NODELABEL(range_selic0)
#define RANGE_SEL_IC1_NODE DT_NODELABEL(range_selic1)
#define RANGE_SEL_IC2_NODE DT_NODELABEL(range_selic2)
#define RANGE_SEL_IC3_NODE DT_NODELABEL(range_selic3)

static const struct gpio_dt_spec s0 =
    GPIO_DT_SPEC_GET(RANGE_SEL_IC0_NODE, gpios);
static const struct gpio_dt_spec s1 =
    GPIO_DT_SPEC_GET(RANGE_SEL_IC1_NODE, gpios);
static const struct gpio_dt_spec s2 =
    GPIO_DT_SPEC_GET(RANGE_SEL_IC2_NODE, gpios);
static const struct gpio_dt_spec s3 =
    GPIO_DT_SPEC_GET(RANGE_SEL_IC3_NODE, gpios);

/* Rotary Switch GPIOs for Manual Mode */
static const struct gpio_dt_spec manual_pins[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual0), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual3), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual4), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual5), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual6), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual7), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual8), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual9), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual10), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(rangemanual11), gpios),
};

static uint8_t current_channel = 0;

void range_param_init(void) {
  if (!gpio_is_ready_dt(&s0) || !gpio_is_ready_dt(&s1) ||
      !gpio_is_ready_dt(&s2) || !gpio_is_ready_dt(&s3)) {
    printk("Error: Range Select GPIOs not ready\n");
    return;
  }

  gpio_pin_configure_dt(&s0, GPIO_OUTPUT_INACTIVE);
  gpio_pin_configure_dt(&s1, GPIO_OUTPUT_INACTIVE);
  gpio_pin_configure_dt(&s2, GPIO_OUTPUT_INACTIVE);
  gpio_pin_configure_dt(&s3, GPIO_OUTPUT_INACTIVE);

  /* Configure Manual Mode Pins with Internal Pull-ups */
  for (int i = 0; i < ARRAY_SIZE(manual_pins); i++) {
    if (gpio_is_ready_dt(&manual_pins[i])) {
      gpio_pin_configure_dt(&manual_pins[i], GPIO_INPUT | GPIO_PULL_UP);
    }
  }

  // Default to channel 0
  range_set(0);
}

void range_set(uint8_t channel) {
  if (channel > 15)
    return;

  gpio_pin_set_dt(&s0, (channel & 0x01) ? 1 : 0);
  gpio_pin_set_dt(&s1, (channel & 0x02) ? 1 : 0);
  gpio_pin_set_dt(&s2, (channel & 0x04) ? 1 : 0);
  gpio_pin_set_dt(&s3, (channel & 0x08) ? 1 : 0);

  current_channel = channel;
  printk("Range set to Channel %d [S3:%d S2:%d S1:%d S0:%d]\n", channel,
         (channel & 0x08) ? 1 : 0, (channel & 0x04) ? 1 : 0,
         (channel & 0x02) ? 1 : 0, (channel & 0x01) ? 1 : 0);
}

uint8_t range_get(void) { return current_channel; }

void range_process_cmd(const char *cmd, uint16_t len) {
  // CMD Format: "S<VoltageString>"

  if (len < 2 || cmd[0] != 'S')
    return;

  const char *voltage = cmd + 1;
  uint16_t v_len = len - 1;

// Helper macro to check string match safely
#define MATCH(str)                                                             \
  (v_len >= strlen(str) && strncmp(voltage, str, strlen(str)) == 0)

  if (MATCH("230V") || MATCH("240V")) {
    range_set(0);
  } else if (MATCH("1.1kV")) {
    range_set(1);
  } else if (MATCH("3.3kV")) {
    range_set(2);
  } else if (MATCH("6.6kV")) {
    range_set(3);
  } else if (MATCH("11kV")) {
    range_set(4);
  } else if (MATCH("25kV")) {
    range_set(5);
  } else if (MATCH("33kV")) {
    range_set(6);
  } else if (MATCH("66kV")) {
    range_set(7);
  } else if (MATCH("132kV")) {
    range_set(8);
  } else if (MATCH("220kV")) {
    range_set(9);
  } else if (MATCH("400kV")) {
    range_set(10);
  } else if (MATCH("765kV")) {
    range_set(11);
  } else if (voltage[0] == '#') {
    int ch = atoi(voltage + 1);
    range_set((uint8_t)ch);
  } else {
    printk("Unknown Range Command: %s\n", cmd);
  }

#undef MATCH
}

void range_update_manual(void) {
  for (uint8_t i = 0; i < ARRAY_SIZE(manual_pins); i++) {
    /* Rotary switch connects pin to GND (Active Low) */
    if (gpio_pin_get_dt(&manual_pins[i]) == 0) {
      if (current_channel != i) {
        range_set(i);
      }
      break; // Stop at the first active channel found
    }
  }
}
