#ifndef ADC_BACKEND_H
#define ADC_BACKEND_H

#include "../common.h"
#include <zephyr/kernel.h>
void adc_param_init(void);
void adc_thread_fn(void *arg1, void *arg2, void *arg3);

extern int16_t adc_blc_buf[SAMPLE_COUNT];
extern int16_t adc_alc_buf[SAMPLE_COUNT];
extern data_t g_data;
extern thresholds_t g_thresholds;

void adc_get_snapshot(data_t *p_data, int16_t *p_blc, int16_t *p_alc);
void buzzer_set(bool on);
void annunciation_boot_selftest(void);
typedef struct {
  bool passed;
  int32_t reported_rms_mv;   // BLC Active RMS
  int32_t baseline_rms_mv;   // BLC Baseline RMS (before toggle)
  int32_t delta_rms_mv;      // BLC Net Injected Delta RMS
  int32_t delta_p2p_mv;      // BLC Net Injected Delta Peak-to-Peak (Step Spike)
  int32_t delta_100hz_mv;    // BLC Net Injected 100Hz Goertzel Lock-In Delta
  int32_t mean_mv;           // BLC DC Bias
  int32_t p2p_mv;            // BLC Peak-to-Peak
  int32_t freq_100hz_rms_mv; // BLC Active 100Hz Goertzel Lock-In RMS
  int32_t alc_rms_mv;        // ALC Active RMS
  int32_t alc_mean_mv;       // ALC DC Bias
  int32_t alc_p2p_mv;        // ALC Peak-to-Peak
} selftest_result_t;

bool hardware_kelvin_selftest(selftest_result_t *out_result);

#endif
