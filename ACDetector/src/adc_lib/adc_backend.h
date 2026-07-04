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

#endif
