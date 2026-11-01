import asyncio
import os
import argparse
from bleak import BleakScanner, BleakClient
from smpclient import SMPClient
from smpclient.generics import success
from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite, ImageErase
from smpclient.requests.os_management import ResetWrite

# Maximum number of retries for the entire process
MAX_RETRIES = 5

# Default device name from prj.confclea
DEFAULT_DEVICE_NAME = "AC_DETECTOR"
# Default firmware path
DEFAULT_FILE_PATH = r"E:\projects\DevelopmentLevelCode\voltagedetection\NRF52\ACDetector\build\ACDetector\zephyr\zephyr.signed.bin"

def extract_mcuboot_hash(image_data: bytes) -> bytes:
    """Extract the true 32-byte SHA-256 hash from the MCUboot TLV trailer."""
    import struct
    if len(image_data) > 32:
        magic, load_addr, hdr_size, prot_tlv_sz, img_size = struct.unpack('<IIHHI', image_data[:16])
        if magic == 0x96f3b83d: # IMAGE_MAGIC
            tlv_off = hdr_size + img_size
            tlv_data = image_data[tlv_off:]
            if len(tlv_data) >= 4:
                off = 4
                while off + 4 <= len(tlv_data):
                    t_type, t_len = struct.unpack('<HH', tlv_data[off:off+4])
                    if t_type == 0x10 and t_len == 32: # IMAGE_TLV_SHA256
                        return tlv_data[off+4:off+4+32]
                    off += 4 + t_len
    import hashlib
    return hashlib.sha256(image_data).digest()

async def find_device(name):
    print(f"Scanning for BLE device with name '{name}'...")
    try:
        devices = await BleakScanner.discover(timeout=5.0)
        print(f"Found {len(devices)} device(s) in range:")
        target_device = None
        for d in devices:
            d_name = d.name if d.name else "Unknown"
            print(f"  - {d_name} [{d.address}]")
            if d.name and d.name.strip() == name:
                target_device = d
        
        if target_device:
            print(f"\nTarget device found: {target_device.name} [{target_device.address}]")
            return target_device.address
    except Exception as e:
        print(f"Scan error: {e}")
    return None

async def run_ota(address, file_path):
    if not os.path.exists(file_path):
        print(f"Error: Firmware file not found at: {file_path}")
        return

    file_size = os.path.getsize(file_path)
    print(f"Connecting to {address}...")
    
    with open(file_path, "rb") as f:
        image_data = f.read()

    for attempt in range(MAX_RETRIES):
        try:
            from smpclient.transport.ble import SMPBLETransport
            transport = SMPBLETransport()
            
            async with SMPClient(transport, address) as client:
                print(f"Connected successfully via SMP (Attempt {attempt + 1}).")
                mtu = getattr(client._transport, 'mtu', getattr(client._transport, '_mtu', 244))
                print(f"Connection MTU: {mtu} bytes")
                
                print("Checking image states...")
                states = await client.request(ImageStatesRead())
                
                print("Erasing secondary slot to ensure clean flash...")
                try:
                    erase_resp = await client.request(ImageErase(slot=1))
                    if success(erase_resp):
                        print("Secondary slot erased.")
                    else:
                        print("Erase returned non-success, continuing anyway...")
                except Exception as e:
                    print(f"Warning: Could not erase secondary slot explicitly: {e}")

                print(f"Uploading firmware ({file_size} bytes)...")
                image_hash = extract_mcuboot_hash(image_data)
                print(f"Target MCUboot Image Hash: {image_hash.hex()[:16]}...")

                try:
                    async for offset in client.upload(image_data):
                        percent = (offset / file_size) * 100
                        print(f"\rProgress: {offset}/{file_size} bytes ({percent:.1f}%)", end="", flush=True)
                except Exception as upload_err:
                    print(f"\nUpload loop interrupted: {upload_err}")
                    raise upload_err
                    
                print("\nUpload complete.")
                
                print("Requesting final image list...")
                states = await client.request(ImageStatesRead())
                
                has_slot_1 = False
                if success(states) and hasattr(states, 'images'):
                    for img in states.images:
                        if img.slot == 1:
                            has_slot_1 = True
                            break
                            
                if not has_slot_1:
                    print("Warning: Slot 1 not reported in image list. Attempting swap by uploaded hash anyway...")

                print(f"Setting image {image_hash.hex()[:8]} to 'pending'...")
                swap_resp = await client.request(ImageStatesWrite(hash=image_hash))
                
                if not success(swap_resp):
                    print(f"Warning: Failed to mark image pending using hash. Response: {swap_resp}")
                else:
                    print("Image marked as pending. Swap will occur on reboot.")
                    if hasattr(swap_resp, 'images'):
                        for img in swap_resp.images:
                            print(f"    Slot {img.slot}: hash={img.hash.hex()[:8]}... pending={getattr(img, 'pending', False)} confirmed={getattr(img, 'confirmed', False)}")

                print("Resetting device to apply update...")
                reset_resp = await client.request(ResetWrite())
                if success(reset_resp):
                    print("Reset command sent. Device should reboot into the new firmware.")
                    return
                else:
                    print(f"Reset command failed: {reset_resp}")
                    return
                
        except Exception as e:
            error_str = str(e)
            print(f"\nAttempt {attempt + 1} Error: {error_str}")
            if any(msg in error_str for msg in ["max_pdu_size", "Unreachable", "Disconnected", "Lock is not acquired", "not found"]):
                if attempt < MAX_RETRIES - 1:
                    print("Retrying in 3 seconds...")
                    await asyncio.sleep(3.0)
                    continue
            print("Fatal error encountered.")
            import traceback
            traceback.print_exc()
            break

async def main():
    parser = argparse.ArgumentParser(description="OTA DFU Update Script for nRF52")
    parser.add_argument("--name", default=DEFAULT_DEVICE_NAME, help="Name or Address of the BLE device")
    parser.add_argument("--file", default=DEFAULT_FILE_PATH, help="Path to the firmware file")
    parser.add_argument("--verify", action="store_true", help="Automatically verify firmware version after reboot")
    
    args = parser.parse_args()

    address = await find_device(args.name)
    if not address:
        print(f"\nDevice '{args.name}' not found.")
        print("Please ensure the device is advertising and Bluetooth is enabled on your laptop.")
        return

    try:
        await run_ota(address, args.file)
        if args.verify:
            print("\nWaiting 5 seconds for device to complete boot...")
            await asyncio.sleep(5.0)
            from test_device import ACDetectorTester, scan_for_device, TX_CHAR_UUID
            new_addr = await scan_for_device(args.name)
            if new_addr:
                print("\n" + "=" * 60)
                print("       POST-OTA AUTOMATED VERIFICATION")
                print("=" * 60)
                tester = ACDetectorTester(new_addr, args.name)
                smp_info = await tester.query_smp_image_states()
                print(f"Active Slot:      Slot {smp_info.get('slot', 'Unknown')}")
                print(f"Active Hash:      {smp_info.get('hash', 'Unknown')}")
                print(f"Confirmation:     {'CONFIRMED' if smp_info.get('confirmed') else 'UNCONFIRMED'}")
                async with BleakClient(new_addr) as client:
                    await client.start_notify(TX_CHAR_UUID, tester.notification_handler)
                    await asyncio.sleep(0.5)
                    ver = await tester.query_version(client)
                    if ver:
                        print(f"Firmware Version: {ver}")
                    await client.stop_notify(TX_CHAR_UUID)
                print("=" * 60)
    except Exception as e:
        print(f"\nCaught unexpected error: {e}")

if __name__ == "__main__":
    asyncio.run(main())
