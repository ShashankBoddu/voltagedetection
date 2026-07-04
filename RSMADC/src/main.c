#include <math.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define SAMPLE_COUNT 200

static int16_t adc_buffer_BLC[SAMPLE_COUNT];
static int16_t adc_buffer_ALC[SAMPLE_COUNT];
static int16_t adc_buffer_BAT[2];

static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
;

static struct adc_sequence_options seq_opts_BLCALC = {
    .interval_us = 100,
    .extra_samplings = SAMPLE_COUNT - 1,
};

static struct adc_sequence_options seq_opts_BAT = {
    .interval_us = 100,
    .extra_samplings = 2 - 1,
};
static struct adc_sequence sequence_BLC = {
    .options = &seq_opts_BLCALC,
    .channels = BIT(7), /* channel@2 */
    .buffer = adc_buffer_BLC,
    .buffer_size = sizeof(adc_buffer_BLC),
    .resolution = 12,
};

static struct adc_sequence sequence_ALC = {
    .options = &seq_opts_BLCALC,
    .channels = BIT(4), /* channel@3 */
    .buffer = adc_buffer_ALC,
    .buffer_size = sizeof(adc_buffer_ALC),
    .resolution = 12,
};

static struct adc_sequence sequence_BAT = {
    .options = &seq_opts_BAT,
    .channels = BIT(5), /* channel@1 */
    .buffer = adc_buffer_BAT,
    .buffer_size = sizeof(adc_buffer_BAT),
    .resolution = 12,
};

float calculate_rms(int16_t *buf, uint32_t len) {
  float mean = 0, sum_sq = 0;
  const float lsb = (0.6f * 6.0f) / 4096.0f;

  for (uint32_t i = 0; i < len; i++)
    mean += buf[i];
  mean /= len;

  for (uint32_t i = 0; i < len; i++) {
    float v = (buf[i] - mean) * lsb;
    sum_sq += v * v;
  }
  return sqrtf(sum_sq / len);
}

int main(void) {
  printk("SAADC RMS start\n");

  if (!device_is_ready(adc_dev)) {
    printk("ADC not ready\n");
    return 0;
  }

  adc_channel_setup(adc_dev, &(struct adc_channel_cfg)ADC_CHANNEL_CFG_DT(
                                 DT_NODELABEL(beforelcch)));
  adc_channel_setup(adc_dev, &(struct adc_channel_cfg)ADC_CHANNEL_CFG_DT(
                                 DT_NODELABEL(afterlcch)));
  adc_channel_setup(adc_dev, &(struct adc_channel_cfg)ADC_CHANNEL_CFG_DT(
                                 DT_NODELABEL(batterych)));

  while (1) {

    /*Befor LC Filter*/
    printk("ADC reading Befor LC Filter\n");
    adc_read(adc_dev, &sequence_BLC);
    for (uint16_t i = 0; i < SAMPLE_COUNT; i++) {
      printk("%d,", adc_buffer_BLC[i]);
    }
    printk("\n");

    float Vrms_BLC = calculate_rms(adc_buffer_BLC, SAMPLE_COUNT);
    printk("RMS = %.3f V\n", Vrms_BLC);
    k_sleep(K_MSEC(200));

    /*After LC Filter*/
    printk("ADC reading After LC Filter\n");
    adc_read(adc_dev, &sequence_ALC);
    for (uint16_t i = 0; i < SAMPLE_COUNT; i++) {
      printk("%d,", adc_buffer_ALC[i]);
    }
    printk("\n");
    printk("ADC reading Battery voltage\n");
    float Vrms_ALC = calculate_rms(adc_buffer_ALC, SAMPLE_COUNT);
    printk("RMS = %.3f V\n", Vrms_ALC);
    k_sleep(K_MSEC(200));

    /*Battery voltage*/
    adc_read(adc_dev, &sequence_BAT);
    for (uint16_t i = 0; i < 2; i++) {
      printk("%d,", adc_buffer_BAT[i]);
    }
    printk("\n");

    float BatteryVoltage = (adc_buffer_BAT[0] + adc_buffer_BAT[1]) / 2;
    printk("RMS = %.3f V\n", BatteryVoltage);
    k_sleep(K_MSEC(20000));
  }
}
