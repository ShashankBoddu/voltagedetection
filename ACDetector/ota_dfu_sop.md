# SOP: Stable OTA DFU over BLE (v1.0)

This document provides a standard configuration and workflow for implementing Over-The-Air (OTA) firmware updates using **MCUboot** and **MCUmgr** in Zephyr-based projects (specifically nRF Connect SDK).

## 1. Project Configuration (`prj.conf`)

Add these blocks to your `prj.conf`. Note that larger stacks and buffers are required for the update process.

### A. Core OTA & Management
```conf
# Enable Bootloader & Management
CONFIG_BOOTLOADER_MCUBOOT=y
CONFIG_MCUBOOT_BOOTUTIL_LIB=y
CONFIG_MCUMGR=y
CONFIG_MCUMGR_TRANSPORT_BT=y
CONFIG_MCUMGR_GRP_IMG=y
CONFIG_MCUMGR_GRP_OS=y

# Memory Management (Critical for DFU stability)
CONFIG_STREAM_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_IMG_MANAGER=y
CONFIG_MCUBOOT_IMG_MANAGER=y
```

### B. Bluetooth Stability & Performance
Essential for handling the large data bursts during firmware upload.
```conf
# Large MTU & Data Length (Fast transfers)
CONFIG_BT_L2CAP_TX_MTU=247
CONFIG_BT_BUF_ACL_RX_SIZE=251
CONFIG_BT_BUF_ACL_TX_SIZE=251
CONFIG_BT_CTLR_DATA_LENGTH_MAX=251

# MCUmgr Transport Tweak (Critical for multi-image lists)
CONFIG_MCUMGR_TRANSPORT_BT_REASSEMBLY=y
CONFIG_MCUMGR_TRANSPORT_NETBUF_COUNT=4
CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE=512

# iOS Stability (Crucial)
CONFIG_BT_GATT_SERVICE_CHANGED=y
```

### C. Resource Requirements
OTA requires more RAM and Stack than a basic app.
```conf
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=3072
CONFIG_BT_RX_STACK_SIZE=1536
```

## 2. Source Code Requirements

### Advertising Data
You must include the **SMP Service UUID** in your advertising or scan response data so update tools can find the device.

```c
/* SMP Service UUID: 8d53dc1d-1db7-4cd3-868b-8a527460aa84 */
#define BT_UUID_SMP_VAL BT_UUID_128_ENCODE(0x8d53dc1d, 0x1db7, 0x4cd3, 0x868b, 0x8a527460aa84)

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_SMP_VAL), // Mandatory for DFU detection
};
```

### Initialization
Modern NCS versions (3.0+) auto-register MCUmgr groups, but ensure `bt_enable()` and `settings_load()` are called correctly.

## 3. Build & Flash
Always use the **sysbuild** flag. 

> [!IMPORTANT]
> **Sysbuild is the "Orchestrator"**: In modern NCS (v2.x/v3.x), if you select "No sysbuild", the compiler only builds your App. It will **not** build MCUboot and it will **not** sign your binary (meaning no `zephyr.signed.bin`).

```powershell
west build -b your_board_name --sysbuild -p always
```
*   **Result**: Use `build/zephyr/merged.hex` for the initial wired flash.
*   **OTA File (Sysbuild)**: Use `build/ACDetector/zephyr/zephyr.signed.bin` (or `app_update.bin` in non-sysbuild) for the wireless update.

## 4. The Swap & Confirm Logic
After the upload hits 100%, the device has the new code in the "Secondary Slot," but it **will not run it** until prompted.

1.  **Mark Pending**: You must send an MCUmgr command to mark the new image hash as `Pending`.
2.  **Reset**: Send a `Reset` command. 
3.  **Swap**: On reboot, MCUboot sees the "Pending" flag, swaps the images, and runs the new code.
4.  **Confirm**: If your app uses "Test Mode," it must call `boot_write_img_confirmed()` within a few seconds, or MCUboot will revert to the old version on the next reboot. (Our current Python script marks it as `Pending` for a permanent switch).

## 5. Troubleshooting Checklist
- [ ] **Wrong File**: Ensure you are uploading `zephyr.signed.bin`, NOT `zephyr.bin`. Unsigned files will be rejected by MCUboot.
- [ ] **Forget Device**: Always forget the device on the phone if you change DFU services/settings.
- [ ] **EMSGSIZE**: If reading image states fails, ensure `CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE=512`.
- [ ] **RAM Usage**: On nRF52832 (64KB RAM), if your usage is >90% without sysbuild, the OTA process will likely fail or crash.
- [ ] **Reason 19**: If disconnecting immediately on iOS, check `CONFIG_BT_GATT_SERVICE_CHANGED`.
