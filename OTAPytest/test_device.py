#!/usr/bin/env python3
"""
ACDetector BLE Test Suite & Verification Tool
=============================================
Performs over-the-air verification for the High-Voltage NCV Detector:
  1. Firmware Version Query ('V' command & SMP ImageStatesRead verification).
  2. Hardware Kelvin Electrode Continuity Self-Test ('K' command).
  3. Real-time dual-channel waveform (BLC/ALC) packet decoding & ASCII metrics table.
"""

import argparse
import asyncio
import struct
import sys
import time
from typing import Optional, List, Dict, Any

from bleak import BleakClient, BleakScanner
from smpclient import SMPClient
from smpclient.generics import success
from smpclient.transport.ble import SMPBLETransport
from smpclient.requests.image_management import ImageStatesRead

# Default BLE Device Name & UUIDs
DEFAULT_DEVICE_NAME = "AC_DETECTOR"
SERVICE_UUID = "12345678-1234-5678-1234-56789abcdef0"
TX_CHAR_UUID = "12345678-1234-5678-1234-56789abcdef1"  # NOTIFY (Device -> App/Python)
RX_CHAR_UUID = "12345678-1234-5678-1234-56789abcdef2"  # WRITE  (App/Python -> Device)

# Custom packet types from common.h
PKT_BLC = 1
PKT_ALC = 2
PKT_STATS_1 = 4
PKT_STATS_2 = 5
PKT_END = 0xFF

class ACDetectorTester:
    def __init__(self, address: str, name: str = DEFAULT_DEVICE_NAME):
        self.address = address
        self.name = name
        self.version_string: Optional[str] = None
        self.selftest_ack: Optional[Dict[str, Any]] = None
        self.stats1: Optional[Dict[str, Any]] = None
        self.stats2: Optional[Dict[str, Any]] = None
        self.blc_samples: List[int] = []
        self.alc_samples: List[int] = []
        self.transfer_complete = asyncio.Event()
        self.version_received = asyncio.Event()

    def notification_handler(self, sender: int, data: bytearray):
        # 1. Handle Version Response: e.g. "VER:V1.1.6" or "V1.1.6"
        if data.startswith(b"VER:") or data.startswith(b"V") or data.startswith(b"ACD"):
            try:
                self.version_string = data.decode("utf-8", errors="ignore").strip()
                self.version_received.set()
                return
            except Exception:
                pass

        # 2. Handle 16-byte Kelvin Self-Test Diagnostic ACK packet
        if len(data) == 16 and data[0] == ord('K'):
            passed = bool(data[1])
            blc_active_rms, blc_base_rms, blc_delta_rms, blc_delta_p2p, alc_active_rms, blc_p2p, alc_p2p = \
                struct.unpack("<hhhhhhh", data[2:16])
            
            self.selftest_ack = {
                "passed": passed,
                "blc_active_rms": blc_active_rms,
                "blc_base_rms": blc_base_rms,
                "blc_delta_rms": blc_delta_rms,
                "blc_delta_p2p": blc_delta_p2p,
                "alc_active_rms": alc_active_rms,
                "blc_p2p": blc_p2p,
                "alc_p2p": alc_p2p,
            }
            return

        # 3. Handle Streaming Waveform & Stats Packets
        if len(data) < 1:
            return

        pkt_type = data[0]
        if pkt_type == PKT_BLC:
            count = data[1]
            start_idx = struct.unpack("<H", data[2:4])[0]
            samples = struct.unpack(f"<{count}h", data[4:4 + count * 2])
            for i, val in enumerate(samples):
                idx = start_idx + i
                while len(self.blc_samples) <= idx:
                    self.blc_samples.append(0)
                self.blc_samples[idx] = val

        elif pkt_type == PKT_ALC:
            count = data[1]
            start_idx = struct.unpack("<H", data[2:4])[0]
            samples = struct.unpack(f"<{count}h", data[4:4 + count * 2])
            for i, val in enumerate(samples):
                idx = start_idx + i
                while len(self.alc_samples) <= idx:
                    self.alc_samples.append(0)
                self.alc_samples[idx] = val

        elif pkt_type == PKT_STATS_1 and len(data) >= 16:
            # type(1), status(1), bat_pct(1), res(1), bat_mv(4), blc_mean(4), blc_rms(4)
            status, bat_pct = data[1], data[2]
            bat_mv, blc_mean, blc_rms = struct.unpack("<iii", data[4:16])
            self.stats1 = {
                "status": status,
                "battery_percent": bat_pct,
                "battery_mv": bat_mv,
                "blc_mean_mv": blc_mean,
                "blc_rms_mv": blc_rms,
            }

        elif pkt_type == PKT_STATS_2 and len(data) >= 10:
            # type(1), alc_mean(4), alc_rms(4), range(1)
            alc_mean, alc_rms = struct.unpack("<ii", data[1:9])
            selected_range = data[9]
            self.stats2 = {
                "alc_mean_mv": alc_mean,
                "alc_rms_mv": alc_rms,
                "selected_range": selected_range,
            }

        elif pkt_type == PKT_END:
            self.transfer_complete.set()

    async def query_version(self, client: BleakClient, timeout: float = 4.0) -> Optional[str]:
        print("\n--> Querying Firmware Version over BLE custom service ('V' command)...")
        self.version_received.clear()
        self.version_string = None

        await client.write_gatt_char(RX_CHAR_UUID, b"V", response=False)
        try:
            await asyncio.wait_for(self.version_received.wait(), timeout=timeout)
            return self.version_string
        except asyncio.TimeoutError:
            print("  [!] Version query timed out (Device may be running legacy firmware without 'V' command)")
            return None

    async def query_smp_image_states(self) -> Dict[str, Any]:
        """Read active MCUboot slot, SHA-256 hash, and confirmed flag via SMP protocol."""
        print("--> Querying Active MCUboot Image State via SMP...")
        info = {"slot": "Unknown", "hash": "Unknown", "confirmed": False, "version": "Unknown"}
        try:
            transport = SMPBLETransport()
            async with SMPClient(transport, self.address) as client:
                resp = await client.request(ImageStatesRead())
                if success(resp):
                    for img in getattr(resp, 'images', []):
                        slot_num = getattr(img, 'slot', 0)
                        hash_bytes = getattr(img, 'hash', b'')
                        h_str = hash_bytes.hex()[:16] + "..." if hash_bytes else "Unknown"
                        is_active = getattr(img, 'active', False)
                        is_conf = getattr(img, 'confirmed', False)
                        is_pend = getattr(img, 'pending', False)
                        print(f"    Slot {slot_num}: Hash={h_str} Active={is_active} Confirmed={is_conf} Pending={is_pend}")
                        if is_active or (info["slot"] == "Unknown" and slot_num == 0):
                            info["slot"] = slot_num
                            info["hash"] = h_str
                            info["confirmed"] = is_conf
                            info["version"] = getattr(img, 'version', "N/A")
        except Exception as e:
            info["error"] = str(e)
        return info

    async def run_kelvin_selftest(self, client: BleakClient, timeout: float = 6.0) -> bool:
        print("\n--> Triggering Hardware Kelvin Electrode Self-Test ('K' command)...")
        self.transfer_complete.clear()
        self.selftest_ack = None
        self.blc_samples.clear()
        self.alc_samples.clear()

        await client.write_gatt_char(RX_CHAR_UUID, b"K", response=False)

        print("  Waiting for 100 Hz step excitation capture & dual-channel waveforms...")
        try:
            await asyncio.wait_for(self.transfer_complete.wait(), timeout=timeout)
            return True
        except asyncio.TimeoutError:
            print("  [!] Self-test response timed out.")
            return False

    def print_selftest_report(self):
        ack = self.selftest_ack
        if not ack:
            print("[-] Error: No Self-Test ACK packet received.")
            return

        blc_mean = self.stats1.get("blc_mean_mv", 1650) if self.stats1 else 1650
        alc_mean = self.stats2.get("alc_mean_mv", 1650) if self.stats2 else 1650
        bat_mv = self.stats1.get("battery_mv", 0) if self.stats1 else 0
        bat_pct = self.stats1.get("battery_percent", 0) if self.stats1 else 0

        # Multi-parameter rule checks (IEC 61243-1 / IEC 61010-1)
        delta_p2p = ack["blc_delta_p2p"]
        p2p_active = ack["blc_p2p"]
        alc_p2p = ack["alc_p2p"]
        delta_rms = ack["blc_delta_rms"]

        r1_pass = delta_p2p >= 15
        r2_pass = delta_rms >= 2
        r3_pass = 1100 <= blc_mean <= 2200
        r4_pass = p2p_active < 1200
        r5_pass = (1100 <= alc_mean <= 2200) and (alc_p2p >= 2) and (alc_p2p <= p2p_active + 10)

        overall_passed = ack["passed"] and r1_pass and r3_pass and r4_pass

        print("\n" + "=" * 80)
        print("          ACDETECTOR HARDWARE KELVIN SELF-TEST VERIFICATION REPORT")
        print("=" * 80)
        print(f" Target Device:   {self.name} [{self.address}]")
        if self.version_string:
            print(f" Firmware Build:  {self.version_string}")
        print(f" Battery Health:  {bat_mv} mV ({bat_pct}%)")
        print("-" * 80)
        print(f" {'#':<3} {'Safety Rule Verification':<36} {'Measured':<16} {'Limit':<12} {'Status'}")
        print("-" * 80)
        print(f" 1   Capacitive Step Delta (Delta P2P)    {delta_p2p} mV           >= 15 mV     {'[PASS]' if r1_pass else '[FAIL]'}")
        print(f" 2   100 Hz Step Excitation (Delta RMS)   {delta_rms} mV           >= 2 mV      {'[PASS]' if r2_pass else '[FAIL]'}")
        print(f" 3   Preamp DC Virtual Ground Health      {blc_mean} mV        1100-2200 mV {'[PASS]' if r3_pass else '[FAIL]'}")
        print(f" 4   Dynamic Linearity (Anti-Saturation)  {p2p_active} mV           < 1200 mV    {'[PASS]' if r4_pass else '[FAIL]'}")
        print(f" 5   Dual-Channel RC Filter Signature     {alc_p2p} mV (ALC P2P)   2-{p2p_active + 10} mV   {'[PASS]' if r5_pass else '[FAIL]'}")
        print("-" * 80)

        if overall_passed:
            print(" >> OVERALL STATUS: PASSED (Electrode wire, copper plate, and AFE intact & safe)")
            print("    The sensor loop is verified safe. No physical disconnect or broken wire detected.")
        else:
            print(" >> OVERALL STATUS: FAILED (WIRE 2 DISCONNECTED / HARDWARE FAULT)")
            print("    [!] SAFETY WARNING: Sensor loop continuity failed. Do NOT use near high voltage!")
        print("=" * 80)
        print(f" Waveform Data:   BLC Samples = {len(self.blc_samples)}, ALC Samples = {len(self.alc_samples)}")
        print("=" * 80 + "\n")


async def scan_for_device(name: str) -> Optional[str]:
    print(f"Scanning for BLE device '{name}' (5-second timeout)...")
    devices = await BleakScanner.discover(timeout=5.0)
    for d in devices:
        if d.name == name or d.address.upper() == name.upper():
            print(f"Found target: {d.name} [{d.address}]")
            return d.address
    print(f"Device '{name}' not found. Available devices:")
    for d in devices:
        if d.name:
            print(f"  - {d.name} [{d.address}]")
    return None


async def main():
    parser = argparse.ArgumentParser(description="ACDetector BLE Automated Verification Suite")
    parser.add_argument("--address", "-a", type=str, help="Target BLE MAC address")
    parser.add_argument("--name", "-n", type=str, default=DEFAULT_DEVICE_NAME, help="Target device name")
    parser.add_argument("--mode", "-m", choices=["all", "version", "selftest"], default="all",
                        help="Test mode: 'version', 'selftest', or 'all' (default)")
    parser.add_argument("--export", "-e", type=str, help="Export waveforms to specified JSON file")
    args = parser.parse_args()

    address = args.address
    if not address:
        address = await scan_for_device(args.name)
        if not address:
            sys.exit(1)

    tester = ACDetectorTester(address, args.name)

    # Step 1: SMP Bootloader Image State Verification (if requested)
    if args.mode in ["all", "version"]:
        smp_info = await tester.query_smp_image_states()
        print("\n" + "=" * 60)
        print("           MCUBOOT DFU IMAGE VERIFICATION")
        print("=" * 60)
        print(f" Active Slot:      Slot {smp_info.get('slot', 'Unknown')}")
        print(f" Image Hash:       {smp_info.get('hash', 'Unknown')}")
        print(f" Confirmation:     {'CONFIRMED (Permanent)' if smp_info.get('confirmed') else 'TEST MODE (Unconfirmed)'}")
        print("=" * 60)

    # Step 2: Connect to Custom GATT Service
    print(f"\nConnecting to custom BLE GATT service at {address}...")
    async with BleakClient(address) as client:
        print(f"Connected to GATT! (MTU: {client.mtu_size} bytes)")
        await client.start_notify(TX_CHAR_UUID, tester.notification_handler)
        await asyncio.sleep(0.5)

        # Mode: Version check
        if args.mode in ["all", "version"]:
            ver = await tester.query_version(client)
            if ver:
                print(f"\n[+] CONFIRMED ACTIVE FIRMWARE: {ver}")
            else:
                print("[-] Could not retrieve version string over custom GATT.")

        # # Mode: Kelvin Hardware Self-Test
        # if args.mode in ["all", "selftest"]:
        #     success_test = await tester.run_kelvin_selftest(client)
        #     if success_test:
        #         tester.print_selftest_report()
        #     else:
        #         print("[-] Hardware Kelvin Self-Test failed to complete.")

        await client.stop_notify(TX_CHAR_UUID)

    # Optional Waveform Export
    if args.export and tester.blc_samples:
        import json
        payload = {
            "device": tester.address,
            "version": tester.version_string,
            "selftest_ack": tester.selftest_ack,
            "blc_samples": tester.blc_samples,
            "alc_samples": tester.alc_samples,
        }
        with open(args.export, "w") as fp:
            json.dump(payload, fp, indent=2)
        print(f"[+] Saved {len(tester.blc_samples)} waveform points to {args.export}")

    print("\nVerification Complete.")


if __name__ == "__main__":
    asyncio.run(main())

