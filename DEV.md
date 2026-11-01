# Developer Reference & System Architecture Guide (`DEV.md`)

> **Project**: High-Voltage Non-Contact Voltage (NCV) Detector & Wireless Telemetry Suite  
> **Last Updated**: 2026-11-01  
> **Active Firmware Version**: `>> ACDetector V1.3.0 (IEC 61243-1 Kelvin Verified) <<`  
> **Working Reference Baseline**: Commit `5806bcc` + IEC 61243-1 5-Rule Kelvin Suite (VER:V0.4)  
> **Target Hardware**: Nordic Semiconductor nRF52832 (ARM Cortex-M4F @ 64 MHz, 64 KB RAM, 512 KB Flash)  
> **Software Stack**: Zephyr OS v4.2.99 / nRF Connect SDK v3.2.1  
> **Full System Documentation**: [PROJECT_DEVELOPMENT_STATE.md](file:///e:/projects/DevelopmentLevelCode/voltagedetection/NRF52/PROJECT_DEVELOPMENT_STATE.md)  
> **Edge AI Implementation Plan**: [AI_TINYML_IMPLEMENTATION_PLAN.md](file:///e:/projects/DevelopmentLevelCode/voltagedetection/NRF52/AI_TINYML_IMPLEMENTATION_PLAN.md)  
> **OTA DFU Procedure**: [ACDetector/ota_dfu_sop.md](file:///e:/projects/DevelopmentLevelCode/voltagedetection/NRF52/ACDetector/ota_dfu_sop.md)

---

## 1. Quick Developer Commands

### Pristine Build (Sysbuild)
```powershell
west build --build-dir e:/projects/DevelopmentLevelCode/voltagedetection/NRF52/ACDetector/build e:/projects/DevelopmentLevelCode/voltagedetection/NRF52/ACDetector --pristine --board WrieLessPotentialDetector_Nrf52832/nrf52832 -- -DBOARD_ROOT="e:/projects/DevelopmentLevelCode/NRFBOARD"
```

### Incremental Build
```powershell
cmd /c "set PATH=C:\ncs\toolchains\66cdf9b75e;C:\ncs\toolchains\66cdf9b75e\mingw64\bin;C:\ncs\toolchains\66cdf9b75e\bin;C:\ncs\toolchains\66cdf9b75e\opt\bin;C:\ncs\toolchains\66cdf9b75e\opt\bin\Scripts;C:\ncs\toolchains\66cdf9b75e\nrfutil\bin;C:\ncs\toolchains\66cdf9b75e\opt\zephyr-sdk\arm-zephyr-eabi\bin;%PATH% && set ZEPHYR_TOOLCHAIN_VARIANT=zephyr && set ZEPHYR_SDK_INSTALL_DIR=C:\ncs\toolchains\66cdf9b75e\opt\zephyr-sdk && set ZEPHYR_BASE=C:\ncs\v3.2.1\zephyr && cd /d e:\projects\DevelopmentLevelCode\voltagedetection\NRF52\ACDetector && west build -d e:/projects/DevelopmentLevelCode/voltagedetection/NRF52/ACDetector/build ."
```

### BLE OTA Firmware Update (SMP DFU)
```powershell
cd e:\projects\DevelopmentLevelCode\voltagedetection\NRF52\OTAPytest
python ota_update.py --verify
```

### BLE Telemetry & Diagnostic Verification
```powershell
cd e:\projects\DevelopmentLevelCode\voltagedetection\NRF52\OTAPytest
python test_device.py
```

---

## 2. Resource & Memory Footprint (Production Baseline)

```
Memory region         Used Size  Region Size  %age Used
           FLASH:      214188 B     224768 B     95.29%
             RAM:       59202 B        64 KB     90.34%
        IDT_LIST:          0 GB        32 KB      0.00%
```

* **Flash Utilization**: 214,188 B / 224,768 B (95.29%). 10,580 bytes free in Slot 0 partition.
* **RAM Footprint**: 59,202 B / 65,536 B (90.34%). 6,334 bytes free RAM. Stack protection guarded via ARM Cortex-M4 MPU.
* **MCUboot Signed Image**: `zephyr.signed.bin` (215,036 bytes), SHA-256: `e75e731b884051065f47284764db64e9792688ef7ea63c6a716c78e90635b0ca`.
* **Version String Broadcast**: `"VER:V0.4"`
* **Stack Allocations**:
  - `CONFIG_MAIN_STACK_SIZE=2048`
  - `CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=2048`
  - `CONFIG_BT_BUF_ACL_RX_COUNT_EXTRA=4`
* **Zero Dynamic Heap Allocation**: `malloc` and `free` are strictly prohibited to prevent memory fragmentation and heap-stack collisions.

---

## 3. Hardware Pinout & Peripheral Mapping

| Function | nRF52832 Pin | Net / Hardware Role | Notes |
| :--- | :--- | :--- | :--- |
| **BLC Sensing (Before LC)** | `P0.31` / `AIN7` | Pre-filter antenna signal from MCP601 op-amp | Biased at $V_{DD}/2 \approx 1650\text{ mV}$ DC |
| **ALC Sensing (After LC)** | `P0.28` / `AIN4` | Hardware RC low-pass filtered signal | $R=220\,\Omega$, $C=10\,\mu\text{F}$ ($f_c \approx 72.3\text{ Hz}$) |
| **Battery Sense** | `P0.29` / `AIN5` | Scaled battery voltage divider | $R_1=1\,\text{M}\Omega$, $R_2=300\,\text{k}\Omega$ (Ratio 4.360) |
| **Kelvin Test Drive** | `P0.07` (Pin 23) | High-speed step drive into isolation barrier | $C_{\text{iso}}=10\,\text{pF}$ (2 kV), $R_{\text{iso}}=10\,\text{M}\Omega$ |
| **Buzzer** | `P0.06` | Acoustic transducer driven via transistor Q2 | Continuous siren (Live), 2 Hz pulse (Induced) |
| **Red Alert LED (U5)** | `P0.05` | Visual alarm driven via transistor Q1 | Solid ON (Live), 4 Hz flash (Induced) |
| **Blue BLE LED (U8)** | `P0.04` | Wireless status driven via transistor Q4 | 1 Hz blink (Adv), Solid ON (Connected) |
| **Range MUX S0–S3** | `P0.12`–`P0.15`| CD74HC4067 16-channel rotary multiplexer | Channels 0–11 (230 V up to 765 kV) |

---

## 4. Hardware Kelvin Self-Test (Wire Connection Detection)

The detector implements an **active 4-wire Kelvin loop-back test** across the physical copper sensing plate in the nosecone:

```
                  ┌──────────────────────────────────────────────────────────────┐
                  │                 HIGH-VOLTAGE SENSING HEAD                     │
                  │                                                              │
nRF52832 MCU      │   Isolation Barrier           Wire 2 (Drive Lead)            │
P0.07 (Pin 23) ───┼──[10 pF, 2kV]───[10 MΩ]─────────────────────────┐            │
(GPIO Step Drive) │                                                 │            │
                  │                                         ┌───────▼──────────┐ │
                  │                                         │  PHYSICAL COPPER │ │
                  │                                         │   SENSING PLATE  │ │
                  │                                         └───────┬──────────┘ │
                  │                                                 │            │
SAADC AIN7 ───────┼──[RC Filter / Pre-amp]──────────────────────────┘            │
(BLC Channel)     │                              Wire 1 (Sense Lead)             │
                  └──────────────────────────────────────────────────────────────┘
```

### Signal Processing Mechanics (`hardware_kelvin_selftest()` in `adc_backend.c`)

1. **Auto-Switch to Channel 0 (230V Range)**:
   - Before taking any measurements, the MUX is automatically set to Channel 0 (`range_set(0)`).
   - *Industrial Rationale*: Channel 0 provides standard input impedance that naturally attenuates high-impedance stray PCB capacitive coupling from 63–67 mV down to 16 mV when Wire 2 is open-circuit, creating a massive **$13.3\times$ physical signal separation**.
   - Background ADC acquisitions are paused (`g_selftest_active = true`).

2. **Phase 1: Baseline Ambient Capture (Drive Disabled)**:
   - Pin P0.07 configured as `GPIO_INPUT` (floating High-Z / tri-state).
   - Samples 2 cycles (400 samples at 10 kHz, 40 ms total).
   - Computes quiescent ambient baseline: `avg_base_p2p_blc` ($2\text{ to }4\text{ mV}$) and `avg_base_rms_blc`.
   - *Ambient Safety Inhibit*: Rejects test if ambient 50 Hz electric field is excessive ($> 60\text{ mV}$ P2P or $> 25\text{ mV}$ RMS) to prevent unsafe false passes near live equipment.

3. **Phase 2: Active 100 Hz Step Injection Pulse Drive**:
   - Pin P0.07 configured as Output, toggled at 100 Hz (every 5 ms) via `selftest_timer`.
   - Fast step transitions ($t_r < 10\text{ ns}$) inject high-frequency displacement current:
     $$I_{\text{inj}} = C_{\text{iso}} \frac{dV}{dt}$$
   - Samples 4 cycles (800 samples, `SELFTEST_CYCLE_COUNT 4`) on BLC (`AIN7`) and ALC (`AIN4`).
   - Waveforms are immediately copied into published buffers *before* restoring MUX and clearing `g_selftest_active` to prevent any SAADC DMA overwriting.
   - Pin P0.07 is returned to safe `GPIO_INPUT` (High-Z).

4. **Phase 3: 5-Point Industrial Standard Evaluation Suite (Zero Floating-Point)**:

   | Rule | Evaluated Metric | Acceptance Range | Connected (Pass) | Disconnected (Fail) | Purpose & Failure Mode Prevented |
   | :--- | :--- | :--- | :---: | :---: | :--- |
   | **Rule 1** | **BLC Peak-to-Peak** | $\ge 150\text{ mV}$ | **$211\text{ to }216\text{ mV}$** | **$16\text{ to }110\text{ mV}$** | Proves physical charge injection through Wire 2 $\rightarrow$ Plate $\rightarrow$ Wire 1. 150 mV floor provides 40 mV margin above max parasitic needle spike (110 mV). |
   | **Rule 2** | **ALC Filtered Output** | $\ge 15\text{ mV}$ | **$36\text{ to }37\text{ mV}$** | **$2\text{ to }3\text{ mV}$** | Proves energy transfer through hardware RC low-pass filter ($220\,\Omega, 10\,\mu\text{F}$). **$12\times$ physical signal separation**. |
   | **Rule 3** | **Dual-Channel Ratio** | $10\% \le \frac{\text{ALC}}{\text{BLC}} \le 50\%$ | **$16.8\%\text{ to }17.5\%$** | **$2.7\%\text{ to }4.5\%$** | Rejects narrow stray PCB needle spikes which lack low-frequency energy ($>4\times$ margin). |
   | **Rule 4** | **Injected Signal Energy** | $\Delta\text{RMS} \ge 15\text{ mV}$ OR $\ge 5\text{ pts}$ | **$\Delta\text{RMS} \approx 65\text{ mV}$** | **$\Delta\text{RMS} \le 4\text{ mV}$** | Differentiates sustained capacitive step pulses from high-pass needle spikes. |
   | **Rule 5** | **Preamp DC Bias** | $1100\text{ mV} \le V_{\text{DC}} \le 2200\text{ mV}$ | **$\approx 1627\text{ mV}$** | $<800\text{ mV}$ or $>2400\text{ mV}$ | Validates MCP601 virtual ground health against ESD damage or trace fracture. |
   | **Inhibit** | **Ambient Noise Floor** | $\le 60\text{ mV}$ P2P & $\le 25\text{ mV}$ RMS | **$2\text{ to }4\text{ mV}$** | **$2\text{ to }4\text{ mV}$** | Prevents false passes caused by testing near an energized 50 Hz utility grid. |

   ### Empirical Bench Data Summary (Physical Device Test Photos)

   | Metric | Wire 2 CONNECTED (Intact) | Wire 2 DISCONNECTED (Open) | Margin / Separation |
   | :--- | :---: | :---: | :---: |
   | **BLC Amplitude (P2P)** | **$211\text{ to }216\text{ mV}$** | **$16\text{ to }110\text{ mV}$** | **$+61\text{ mV}$ vs $-40\text{ mV}$** (relative to 150 mV threshold) |
   | **ALC Filtered Output (P2P)** | **$36\text{ to }37\text{ mV}$** | **$2\text{ to }3\text{ mV}$** | **$12\times$ Physical Contrast** (36 mV vs 3 mV) |
   | **ALC / BLC Attenuation Ratio** | **$16.8\%\text{ to }17.5\%$** | **$2.7\%\text{ to }4.5\%$** | **$>4\times$ Contrast** (17% vs 3%) |
   | **Injected Delta ($\Delta\text{P2P}$)** | **$+190\text{ to }+200\text{ mV}$** | **$+16\text{ to }+92\text{ mV}$** | Clear separation |
   | **Evaluation Result** | **PASS (All 5 Rules OK)** | **FAIL (Rules 1, 2, 3, 4 Fail)** | **Zero False Results** |

5. **Power-On Boot Kelvin Self-Test Integration**:
   - Runs cleanly inside `adc_thread_fn` on the first steady-state cycle (`count == ADC_AVG_COUNT`, ~80 ms after boot) after the SAADC driver and power rails are fully settled.
   - `adc_thread` stack expanded to **3072 bytes** with Cortex-M4 MPU hardware stack protection (`CONFIG_MPU_STACK_GUARD=y`).
   - Replaced floating-point library `sqrt` with non-recursive 64-bit integer `isqrt64()`, eliminating software float stack frames and preventing MPU guard faults during MCUboot image verification.
   - Single retry mechanism before locking into `STATUS_FAULT`.

---

## 5. System Annunciation Matrix (IEC 61243-1)

| Status Code | Status Name | Signal Condition | Buzzer (`P0.06`) | Red LED U5 (`P0.05`) | Blue LED U8 (`P0.04`) | Mobile App |
| :---: | :--- | :--- | :--- | :--- | :--- | :--- |
| **0** | `STATUS_SAFE` | Signal below threshold or SMPS noise rejected | Silent | OFF | Blink 1Hz (Adv) / Solid ON (Conn) | Green "DE-ENERGIZED" |
| **1** | `STATUS_LIVE` | BLC $\ge 100\%$ Live threshold & ALC $\ge 100\%$ Live threshold | Continuous Loud Siren | Solid ON (100% duty) | Blink 1Hz (Adv) / Solid ON (Conn) | Red "LIVE LINE - DANGER" |
| **2** | `STATUS_INDUCED` | BLC $\ge 80\%$ (min 20 mV) & ALC $\ge 75\%$ (min 22 mV) (Ch $\ge 2$) | Pulsing Beep 2 Hz (50% duty) | Rapid Flash 4 Hz (50% duty) | Blink 1Hz (Adv) / Solid ON (Conn) | Amber "INDUCED VOLTAGE" |
| **3** | `STATUS_FAULT` | Preamp DC bias out of range or Kelvin loop fail | Synchronized 5 Hz Pulsing Beep | Alternating 5 Hz Strobe | Alternating 5 Hz Strobe | Red "HARDWARE FAULT" |

---

## 6. Battery Management (Envie 9V 300 mAh Ni-MH)

Powered by a 7-cell Ni-MH rechargeable battery (8.4V nominal):
* **$\ge 9600\text{ mV}$**: 100% (Fresh off charger)
* **$8400\text{ mV}$**: 50% (Flat discharge plateau)
* **$7700\text{ mV}$**: 10% (Low battery warning)
* **$7350\text{ mV}$**: 5% (Critical warning: double audio chirp + double LED flash)
* **$\le 7000\text{ mV}$**: 0% (Hardware cutoff to prevent cell reversal damage)
* **Filtering**: 16 oversamples per acquisition + Exponential Moving Average (EMA, $\alpha=1/8$).

---

## 7. Switched-Mode Power Supply (SMPS) Rejection

To eliminate false alarms from mobile phone chargers and laptop adapters:
* **Dual-frequency Goertzel algorithm**: Computes 50 Hz fundamental ($k=1.0$) and 150 Hz 3rd harmonic ($k=3.0$).
* **Dual-Channel Discrimination**:
  ```c
  bool is_smps_charger = (blc_150hz_pct >= 150) || (alc_150hz_pct >= 15);
  ```
  - Genuine utility power: 150 Hz harmonic $< 5\%$ to $10\%$.
  - SMPS charger leakage: 150 Hz harmonic $\ge 30\%$ to $40\%$.
  - Automatically suppressed as `STATUS_SAFE` across all 12 rotary ranges.

---

## 8. Recent Engineering Changelog Summary

### 2026-11-01: 5-Point Industrial Standard Kelvin Self-Test & Safe Boot Architecture
* **Race Condition RCA & Fix**: Eliminated waveform buffer truncation (BLC missing first half-cycle in Image 5) by taking waveform snapshots *before* resetting MUX and releasing `g_selftest_active`.
* **5-Point Industrial Standard Evaluation**: Replaced single-point P2P delta threshold with comprehensive 5-rule criteria ($BLC \ge 100\text{ mV}$, $ALC \ge 15\text{ mV}$, $10\% \le \text{Ratio} \le 45\%$, elevated samples $\ge 15$, DC bias $1.1\text{ to }2.2\text{ V}$).
* **Channel 0 Auto-Switch**: Enforced Channel 0 during self-test, shunting stray capacitive spikes on broken wire down to $16\text{ mV}$ ($13.3\times$ separation from $214\text{ mV}$ connected).
* **Safe Power-On Boot Self-Test**: Relocated boot self-test from `main()` to `adc_thread_fn` after a 100 ms power-rail settling delay, completely preventing MCUboot test-swap timeouts, stack collisions, or boot freezes.

### 2026-10-31: Option B Transient Step Self-Test & OTA DFU Swap Optimization
* Fixed MCUboot SMP DFU test swap in `ota_update.py` (avoid premature `confirm=True` on secondary slot).
* Enabled synchronous UART console logging (`CONFIG_PRINTK_SYNC=y`, `CONFIG_LOG_PRINTK=n`).
* Implemented immediate confirmation via `boot_write_img_confirmed()` at the start of `main()`.
* Empirical bench validation: $54\times$ dynamic signal separation for Kelvin electrode continuity.
