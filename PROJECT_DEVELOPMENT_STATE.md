# High-Voltage Non-Contact Voltage (NCV) Detector & Mobile App System Documentation

> **Last Updated Date**: 2026-07-30  
> **Target Hardware**: nRF52832 MCU (nRF Connect SDK / Zephyr RTOS)  
> **Firmware Path**: `E:\projects\DevelopmentLevelCode\voltagedetection\NRF52\ACDetector`  
> **Mobile App Path**: `E:\projects\DevelopmentLevelCode\App\ac-detector-expo`  
> **Schematic File**: `E:\projects\DevelopmentLevelCode\voltagedetection\NRF52\SCH_Schematic high voltage NCV detector.png`

---

## 1. System Overview & Core Objective

This system is a **capacitor-based non-contact voltage (NCV) detector** and remote monitoring suite designed for high-voltage power generation, transmission, and distribution environments.

### Primary Purpose
1. **Energized Line Detection**: Determine if a conductor/wire is energized at the target voltage (e.g., 230 VAC line).
2. **Hazardous Induced Voltage Detection**: Detect dangerous capacitive/electromagnetic **induced voltages present on de-energized / uncharged lines** (caused by proximity to adjacent high-voltage lines), warning maintenance personnel before touching the line.
3. **EMI Noise Immunity**: Isolate pure 50 Hz / 60 Hz power grid frequencies using digital filtering to eliminate ambient electromagnetic interference (EMI) false alarms.

---

## 2. Hardware Architecture & Schematic Parameters

### Hardware Components
* **MCU**: Nordic Semiconductor nRF52832 (ARM Cortex-M4F)
* **Battery Resistor Divider**:
  * **$R_1$ (R42)** = $1\,\text{M}\Omega$ ($1000\,\text{k}\Omega$)
  * **$R_2$ (R46)** = $300\,\text{k}\Omega$
  * **Theoretical Ratio**: $\frac{R_1 + R_2}{R_2} = \frac{1300}{300} = 4.3333$
  * **Calibrated Fine Ratio**: $\frac{1308}{300} = 4.3600$ (tuned for exact 8830 mV readout accuracy)
* **Analog Channels (SAADC)**:
  * `P0.31` / `AIN7`: Before LC Filter Channel (BLC)
  * `P0.28` / `AIN4`: After LC Filter Channel (ALC)
  * `P0.29` / `AIN5`: Battery Voltage Measurement (`BATVOLT`)
* **Range Selector**: Multi-channel gain/attenuation multiplexer (CD74HC4067 / rotary switch)

---

## 3. Firmware Processing & Detection Logic (`ACDetector`)

### A. Battery Voltage Calculation & Noise Suppression
* **Multi-Sample Averaging**: 16 consecutive ADC samples (`extra_samplings = 15`) per conversion cycle.
* **Low-Pass Filter**: Exponential Moving Average (EMA, $\alpha = \frac{1}{8}$) across cycles:
  $$\text{Bat}_{\text{filtered}} = \frac{\text{Bat}_{\text{filtered}} \times 7 + \text{Bat}_{\text{raw}}}{8}$$
* **Formula**:
  $$V_{\text{bat\_mV}} = \left( \frac{\text{ADC}_{\text{counts}} \times 879}{1000} \right) \times \frac{1308}{300}$$

### B. 50Hz Fundamental Bandpass Filter (Goertzel DFT)
To eliminate broadband EMI switching noise and high-frequency transients, the raw BLC and ALC buffers (200 samples @ 10 kHz sample rate = 20 ms window) are processed through a **Goertzel algorithm**:
* **Target Frequency**: $f_0 = 50\,\text{Hz}$ ($k = 1.0$)
* **Output**: Pure fundamental 50Hz RMS voltage in mV ($\text{BLC}_{\text{50Hz\_RMS}}$, $\text{ALC}_{\text{50Hz\_RMS}}$). Broadband high-frequency EMI produces near-zero response.

### C. 3-State Safety Detection Architecture
Automatic channel switching has been disabled to maintain fixed sensor input impedance. Line status is evaluated against the selected range sensitivity thresholds:

| Status Code | Status Name | Signal Condition | Audio / Visual Indication |
| :---: | :---: | :--- | :--- |
| **0** | **`STATUS_SAFE`** | Signal $< 90\%$ live threshold (or $< 100\%$ for Channels 0 & 1) | **Buzzer OFF / LED OFF** |
| **1** | **`STATUS_LIVE`** | Signal $\ge 100\%$ live threshold ($\text{BLC} \ge 35\text{mV}$, $\text{ALC} \ge 25\text{mV}$) | **Solid Buzzer ON & Solid LED ON** |
| **2** | **`STATUS_INDUCED`** | Signal between $90\% - 99\%$ live threshold (Channels $\ge 2$ / $\ge 3.3\text{kV}$ only) | **Pulsing/Beeping Buzzer & Flashing LED** |

> **Channel Rules for Induced Voltage**:
> * **Channels 0 & 1 (230V & 1.1kV)**: `STATUS_INDUCED` is **disabled**. Any signal below 100% live threshold is categorized as `STATUS_SAFE` (0).
> * **Channels 2 – 11 (3.3kV – 765kV)**: `STATUS_INDUCED` triggers when signal reaches $\ge 90\%$ of the live threshold, alerting operators to dangerous electromagnetic/capacitive induced voltage on uncharged lines.

---

## 4. Mobile App Architecture (`ac-detector-expo`)

* **Framework**: React Native with Expo (TypeScript)
* **BLE Communication**: `react-native-ble-plx`
* **Telemetry Data Packets**:
  * `PKT_STATS_1` (Type 4, 16 Bytes): `Status Code`, `Battery %`, `Battery mV`, `BLC Mean mV`, `BLC RMS mV`
  * `PKT_STATS_2` (Type 5, 10 Bytes): `ALC Mean mV`, `ALC RMS mV`, `Selected Range`
  * `PKT_BLC` / `PKT_ALC` (Types 1 & 2): Raw 200-point waveform buffers
* **3-State Visual Alerts**:
  * 🔴 **LIVE Line**: Red/Orange Alert Gradient (`⚡ LIVE LINE` / `⚠️ LINE ENERGIZED - DANGER`) + Error Haptics
  * 🟠 **INDUCED Voltage**: Amber/Yellow Warning Gradient (`🧲 INDUCED` / `⚠️ HAZARDOUS INDUCED VOLTAGE`) + Warning Haptics
  * 🟢 **SAFE Line**: Green/Teal Safe Gradient (`✓ SAFE / DE-ENERGIZED LINE`)

---

## 5. Current Calibration & Baseline Setup (230 VAC & High-Voltage Testing)

* **Test Voltage**: 230 VAC Line (Channel 0) / High Voltage Lines (Channel $\ge 2$)
* **Selected Sensitivity**: Channel-dependent multiplexer gain setting
* **Calibrated Live Thresholds**:
  * BLC $V_{\text{rms}}$ threshold high: **35 mV**
  * ALC $V_{\text{rms}}$ threshold high: **25 mV**
* **Induced Voltage Warning Threshold**: **$90\%$ of Live threshold** for Channels $\ge 2$ (Disabled for Channels 0 & 1).

---

## 6. Change Log & Revision History

| Date | Component | Description of Changes |
| :---: | :---: | :--- |
| **2026-08-01** | Firmware & App | • Removed `STATUS_INDUCED` for Channels 0 & 1 (230V and 1.1kV) so low-voltage signals $< 100\%$ are marked `STATUS_SAFE`.<br>• Adjusted `STATUS_INDUCED` threshold from 50% to **90%** of live threshold for Channels $\ge 2$ (3.3kV – 765kV) to accurately detect dangerous induced voltage on uncharged lines.<br>• Updated hardware audio indication for `STATUS_INDUCED` to use a **pulsing/beeping buzzer** alongside the flashing LED for clear auditory distinction from a solid LIVE line alert.<br>• Verified app UI visualization for 3-state detection. |
| **2026-07-30** | Firmware & App | • Fixed battery reading factor to 1308/300 ($R_1=1\text{M}\Omega, R_2=300\text{k}\Omega$) for 8830mV accuracy.<br>• Added 16-sample averaging + EMA filter to eliminate battery readout fluctuations.<br>• Implemented 50Hz/60Hz Goertzel fundamental bandpass filter with explicit DC mean subtraction to eliminate false RMS signal explosion.<br>• Fixed live threshold selection to use `blc_rms_min` (35mV) and `alc_rms_min` (25mV).<br>• Removed automatic channel switching (fixed impedance).<br>• Implemented 3-state safety detection (SAFE, LIVE, INDUCED) with rapid 10Hz alarm for induced voltage.<br>• Updated Expo app UI and BLE status parser to match 3-state architecture. |

---

## 7. Ongoing Testing Notes & Future Tasks

> **Developer Note**: Use this section to log live testing results, threshold adjustments, or planned features as development continues.

* [ ] Test 230 VAC live line detection in physical high-EMI laboratory environment.
* [ ] Verify induced voltage warning levels on uncharged line adjacent to live 11kV/33kV test rig.
* [ ] Fine-tune per-channel threshold tables in `g_thresholds` for higher voltage ranges (1kV, 11kV, 33kV, 132kV).
* [ ] Log battery discharge curve and adjust `battery_percent_from_mv()` LUT if required.
