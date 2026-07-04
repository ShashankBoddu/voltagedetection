#include <errno.h>
#include <stdio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include "../adc_lib/adc_backend.h"
#include "../range_lib/range_backend.h"
#include "../storage_lib/storage_backend.h"
#include "ble_backend.h"

/* ================= BLE GATT Micro ================= */
#define BT_UUID_CUSTOM_SERVICE_VAL                                             \
  BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_TX_CHAR_VAL                                                    \
  BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1)

#define BT_UUID_RX_CHAR_VAL                                                    \
  BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef2)

/* SMP Service UUID: 8d53dc1d-1db7-4cd3-868b-8a527460aa84 */
#define BT_UUID_SMP_VAL                                                        \
  BT_UUID_128_ENCODE(0x8d53dc1d, 0x1db7, 0x4cd3, 0x868b, 0x8a527460aa84)

K_SEM_DEFINE(ble_tx_sem, 0, 1);

/* ================= BLE Variables ================= */
static struct bt_uuid_128 service_uuid =
    BT_UUID_INIT_128(BT_UUID_CUSTOM_SERVICE_VAL);
static struct bt_uuid_128 tx_uuid = BT_UUID_INIT_128(BT_UUID_TX_CHAR_VAL);
static struct bt_uuid_128 rx_uuid = BT_UUID_INIT_128(BT_UUID_RX_CHAR_VAL);
static const struct bt_gatt_attr *tx_attr;
static bool notify_enabled;
static bool send_full_data = false;
static const struct gpio_dt_spec ble_led_spec =
    GPIO_DT_SPEC_GET(DT_NODELABEL(blemode), gpios);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_SMP_VAL),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static struct bt_conn_cb conn_callbacks;
static struct bt_conn *current_conn;

/* ================= BLE Helper Functions ================= */
static void start_advertising(void) {
  int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd,
                            ARRAY_SIZE(sd));
  if (err) {
    printk("Advertising start failed (%d)\n", err);
  } else {
    printk("Advertising started\n");
  }
  gpio_pin_set_dt(&ble_led_spec, 0);
}

static void connected(struct bt_conn *conn, uint8_t err) {
  if (err) {
    printk("BLE connection failed (err %u)\n", err);
    return;
  }
  printk("BLE connected\n");

  gpio_pin_set_dt(&ble_led_spec, 1);

  if (current_conn) {
    bt_conn_unref(current_conn);
  }
  current_conn = bt_conn_ref(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
  printk("BLE disconnected (reason %u)\n", reason);
  gpio_pin_set_dt(&ble_led_spec, 0);

  if (current_conn == conn) {
    bt_conn_unref(current_conn);
    current_conn = NULL;
  }
}

static void recycled(void) {
  printk("Connection recycled\n");
  start_advertising();
}

static void bt_ready(int err) {
  if (err) {
    printk("Bluetooth init failed (%d)\n", err);
    return;
  }
  printk("Bluetooth initialized\n");
  int settings_err = settings_load();
  printk("Settings load result: %d\n", settings_err);
  start_advertising();
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
  notify_enabled = (value == BT_GATT_CCC_NOTIFY);
  printk("Notify %s\n", notify_enabled ? "ON" : "OFF");
}

void ble_send(const char *data, uint16_t len);

static ssize_t rx_write_cb(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr, const void *buf,
                           uint16_t len, uint16_t offset, uint8_t flags) {
  uint8_t *data = (uint8_t *)buf;
  char cmd = data[0];
  printk("RX CMD: %c\n", cmd);

  if (cmd == 'R') {
    send_full_data = true;
    k_sem_give(&ble_tx_sem);
  } else if (cmd == 'S') {
    if (len <= 2) { // Save command (handle 'S' or 'S\n' or 'S\r')
      storage_save_thresholds(&g_thresholds);
      printk("Saved thresholds to Flash\n");
    } else {
      send_full_data = false;
      range_process_cmd(buf, len);
      k_sem_give(&ble_tx_sem);
    }
  } else if (cmd == 'T' && (len == 7 || len == 8 || len == 9)) {
    // Handle variable length 'T' packets if they include newlines
    uint8_t channel = data[1];
    uint8_t index = data[2];
    int32_t value = sys_get_le32(&data[3]);

    if (channel < 16 && index < 8) {
      int32_t *p_params = (int32_t *)&g_thresholds.channels[channel];
      p_params[index] = value;
      printk("Updated Channel %d Param %d to %d\n", channel, index, value);
    } else {
      printk("Invalid Channel %d or Param %d\n", channel, index);
    }
  } else if (cmd == 'L' && len == 2) {
    uint8_t channel = data[1];
    if (channel < 16) {
      channel_thresholds_t *t = &g_thresholds.channels[channel];

      Ret_Settings_packet_1 p1;
      p1.type = PKT_RET_SETTINGS_1;
      p1.channel_index = channel;
      p1.blc_mean_min = t->blc_mean_min;
      p1.blc_mean_max = t->blc_mean_max;
      p1.blc_rms_min = t->blc_rms_min;
      p1.blc_rms_max = t->blc_rms_max;
      ble_send((uint8_t *)&p1, sizeof(p1));

      k_sleep(K_MSEC(20));

      Ret_Settings_packet_2 p2;
      p2.type = PKT_RET_SETTINGS_2;
      p2.channel_index = channel;
      p2.alc_mean_min = t->alc_mean_min;
      p2.alc_mean_max = t->alc_mean_max;
      p2.alc_rms_min = t->alc_rms_min;
      p2.alc_rms_max = t->alc_rms_max;
      ble_send((uint8_t *)&p2, sizeof(p2));

      printk("Sent Settings for Channel %d\n", channel);
    } else {
      printk("Invalid Channel %d for Read\n", channel);
    }
  } else if (cmd == 'C') {
    // Relaxed 'C' command (Continuous status)
    send_full_data = false;
    k_sem_give(&ble_tx_sem);
  }
  return len;
}

BT_GATT_SERVICE_DEFINE(
    custom_svc, BT_GATT_PRIMARY_SERVICE(&service_uuid),
    BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_NONE, NULL, NULL, NULL),
    BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(&rx_uuid.uuid, BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE, NULL, rx_write_cb, NULL), );

static int init_tx_attr(void) {
  tx_attr = &custom_svc.attrs[2];
  return 0;
}

void ble_send(const char *data, uint16_t len) {
  if (!notify_enabled || !tx_attr) {
    return;
  }
  bt_gatt_notify(NULL, tx_attr, data, len);
}

/* ================= Public Functions ================= */
bool ble_is_connected(void) { return (current_conn != NULL); }

static void pairing_complete(struct bt_conn *conn, bool bonded) {
  printk("Pairing complete (bonded: %s)\n", bonded ? "yes" : "no");
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason) {
  printk("Pairing failed (reason %d)\n", reason);
}

static void bond_deleted(uint8_t id, const bt_addr_le_t *peer) {
  printk("Bond deleted\n");
}

static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed,
    .bond_deleted = bond_deleted,
};

void ble_param_init(void) {
  conn_callbacks.connected = connected;
  conn_callbacks.disconnected = disconnected;
  conn_callbacks.recycled = recycled;
  bt_conn_cb_register(&conn_callbacks);
  bt_conn_auth_info_cb_register(&auth_info_cb);

  init_tx_attr();

  int err = bt_enable(bt_ready);
  if (err) {
    printk("bt_enable failed (%d)\n", err);
  }

  if (gpio_is_ready_dt(&ble_led_spec)) {
    gpio_pin_configure_dt(&ble_led_spec, GPIO_OUTPUT_INACTIVE);
  }

}

/* ================= TX Thread ================= */
static int16_t safe_blc_buf[SAMPLE_COUNT];
static int16_t safe_alc_buf[SAMPLE_COUNT];
static Data_packet pkt;

extern struct k_sem boot_done_sem;

void ble_tx_thread_fn(void *arg1, void *arg2, void *arg3) {
  k_sem_take(&boot_done_sem, K_FOREVER);
  while (1) {
    k_sem_take(&ble_tx_sem, K_FOREVER);

    printk("Starting BLE Transfer...\n");

    /* 1. Capture Data Snapshot from ADC Library */
    data_t safe_data;
    adc_get_snapshot(&safe_data, safe_blc_buf, safe_alc_buf);

    uint16_t pkt_len = 0;

    /* ----------- BLC ----------- */
    if (send_full_data) {
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
        k_sleep(K_MSEC(20));
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
        k_sleep(K_MSEC(20));
      }
    }

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

    /* ----------- MEAN & RMS (Split 2) ----------- */
    Stats_packet_2 stats2;
    stats2.type = PKT_STATS_2;
    stats2.alc_mean_mV = safe_data.alc_mean_mv;
    stats2.alc_rms_mV = safe_data.alc_rms_mv;
    stats2.selected_range = safe_data.selected_range;
    if (send_full_data) {
      printk("Channel %d\n", safe_data.selected_range);
    }

    ble_send((uint8_t *)&stats2, sizeof(Stats_packet_2));

    /* ----------- END ----------- */
    pkt.type = PKT_END;
    pkt.count = 0;
    pkt.start_index = 0;
    pkt_len = offsetof(Data_packet, samples) + pkt.count * sizeof(int16_t);

    ble_send((uint8_t *)&pkt, pkt_len);
    printk("BLE Transfer Complete\n");
  }
}
