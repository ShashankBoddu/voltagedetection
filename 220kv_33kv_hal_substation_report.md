# HAL 220kV / 33kV Substation Field Test & Induced Voltage Report

> **Location**: HAL 220kV / 33kV Transmission Substation, Hyderabad  
> **Date of Testing**: 07-Nov-2026  
> **Hardware Tested**: AC Detector Device ID1 & Device ID4 (Firmware V1.4.0, dual-channel BLC/ALC)  
> **Total Recordings**: 25 PDF Reports across 5 Operational Conditions  

---

## 1. Key Executive Takeaways (Points)

* **Live 220kV Line Consistency**:
  * Active 220kV lines at safe standoff distance consistently produced **14.0 to 16.0 mV RMS on ALC** (average **14.8 mV**, CV = 6.4%) across both Device ID1 and ID4.
  * Preliminary theoretical default of 25 mV failed detection; Channel 9 factory threshold was officially recalibrated to **9 mV ALC / 12 mV BLC**, providing +55% trigger margin on live lines and >2x margin above noise floor.
* **Live 33kV Line Consistency**:
  * Active 33kV feeders produced **24.0 to 37.0 mV RMS on ALC** (average **32.0 mV**, THD ~ 11%).
  * Channel 6 threshold was recalibrated from 25 mV to **18 mV ALC / 24 mV BLC** to eliminate borderline triggers.
* **CD74HC4067 Shunt Attenuator Suppression**:
  * On an open 220kV isolator without earth rods, setting the rotary switch to **Channel 220kV** (24 kΩ shunt) drained all electrostatic pickup down to **2.0 to 4.0 mV RMS** (quiescent safe state).
  * Probing the exact same floating line on **Channel 230V** (10 MΩ impedance) produced **85 to 283 mV RMS** (V_pp up to 1.2 V).
* **Quantification of Induced Voltage (V_induced) on Inactive Lines**:
  * Derived from empirical 230V reference calibration (230 VAC contact produces 17.6 mV RMS ALC -> ~13.07 VAC per mV of ALC):
    1. **De-energized 33kV Lines**: Induced voltage ranged from **~39 V** (isolated bay) to **~288 VAC** (R-phase standoff adjacent to running feeder).
    2. **220kV LC Lines (Earthed Both Ends)**: Mutual magnetic coupling with parallel active lines induced circulating currents, producing **157 V to 1032 VAC (~1.03 kV)** along the grounded span (261 V at the earth clamp itself).
    3. **220kV Open Isolator (Ungrounded / Floating)**: Floating capacitive voltage divider created a lethal **1.11 kV to 3.70 kV AC** electrostatic potential.
* **Standard Operating Procedure (SOP) Rule**:
  * Never test high-voltage lines with the 230V channel. High input impedance (10 MΩ) will trigger on harmless or standing induced voltage. Operators must strictly test on the matching high-voltage rotary channel.

---

## 2. Complete 25-Point Field Test Master Table

| # | Test Scenario / Conductor State | Dev | Channel | Physical Contact | BLC RMS | ALC RMS | ALC Pk-Pk | Dom Freq | ALC THD | Est. AC Voltage | Detection Result |
| :-: | :--- | :-: | :-: | :---: | :-: | :-: | :-: | :-: | :-: | :--- | :--- |
| 1 | `active220kv_1` | **ID1** | 220kV | Standoff Distance | 20 mV | 15 mV | 58 mV | 50 Hz | 19.1% | **220 kV AC (Active Grid)** | LIVE (Triggered) |
| 2 | `active220kv_2` | **ID1** | 220kV | Standoff Distance | 17 mV | 14 mV | 50 mV | 50 Hz | 20.5% | **220 kV AC (Active Grid)** | LIVE (Triggered) |
| 3 | `active220kv_1` | **ID4** | 220kV | Standoff Distance | 18 mV | 14 mV | 53 mV | 50 Hz | 18.5% | **220 kV AC (Active Grid)** | LIVE (Triggered) |
| 4 | `active220kv_2` | **ID4** | 220kV | Standoff Distance | 20 mV | 16 mV | 55 mV | 50 Hz | 17.0% | **220 kV AC (Active Grid)** | LIVE (Triggered) |
| 5 | `active33kv_1` | **ID1** | 33kV | Standoff Distance | 46 mV | 36 mV | 117 mV | 50 Hz | 11.7% | **33 kV AC (Active Grid)** | LIVE (Triggered) |
| 6 | `active_33kv_1` | **ID4** | 33kV | Standoff Distance | 34 mV | 27 mV | 92 mV | 50 Hz | 10.5% | **33 kV AC (Active Grid)** | LIVE (Triggered) |
| 7 | `active_33kv_2` | **ID4** | 33kV | Standoff Distance | 30 mV | 24 mV | 82 mV | 50 Hz | 11.8% | **33 kV AC (Active Grid)** | LIVE (Triggered) |
| 8 | `active_33kv_3` | **ID4** | 33kV | Standoff Distance | 48 mV | 37 mV | 125 mV | 50 Hz | 10.5% | **33 kV AC (Active Grid)** | LIVE (Triggered) |
| 9 | `active_33kv_4` | **ID4** | 33kV | Standoff Distance | 46 mV | 36 mV | 124 mV | 50 Hz | 11.2% | **33 kV AC (Active Grid)** | LIVE (Triggered) |
| 10 | `IsolatorOpenNotConnectedEarthRod_220kvAtInComingLine_ch220kv` | **ID1** | 220kV | Standoff Distance | 3 mV | 2 mV | 21 mV | 50 Hz | 158.2% | **< 50 V (Shunted to GND)** | SAFE (Quiescent) |
| 11 | `IsolatorOpenNotConnectedEarthRod_220kvBetweenIsolator_ch220kv_1` | **ID1** | 220kV | Standoff Distance | 2 mV | 2 mV | 16 mV | 950 Hz | 408.2% | **< 50 V (Shunted to GND)** | SAFE (Quiescent) |
| 12 | `IsolatorOpenNotConnectedEarthRod_220kvBetweenIsolator_ch220kv_2` | **ID1** | 220kV | Standoff Distance | 4 mV | 4 mV | 25 mV | 50 Hz | 81.7% | **< 50 V (Shunted to GND)** | SAFE (Quiescent) |
| 13 | `IsolatorOpenNotConnectedEarthRod_220kvBetweenIsolator_ch220v` | **ID1** | 230V | Touched (Hot-Stick) | 168 mV | 136 mV | 426 mV | 50 Hz | 16.8% | **~1.78 kV (Induced)** | INDUCED ALARM |
| 14 | `IsolatorOpenNotConnectedEarthRod_220kvBetweenIsolator_ch220v_TouchedLine` | **ID1** | 230V | Touched (Hot-Stick) | 357 mV | 283 mV | 898 mV | 50 Hz | 20.1% | **~3.70 kV (Induced)** | INDUCED ALARM |
| 15 | `IsolatorOpenNotConnectedRarthRod_220kvAtInComingLine_ch230v` | **ID1** | 230V | Touched (Hot-Stick) | 103 mV | 85 mV | 267 mV | 50 Hz | 13.4% | **~1.11 kV (Induced)** | INDUCED ALARM |
| 16 | `BothSideConnectedEarth_220kvAtInComingLine_ch220v_TouchedAtEarthrodconnected` | **ID1** | 230V | Touched (Hot-Stick) | 24 mV | 20 mV | 74 mV | 50 Hz | 16.9% | **~261 V (Induced)** | INDUCED ALARM |
| 17 | `BothSideConnectedEarth_220kvAtInComingLine_ch220v_TouchedLine_1` | **ID1** | 230V | Touched (Hot-Stick) | 114 mV | 29 mV | 96 mV | 50 Hz | 14.5% | **~379 V (Induced)** | INDUCED ALARM |
| 18 | `BothSideConnectedEarth_220kvAtInComingLine_ch220v_TouchedLine_2` | **ID1** | 230V | Touched (Hot-Stick) | 95 mV | 76 mV | 239 mV | 50 Hz | 15.4% | **~993 V (Induced)** | INDUCED ALARM |
| 19 | `BothSideConnectedEarth_220kvAtInComingLine_ch220v_TouchedLine_3` | **ID1** | 230V | Touched (Hot-Stick) | 69 mV | 79 mV | 246 mV | 50 Hz | 14.3% | **~1.03 kV (Induced)** | INDUCED ALARM |
| 20 | `BothSideConnectedEarth_220kvAtInComingLine_ch220v_TouchedLine_Rphase` | **ID1** | 230V | Touched (Hot-Stick) | 16 mV | 12 mV | 44 mV | 50 Hz | 26.9% | **~157 V (Induced)** | QUIET / SAFE |
| 21 | `Ideal_R_Line_AtIsolatedStand_33kvFeeder_NearActive33kvLine` | **ID1** | 230V | Standoff Distance | 27 mV | 22 mV | 74 mV | 50 Hz | 13.4% | **~287 V (Induced)** | INDUCED ALARM |
| 22 | `ideal33kv` | **ID1** | 230V | Touched (Hot-Stick) | 7 mV | 7 mV | 29 mV | 50 Hz | 46.9% | **~91 V (Induced)** | QUIET / SAFE |
| 23 | `ideal_R_Line33kvFeeder_NearActive33kvLine` | **ID1** | 230V | Standoff Distance | 18 mV | 15 mV | 55 mV | 50 Hz | 19.3% | **~196 V (Induced)** | INDUCED ALARM |
| 24 | `ideal_Y_Line33kvFeeder_NearActive33kvLine` | **ID1** | 230V | Standoff Distance | 6 mV | 6 mV | 24 mV | 50 Hz | 51.8% | **~78 V (Induced)** | QUIET / SAFE |
| 25 | `ideal_33kvLineTouched_ch220v` | **ID4** | 230V | Touched (Hot-Stick) | 3 mV | 3 mV | 17 mV | 50 Hz | 105.9% | **~39 V (Induced)** | QUIET / SAFE |

---

## 3. Calibrated Factory Threshold Matrix (Updated)

| Channel | Voltage Rating | Voltrack Standoff | Schematic Shunt (R_shunt) | Live ALC Min | Live BLC Min | Field Benchmark Baseline |
| :---: | :--- | :---: | :---: | :---: | :---: | :--- |
| **0** | **230 V / 415 V** | 0.05 m | 10 MΩ (R6) | **15 mV** | **21 mV** | Substation ambient immunity (<13 mV) vs cable (18 mV) |
| **6** | **33 kV** | 0.50 m | 220 kΩ (R31) | **18 mV** | **24 mV** | Active 33kV measured 24-37 mV ALC (Avg: 32 mV) |
| **9** | **220 kV** | 2.00 m | 24 kΩ (R8) | **9 mV** | **12 mV** | Active 220kV measured 14-16 mV ALC vs 2-4 mV open |
