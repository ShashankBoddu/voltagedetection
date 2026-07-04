import asyncio
from bleak import BleakClient, BleakScanner

async def list_services(address):
    print(f"Connecting to {address} for service discovery...")
    try:
        async with BleakClient(address) as client:
            print(f"Connected: {client.is_connected}")
            print("\nServices and Characteristics:")
            for service in client.services:
                print(f"\nService: {service.uuid} ({service.description})")
                for char in service.characteristics:
                    print(f"  - Char: {char.uuid} ({char.description}) | Handle: {char.handle:04X} | Props: {char.properties}")
                    for descriptor in char.descriptors:
                        print(f"    - Descriptor: {descriptor.uuid} | Handle: {descriptor.handle:04X}")
    except Exception as e:
        print(f"Failed to discover services: {e}")

async def main():
    print("Scanning for AC_DETECTOR...")
    device = await BleakScanner.find_device_by_name("AC_DETECTOR")
    if device:
        print(f"Found {device.name} [{device.address}]")
        await list_services(device.address)
    else:
        print("Device not found.")

if __name__ == "__main__":
    asyncio.run(main())
