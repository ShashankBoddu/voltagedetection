#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/atomic.h>

#include <math.h>

/* ================= CONFIG ================= */
#define SAMPLE_COUNT 200
#define BATTERY_SCALE 2
#define ADC_PERIOD_MS 500
#define SAMPLES_PER_PKT 16
/* ================= DATA ================= */
typedef struct
{
        int32_t blc_mean_mv;
        int32_t blc_rms_mv;
        int32_t alc_mean_mv;
        int32_t alc_rms_mv;
        int32_t battery_mv;
} data_t;
static data_t g_data;
/* ================= ADC ================= */
static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));

enum pkt_type
{
        PKT_BLC = 1,
        PKT_ALC = 2,
        PKT_BAT = 3,
        PKT_END = 0xFF
};

struct adc_packet
{
        uint8_t type;         // BLC / ALC / BAT / END
        uint8_t count;        // number of samples
        uint16_t start_index; // sample index
        int16_t samples[SAMPLES_PER_PKT];
} __packed;

static int16_t adc_blc_buf[SAMPLE_COUNT];
static int16_t adc_alc_buf[SAMPLE_COUNT];
static int16_t adc_bat_buf[2];

static struct adc_sequence_options seq_opts_200 = {
    .interval_us = 100,
    .extra_samplings = SAMPLE_COUNT - 1,
};

static struct adc_sequence_options seq_opts_2 = {
    .interval_us = 100,
    .extra_samplings = 1,
};

static struct adc_sequence seq_blc = {
    .options = &seq_opts_200,
    .channels = BIT(7),
    .buffer = adc_blc_buf,
    .buffer_size = sizeof(adc_blc_buf),
    .resolution = 12,
};

static struct adc_sequence seq_alc = {
    .options = &seq_opts_200,
    .channels = BIT(6),
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

/* ================= SYNC ================= */

K_MUTEX_DEFINE(data_mutex);

/* ================= UTILS ================= */

static void calc_mean_rms(int16_t *buf, int count,
                          int32_t *mean, int32_t *rms)
{
        int64_t sum = 0;
        int64_t sq = 0;

        for (int i = 0; i < count; i++)
        {
                int32_t v = buf[i];
                sum += v;
                sq += (int64_t)v * v;
        }

        *mean = sum / count;
        *rms = (int32_t)sqrt((double)sq / count);
}

/* ================= ADC THREAD ================= */

void adc_thread_fn(void)
{
        while (!device_is_ready(adc_dev))
        {
                k_sleep(K_MSEC(100));
        }

        while (1)
        {
                // printf("ADC %u\n",k_uptime_get_32());
                if (adc_read(adc_dev, &seq_blc) ||
                    adc_read(adc_dev, &seq_alc) ||
                    adc_read(adc_dev, &seq_bat))
                {
                        printk("ADC read error\n");
                        k_sleep(K_MSEC(100));
                        continue;
                }

                k_mutex_lock(&data_mutex, K_FOREVER);

                calc_mean_rms(adc_blc_buf, SAMPLE_COUNT,
                              &g_data.blc_mean_mv,
                              &g_data.blc_rms_mv);

                calc_mean_rms(adc_alc_buf, SAMPLE_COUNT,
                              &g_data.alc_mean_mv,
                              &g_data.alc_rms_mv);

                g_data.battery_mv =
                    ((adc_bat_buf[0] + adc_bat_buf[1]) / 2) * BATTERY_SCALE;
                // printf("En %u\n",k_uptime_get_32());
                k_mutex_unlock(&data_mutex);

                k_sleep(K_MSEC(ADC_PERIOD_MS));
        }
}

K_THREAD_DEFINE(adc_thread, 2048,
                adc_thread_fn, NULL, NULL, NULL,
                5, 0, 0);

/* ================= BLE ================= */
static struct k_sem tx_sem;
static struct bt_conn *current_conn;
static struct bt_conn_cb conn_callbacks;
static bool notify_enabled;
static const struct bt_gatt_attr *tx_attr;
static atomic_t tx_busy;
static atomic_t att_ready;

/* UUIDs */
#define BT_UUID_CUSTOM_SERVICE_VAL \
        BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF0)

#define BT_UUID_TX_CHAR_VAL \
        BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF1)

#define BT_UUID_RX_CHAR_VAL \
        BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF2)

static struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(BT_UUID_CUSTOM_SERVICE_VAL);
static struct bt_uuid_128 tx_uuid = BT_UUID_INIT_128(BT_UUID_TX_CHAR_VAL);
static struct bt_uuid_128 rx_uuid = BT_UUID_INIT_128(BT_UUID_RX_CHAR_VAL);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS,
                  BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE,
            CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
        printk("MTU updated: TX=%u RX=%u\n", tx, rx);

        if (rx > 23)
        {
                atomic_set(&att_ready, 1);
        }
}

static struct bt_gatt_cb gatt_cb = {
    .att_mtu_updated = mtu_updated,
};

static void notify_cb(struct bt_conn *conn, void *user_data)
{
        k_sem_give(&tx_sem);
}
/* ================= ADV ================= */

static void start_advertising(void)
{
        int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2,
                                  ad, ARRAY_SIZE(ad),
                                  NULL, 0);

        if (err && err != -EALREADY)
        {
                printk("Advertising failed (%d)\n", err);
        }
        else
        {
                printk("Advertising started\n");
        }
}

static void connected(struct bt_conn *conn, uint8_t err)
{
        if (err)
        {
                printk("BLE connect failed (%u)\n", err);
                return;
        }

        current_conn = bt_conn_ref(conn);
        printk("BLE connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
        printk("BLE disconnected (%u)\n", reason);

        if (current_conn)
        {
                bt_conn_unref(current_conn);
                current_conn = NULL;
        }

        notify_enabled = false;
        atomic_set(&tx_busy, 0);

        k_sleep(K_MSEC(200));
        start_advertising();
}

static void bt_ready(int err)
{
        if (err)
        {
                printk("Bluetooth init failed (%d)\n", err);
                return;
        }
        printk("Bluetooth ready\n");
        start_advertising();
}

/* ================= TX WORK ================= */

static struct k_work tx_work;

static void tx_work_fn(struct k_work *work)
{
        struct adc_packet pkt;
        int err;

        if (!notify_enabled || !tx_attr || !current_conn)
        {
                atomic_set(&tx_busy, 0);
                return;
        }
        if (!atomic_get(&att_ready))
        {
                printk("ATT not ready yet\n");
                atomic_set(&tx_busy, 0);
                return;
        }
        k_mutex_lock(&data_mutex, K_FOREVER);
        size_t pkt_len = 1;
        struct bt_gatt_notify_params params = {
            .attr = tx_attr,
            .data = &pkt,
            .len = pkt_len,
            .func = notify_cb,
        };
        /* ----------- BLC ----------- */
        for (int i = 0; i < SAMPLE_COUNT; i += SAMPLES_PER_PKT)
        {
                pkt.type = PKT_BLC;
                pkt.start_index = i;
                pkt.count = MIN(SAMPLES_PER_PKT, SAMPLE_COUNT - i);

                for (int j = 0; j < pkt.count; j++)
                {
                        pkt.samples[j] = adc_blc_buf[i + j];
                }
                pkt_len = offsetof(struct adc_packet, samples) + pkt.count * sizeof(int16_t);
                k_sem_take(&tx_sem, K_FOREVER);
                params.len = pkt_len;
                err = bt_gatt_notify_cb(current_conn, &params);
                if (err)
                {
                        printk("notify failed: %d\n", err);
                        k_sem_give(&tx_sem);
                        goto out;
                }

                // k_sleep(K_MSEC(10)); // ✅ SAFE pacing
        }

        /* ----------- ALC ----------- */
        for (int i = 0; i < SAMPLE_COUNT; i += SAMPLES_PER_PKT)
        {
                pkt.type = PKT_ALC;
                pkt.start_index = i;
                pkt.count = MIN(SAMPLES_PER_PKT, SAMPLE_COUNT - i);

                for (int j = 0; j < pkt.count; j++)
                {
                        pkt.samples[j] = adc_alc_buf[i + j];
                }

                pkt_len = offsetof(struct adc_packet, samples) + pkt.count * sizeof(int16_t);

                k_sem_take(&tx_sem, K_FOREVER);
                params.len = pkt_len;
                err = bt_gatt_notify_cb(current_conn, &params);
                if (err)
                {
                        printk("notify failed: %d\n", err);
                        k_sem_give(&tx_sem);
                        goto out;
                }

                // k_sleep(K_MSEC(10));
        }

        /* ----------- BATTERY ----------- */
        pkt.type = PKT_BAT;
        pkt.start_index = 0;
        pkt.count = 1;
        pkt.samples[0] = g_data.battery_mv;

        pkt_len = offsetof(struct adc_packet, samples) + pkt.count * sizeof(int16_t);

        k_sem_take(&tx_sem, K_FOREVER);
        params.len = pkt_len;
        err = bt_gatt_notify_cb(current_conn, &params);
        if (err)
        {
                printk("notify failed: %d\n", err);
                k_sem_give(&tx_sem);
                goto out;
        }
        // k_sleep(K_MSEC(10));
        /* ----------- END ----------- */
        pkt.type = PKT_END;
        pkt.count = 0;
        pkt.start_index = 0;

        pkt_len = offsetof(struct adc_packet, samples) + pkt.count * sizeof(int16_t);

        k_sem_take(&tx_sem, K_FOREVER);
        params.len = pkt_len;
        err = bt_gatt_notify_cb(current_conn, &params);
        if (err)
        {
                printk("notify failed: %d\n", err);
                k_sem_give(&tx_sem);
                goto out;
        }

out:
        k_mutex_unlock(&data_mutex);
        atomic_set(&tx_busy, 0);
}

/* ================= RX ================= */

static ssize_t rx_write_cb(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           const void *buf,
                           uint16_t len,
                           uint16_t offset,
                           uint8_t flags)
{
        if (((char *)buf)[0] == 'R')
        {
                if (notify_enabled &&
                    atomic_get(&att_ready) &&
                    atomic_cas(&tx_busy, 0, 1))
                {
                        k_work_submit(&tx_work);
                }
        }

        return len;
}

/* ================= GATT ================= */

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
        notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

BT_GATT_SERVICE_DEFINE(custom_svc,
                       BT_GATT_PRIMARY_SERVICE(&svc_uuid),
                       BT_GATT_CHARACTERISTIC(&tx_uuid.uuid,
                                              BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_NONE,
                                              NULL, NULL, NULL),
                       BT_GATT_CCC(ccc_changed,
                                   BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
                       BT_GATT_CHARACTERISTIC(&rx_uuid.uuid,
                                              BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_WRITE,
                                              NULL, rx_write_cb, NULL));

static int init_tx_attr(void)
{
        tx_attr = &custom_svc.attrs[2];
        k_work_init(&tx_work, tx_work_fn);
        return 0;
}

SYS_INIT(init_tx_attr, APPLICATION, 90);

/* ================= MAIN ================= */

int main(void)
{
        printk("AC Detector BLE + ADC\n");

        if (!device_is_ready(adc_dev))
        {
                printk("ADC not ready\n");
                return 0;
        }

        adc_channel_setup(adc_dev,
                          &(struct adc_channel_cfg)
                              ADC_CHANNEL_CFG_DT(DT_NODELABEL(beforelcch)));

        adc_channel_setup(adc_dev,
                          &(struct adc_channel_cfg)
                              ADC_CHANNEL_CFG_DT(DT_NODELABEL(afterlcch)));

        adc_channel_setup(adc_dev,
                          &(struct adc_channel_cfg)
                              ADC_CHANNEL_CFG_DT(DT_NODELABEL(batterych)));

        conn_callbacks.connected = connected;
        conn_callbacks.disconnected = disconnected;
        bt_conn_cb_register(&conn_callbacks);
        bt_gatt_cb_register(&gatt_cb);
        k_sem_init(&tx_sem, 1, 1);

        int err = bt_enable(bt_ready);
        if (err)
        {
                printk("bt_enable failed (%d)\n", err);
        }

        return 0;
}
