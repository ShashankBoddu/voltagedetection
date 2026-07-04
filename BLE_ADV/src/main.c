#include <stdint.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
/*GATT*/
#include <zephyr/bluetooth/gatt.h>
/*ADC*/
#include <math.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>

/* ================= Data Micro ================= */
#define SAMPLE_COUNT 200
#define ADC_PERIOD_MS 200
#define SAMPLES_PER_PKT 8
#define ADC_LSB_uV 879
// Ratio: 9.37V / 3.117V = 3.006
#define BATTERY_SCALE_MUL 3006
#define BATTERY_SCALE_DIV 1000

/* ================= BLE GATT Micro ================= */
#define BT_UUID_CUSTOM_SERVICE_VAL                                             \
  BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_TX_CHAR_VAL                                                    \
  BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1)

#define BT_UUID_RX_CHAR_VAL                                                    \
  BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef2)

/* RTO Definations */
K_MUTEX_DEFINE(data_mutex);
K_SEM_DEFINE(ble_tx_sem, 0, 1);

/* ================= Data Enum ================= */
enum pkt_type {
  PKT_BLC = 1,
  PKT_ALC = 2,
  // PKT_BAT = 3, // Removed
  PKT_STATS_1 = 4,
  PKT_STATS_2 = 5,
  PKT_END = 0xFF
};
/* ================= Data Enum ================= */
typedef struct {
  int32_t blc_mean_mv;
  int32_t blc_rms_mv;
  int32_t alc_mean_mv;
  int32_t alc_rms_mv;
  int32_t battery_mv;
  uint8_t battery_percent;
  bool Line_detector_Status;
} data_t;
/* ================= Data Struct ================= */
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
} Stats_packet_2;

/* ================= ADC config Struct ================= */
static struct adc_sequence_options seq_opts_200 = {
    .interval_us = 100,
    .extra_samplings = SAMPLE_COUNT - 1,
};

static struct adc_sequence_options seq_opts_2 = {
    .interval_us = 100,
    .extra_samplings = 1,
};
static int16_t adc_blc_buf[SAMPLE_COUNT];
static int16_t adc_alc_buf[SAMPLE_COUNT];
static int16_t adc_bat_buf[2];
static struct adc_sequence seq_blc = {
    .options = &seq_opts_200,
    .channels = BIT(7),
    .buffer = adc_blc_buf,
    .buffer_size = sizeof(adc_blc_buf),
    .resolution = 12,
};

static struct adc_sequence seq_alc = {
    .options = &seq_opts_200,
    .channels = BIT(4),
    .buffer = adc_alc_buf,
    .buffer_size = sizeof(adc_alc_buf),
    .resolution = 12,
};

static struct adc_sequence seq_bat = {
    .options = &seq_opts_2,
    .channels = BIT(5),
    .buffer = adc_bat_buf,
    .buffer_size = sizeof(adc_bat_buf),
    .resolution = 12,
};
/* ================= BLE Struct ================= */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),

    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),

    /* Advertise TX power (static) */
    BT_DATA_BYTES(BT_DATA_TX_POWER, 4), /* +4 dBm */
};
/* ================= Data Variables ================= */
static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
Data_packet pkt;
static data_t g_data;

/* Advertising data */
static struct bt_conn_cb conn_callbacks;
static struct bt_uuid_128 service_uuid =
    BT_UUID_INIT_128(BT_UUID_CUSTOM_SERVICE_VAL);
static struct bt_uuid_128 tx_uuid = BT_UUID_INIT_128(BT_UUID_TX_CHAR_VAL);
static struct bt_uuid_128 rx_uuid = BT_UUID_INIT_128(BT_UUID_RX_CHAR_VAL);
static const struct bt_gatt_attr *tx_attr;
static bool notify_enabled;

/* ================= BLE Function ================= */
/* BLE connection */
static void start_advertising(void) {
  int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), NULL, 0);

  if (err) {
    printk("Advertising start failed (%d)\n", err);
  } else {
    printk("Advertising started\n");
  }
}

static void connected(struct bt_conn *conn, uint8_t err) {
  if (err) {
    printk("BLE connection failed (err %u)\n", err);
    return;
  }

  printk("BLE connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
  printk("BLE disconnected (reason %u)\n", reason);
}
static void recycled() {
  printk("Connection recycled\n");
  start_advertising(); /* ✅ Correct place */
}
static void bt_ready(int err) {
  if (err) {
    printk("Bluetooth init failed (%d)\n", err);
    return;
  }

  printk("Bluetooth initialized\n");
  start_advertising();
}

/* ================= BLE CALLBACKS ================= */

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
  notify_enabled = (value == BT_GATT_CCC_NOTIFY);
  printk("Notify %s\n", notify_enabled ? "ON" : "OFF");
}

/* ================= GATT ================= */
static ssize_t rx_write_cb(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr, const void *buf,
                           uint16_t len, uint16_t offset, uint8_t flags);

BT_GATT_SERVICE_DEFINE(
    custom_svc, BT_GATT_PRIMARY_SERVICE(&service_uuid),

    BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),

    BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    BT_GATT_CHARACTERISTIC(&rx_uuid.uuid, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, rx_write_cb, NULL), );

void ble_send(const char *data, uint16_t len) {
  if (!notify_enabled || !tx_attr) {
    return;
  }
  bt_gatt_notify(NULL, tx_attr, data, len);
}

/* ================= BLE TX THREAD ================= */
/* Local buffers for safe transmission */
static int16_t safe_blc_buf[SAMPLE_COUNT];
static int16_t safe_alc_buf[SAMPLE_COUNT];

void ble_tx_thread_fn(void) {
  while (1) {
    k_sem_take(&ble_tx_sem, K_FOREVER);

    printk("Starting BLE Transfer...\n");

    /* 1. Capture Data Snapshot */
    k_mutex_lock(&data_mutex, K_FOREVER);
    g_data.Line_detector_Status = !g_data.Line_detector_Status;
    memcpy(safe_blc_buf, adc_blc_buf, sizeof(adc_blc_buf));
    memcpy(safe_alc_buf, adc_alc_buf, sizeof(safe_alc_buf));
    data_t safe_data = g_data; // Copy stats
    k_mutex_unlock(&data_mutex);

    uint16_t pkt_len = 0;

    /* ----------- BLC ----------- */
    for (int i = 0; i < SAMPLE_COUNT; i += SAMPLES_PER_PKT) {
      pkt.type = PKT_BLC;
      pkt.start_index = i;
      pkt.count = MIN(SAMPLES_PER_PKT, SAMPLE_COUNT - i);
      memset(pkt.samples, 0, sizeof(pkt.samples));
      for (int j = 0; j < pkt.count; j++) {
        pkt.samples[j] = safe_blc_buf[i + j];
      }
      pkt_len = offsetof(Data_packet, samples) + pkt.count * sizeof(int16_t);
      ble_send((uint8_t *)&pkt, pkt_len);

      k_sleep(K_MSEC(20)); // ✅ SAFE pacing
    }

    /* ----------- ALC ----------- */
    for (int i = 0; i < SAMPLE_COUNT; i += SAMPLES_PER_PKT) {
      pkt.type = PKT_ALC;
      pkt.start_index = i;
      pkt.count = MIN(SAMPLES_PER_PKT, SAMPLE_COUNT - i);
      memset(pkt.samples, 0, sizeof(pkt.samples));
      for (int j = 0; j < pkt.count; j++) {
        pkt.samples[j] = safe_alc_buf[i + j];
      }

      pkt_len = offsetof(Data_packet, samples) + pkt.count * sizeof(int16_t);
      ble_send((uint8_t *)&pkt, pkt_len);

      k_sleep(K_MSEC(20)); // ✅ SAFE pacing
    }

    /* ----------- BATTERY (Removed) ----------- */
    // pkt.type = PKT_BAT;
    // pkt.start_index = 0;
    // pkt.count = 1;
    // memset(pkt.samples, 0, sizeof(pkt.samples));
    // pkt.samples[0] = safe_data.battery_mv;

    // pkt_len = offsetof(Data_packet, samples) + pkt.count * sizeof(int16_t);
    // ble_send((uint8_t *)&pkt, pkt_len);

    // k_sleep(K_MSEC(20)); // ✅ SAFE

    /* ----------- MEAN & RMS (Split 1) ----------- */
    Stats_packet_1 stats1;
    stats1.type = PKT_STATS_1;
    stats1.reserved = 0;
    stats1.Line_detector_Status = safe_data.Line_detector_Status;
    stats1.battery_percent = safe_data.battery_percent;
    stats1.battery_mv = safe_data.battery_mv;
    stats1.blc_mean_mV = safe_data.blc_mean_mv;
    stats1.blc_rms_mV = safe_data.blc_rms_mv;

    ble_send((uint8_t *)&stats1, sizeof(Stats_packet_1));
    k_sleep(K_MSEC(20));

    /* ----------- MEAN & RMS (Split 2) ----------- */
    Stats_packet_2 stats2;
    stats2.type = PKT_STATS_2;    stats2.alc_mean_mV = safe_data.alc_mean_mv;
    stats2.alc_rms_mV = safe_data.alc_rms_mv;

    ble_send((uint8_t *)&stats2, sizeof(Stats_packet_2));
    k_sleep(K_MSEC(20));

    /* ----------- END ----------- */
    pkt.type = PKT_END;
    pkt.count = 0;
    pkt.start_index = 0;

    pkt_len = offsetof(Data_packet, samples) + pkt.count * sizeof(int16_t);

    ble_send((uint8_t *)&pkt, pkt_len);

    printk("BLE Transfer Complete\n");
  }
}

K_THREAD_DEFINE(ble_thread_id, 2048, ble_tx_thread_fn, NULL, NULL, NULL, 6, 0,
                0);

static ssize_t rx_write_cb(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr, const void *buf,
                           uint16_t len, uint16_t offset, uint8_t flags) {
  char cmd = ((char *)buf)[0];

  printk("RX CMD: %c\n", cmd);
  if (cmd == 'R') {
    k_sem_give(&ble_tx_sem);
  }

  return len;
}

static int init_tx_attr(void) {
  /* TX characteristic value attribute is index 1 */
  tx_attr = &custom_svc.attrs[2];
  return 0;
}

/* ================= ADC ================= */

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

static void calc_mean_rms_mV(int16_t *buf, int count, int32_t *mean_mV,
                             int32_t *rms_mV) {
  int64_t sum = 0;
  int64_t sq = 0;

  /* ---------- First pass: mean ---------- */
  for (int i = 0; i < count; i++) {
    sum += buf[i];
  }

  int32_t mean_counts = sum / count;

  /* ---------- Second pass: AC RMS ---------- */
  for (int i = 0; i < count; i++) {
    int32_t v = buf[i] - mean_counts; // 🔑 DC removed
    sq += (int64_t)v * v;
  }

  int32_t rms_counts = (int32_t)sqrt((double)sq / count);

  /* ---------- Convert to mV ---------- */
  *mean_mV = (mean_counts * ADC_LSB_uV) / 1000;
  *rms_mV = (rms_counts * ADC_LSB_uV) / 1000;
}

/*  ADC THREAD  */

void adc_thread_fn(void) {
  while (!device_is_ready(adc_dev)) {
    k_sleep(K_MSEC(100));
  }
  static uint64_t AVGblc_mean_mv = 0;
  static uint64_t AVGblc_rms_mv = 0;
  static uint64_t AVGalc_mean_mv = 0;
  static uint64_t AVGalc_rms_mv = 0;
  static uint64_t AVGbattery_mv = 0;
  static uint64_t AVGbattery_percent = 0;
  static uint32_t count = 0;
  static uint32_t holdblc_mean_mv = 0;
  static uint32_t holdblc_rms_mv = 0;
  static uint32_t holdalc_mean_mv = 0;
  static uint32_t holdalc_rms_mv = 0;
  static uint32_t battery_mv = 0;
  static uint32_t battery_percent = 0;
  while (1) {
    // printf("ADC %u\n",k_uptime_get_32());
    if (adc_read(adc_dev, &seq_blc) || adc_read(adc_dev, &seq_alc) ||
        adc_read(adc_dev, &seq_bat)) {
      printk("ADC read error\n");
      k_sleep(K_MSEC(100));
      continue;
    }

    k_mutex_lock(&data_mutex, K_FOREVER);

    battery_mv = calc_battery_mv(adc_bat_buf[0]);
    battery_percent = battery_percent_from_mv(battery_mv);
    AVGbattery_mv += battery_mv;
    AVGbattery_percent += battery_percent;

    calc_mean_rms_mV(adc_blc_buf, SAMPLE_COUNT, &holdblc_mean_mv,
                     &holdblc_rms_mv);

    calc_mean_rms_mV(adc_alc_buf, SAMPLE_COUNT, &holdalc_mean_mv,
                     &holdalc_rms_mv);
    AVGblc_mean_mv += holdblc_mean_mv;
    AVGblc_rms_mv += holdblc_rms_mv;
    AVGalc_mean_mv += holdalc_mean_mv;
    AVGalc_rms_mv += holdalc_rms_mv;
    count++;
    if (count == 10) {
      g_data.blc_mean_mv = AVGblc_mean_mv / count;
      g_data.blc_rms_mv = AVGblc_rms_mv / count;
      g_data.alc_mean_mv = AVGalc_mean_mv / count;
      g_data.alc_rms_mv = AVGalc_rms_mv / count;
      g_data.battery_mv = AVGbattery_mv / count;
      g_data.battery_percent = AVGbattery_percent / count;

      AVGblc_mean_mv = 0;
      AVGblc_rms_mv = 0;
      AVGalc_mean_mv = 0;
      AVGalc_rms_mv = 0;
      AVGbattery_mv = 0;
      AVGbattery_percent = 0;
      count = 0;
    }

    k_mutex_unlock(&data_mutex);

    k_sleep(K_MSEC(ADC_PERIOD_MS));
  }
}

K_THREAD_DEFINE(adc_thread, 2048, adc_thread_fn, NULL, NULL, NULL, 5, 0, 0);

SYS_INIT(init_tx_attr, APPLICATION, 90);

int main(void) {
  printk("BLE ADV 2\n");
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
  conn_callbacks.connected = connected,
  conn_callbacks.disconnected = disconnected,
  conn_callbacks.recycled = recycled,

  bt_conn_cb_register(&conn_callbacks);
  printk("BLE ONLY – STATIC ADC DATA\n");

  int err = bt_enable(bt_ready);
  if (err) {
    printk("bt_enable failed (%d)\n", err);
  }
}
