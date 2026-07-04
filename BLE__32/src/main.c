#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

/* Simple advertising data */
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

void main(void)
{
        int err;

        printk("BLE minimal test start\n");
        conn_callbacks.connected = connected,
        conn_callbacks.disconnected = disconnected,
        conn_callbacks.recycled = recycled,
        bt_conn_cb_register(&conn_callbacks);
        err = bt_enable(bt_ready);
        if (err)
        {
                printk("bt_enable failed (%d)\n", err);
        }

        printk("Bluetooth enabled\n");

        while (1)
        {
                k_sleep(K_SECONDS(1));
                printk("1,");
        }
}
