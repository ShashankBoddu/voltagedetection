#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stdint.h>

/* ================= Data Micro ================= */
#define SAMPLE_COUNT 200
#define ADC_PERIOD_MS 10
#define ADC_AVG_COUNT 15
#define SAMPLES_PER_PKT 8
#define ADC_LSB_uV 879
// Ratio: (100k + 47k) / 47k = 147 / 47
#define BATTERY_SCALE_MUL 147
#define BATTERY_SCALE_DIV 47

/* ================= Data Enum ================= */
enum pkt_type {
  PKT_BLC = 1,
  PKT_ALC = 2,
  PKT_STATS_1 = 4,
  PKT_STATS_2 = 5,
  PKT_RET_SETTINGS_1 = 6,
  PKT_RET_SETTINGS_2 = 7,
  PKT_END = 0xFF
};

/* ================= Data Struct ================= */
typedef struct {
  int32_t blc_mean_min;
  int32_t blc_mean_max;
  int32_t blc_rms_min;
  int32_t blc_rms_max;

  int32_t alc_mean_min;
  int32_t alc_mean_max;
  int32_t alc_rms_min;
  int32_t alc_rms_max;
} channel_thresholds_t;

typedef struct {
  channel_thresholds_t channels[16];
} thresholds_t;

typedef struct {
  int32_t blc_mean_mv;
  int32_t blc_rms_mv;
  int32_t alc_mean_mv;
  int32_t alc_rms_mv;
  int32_t battery_mv;
  uint8_t battery_percent;
  bool Line_detector_Status;
  uint8_t selected_range;
} data_t;

/* ================= Packet Structs ================= */
typedef struct __packed {
  uint8_t type;         // BLC / ALC / BAT / END
  uint8_t count;        // number of samples
  uint16_t start_index; // sample index
  int16_t samples[SAMPLES_PER_PKT];
} Data_packet;

// Packet 1: Status + Battery + BLC (16 bytes)
typedef struct __packed {
  uint8_t type; // PKT_STATS_1
  uint8_t Line_detector_Status;
  uint8_t battery_percent;
  uint8_t reserved;

  int32_t battery_mv;
  int32_t blc_mean_mV;
  int32_t blc_rms_mV;
} Stats_packet_1;

// Packet 2: ALC (9 bytes)
typedef struct __packed {
  uint8_t type; // PKT_STATS_2
  int32_t alc_mean_mV;
  int32_t alc_rms_mV;
  uint8_t selected_range;
} Stats_packet_2;

// Packet to return Settings Part 1 (BLC)
typedef struct __packed {
  uint8_t type; // PKT_RET_SETTINGS_1
  uint8_t channel_index;
  int32_t blc_mean_min;
  int32_t blc_mean_max;
  int32_t blc_rms_min;
  int32_t blc_rms_max; // Total 1+1+16 = 18 bytes
} Ret_Settings_packet_1;

// Packet to return Settings Part 2 (ALC)
typedef struct __packed {
  uint8_t type; // PKT_RET_SETTINGS_2
  uint8_t channel_index;
  int32_t alc_mean_min;
  int32_t alc_mean_max;
  int32_t alc_rms_min;
  int32_t alc_rms_max; // Total 1+1+16 = 18 bytes
} Ret_Settings_packet_2;

#endif // COMMON_H
