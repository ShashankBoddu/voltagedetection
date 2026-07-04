#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
/*GATT*/
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
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

static struct bt_uuid_128 custom_service_uuid = BT_UUID_INIT_128(BT_UUID_CUSTOM_SERVICE_VAL);
static struct bt_uuid_128 tx_char_uuid       = BT_UUID_INIT_128(BT_UUID_TX_CHAR_VAL);
static struct bt_uuid_128 rx_char_uuid       = BT_UUID_INIT_128(BT_UUID_RX_CHAR_VAL);

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

/*GATT Write and Read Callback*/
static ssize_t rx_write_cb(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   const void *buf,
			   uint16_t len,
			   uint16_t offset,
			   uint8_t flags)
{
	printk("RX (%d bytes): ", len);

	for (int i = 0; i < len; i++) {
		printk("%c", ((uint8_t *)buf)[i]);
	}
	printk("\n");

	/* Example: react to command */
	if (((uint8_t *)buf)[0] == '1') {
		printk("Command ON received\n");
	}

	return len;
}

static void tx_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
        notify_enabled = (value == BT_GATT_CCC_NOTIFY);
        printk("Notify %s\n", notify_enabled ? "ON" : "OFF");
}



BT_GATT_SERVICE_DEFINE(custom_svc,
	BT_GATT_PRIMARY_SERVICE(&custom_service_uuid),

	/* TX Characteristic (MCU → Mobile) */
	BT_GATT_CHARACTERISTIC(&tx_char_uuid.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),

	BT_GATT_CCC(tx_ccc_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* RX Characteristic (Mobile → MCU) */
	BT_GATT_CHARACTERISTIC(&rx_char_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
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

int main(void)
{
        printk("AC Detector BLE 123\n");
        
        conn_callbacks.connected = connected,
        conn_callbacks.disconnected = disconnected,
        conn_callbacks.recycled = recycled,

        bt_conn_cb_register(&conn_callbacks);
        int err = bt_enable(bt_ready);
        if (err)
        {
                printk("bt_enable failed (%d)\n", err);
        }

        while (1)
        {
                ble_send_data("Hello from MCU\n", 16);
                k_sleep(K_SECONDS(1));
        }
}
