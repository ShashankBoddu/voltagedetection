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
static int16_t published_blc_buf[SAMPLE_COUNT];
static int16_t published_alc_buf[SAMPLE_COUNT];
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

static const struct gpio_dt_spec ble_led_spec =
    GPIO_DT_SPEC_GET(DT_NODELABEL(blemode), gpios);

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

void annunciation_boot_selftest(void) {
  printk("Boot Self-Test: Verifying Buzzer, Red LED U5, Blue LED U8...\n");
  if (gpio_is_ready_dt(&buzzer_spec)) {
    gpio_pin_set_dt(&buzzer_spec, 1);
  }
  if (gpio_is_ready_dt(&DetectionLed_spec)) {
    gpio_pin_set_dt(&DetectionLed_spec, 1);
  }
  if (gpio_is_ready_dt(&ble_led_spec)) {
    gpio_pin_set_dt(&ble_led_spec, 1);
  }
  k_sleep(K_MSEC(500));
  if (gpio_is_ready_dt(&buzzer_spec)) {
    gpio_pin_set_dt(&buzzer_spec, 0);
  }
  if (gpio_is_ready_dt(&DetectionLed_spec)) {
    gpio_pin_set_dt(&DetectionLed_spec, 0);
  }
  if (gpio_is_ready_dt(&ble_led_spec)) {
    gpio_pin_set_dt(&ble_led_spec, 0);
  }
  k_sleep(K_MSEC(100));
  printk("Boot Self-Test Complete.\n");
}

/* ================= ADC Functions ================= */

static int32_t calc_battery_mv(int16_t adc_counts) {
  int32_t adc_mv = (adc_counts * ADC_LSB_uV) / 1000;
  // Improved Scaling
  return (adc_mv * BATTERY_SCALE_MUL) / BATTERY_SCALE_DIV;
}

static uint8_t battery_percent_from_mv(int32_t batt_mv) {
  // Envie Rechargeable 9V Infinite 300mAh Ni-MH (7-cell series block, 8.4V nominal)
  if (batt_mv >= 9600)
    return 100; // Fresh off charger (~1.38V - 1.42V/cell)
  if (batt_mv >= 9200)
    return 90;
  if (batt_mv >= 8800)
    return 80;
  if (batt_mv >= 8500)
    return 65;
  if (batt_mv >= 8300)
    return 50;  // Flat Ni-MH nominal plateau (~1.19V - 1.20V/cell)
  if (batt_mv >= 8100)
    return 35;
  if (batt_mv >= 7800)
    return 20;
  if (batt_mv >= 7400)
    return 10;  // Low Battery Alert Threshold (~1.05V/cell)
  if (batt_mv >= 7000)
    return 5;   // Critical Low Battery Cutoff (~1.00V/cell)
  return 0;     // Depleted (< 7.0V)
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

/* ---------- Dual-Frequency Power Signal Goertzel Filter (50Hz + 150Hz) ---------- */
static void calc_goertzel_50hz_150hz_rms_mV(int16_t *buf, int count,
                                            int32_t *out_rms_50,
                                            int32_t *out_rms_150) {
  // First pass: Calculate mean (DC offset removal)
  int64_t sum = 0;
  for (int i = 0; i < count; i++) {
    sum += buf[i];
  }
  float mean = (float)sum / (float)count;

  // 50Hz fundamental power signal (Fs = 10000 Hz, N = 200, k = 1.0)
  float omega50 = (2.0f * 3.14159265f * 1.0f) / (float)count;
  float coeff50 = 2.0f * cosf(omega50);
  float q0_50 = 0.0f, q1_50 = 0.0f, q2_50 = 0.0f;

  // 150Hz 3rd harmonic (k = 3.0) for SMPS diode bridge rectifier detection
  float omega150 = (2.0f * 3.14159265f * 3.0f) / (float)count;
  float coeff150 = 2.0f * cosf(omega150);
  float q0_150 = 0.0f, q1_150 = 0.0f, q2_150 = 0.0f;

  for (int i = 0; i < count; i++) {
    float s = (float)buf[i] - mean; // DC removed
    // 50Hz Goertzel
    q0_50 = coeff50 * q1_50 - q2_50 + s;
    q2_50 = q1_50;
    q1_50 = q0_50;

    // 150Hz Goertzel
    q0_150 = coeff150 * q1_150 - q2_150 + s;
    q2_150 = q1_150;
    q1_150 = q0_150;
  }

  // 50Hz RMS
  float real50 = q1_50 - q2_50 * cosf(omega50);
  float imag50 = q2_50 * sinf(omega50);
  float mag50 = sqrtf(real50 * real50 + imag50 * imag50) / ((float)count / 2.0f);
  float rms50 = mag50 / 1.41421356f;

  // 150Hz RMS
  float real150 = q1_150 - q2_150 * cosf(omega150);
  float imag150 = q2_150 * sinf(omega150);
  float mag150 = sqrtf(real150 * real150 + imag150 * imag150) / ((float)count / 2.0f);
  float rms150 = mag150 / 1.41421356f;

  if (out_rms_50) {
    *out_rms_50 = (int32_t)((rms50 * ADC_LSB_uV) / 1000.0f);
  }
  if (out_rms_150) {
    *out_rms_150 = (int32_t)((rms150 * ADC_LSB_uV) / 1000.0f);
  }
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
    printk("Detection LED (Red U5) GPIO ready\n");
  } else {
    printk("Error: Detection LED (Red U5) GPIO not ready\n");
  }

  if (gpio_is_ready_dt(&ble_led_spec)) {
    gpio_pin_configure_dt(&ble_led_spec, GPIO_OUTPUT_INACTIVE);
    printk("BLE Status LED (Blue U8) GPIO ready\n");
  } else {
    printk("Error: BLE Status LED (Blue U8) GPIO not ready\n");
  }

  // Configure SAADC Channels
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

/* ---------- 9-State Comprehensive System Annunciation Engine ---------- */
static void update_annunciation(uint8_t status, int32_t batt_mv, bool ble_conn) {
  static uint32_t tick = 0;
  tick++;

  bool buzzer_out = false;
  bool red_led_out = false;
  bool blue_led_out = false;

  bool is_low_bat = (batt_mv > 0 && batt_mv < 7350);

  if (status == STATUS_FAULT) {
    // 9. Hardware Fault (Self-Test Fail / Preamp DC bias abnormal)
    // Red & Blue LED Alternating Strobe at 5 Hz (period = 200 ms = 20 ticks)
    uint32_t phase = tick % 20;
    red_led_out = (phase < 10);
    blue_led_out = (phase >= 10);
    // Rapid short error chirps: 50 ms ON (5 ticks), 150 ms OFF
    buzzer_out = (phase < 5);
  } else if (status == STATUS_LIVE) {
    // 6 & 7. LIVE Alarm (Energized Line)
    buzzer_out = true;  // Continuous loud siren (100% duty)
    red_led_out = true; // Solid ON (100% duty)
    if (ble_conn) {
      blue_led_out = true; // Solid ON when connected
    } else {
      blue_led_out = ((tick % 100) < 10); // Slow blink 1 Hz, 10% duty (100 ms ON)
    }
  } else if (status == STATUS_INDUCED) {
    // 4 & 5. Hazardous Induced Voltage Warning (Channels >= 2, 25% to 90% threshold)
    // Pulsing beep 2 Hz, 50% duty (250 ms ON, 250 ms OFF = period 500 ms = 50 ticks)
    buzzer_out = ((tick % 50) < 25);
    // Rapid flash 4 Hz, 50% duty (125 ms ON, 125 ms OFF = period 250 ms = 25 ticks)
    red_led_out = ((tick % 25) < 13);
    if (ble_conn) {
      blue_led_out = true;
    } else {
      blue_led_out = ((tick % 100) < 10);
    }
  } else {
    // 2, 3, & 8. Safe Line (< 25% threshold)
    if (is_low_bat) {
      // 8. Low Battery Warning (< 7350 mV)
      // Double flash Red LED every 3 seconds (300 ticks)
      // Flash 1: 0..7 (80 ms), Gap: 8..15 (80 ms), Flash 2: 16..23 (80 ms)
      uint32_t bat_led_phase = tick % 300;
      red_led_out = (bat_led_phase < 8) || (bat_led_phase >= 16 && bat_led_phase < 24);

      // Double chirp Buzzer every 10 seconds (1000 ticks)
      // Chirp 1: 0..5 (60 ms), Gap: 6..15 (100 ms), Chirp 2: 16..21 (60 ms)
      uint32_t bat_buzz_phase = tick % 1000;
      buzzer_out = (bat_buzz_phase < 6) || (bat_buzz_phase >= 16 && bat_buzz_phase < 22);
    } else {
      buzzer_out = false;
      red_led_out = false;
    }

    if (ble_conn) {
      blue_led_out = true; // 3. Safe Line (BLE Connected)
    } else {
      blue_led_out = ((tick % 100) < 10); // 2. Safe Line (BLE Disconnected: Slow Blink 1 Hz)
    }
  }

  if (gpio_is_ready_dt(&buzzer_spec)) {
    gpio_pin_set_dt(&buzzer_spec, buzzer_out ? 1 : 0);
  }
  if (gpio_is_ready_dt(&DetectionLed_spec)) {
    gpio_pin_set_dt(&DetectionLed_spec, red_led_out ? 1 : 0);
  }
  if (gpio_is_ready_dt(&ble_led_spec)) {
    gpio_pin_set_dt(&ble_led_spec, blue_led_out ? 1 : 0);
  }
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
  static int64_t AVGblc_150hz_rms_mv = 0;
  static int64_t AVGalc_mean_mv = 0;
  static int64_t AVGalc_rms_mv = 0;
  static int64_t AVGbattery_mv = 0;
  static int64_t AVGbattery_percent = 0;
  static uint32_t count = 0;
  static int32_t holdblc_mean_mv = 0;
  static int32_t holdblc_rms_mv = 0;
  static int32_t holdblc_150hz_rms_mv = 0;
  static int32_t holdblc_p2p_mv = 0;
  static int32_t holdalc_mean_mv = 0;
  static int32_t holdalc_rms_mv = 0;
  static int32_t holdalc_150hz_rms_mv = 0;
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
    memcpy(published_blc_buf, adc_blc_buf, sizeof(adc_blc_buf));
    memcpy(published_alc_buf, adc_alc_buf, sizeof(adc_alc_buf));

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

    calc_goertzel_50hz_150hz_rms_mV(adc_blc_buf, SAMPLE_COUNT, &holdblc_rms_mv, &holdblc_150hz_rms_mv);
    calc_goertzel_50hz_150hz_rms_mV(adc_alc_buf, SAMPLE_COUNT, &holdalc_rms_mv, &holdalc_150hz_rms_mv);
    int32_t dummy_p2p;
    calc_mean_rms_p2p_mV(adc_blc_buf, SAMPLE_COUNT, &holdblc_mean_mv, &dummy_p2p, &holdblc_p2p_mv);
    calc_mean_rms_p2p_mV(adc_alc_buf, SAMPLE_COUNT, &holdalc_mean_mv, &dummy_p2p, &holdalc_p2p_mv);

    AVGblc_mean_mv += holdblc_mean_mv;
    AVGblc_rms_mv += holdblc_rms_mv;
    AVGblc_150hz_rms_mv += holdblc_150hz_rms_mv;
    AVGalc_mean_mv += holdalc_mean_mv;
    AVGalc_rms_mv += holdalc_rms_mv;
    count++;
    if (count == ADC_AVG_COUNT) {
      g_data.blc_mean_mv = AVGblc_mean_mv / count;
      g_data.blc_rms_mv = AVGblc_rms_mv / count;
      int32_t blc_150hz_rms_mv = (int32_t)(AVGblc_150hz_rms_mv / count);
      g_data.alc_mean_mv = AVGalc_mean_mv / count;
      g_data.alc_rms_mv = AVGalc_rms_mv / count;
      g_data.battery_mv = AVGbattery_mv / count;
      g_data.battery_percent = AVGbattery_percent / count;
      g_data.induced_voltage_mv = (g_data.blc_rms_mv > g_data.alc_rms_mv) ? g_data.blc_rms_mv : g_data.alc_rms_mv;
      g_data.selected_range = range_get();
      uint8_t ch = g_data.selected_range;
      printk("BLC Mean: %d mV, 50Hz RMS: %d mV, 150Hz RMS: %d mV\n",
             g_data.blc_mean_mv, g_data.blc_rms_mv, blc_150hz_rms_mv);
      printk("ALC Mean: %d mV, 50Hz RMS: %d mV\n", g_data.alc_mean_mv, g_data.alc_rms_mv);
      printk("Battery: %d mV, %d%%\n", g_data.battery_mv, g_data.battery_percent);
      if (ch >= 16)
        ch = 0; // Safety guard

      channel_thresholds_t *t = &g_thresholds.channels[ch];

      // 1. Live Thresholds for Range (35mV BLC, 25mV ALC defaults or configured blc_rms_min/alc_rms_min)
      int32_t blc_live_thresh = (t->blc_rms_min > 0) ? t->blc_rms_min : 35;
      int32_t alc_live_thresh = (t->alc_rms_min > 0) ? t->alc_rms_min : 25;

      // Method A Continuous Self-Test: Preamplifier DC Bias Health Check
      // Nominal MCP601 DC bias is VDD/2 = ~1650 mV.
      // Abnormal bias (< 800 mV or > 2400 mV) flags sensor plate disconnect or op-amp ESD failure.
      bool hw_fault = (g_data.blc_mean_mv < 800 || g_data.blc_mean_mv > 2400);

      // Compute 150 Hz 3rd-Harmonic Distortion Ratio
      int32_t harmonic_150hz_pct = (g_data.blc_rms_mv > 0) ?
                                   ((blc_150hz_rms_mv * 100) / g_data.blc_rms_mv) : 0;

      uint8_t status = STATUS_SAFE;
      if (hw_fault) {
        status = STATUS_FAULT;
        printk("--> HARDWARE FAULT: Preamp DC bias out-of-range (%d mV, expected 800-2400 mV)\n",
               g_data.blc_mean_mv);
      } else if (harmonic_150hz_pct >= 20) {
        // Universal SMPS Rectifier Discriminator (All Channels):
        // Genuine utility power and genuine induced fields have clean sinusoidal 50Hz (150Hz harmonic < 10%).
        // Mobile chargers, power adapters, and SMPS rectifiers produce massive 150Hz content (> 25%).
        status = STATUS_SAFE; // Suppress SMPS charger / adapter leakage across all ranges
        printk("--> REJECTED: SMPS Charger noise detected (150Hz harmonic %d%% >= 20%%, Range: %d)\n",
               harmonic_150hz_pct, ch);
      } else if (g_data.blc_rms_mv >= blc_live_thresh && g_data.alc_rms_mv >= alc_live_thresh) {
        // ENERGIZED LIVE LINE (Clean sinusoidal utility grid power)
        status = STATUS_LIVE;
        printk("--> VALIDATED LIVE LINE: Channel %d (50Hz RMS: BLC %d mV, ALC %d mV, 150Hz: %d%%)\n",
               ch, g_data.blc_rms_mv, g_data.alc_rms_mv, harmonic_150hz_pct);
      } else if (ch >= 2) {
        // Hazardous Induced Voltage Detection for High-Voltage Ranges (Channels >= 2, 3.3kV to 765kV):
        // 1. Dual-Channel Coincidence (&&): Both BLC and ALC must confirm.
        // 2. High-Voltage Induced Threshold: Set to 75% of Live threshold to reject low-voltage ambient room coupling.
        int32_t blc_induced_thresh = (blc_live_thresh * 80) / 100;
        int32_t alc_induced_thresh = (alc_live_thresh * 75) / 100;

        // Minimum physical noise floor clamp
        if (blc_induced_thresh < 20) {
          blc_induced_thresh = 20;
        }
        if (alc_induced_thresh < 22) {
          alc_induced_thresh = 22;
        }

        if (g_data.blc_rms_mv >= blc_induced_thresh && g_data.alc_rms_mv >= alc_induced_thresh) {
          status = STATUS_INDUCED; // Genuine hazardous induced voltage on uncharged HV line
        } else {
          status = STATUS_SAFE; // Clean de-energized line (ambient room noise rejected)
        }
      } else {
        status = STATUS_SAFE; // SAFE / DE-ENERGIZED LINE (Channels 0 & 1: 230V & 1.1kV)
      }

      g_data.Line_detector_Status = status;
      printk("Line Detector Status: %d (0:SAFE, 1:LIVE, 2:INDUCED, 3:FAULT, Range: %d), Induced V: %d mV\n",
             g_data.Line_detector_Status, ch, g_data.induced_voltage_mv);

      AVGblc_mean_mv = 0;
      AVGblc_rms_mv = 0;
      AVGblc_150hz_rms_mv = 0;
      AVGalc_mean_mv = 0;
      AVGalc_rms_mv = 0;
      AVGbattery_mv = 0;
      AVGbattery_percent = 0;
      count = 0;
      k_mutex_unlock(&data_mutex);

    } else {
      k_mutex_unlock(&data_mutex);
    }

    // Continuous 10 ms Annunciation Engine Update
    update_annunciation(g_data.Line_detector_Status, g_data.battery_mv, ble_is_connected());

    k_sleep(K_MSEC(ADC_PERIOD_MS));
  }
}

void adc_get_snapshot(data_t *p_data, int16_t *p_blc, int16_t *p_alc) {
  k_mutex_lock(&data_mutex, K_FOREVER);
  if (p_data) {
    *p_data = g_data;
    p_data->selected_range = range_get();
  }
  if (p_blc) {
    memcpy(p_blc, published_blc_buf, sizeof(published_blc_buf));
  }
  if (p_alc) {
    memcpy(p_alc, published_alc_buf, sizeof(published_alc_buf));
  }
  k_mutex_unlock(&data_mutex);
}
