#include "adc_backend.h"
#include "../ble_lib/ble_backend.h"
#include "../range_lib/range_backend.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

/* ================= ADC config Struct ================= */
static struct adc_sequence_options seq_opts_200 = {
    .interval_us = 100,
    .extra_samplings = SAMPLE_COUNT - 1,
};

static struct adc_sequence_options seq_opts_bat = {
    .interval_us = 100,
    .extra_samplings = 15,
};

int16_t adc_blc_buf[SAMPLE_COUNT];
int16_t adc_alc_buf[SAMPLE_COUNT];
static int16_t adc_bat_buf[16];

static struct adc_sequence seq_blc = {
    .options = &seq_opts_200,
    .channels = BIT(7), // P0.31 / AIN7
    .buffer = adc_blc_buf,
    .buffer_size = sizeof(adc_blc_buf),
    .resolution = 12,
};

static struct adc_sequence seq_alc = {
    .options = &seq_opts_200,
    .channels = BIT(4), // P0.28 / AIN4
    .buffer = adc_alc_buf,
    .buffer_size = sizeof(adc_alc_buf),
    .resolution = 12,
};

static struct adc_sequence seq_bat = {
    .options = &seq_opts_bat,
    .channels = BIT(5), // P0.29 / AIN5
    .buffer = adc_bat_buf,
    .buffer_size = sizeof(adc_bat_buf),
    .resolution = 12,
};

/* ================= Data Variables ================= */
static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
static const struct gpio_dt_spec buzzer_spec =
    GPIO_DT_SPEC_GET(DT_NODELABEL(buzzer), gpios);

static const struct gpio_dt_spec DetectionLed_spec =
    GPIO_DT_SPEC_GET(DT_NODELABEL(lowbat), gpios);

// static const struct gpio_dt_spec ble_led_spec =
//     GPIO_DT_SPEC_GET(DT_NODELABEL(blemode), gpios);
data_t g_data;
thresholds_t g_thresholds;

K_MUTEX_DEFINE(data_mutex);

void buzzer_set(bool on) {
  if (gpio_is_ready_dt(&buzzer_spec)) {
    gpio_pin_set_dt(&buzzer_spec, on ? 1 : 0);
  }
  if (gpio_is_ready_dt(&DetectionLed_spec)) {
    gpio_pin_set_dt(&DetectionLed_spec, on ? 1 : 0);
  }
}

/* ================= ADC Functions ================= */

static int32_t calc_battery_mv(int16_t adc_counts) {
  int32_t adc_mv = (adc_counts * ADC_LSB_uV) / 1000;
  // Improved Scaling
  return (adc_mv * BATTERY_SCALE_MUL) / BATTERY_SCALE_DIV;
}

static uint8_t battery_percent_from_mv(int32_t batt_mv) {
  if (batt_mv >= 9400)
    return 100;
  if (batt_mv >= 9200)
    return 90;
  if (batt_mv >= 9000)
    return 80;
  if (batt_mv >= 8800)
    return 65;
  if (batt_mv >= 8600)
    return 50;
  if (batt_mv >= 8400)
    return 35;
  if (batt_mv >= 8200)
    return 20;
  if (batt_mv >= 8000)
    return 10;
  return 0;
}

static void calc_mean_rms_p2p_mV(int16_t *buf, int count, int32_t *mean_mV,
                                 int32_t *rms_mV, int32_t *p2p_mV) {
  int64_t sum = 0;
  int64_t sq = 0;
  int16_t min = buf[0];
  int16_t max = buf[0];
  /* ---------- First pass: mean, min, max ---------- */
  for (int i = 0; i < count; i++) {
    sum += buf[i];
    if (buf[i] < min)
      min = buf[i];
    if (buf[i] > max)
      max = buf[i];
  }

  int32_t mean_counts = sum / count;

  /* ---------- Second pass: AC RMS ---------- */
  for (int i = 0; i < count; i++) {
    int32_t v = buf[i] - mean_counts; // 🔑 DC removed
    sq += (int64_t)v * v;
  }

  int32_t rms_counts = (int32_t)sqrt((double)sq / count);
  int32_t p2p_counts = max - min;

  /* ---------- Convert to mV ---------- */
  *mean_mV = (mean_counts * ADC_LSB_uV) / 1000;
  *rms_mV = (rms_counts * ADC_LSB_uV) / 1000;
  *p2p_mV = (p2p_counts * ADC_LSB_uV) / 1000;
}

/* ---------- 50Hz Fundamental Power Frequency Filter (Goertzel) ---------- */
static int32_t calc_50hz_fundamental_rms_mV(int16_t *buf, int count) {
  // First pass: Calculate mean (DC offset removal)
  int64_t sum = 0;
  for (int i = 0; i < count; i++) {
    sum += buf[i];
  }
  float mean = (float)sum / (float)count;

  // Goertzel algorithm for 50Hz fundamental power signal extraction
  // Fs = 10000 Hz (100us sampling period), N = 200 (k = 1.0)
  float omega50 = (2.0f * 3.14159265f * 1.0f) / (float)count;
  float coeff50 = 2.0f * cosf(omega50);
  float q0_50 = 0.0f, q1_50 = 0.0f, q2_50 = 0.0f;

  for (int i = 0; i < count; i++) {
    float s = (float)buf[i] - mean; // 🔑 DC REMOVED PREVENTING FALSE RMS EXPLOSION
    q0_50 = coeff50 * q1_50 - q2_50 + s;
    q2_50 = q1_50;
    q1_50 = q0_50;
  }

  float real50 = q1_50 - q2_50 * cosf(omega50);
  float imag50 = q2_50 * sinf(omega50);
  float mag50 = sqrtf(real50 * real50 + imag50 * imag50) / ((float)count / 2.0f);
  float rms50 = mag50 / 1.41421356f;

  return (int32_t)((rms50 * ADC_LSB_uV) / 1000.0f);
}

void adc_param_init(void) {
  if (!device_is_ready(adc_dev)) {
    printk("Error: ADC device not ready\n");
    return;
  }
  printk("ADC device ready\n");

  if (gpio_is_ready_dt(&buzzer_spec)) {
    gpio_pin_configure_dt(&buzzer_spec, GPIO_OUTPUT_INACTIVE);
    printk("Buzzer GPIO ready\n");
  } else {
    printk("Error: Buzzer GPIO not ready\n");
  }

  if (gpio_is_ready_dt(&DetectionLed_spec)) {
    gpio_pin_configure_dt(&DetectionLed_spec, GPIO_OUTPUT_INACTIVE);
    printk("Detection LED (LOWBAT) GPIO ready\n");
  } else {
    printk("Error: Detection LED (LOWBAT) GPIO not ready\n");
  }

  // Use the board's DeviceTree to configure the channels correctly,
  // including the critical .input_positive routing which prevents reading
  // garbage.
  static struct adc_channel_cfg cfg_blc =
      ADC_CHANNEL_CFG_DT(DT_NODELABEL(beforelcch));
  adc_channel_setup(adc_dev, &cfg_blc);

  static struct adc_channel_cfg cfg_alc =
      ADC_CHANNEL_CFG_DT(DT_NODELABEL(afterlcch));
  adc_channel_setup(adc_dev, &cfg_alc);

  static struct adc_channel_cfg cfg_bat =
      ADC_CHANNEL_CFG_DT(DT_NODELABEL(batterych));
  adc_channel_setup(adc_dev, &cfg_bat);
}

// Wrapper for channel setup using DT if preferred, but simplified above for
// library portability if DT nodes aren't guaranteed. Actually, to matching
// original code exactly, let's stick to the BIT masks and rely on standard
// setup if we can't see the overlay. Since I don't see app.overlay, I'll use
// the generic setup above which is common for NRF52 SAADC.

extern struct k_sem boot_done_sem;

void adc_thread_fn(void *arg1, void *arg2, void *arg3) {
  k_sem_take(&boot_done_sem, K_FOREVER);

  while (!device_is_ready(adc_dev)) {
    k_sleep(K_MSEC(100));
  }

  static int64_t AVGblc_mean_mv = 0;
  static int64_t AVGblc_rms_mv = 0;
  static int64_t AVGalc_mean_mv = 0;
  static int64_t AVGalc_rms_mv = 0;
  static int64_t AVGbattery_mv = 0;
  static int64_t AVGbattery_percent = 0;
  static uint32_t count = 0;
  static int32_t holdblc_mean_mv = 0;
  static int32_t holdblc_rms_mv = 0;
  static int32_t holdblc_p2p_mv = 0;
  static int32_t holdalc_mean_mv = 0;
  static int32_t holdalc_rms_mv = 0;
  static int32_t holdalc_p2p_mv = 0;
  static int32_t battery_mv = 0;
  static int32_t battery_percent = 0;

  while (1) {
    /* Check Rotary Switch if in Manual Mode (BLE disconnected) */
    if (!ble_is_connected()) {
      range_update_manual();
    }

    if (adc_read(adc_dev, &seq_blc) || adc_read(adc_dev, &seq_alc) ||
        adc_read(adc_dev, &seq_bat)) {
      printk("ADC read error\n");
      k_sleep(K_MSEC(100));
      continue;
    }

    k_mutex_lock(&data_mutex, K_FOREVER);

    int32_t bat_sum = 0;
    for (int i = 0; i < 16; i++) {
      bat_sum += adc_bat_buf[i];
    }
    int16_t raw_bat_adc = (int16_t)(bat_sum / 16);
    int32_t raw_battery_mv = calc_battery_mv(raw_bat_adc);

    static int32_t ema_battery_mv = 0;
    if (ema_battery_mv == 0) {
      ema_battery_mv = raw_battery_mv;
    } else {
      // EMA low-pass filter (alpha = 1/8) to eliminate fluctuation noise
      ema_battery_mv = (ema_battery_mv * 7 + raw_battery_mv) / 8;
    }
    battery_mv = ema_battery_mv;
    battery_percent = battery_percent_from_mv(battery_mv);
    AVGbattery_mv += battery_mv;
    AVGbattery_percent += battery_percent;

    holdblc_rms_mv = calc_50hz_fundamental_rms_mV(adc_blc_buf, SAMPLE_COUNT);
    holdalc_rms_mv = calc_50hz_fundamental_rms_mV(adc_alc_buf, SAMPLE_COUNT);
    int32_t dummy_p2p;
    calc_mean_rms_p2p_mV(adc_blc_buf, SAMPLE_COUNT, &holdblc_mean_mv, &dummy_p2p, &holdblc_p2p_mv);
    calc_mean_rms_p2p_mV(adc_alc_buf, SAMPLE_COUNT, &holdalc_mean_mv, &dummy_p2p, &holdalc_p2p_mv);

    AVGblc_mean_mv += holdblc_mean_mv;
    AVGblc_rms_mv += holdblc_rms_mv;
    AVGalc_mean_mv += holdalc_mean_mv;
    AVGalc_rms_mv += holdalc_rms_mv;
    count++;
    if (count == ADC_AVG_COUNT) {
      g_data.blc_mean_mv = AVGblc_mean_mv / count;
      g_data.blc_rms_mv = AVGblc_rms_mv / count;
      g_data.alc_mean_mv = AVGalc_mean_mv / count;
      g_data.alc_rms_mv = AVGalc_rms_mv / count;
      g_data.battery_mv = AVGbattery_mv / count;
      g_data.battery_percent = AVGbattery_percent / count;
      g_data.induced_voltage_mv = (g_data.blc_rms_mv > g_data.alc_rms_mv) ? g_data.blc_rms_mv : g_data.alc_rms_mv;
      g_data.selected_range = range_get();
      uint8_t ch = g_data.selected_range;
      printk("BLC Mean: %d mV, 50Hz RMS: %d mV\n", g_data.blc_mean_mv, g_data.blc_rms_mv);
      printk("ALC Mean: %d mV, 50Hz RMS: %d mV\n", g_data.alc_mean_mv, g_data.alc_rms_mv);
      printk("Battery: %d mV, %d%%\n", g_data.battery_mv, g_data.battery_percent);
      if (ch >= 16)
        ch = 0; // Safety guard

      channel_thresholds_t *t = &g_thresholds.channels[ch];

      // 1. Live Thresholds for Range (35mV BLC, 25mV ALC defaults or configured blc_rms_min/alc_rms_min)
      int32_t blc_live_thresh = (t->blc_rms_min > 0) ? t->blc_rms_min : 35;
      int32_t alc_live_thresh = (t->alc_rms_min > 0) ? t->alc_rms_min : 25;

      uint8_t status = STATUS_SAFE;
      if (g_data.blc_rms_mv >= blc_live_thresh && g_data.alc_rms_mv >= alc_live_thresh) {
        status = STATUS_LIVE; // ENERGIZED LIVE LINE
      } else if (ch >= 2) {
        // 2. Induced Danger Thresholds (90% of Live Range Sensitivity) for Channels >= 2 (3.3kV and above)
        // Disable induced status for Channels 0 & 1 (230V & 1.1kV)
        int32_t blc_induced_thresh = (blc_live_thresh * 90) / 100;
        int32_t alc_induced_thresh = (alc_live_thresh * 90) / 100;

        if (g_data.blc_rms_mv >= blc_induced_thresh || g_data.alc_rms_mv >= alc_induced_thresh) {
          status = STATUS_INDUCED; // HAZARDOUS INDUCED VOLTAGE ON UNCHARGED LINE
        } else {
          status = STATUS_SAFE; // SAFE / DE-ENERGIZED LINE
        }
      } else {
        status = STATUS_SAFE; // SAFE / DE-ENERGIZED LINE (Channels 0 & 1)
      }

      g_data.Line_detector_Status = status;
      printk("Line Detector Status: %d (0:SAFE, 1:LIVE, 2:INDUCED, Range: %d), Induced V: %d mV\n",
             g_data.Line_detector_Status, ch, g_data.induced_voltage_mv);

      // Audio / Visual Signaling
      if (g_data.Line_detector_Status == STATUS_LIVE) {
        // Continuous Solid Tone & Solid LED for Live Line
        if (gpio_is_ready_dt(&buzzer_spec)) {
          gpio_pin_set_dt(&buzzer_spec, 1);
        }
        if (gpio_is_ready_dt(&DetectionLed_spec)) {
          gpio_pin_set_dt(&DetectionLed_spec, 1);
        }
      } else if (g_data.Line_detector_Status == STATUS_INDUCED) {
        // Pulsing/Beeping Buzzer & Flashing LED for Hazardous Induced Voltage
        static uint8_t induced_blink = 0;
        induced_blink = !induced_blink;
        if (gpio_is_ready_dt(&buzzer_spec)) {
          gpio_pin_set_dt(&buzzer_spec, induced_blink); // Beeping buzzer tone
        }
        if (gpio_is_ready_dt(&DetectionLed_spec)) {
          gpio_pin_set_dt(&DetectionLed_spec, induced_blink); // Flashing LED
        }
      } else {
        // Silent / Off for Safe Line
        if (gpio_is_ready_dt(&buzzer_spec)) {
          gpio_pin_set_dt(&buzzer_spec, 0); // OFF
        }
        if (gpio_is_ready_dt(&DetectionLed_spec)) {
          gpio_pin_set_dt(&DetectionLed_spec, 0); // OFF
        }
      }

      AVGblc_mean_mv = 0;
      AVGblc_rms_mv = 0;
      AVGalc_mean_mv = 0;
      AVGalc_rms_mv = 0;
      AVGbattery_mv = 0;
      AVGbattery_percent = 0;
      count = 0;
      k_mutex_unlock(&data_mutex);

    } else {
      k_mutex_unlock(&data_mutex);
    }

    k_sleep(K_MSEC(ADC_PERIOD_MS));
  }
}

void adc_get_snapshot(data_t *p_data, int16_t *p_blc, int16_t *p_alc) {
  k_mutex_lock(&data_mutex, K_FOREVER);
  if (p_data) {
    *p_data = g_data;
  }
  if (p_blc) {
    memcpy(p_blc, adc_blc_buf, sizeof(adc_blc_buf));
  }
  if (p_alc) {
    memcpy(p_alc, adc_alc_buf, sizeof(adc_alc_buf));
  }
  k_mutex_unlock(&data_mutex);
}
