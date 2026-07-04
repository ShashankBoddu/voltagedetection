#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
/*GATT*/
#include <zephyr/bluetooth/gatt.h>

/* ================= STATIC DATA ================= */

#define SAMPLE_COUNT 100

static int16_t adc1_samples[SAMPLE_COUNT];
static int16_t adc2_samples[SAMPLE_COUNT];

static float adc1_mean = 1.23f;
static float adc1_rms  = 1.56f;

static float adc2_mean = 2.01f;
static float adc2_rms  = 2.22f;

static float battery_voltage = 3.72f;
/* Advertising data */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS,
                  BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),

    BT_DATA(BT_DATA_NAME_COMPLETE,
            CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),

    /* Advertise TX power (static) */
    BT_DATA_BYTES(BT_DATA_TX_POWER, 4), /* +4 dBm */
};
static struct bt_conn_cb conn_callbacks;

/*GATT*/
#define BT_UUID_CUSTOM_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define BT_UUID_TX_CHAR_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1)

#define BT_UUID_RX_CHAR_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef2)

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(BT_UUID_CUSTOM_SERVICE_VAL);
static struct bt_uuid_128 tx_uuid      = BT_UUID_INIT_128(BT_UUID_TX_CHAR_VAL);
static struct bt_uuid_128 rx_uuid      = BT_UUID_INIT_128(BT_UUID_RX_CHAR_VAL);
static const struct bt_gatt_attr *tx_attr;

static bool notify_enabled;

/* BLE connection */
static void start_advertising(void)
{
        int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2,
                                  ad, ARRAY_SIZE(ad),
                                  NULL, 0);

        if (err)
        {
                printk("Advertising start failed (%d)\n", err);
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
                printk("BLE connection failed (err %u)\n", err);
                return;
        }

        printk("BLE connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
        printk("BLE disconnected (reason %u)\n", reason);
}

static void recycled()
{
        printk("Connection recycled\n");
        start_advertising(); /* ✅ Correct place */
}

static void bt_ready(int err)
{
        if (err)
        {
                printk("Bluetooth init failed (%d)\n", err);
                return;
        }

        printk("Bluetooth initialized\n");
        start_advertising();
}

/* ================= BLE CALLBACKS ================= */

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	printk("Notify %s\n", notify_enabled ? "ON" : "OFF");
}

static void ble_send(const char *msg)
{
	if (!notify_enabled || !tx_attr) {
		return;
	}

	bt_gatt_notify(NULL, tx_attr, msg, strlen(msg));
}
/* ================= RX COMMAND HANDLER ================= */

static ssize_t rx_write_cb(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   const void *buf,
			   uint16_t len,
			   uint16_t offset,
			   uint8_t flags)
{
	char cmd = ((char *)buf)[0];
	char msg[64];

	printk("RX CMD: %c\n", cmd);

	if (cmd == 'R') {
		ble_send("ADC1_SAMPLES:100\n");
                printf("ADC1_SAMPLES:100\n");
               k_sleep(K_MSEC(10));
		for (int i = 0; i < SAMPLE_COUNT; i++) {
			snprintf(msg, sizeof(msg), "A1,%d,%d\n", i, adc1_samples[i]);
                        printf("%s",msg);
			ble_send(msg);
                       k_sleep(K_MSEC(10));
		}

		snprintf(msg, sizeof(msg),
			"ADC1_MEAN:%.2f\nADC1_RMS:%.2f\n",
			adc1_mean, adc1_rms);
                printf("%s",msg);
		ble_send(msg);
               k_sleep(K_MSEC(10));
		ble_send("ADC2_SAMPLES:100\n");
                printf("ADC2_SAMPLES:100\n");
               k_sleep(K_MSEC(10));
		for (int i = 0; i < SAMPLE_COUNT; i++) {
			snprintf(msg, sizeof(msg), "A2,%d,%d\n", i, adc2_samples[i]);
                        printf("%s",msg);
			ble_send(msg);
                       k_sleep(K_MSEC(10));
		}

		snprintf(msg, sizeof(msg),
			"ADC2_MEAN:%.2f\nADC2_RMS:%.2f\n",
			adc2_mean, adc2_rms);
                printf("%s",msg);
		ble_send(msg);
               k_sleep(K_MSEC(10));

		snprintf(msg, sizeof(msg),
			"BATTERY:%.2f\nEND\n", battery_voltage);
                printf("%s",msg);
		ble_send(msg);
               k_sleep(K_MSEC(10));
	}

	return len;
}

/* ================= GATT ================= */

BT_GATT_SERVICE_DEFINE(custom_svc,
	BT_GATT_PRIMARY_SERVICE(&service_uuid),

	BT_GATT_CHARACTERISTIC(&tx_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),

	BT_GATT_CCC(ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&rx_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, rx_write_cb, NULL),
);


void ble_send_data(const char *data, uint16_t len)
{
	if (!notify_enabled) {
		return;
	}
	bt_gatt_notify(NULL, &custom_svc.attrs[1], data, len);
}

static int init_tx_attr(void)
{
	/* TX characteristic value attribute is index 1 */
	tx_attr = &custom_svc.attrs[1];
	return 0;
}

SYS_INIT(init_tx_attr, APPLICATION, 90);

int main(void)
{
        printk("BLE ADV 1\n");
        
        conn_callbacks.connected = connected,
        conn_callbacks.disconnected = disconnected,
        conn_callbacks.recycled = recycled,

        bt_conn_cb_register(&conn_callbacks);
	printk("BLE ONLY – STATIC ADC DATA\n");

	for (int i = 0; i < SAMPLE_COUNT; i++) {
		adc1_samples[i] = 1200 + i;
		adc2_samples[i] = 2200 + i;
	}

	int err = bt_enable(bt_ready);
	if (err) {
		printk("bt_enable failed (%d)\n", err);
	}
}
