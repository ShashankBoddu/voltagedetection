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

/* ================= GPIO Specs ================= */
static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
static const struct gpio_dt_spec buzzer_spec =
    GPIO_DT_SPEC_GET(DT_NODELABEL(buzzer), gpios);

static const struct gpio_dt_spec DetectionLed_spec =
    GPIO_DT_SPEC_GET(DT_NODELABEL(lowbat), gpios);

static const struct gpio_dt_spec ble_led_spec =
    GPIO_DT_SPEC_GET(DT_NODELABEL(blemode), gpios);

static const struct gpio_dt_spec selftest_drive_spec =
    GPIO_DT_SPEC_GET_OR(DT_NODELABEL(selftest_drive), gpios, {0});

/* ================= ADC Buffers ================= */
int16_t adc_blc_buf[SAMPLE_COUNT];
int16_t adc_alc_buf[SAMPLE_COUNT];
static int16_t published_blc_buf[SAMPLE_COUNT];
static int16_t published_alc_buf[SAMPLE_COUNT];
static int16_t adc_bat_buf[16];

/* ================= ADC config Struct ================= */
static struct adc_sequence_options seq_opts_200 = {
    .interval_us = 100,
    .extra_samplings = SAMPLE_COUNT - 1,
};

static struct adc_sequence_options seq_opts_bat = {
    .interval_us = 100,
    .extra_samplings = 15,
};

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

/* ================= Dedicated 50Hz Hardware Self-Test Timer ================= */
static volatile bool g_selftest_drive_state = false;
static void selftest_timer_expiry(struct k_timer *timer_id) {
  ARG_UNUSED(timer_id);
  g_selftest_drive_state = !g_selftest_drive_state;
  if (gpio_is_ready_dt(&selftest_drive_spec)) {
    gpio_pin_set_dt(&selftest_drive_spec, g_selftest_drive_state ? 1 : 0);
  }
}
static K_TIMER_DEFINE(selftest_timer, selftest_timer_expiry, NULL);

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

static void calc_goertzel_50hz_150hz_rms_mV(int16_t *buf, int count,
                                            int32_t *out_rms_50,
                                            int32_t *out_rms_150);

static void calc_goertzel_50hz_100hz_rms_mV(int16_t *buf, int count,
                                            int32_t *out_rms_50,
                                            int32_t *out_rms_100);

static void calc_mean_rms_p2p_mV(int16_t *buf, int count, int32_t *mean_mV,
                                 int32_t *rms_mV, int32_t *p2p_mV);

static bool g_kelvin_hardware_failed = false;

static volatile bool g_selftest_active = false;

bool hardware_kelvin_selftest(selftest_result_t *out_result) {
  printk("=== Starting Differential Kelvin Hardware Self-Test (P0.07 / Pin 23) ===\n");

  if (!gpio_is_ready_dt(&selftest_drive_spec)) {
    printk("Error: selftest_drive GPIO not ready! Failing self-test for safety.\n");
    if (out_result) {
      memset(out_result, 0, sizeof(selftest_result_t));
      out_result->passed = false;
    }
    return false;
  }

  // Switch to Channel 0 (230V) for standard self-test impedance
  uint8_t prev_ch = range_get();
  range_set(0);
  k_sleep(K_MSEC(25)); // Allow MUX transition and capacitance to settle

  // Pause background ADC thread from reading SAADC concurrently
  g_selftest_active = true;
  k_sleep(K_MSEC(30)); // Allow background thread to finish current cycle

  /* =========================================================================
   * PHASE 1: Baseline Ambient Capture (GPIO P0.07 High-Z Input / Tri-State)
   * Measures quiescent ambient noise and any stray grid coupling BEFORE drive.
   * Uses dual Goertzel: 50 Hz (grid ambient) and 100 Hz (quiescent floor).
   * Measures both BLC (core sensor) and ALC (filtered sensor) baseline.
   * ========================================================================= */
  k_timer_stop(&selftest_timer);
  gpio_pin_configure_dt(&selftest_drive_spec, GPIO_INPUT);
  k_sleep(K_MSEC(30)); // Settle in high-Z

  int32_t base_50hz_blc = 0, base_100hz_blc = 0, base_mean_blc = 0, base_rms_blc = 0, base_p2p_blc = 0;
  int32_t base_mean_alc = 0, base_rms_alc = 0, base_p2p_alc = 0;
  
  // Average 2 cycles of baseline
  int base_samples = 0;
  int32_t sum_base_rms_blc = 0, sum_base_p2p_blc = 0, sum_base_50hz_blc = 0, sum_base_100hz_blc = 0;
  int32_t sum_base_p2p_alc = 0;
  for (int b = 0; b < 2; b++) {
    if (!adc_read(adc_dev, &seq_blc) && !adc_read(adc_dev, &seq_alc)) {
      calc_goertzel_50hz_100hz_rms_mV(adc_blc_buf, SAMPLE_COUNT, &base_50hz_blc, &base_100hz_blc);
      calc_mean_rms_p2p_mV(adc_blc_buf, SAMPLE_COUNT, &base_mean_blc, &base_rms_blc, &base_p2p_blc);
      calc_mean_rms_p2p_mV(adc_alc_buf, SAMPLE_COUNT, &base_mean_alc, &base_rms_alc, &base_p2p_alc);
      sum_base_rms_blc += base_rms_blc;
      sum_base_p2p_blc += base_p2p_blc;
      sum_base_50hz_blc += base_50hz_blc;
      sum_base_100hz_blc += base_100hz_blc;
      sum_base_p2p_alc += base_p2p_alc;
      base_samples++;
    }
  }
  int32_t avg_base_rms_blc = base_samples ? (sum_base_rms_blc / base_samples) : 0;
  int32_t avg_base_p2p_blc = base_samples ? (sum_base_p2p_blc / base_samples) : 0;
  int32_t avg_base_50hz_blc = base_samples ? (sum_base_50hz_blc / base_samples) : 0;
  int32_t avg_base_100hz_blc = base_samples ? (sum_base_100hz_blc / base_samples) : 0;
  int32_t avg_base_p2p_alc = base_samples ? (sum_base_p2p_alc / base_samples) : 0;

  printk("Kelvin Baseline Ambient: BLC RMS %d mV, P2P %d mV, 50Hz %d mV, 100Hz %d mV | ALC P2P %d mV\n", 
         avg_base_rms_blc, avg_base_p2p_blc, avg_base_50hz_blc, avg_base_100hz_blc, avg_base_p2p_alc);

  /* =========================================================================
   * PHASE 2: Transient Step-Response Pulse Drive (100 Hz / 5 ms Toggles)
   * Drives fast rising/falling step edges through C_iso (10 pF) & R_iso (10 MΩ)
   * into Wire 2 to inject transient charge spikes into the copper sensing plate.
   * Lock-in Goertzel detects the fundamental 100 Hz signal (k = 2.0).
   * ========================================================================= */
  gpio_pin_configure_dt(&selftest_drive_spec, GPIO_OUTPUT_INACTIVE);
  g_selftest_drive_state = false;
  k_timer_start(&selftest_timer, K_NO_WAIT, K_MSEC(5)); // 5 ms toggle = 100 Hz step edges
  k_sleep(K_MSEC(20)); // Allow step transitions to establish rhythm

  #define SELFTEST_CYCLE_COUNT 4
  int32_t sum_100hz_blc = 0, sum_50hz_blc = 0, sum_total_blc = 0, sum_p2p_blc = 0, sum_mean_blc = 0;
  int32_t sum_100hz_alc = 0, sum_50hz_alc = 0, sum_total_alc = 0, sum_p2p_alc = 0, sum_mean_alc = 0;
  int32_t sum_active_samples = 0;
  int valid_cycles = 0;

  for (int cycle = 0; cycle < SELFTEST_CYCLE_COUNT; cycle++) {
    int err_blc = adc_read(adc_dev, &seq_blc);
    int err_alc = adc_read(adc_dev, &seq_alc);
    if (!err_blc && !err_alc) {
      int32_t c_50hz_blc = 0, c_100hz_blc = 0, c_mean_blc = 0, c_rms_blc = 0, c_p2p_blc = 0;
      int32_t c_50hz_alc = 0, c_100hz_alc = 0, c_mean_alc = 0, c_rms_alc = 0, c_p2p_alc = 0;

      calc_goertzel_50hz_100hz_rms_mV(adc_blc_buf, SAMPLE_COUNT, &c_50hz_blc, &c_100hz_blc);
      calc_mean_rms_p2p_mV(adc_blc_buf, SAMPLE_COUNT, &c_mean_blc, &c_rms_blc, &c_p2p_blc);

      calc_goertzel_50hz_100hz_rms_mV(adc_alc_buf, SAMPLE_COUNT, &c_50hz_alc, &c_100hz_alc);
      calc_mean_rms_p2p_mV(adc_alc_buf, SAMPLE_COUNT, &c_mean_alc, &c_rms_alc, &c_p2p_alc);

      sum_100hz_blc += c_100hz_blc;
      sum_50hz_blc += c_50hz_blc;
      sum_total_blc += c_rms_blc;
      sum_p2p_blc += c_p2p_blc;
      sum_mean_blc += c_mean_blc;

      sum_100hz_alc += c_100hz_alc;
      sum_50hz_alc += c_50hz_alc;
      sum_total_alc += c_rms_alc;
      sum_p2p_alc += c_p2p_alc;
      sum_mean_alc += c_mean_alc;

      // Count active elevated pulse samples (differentiates wide square wave vs needle spike)
      int active_samples = 0;
      int32_t thresh_counts = (25 * 1000) / ADC_LSB_uV; // 25 mV threshold
      int32_t mean_counts = (c_mean_blc * 1000) / ADC_LSB_uV;
      for (int i = 0; i < SAMPLE_COUNT; i++) {
        int32_t diff = (int32_t)adc_blc_buf[i] - mean_counts;
        if (diff > thresh_counts || diff < -thresh_counts) {
          active_samples++;
        }
      }
      sum_active_samples += active_samples;
      valid_cycles++;
    }
  }

  // 1. Immediately snapshot active waveforms while background thread is STILL paused!
  k_mutex_lock(&data_mutex, K_FOREVER);
  memcpy(published_blc_buf, adc_blc_buf, sizeof(published_blc_buf));
  memcpy(published_alc_buf, adc_alc_buf, sizeof(published_alc_buf));
  k_mutex_unlock(&data_mutex);

  // 2. Stop the step drive timer and return P0.07 to High-Z INPUT (Tri-state)
  k_timer_stop(&selftest_timer);
  gpio_pin_set_dt(&selftest_drive_spec, 0);
  gpio_pin_configure_dt(&selftest_drive_spec, GPIO_INPUT);

  // 3. Restore original range channel and settle
  if (!ble_is_connected()) {
    range_update_manual();
  } else {
    range_set(prev_ch);
  }
  k_sleep(K_MSEC(25)); // Settle MUX while background thread is STILL paused!

  // 4. NOW safe to resume background ADC thread
  g_selftest_active = false;

  if (valid_cycles == 0) {
    printk("Self-Test Error: SAADC reads failed on all cycles\n");
    if (out_result) {
      memset(out_result, 0, sizeof(selftest_result_t));
    }
    g_kelvin_hardware_failed = true;
    return false;
  }

  int32_t avg_active_100hz_blc = sum_100hz_blc / valid_cycles;
  int32_t avg_active_50hz_blc = sum_50hz_blc / valid_cycles;
  ARG_UNUSED(avg_active_50hz_blc);
  int32_t avg_active_rms_blc = sum_total_blc / valid_cycles;
  int32_t avg_p2p_blc = sum_p2p_blc / valid_cycles;
  int32_t avg_mean_blc = sum_mean_blc / valid_cycles;

  int32_t avg_active_100hz_alc = sum_100hz_alc / valid_cycles;
  int32_t avg_active_50hz_alc = sum_50hz_alc / valid_cycles;
  ARG_UNUSED(avg_active_50hz_alc);
  ARG_UNUSED(avg_active_100hz_alc);
  int32_t avg_active_rms_alc = sum_total_alc / valid_cycles;
  int32_t avg_p2p_alc = sum_p2p_alc / valid_cycles;
  int32_t avg_mean_alc = sum_mean_alc / valid_cycles;

  int32_t avg_active_samples = sum_active_samples / valid_cycles;
  ARG_UNUSED(avg_active_samples);

  /* Update published mean and RMS under mutex */
  k_mutex_lock(&data_mutex, K_FOREVER);
  g_data.blc_mean_mv = avg_mean_blc;
  g_data.blc_rms_mv = avg_active_rms_blc;
  g_data.alc_mean_mv = avg_mean_alc;
  g_data.alc_rms_mv = avg_active_rms_alc;
  k_mutex_unlock(&data_mutex);

  /* =========================================================================
   * PHASE 3: Coincident Dual-Channel & 100 Hz Lock-In Evaluation Suite
   * Prevents false passes (e.g. from stray PCB capacitive spikes or live 50 Hz fields)
   * and prevents false fails on intact hardware (R_limit:10k, R_div:10k, C_iso:2.2nF, R_iso:10M).
   * ========================================================================= */
  int32_t delta_p2p_blc = avg_p2p_blc - avg_base_p2p_blc;
  if (delta_p2p_blc < 0) delta_p2p_blc = 0;

  int32_t delta_rms_blc = avg_active_rms_blc - avg_base_rms_blc;
  if (delta_rms_blc < 0) delta_rms_blc = 0;

  int32_t delta_100hz_blc = avg_active_100hz_blc - avg_base_100hz_blc;
  if (delta_100hz_blc < 0) delta_100hz_blc = 0;

  int32_t delta_alc_p2p = avg_p2p_alc - avg_base_p2p_alc;
  if (delta_alc_p2p < 0) delta_alc_p2p = 0;

  // Calculate ALC/BLC attenuation ratio percentage (integer math)
  int32_t alc_blc_ratio_pct = (avg_p2p_blc > 0) ? ((avg_p2p_alc * 100) / avg_p2p_blc) : 0;

  // RULE 1: Core BLC Injected Step Delta & Amplitude Floor (Active - Baseline)
  // Intact Wire 2 delivers true capacitive charge: measured Delta P2P ~ 195 to 212 mV (Active P2P ~ 211 to 216 mV)
  // Disconnected Wire 2 only has stray PCB needle spikes: measured Delta P2P ~ 50 to 77 mV (Active P2P ~ 64 to 81 mV)
  // Net differential subtraction (Active - Baseline) cancels ambient common-mode EMI noise.
  bool rule1_blc_amp = (delta_p2p_blc >= 110 && avg_p2p_blc >= 130);

  // RULE 2: Filtered ALC Channel Floor & Injected Delta
  // Hardware RC low-pass filter (220 ohm, 10 uF) integrates intact 100 Hz pulses: measured RMS ~ 34 to 37 mV (P2P ~ 85 to 105 mV)
  // Narrow stray needle spikes carry no low-frequency energy and are suppressed: measured RMS ~ 2 mV (P2P ~ 2 to 4 mV)
  bool rule2_alc_amp = ((avg_p2p_alc >= 20 || avg_active_rms_alc >= 12) && 
                        (delta_alc_p2p >= 8 || avg_active_rms_alc >= 12));

  // RULE 3: Dual-Channel Filter Ratio (ALC / BLC)
  // Intact loop ratio is nominally ~16.8% to 50% (ALC P2P ~ 90 mV on 212 mV BLC)
  // Disconnected needle ratio is only ~2.4% to 4.5% (2 mV on 81 mV BLC)
  // Upper limit relaxed to 75% so strong capacitive coupling through 2.2 nF NEVER false-fails
  bool rule3_ratio = (alc_blc_ratio_pct >= 8 && alc_blc_ratio_pct <= 75);

  // RULE 4: Sustained Injected 100 Hz Lock-In Energy & Coincident Confirmation
  // Intact loop injects sustained 100 Hz fundamental: delta 100Hz >= 20 mV OR coincident (BLC >= 130 && ALC RMS >= 15)
  // Disconnected stray needles carry almost zero 100 Hz fundamental (delta 100Hz <= 4 mV, ALC RMS ~ 2 mV)
  bool rule4_pulse_energy = (delta_100hz_blc >= 20 || (avg_p2p_blc >= 130 && avg_active_rms_alc >= 15));

  // RULE 5: Preamp DC Bias Operating Point
  // MCP601 virtual ground must be within normal linear dynamic range (800 to 2500 mV)
  bool rule5_dc_bias = (avg_mean_blc >= 800 && avg_mean_blc <= 2500) &&
                       (avg_mean_alc >= 800 && avg_mean_alc <= 2500);

  // Check if strong 100 Hz square wave was injected and cleanly recovered by lock-in filter
  bool strong_100hz_lockin = (delta_100hz_blc >= 25 && delta_p2p_blc >= 100 && avg_active_rms_alc >= 15);

  // Safety Inhibit:
  // In quiet air: baseline must be low (<= 60 mV P2P and <= 25 mV RMS).
  // Near AC cable with acceptable interference: If a strong 100 Hz square wave is cleanly resolved,
  // allow ambient baseline up to 50 mV RMS / 130 mV P2P.
  bool ambient_quiet = (avg_base_p2p_blc <= 60 && avg_base_rms_blc <= 25);
  bool ambient_acceptable = (avg_base_p2p_blc <= 130 && avg_base_rms_blc <= 50) && strong_100hz_lockin;
  bool rule_ambient_safe = ambient_quiet || ambient_acceptable;

  bool passed = rule1_blc_amp && rule2_alc_amp && rule3_ratio && 
                rule4_pulse_energy && rule5_dc_bias && rule_ambient_safe;

  uint8_t status_code;
  if (passed) {
    status_code = 1; // Passed
  } else if (!rule_ambient_safe || avg_base_rms_blc > 25) {
    status_code = 2; // Near AC Line (ambient AC interference too high)
  } else if (!rule5_dc_bias) {
    status_code = 3; // Hardware Damaged (preamp virtual ground bias failure)
  } else {
    status_code = 0; // Sensor Damaged (wire disconnected / open loop)
  }

  if (out_result) {
    out_result->passed = passed;
    out_result->status_code = status_code;
    out_result->reported_rms_mv = avg_active_rms_blc;
    out_result->baseline_rms_mv = avg_base_rms_blc;
    out_result->delta_rms_mv = delta_rms_blc;
    out_result->delta_p2p_mv = (delta_p2p_blc > 0) ? delta_p2p_blc : avg_p2p_blc;
    out_result->delta_100hz_mv = delta_100hz_blc;
    out_result->mean_mv = avg_mean_blc;
    out_result->p2p_mv = avg_p2p_blc;
    out_result->freq_100hz_rms_mv = avg_active_100hz_blc;
    out_result->alc_rms_mv = avg_active_rms_alc;
    out_result->alc_mean_mv = avg_mean_alc;
    out_result->alc_p2p_mv = avg_p2p_alc;
  }

  if (passed) {
    g_kelvin_hardware_failed = false;
    printk("Kelvin Hardware Self-Test: PASS (All Rules OK: BLC=%d mV [>=130, d=%d >=110], ALC=%d mV [d=%d], 100Hz_d=%d mV, Ratio=%d%% [8-75%%])\n",
           avg_p2p_blc, delta_p2p_blc, avg_p2p_alc, delta_alc_p2p, delta_100hz_blc, alc_blc_ratio_pct);
    return true;
  } else {
    g_kelvin_hardware_failed = true;
    printk("Kelvin Hardware Self-Test: FAIL (R1_BLC:%d, R2_ALC:%d, R3_Ratio:%d, R4_100Hz:%d, R5_DC:%d, AmbSafe:%d) [BLC=%d mV (d=%d), ALC=%d mV (d=%d), 100Hz_d=%d mV, Ratio=%d%%]\n",
           rule1_blc_amp, rule2_alc_amp, rule3_ratio, rule4_pulse_energy, rule5_dc_bias, rule_ambient_safe,
           avg_p2p_blc, delta_p2p_blc, avg_p2p_alc, delta_alc_p2p, delta_100hz_blc, alc_blc_ratio_pct);
    return false;
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

static uint32_t isqrt64(uint64_t n) {
  uint64_t root = 0;
  uint64_t bit = (uint64_t)1 << 62;
  while (bit > n) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (n >= root + bit) {
      n -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return (uint32_t)root;
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

  int32_t rms_counts = (int32_t)isqrt64((uint64_t)(sq / count));
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

/* ---------- Dual-Frequency Goertzel Filter (50Hz Grid + 100Hz Self-Test Lock-In) ---------- */
static void calc_goertzel_50hz_100hz_rms_mV(int16_t *buf, int count,
                                             int32_t *out_rms_50,
                                             int32_t *out_rms_100) {
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

  // 100Hz self-test lock-in fundamental (Fs = 10000 Hz, N = 200, k = 2.0)
  float omega100 = (2.0f * 3.14159265f * 2.0f) / (float)count;
  float coeff100 = 2.0f * cosf(omega100);
  float q0_100 = 0.0f, q1_100 = 0.0f, q2_100 = 0.0f;

  for (int i = 0; i < count; i++) {
    float s = (float)buf[i] - mean; // DC removed
    // 50Hz Goertzel
    q0_50 = coeff50 * q1_50 - q2_50 + s;
    q2_50 = q1_50;
    q1_50 = q0_50;

    // 100Hz Goertzel
    q0_100 = coeff100 * q1_100 - q2_100 + s;
    q2_100 = q1_100;
    q1_100 = q0_100;
  }

  // 50Hz RMS
  float real50 = q1_50 - q2_50 * cosf(omega50);
  float imag50 = q2_50 * sinf(omega50);
  float mag50 = sqrtf(real50 * real50 + imag50 * imag50) / ((float)count / 2.0f);
  float rms50 = mag50 / 1.41421356f;

  // 100Hz RMS
  float real100 = q1_100 - q2_100 * cosf(omega100);
  float imag100 = q2_100 * sinf(omega100);
  float mag100 = sqrtf(real100 * real100 + imag100 * imag100) / ((float)count / 2.0f);
  float rms100 = mag100 / 1.41421356f;

  if (out_rms_50) {
    *out_rms_50 = (int32_t)((rms50 * ADC_LSB_uV) / 1000.0f);
  }
  if (out_rms_100) {
    *out_rms_100 = (int32_t)((rms100 * ADC_LSB_uV) / 1000.0f);
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

  // Initialize Kelvin Self-Test Drive Pin P0.07 to safe High-Z Input (Tri-state)
  if (gpio_is_ready_dt(&selftest_drive_spec)) {
    gpio_pin_configure_dt(&selftest_drive_spec, GPIO_INPUT);
    printk("Kelvin Self-Test Drive (P0.07 / Pin 23) initialized in High-Z Input mode\n");
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
    // 9. Hardware Fault (IEC 61243-1: Kelvin Loop-Back Failure or Preamp Bias Collapse)
    // Red & Blue LED Alternating Strobe at 5 Hz (period = 200 ms = 20 ticks)
    uint32_t phase = tick % 20;
    red_led_out = (phase < 10);
    blue_led_out = (phase >= 10);
    // Buzzer also pulses at 5 Hz synchronous with the optical strobe
    buzzer_out = (phase < 10);
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

  /* Allow analog power rails and MCP601 bias point to settle for 100 ms */
  k_sleep(K_MSEC(100));

  static bool g_boot_selftest_done = false;
  static int64_t AVGblc_mean_mv = 0;
  static int64_t AVGblc_rms_mv = 0;
  static int64_t AVGblc_150hz_rms_mv = 0;
  static int64_t AVGalc_mean_mv = 0;
  static int64_t AVGalc_rms_mv = 0;
  static int64_t AVGalc_150hz_rms_mv = 0;
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
    if (g_selftest_active) {
      k_sleep(K_MSEC(20));
      continue;
    }

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
    AVGalc_150hz_rms_mv += holdalc_150hz_rms_mv;
    count++;
    if (count == ADC_AVG_COUNT) {
      /* Power-On Boot Kelvin Self-Test: Run once after initial 8 cycles prove ADC & rail stability */
      if (!g_boot_selftest_done) {
        g_boot_selftest_done = true;
        printk("=== Executing Power-On Boot Kelvin Self-Test (Steady-State) ===\n");
        static selftest_result_t boot_res;
        bool boot_passed = hardware_kelvin_selftest(&boot_res);
        if (!boot_passed) {
          printk("Boot Kelvin Self-Test Attempt 1 failed. Retrying in 50 ms...\n");
          k_sleep(K_MSEC(50));
          boot_passed = hardware_kelvin_selftest(&boot_res);
        }
        if (boot_passed) {
          printk("Boot Kelvin Self-Test: ALL 5 RULES PASSED (Sensor Loop Intact & Verified Safe)\n");
          g_kelvin_hardware_failed = false;
        } else {
          printk("--> CRITICAL FAULT: Boot Kelvin Self-Test FAILED! (Wire 2 Disconnected or Sensor Damaged)\n");
          g_kelvin_hardware_failed = true;
        }
      }

      g_data.blc_mean_mv = AVGblc_mean_mv / count;
      g_data.blc_rms_mv = AVGblc_rms_mv / count;
      int32_t blc_150hz_rms_mv = (int32_t)(AVGblc_150hz_rms_mv / count);
      g_data.alc_mean_mv = AVGalc_mean_mv / count;
      g_data.alc_rms_mv = AVGalc_rms_mv / count;
      int32_t alc_150hz_rms_mv = (int32_t)(AVGalc_150hz_rms_mv / count);
      g_data.battery_mv = AVGbattery_mv / count;
      g_data.battery_percent = AVGbattery_percent / count;
      g_data.induced_voltage_mv = (g_data.blc_rms_mv > g_data.alc_rms_mv) ? g_data.blc_rms_mv : g_data.alc_rms_mv;
      g_data.selected_range = range_get();
      uint8_t ch = g_data.selected_range;
      printk("BLC Mean: %d mV, 50Hz RMS: %d mV, 150Hz RMS: %d mV\n",
             g_data.blc_mean_mv, g_data.blc_rms_mv, blc_150hz_rms_mv);
      printk("ALC Mean: %d mV, 50Hz RMS: %d mV, 150Hz RMS: %d mV\n",
             g_data.alc_mean_mv, g_data.alc_rms_mv, alc_150hz_rms_mv);
      printk("Battery: %d mV, %d%%\n", g_data.battery_mv, g_data.battery_percent);
      if (ch >= 16)
        ch = 0; // Safety guard

      channel_thresholds_t *t = &g_thresholds.channels[ch];

      // 1. Live Thresholds for Range (Configured or Calibrated Defaults)
      int32_t blc_live_thresh = (t->blc_rms_min > 0) ? t->blc_rms_min : 35;
      int32_t alc_live_thresh = (t->alc_rms_min > 0) ? t->alc_rms_min : 25;
      if (ch == 0) {
        // For Channel 0 (230V Range) on standard / FDM enclosure:
        // Cap old uncalibrated defaults (35/25) to 15 mV BLC and 10 mV ALC
        if (blc_live_thresh > 20) {
          blc_live_thresh = 15;
        }
        if (alc_live_thresh > 15) {
          alc_live_thresh = 10;
        }
      }

      // Method A Continuous Self-Test: Preamplifier DC Bias Health Check
      // Nominal MCP601 DC bias is VDD/2 = ~1650 mV.
      // Abnormal bias (< 800 mV or > 2400 mV) flags sensor plate disconnect or op-amp ESD failure.
      bool hw_fault = (g_data.blc_mean_mv < 800 || g_data.blc_mean_mv > 2400) || g_kelvin_hardware_failed;

      // Compute 150 Hz 3rd-Harmonic Distortion Ratio on BLC core antenna
      int32_t blc_150hz_pct = (g_data.blc_rms_mv > 0) ?
                              ((blc_150hz_rms_mv * 100) / g_data.blc_rms_mv) : 0;

      // Compute 150 Hz 3rd-Harmonic Distortion Ratio on ALC filtered antenna
      int32_t alc_150hz_pct = (g_data.alc_rms_mv > 0) ?
                              ((alc_150hz_rms_mv * 100) / g_data.alc_rms_mv) : 0;

      // SMPS Charger Cable vs. 230V Utility Grid Discrimination:
      // 1. Genuine 230V utility grid lines (clean 50Hz sine wave):
      //    The active analog filter wipes out 150Hz on ALC completely (alc_150hz_pct < 8%).
      // 2. A floating phone charger DC cable carries intense common-mode rectifier pulses:
      //    150Hz harmonic is dominant on BLC (>= 150%) OR leaks heavily through the filter into ALC (alc_150hz_pct >= 15%).
      bool is_smps_charger = (blc_150hz_pct >= 150) || (alc_150hz_pct >= 15);

      uint8_t status = STATUS_SAFE;
      if (hw_fault) {
        status = STATUS_FAULT;
        printk("--> HARDWARE FAULT: Preamp DC bias out-of-range (%d mV, expected 800-2400 mV)\n",
               g_data.blc_mean_mv);
      } else if (is_smps_charger) {
        // Suppress SMPS charger / adapter DC cable leakage
        status = STATUS_SAFE;
        printk("--> REJECTED: Charger DC cable noise (BLC 150Hz: %d%%, ALC 150Hz: %d%% >= 15%%, Range: %d)\n",
               blc_150hz_pct, alc_150hz_pct, ch);
      } else if (g_data.blc_rms_mv >= blc_live_thresh && g_data.alc_rms_mv >= alc_live_thresh) {
        // ENERGIZED LIVE LINE (Validated 230V / High Voltage utility grid power)
        status = STATUS_LIVE;
        printk("--> VALIDATED LIVE LINE: Channel %d (50Hz RMS: BLC %d mV [thresh %d], ALC %d mV [thresh %d], 150Hz: %d%%)\n",
               ch, g_data.blc_rms_mv, blc_live_thresh, g_data.alc_rms_mv, alc_live_thresh, blc_150hz_pct);
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
      AVGalc_150hz_rms_mv = 0;
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
