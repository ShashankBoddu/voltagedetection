import asyncio
import argparse
import os
import sys
import time
from typing import Final

from bleak import BleakScanner, BleakClient
from smpclient import SMPClient
from smpclient.generics import success, error
from smpclient.transport.ble import SMPBLETransport
from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite, ImageErase
from smpclient.requests.os_management import ResetWrite

# Default device name from prj.conf
DEFAULT_DEVICE_NAME = "AC_DETECTOR"
# Default firmware path
DEFAULT_FILE_PATH = r"E:\projects\DevelopmentLevelCode\voltagedetection\NRF52\ACDetector\build\ACDetector\zephyr\zephyr.signed.bin"

async def find_device(name):
    print(f"Scanning for BLE device with name '{name}'...")
    try:
        # Use a generic Bleak scanner for higher reliability
        devices = await BleakScanner.discover(timeout=5.0)
        
        print(f"Found {len(devices)} device(s) in range:")
        target_device = None
        for d in devices:
            d_name = d.name if d.name else "Unknown"
            print(f"  - {d_name} [{d.address}]")
            if d.name == name or d.address == name:
                target_device = d
        
        if target_device:
            print(f"\nTarget device found: {target_device.name} [{target_device.address}]")
            return target_device.address
            
    except Exception as e:
        print(f" Scan failed: {e}")
    return None

async def run_ota(address, file_path):
    if not os.path.exists(file_path):
        print(f"Error: File {file_path} not found.")
        return

    # Read the binary file
    with open(file_path, "rb") as f:
        image_data = f.read()

    print(f"Connecting to {address}...")
    
    MAX_RETRIES = 3
    for attempt in range(MAX_RETRIES):
        transport = SMPBLETransport()
        try:
            async with SMPClient(transport, address) as client:
                print(f"Connected successfully via SMP (Attempt {attempt + 1}).")
                print(f"Connection MTU: {transport.mtu} bytes")
                
                # --- Step 1: Confirmation & Slot Cleanup ---
                print("Checking image states...")
                state_resp = await client.request(ImageStatesRead())
                if success(state_resp):
                    active_image_hash = None
                    needs_confirmation = False
                    has_secondary = False
                    
                    for img in getattr(state_resp, 'images', []):
                        if img.active:
                            active_image_hash = img.hash
                            if not img.confirmed:
                                needs_confirmation = True
                        else:
                            has_secondary = True
                    
                    if needs_confirmation and active_image_hash:
                        print(f"Current image is in 'test' mode. Confirming it...")
                        conf_resp = await client.request(ImageStatesWrite(hash=active_image_hash, confirm=True))
                        if success(conf_resp):
                            print("Current image confirmed.")
                        else:
                            print(f"Warning: Failed to confirm current image: {conf_resp}")

                    # Explicitly erase the secondary slot to avoid "NO_FREE_SLOT" mid-upload
                    if has_secondary:
                        print("Erasing secondary slot to ensure it's clean...")
                        erase_resp = await client.request(ImageErase())
                        if success(erase_resp):
                            print("Secondary slot erased.")
                        else:
                            print(f"Warning: Failed to erase secondary slot (may already be empty): {erase_resp}")
                
                # --- Step 2: Upload ---
                print(f"Uploading firmware ({len(image_data)} bytes)...")
                start_s = time.time()
                
                # We need the hash for slot confirmation later. 
                # Calculating hash manually might be needed if protocol read fails.
                import hashlib
                image_hash = hashlib.sha256(image_data).digest()
                print(f"Target Image Hash: {image_hash.hex()[:10]}...")

                try:
                    async for offset in client.upload(image_data):
                        elapsed = time.time() - start_s
                        speed = (offset / elapsed / 1024) if elapsed > 0 else 0
                        progress = (offset / len(image_data)) * 100
                        print(f"\rProgress: {offset}/{len(image_data)} bytes ({progress:.1f}%) | {speed:.2f} KB/s", end="", flush=True)
                    
                    print(f"\nUpload complete.")
                except Exception as upload_err:
                    print(f"\nUpload failed: {upload_err}")
                    raise upload_err

                # --- Step 3: Verify and Reset ---
                print("Requesting final image list...")
                try:
                    response = await client.request(ImageStatesRead())
                    
                    if success(response):
                        new_image_hash = None
                        for img in getattr(response, 'images', []):
                            if not img.active:
                                new_image_hash = img.hash
                                break
                        
                        if new_image_hash:
                            image_hash = new_image_hash # Use the one from the device if possible
                            print(f"Found uploaded image in secondary slot.")
                        else:
                            print("Warning: Could not find uploaded image in slot 1. Using calculated hash.")
                    else:
                        print(f"Warning: Failed to read image states ({response}). Using calculated hash for swap.")
                except Exception as read_err:
                    print(f"Warning: Exception reading image states: {read_err}. Proceeding with blind confirmation.")

                # Mark the image as 'pending' (this triggers the swap on next boot)
                print(f"Setting image {image_hash.hex()[:8]} to 'pending'...")
                write_resp = await client.request(ImageStatesWrite(hash=image_hash))
                if success(write_resp):
                    print("Image marked as pending. Swap will occur on reboot.")
                else:
                    print(f"FAILED to mark image as pending: {write_resp}")
                    print("The device will likely NOT update. Ensure MCUMGR_TRANSPORT_NETBUF_SIZE is large enough (>= 512).")

                print("Resetting device to apply update...")
                reset_resp = await client.request(ResetWrite())
                if success(reset_resp):
                    print("Reset command sent. Device should reboot into the new firmware.")
                    return # Successfully finished
                else:
                    print(f"Reset command failed: {reset_resp}")
                    return
                
        except Exception as e:
            error_str = str(e)
            print(f"\nAttempt {attempt + 1} Error: {error_str}")
            
            # Catch common Windows/BLE disconnect issues
            if any(msg in error_str for msg in ["max_pdu_size", "Unreachable", "Disconnected", "Lock is not acquired", "not found"]):
                if attempt < MAX_RETRIES - 1:
                    print(f"Retrying in 3 seconds...")
                    await asyncio.sleep(3.0)
                    continue
            
            # If it's a protocol error, don't necessarily retry unless we think it's transient
            print("Fatal error encountered.")
            import traceback
            traceback.print_exc()
            break

async def main():
    parser = argparse.ArgumentParser(description="OTA DFU Update Script for nRF52")
    parser.add_argument("--name", default=DEFAULT_DEVICE_NAME, help="Name or Address of the BLE device")
    parser.add_argument("--file", default=DEFAULT_FILE_PATH, help="Path to the firmware file")
    
    args = parser.parse_args()

    address = await find_device(args.name)
    if not address:
        print(f"\nDevice '{args.name}' not found.")
        print("Please ensure the device is advertising and Bluetooth is enabled on your laptop.")
        return

    try:
        await run_ota(address, args.file)
    except Exception as e:
        print(f"\nCaught unexpected error: {e}")

if __name__ == "__main__":
    asyncio.run(main())
