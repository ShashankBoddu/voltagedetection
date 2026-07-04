import asyncio
import struct
import numpy as np
from scipy.fft import rfft, rfftfreq
from bleak import BleakScanner, BleakClient
from datetime import datetime
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

# --- BLE Constants ---
SERVICE_UUID    = "12345678-1234-5678-1234-56789abcdef0"
TX_UUID         = "12345678-1234-5678-1234-56789abcdef1"  
RX_UUID         = "12345678-1234-5678-1234-56789abcdef2"  
DEVICE_NAME     = "AC_DETECTOR"

# --- Signal Characteristics ---
SAMPLE_COUNT    = 200
FS              = 10000.0  # Sampling frequency in Hz
DATASETS_NEEDED = 10  
ADC_LSB_uV      = 879

global_datasets = []
current_buf_blc = [0] * SAMPLE_COUNT
current_buf_alc = [0] * SAMPLE_COUNT
dataset_ready_event = asyncio.Event()

def handle_notification(sender: int, data: bytearray):
    global current_buf_blc, current_buf_alc, global_datasets
    if len(data) < 1: return
    pkt_type = data[0]

    if pkt_type in [1, 2, 0xFF]:
        if len(data) < 4: return
        count = data[1]
        start_idx = data[2] | (data[3] << 8)

        if pkt_type == 0xFF:  
            if any(current_buf_blc) or any(current_buf_alc):
                global_datasets.append({
                    'blc': list(current_buf_blc),
                    'alc': list(current_buf_alc)
                })
            current_buf_blc = [0] * SAMPLE_COUNT
            current_buf_alc = [0] * SAMPLE_COUNT
            dataset_ready_event.set()
            return

        fmt = f"<{count}h"
        expected_bytes = 4 + (count * 2)
        if len(data[:expected_bytes]) < expected_bytes: return
        
        samples = struct.unpack_from(fmt, data, 4)
        if pkt_type == 1:
            for i in range(count):
                if (start_idx + i) < SAMPLE_COUNT:
                    current_buf_blc[start_idx + i] = samples[i]
        elif pkt_type == 2:
            for i in range(count):
                if (start_idx + i) < SAMPLE_COUNT:
                    current_buf_alc[start_idx + i] = samples[i]


def extract_best_sample(datasets, channel_key):
    """Selects the perfectly captured trace having minimum Total Harmonic Distortion."""
    best_thd = float('inf')
    best_trace = None
    best_metrics = {}

    for d in datasets:
        # Replicate strict Firmware math to match the phone app perfectly!
        raw_counts = np.array(d[channel_key], dtype=np.int32)
        count = len(raw_counts)
        if count == 0 or np.max(raw_counts) == 0:
            continue
            
        # First Pass: Mean, Min, Max
        sum_counts = int(np.sum(raw_counts))
        mean_counts = sum_counts // count
        min_c, max_c = int(np.min(raw_counts)), int(np.max(raw_counts))
        
        # Second Pass: AC RMS
        ac_counts = raw_counts - mean_counts
        sq_sum = int(np.sum(ac_counts.astype(np.int64) ** 2))
        rms_counts = int(np.sqrt(sq_sum / count))
        p2p_counts = max_c - min_c
        
        # Exact firmware integer mV mappings
        mean_mv = (mean_counts * ADC_LSB_uV) // 1000
        rms_mv = (rms_counts * ADC_LSB_uV) // 1000
        p2p_mv = (p2p_counts * ADC_LSB_uV) // 1000
        
        # Frequency Domain floating point computations
        trace_mv = raw_counts * (ADC_LSB_uV / 1000.0)
        ac_signal = trace_mv - float(mean_mv)
        N = len(trace_mv)
        yf = rfft(ac_signal)
        xf = rfftfreq(N, 1 / FS)
        
        mags = np.abs(yf)
        mags[0] = 0 # Ignore DC 
        
        peak_idx = np.argmax(mags)
        fund_freq = xf[peak_idx]
        fund_mag = mags[peak_idx]
        
        if fund_mag < 10.0:
            thd = 100.0  
        else:
            sum_sq_harmonics = np.sum(mags**2) - fund_mag**2
            if sum_sq_harmonics < 0: sum_sq_harmonics = 0
            thd = (np.sqrt(sum_sq_harmonics) / fund_mag) * 100
            
        # Record Best Trace
        if thd < best_thd:
            best_thd = thd
            
            top_harmonics = []
            top_indices = np.argsort(mags)[-5:][::-1]
            for idx in top_indices:
                if mags[idx] > 0.05 * fund_mag and xf[idx] > 0:
                    top_harmonics.append((xf[idx], mags[idx]))

            best_trace = ac_signal  # Zero-center the waveform by removing the hardware DC bias
            best_metrics = {
                "mean_val": mean_mv,
                "rms_val": rms_mv,
                "p2p": p2p_mv,
                "fund_freq": fund_freq,
                "fund_mag": fund_mag,
                "thd": thd,
                "harmonics": top_harmonics,
                "xf": xf,
                "mags": mags
            }
            
    return best_trace, best_metrics


def render_industrial_dashboard(blc_trace, blc_metrics, alc_trace, alc_metrics):
    """Constructs a professional Matplotlib UI layout formatted as an Industrial Standard Report."""
    
    # Enable a modern aesthetic look
    plt.style.use('bmh')
    
    fig = plt.figure(figsize=(16, 10))
    fig.canvas.manager.set_window_title('Industrial Standard AC Signal Analysis Report')
    
    gs = gridspec.GridSpec(3, 2, height_ratios=[0.8, 2, 2])
    
    # -------------------------------------------------------------
    # 1. Report Header & Details (Top Row)
    # -------------------------------------------------------------
    ax_header = fig.add_subplot(gs[0, :])
    ax_header.axis('off')
    
    current_time = datetime.now().strftime('%d %B %Y | %H:%M:%S')
    
    header_text = (
        "INDUSTRIAL STANDARD SIGNAL ANALYSIS REPORT (AC)\n"
        "Reference Standard: IS 14700 / IEC 61000\n"
        f"Device Under Test (DUT): {DEVICE_NAME} (nRF52)      Test Timestamp: {current_time}\n"
        "Internal Processing: Hardware Capacitive Antenna -> Active Filtration -> ADC -> 10 kHz DSP\n"
    )
    
    ax_header.text(0.5, 0.6, header_text, ha='center', va='center', 
                   fontsize=14, fontweight='bold', color='#1e3a8a')
    
    # -------------------------------------------------------------
    # Helper for Metrics Display
    # -------------------------------------------------------------
    def get_quality_and_type(thd):
        if thd < 5.0: return "EXCELLENT", "Pure Sine Wave"
        elif thd < 15.0: return "ACCEPTABLE", "Distorted Sine"
        else: return "HIGH DISTORTION", "Complex / Noisy"
        
    def add_metrics_box(ax, metrics, title, color):
        """Places a styled text block summarizing the core standard values."""
        quality, wave_type = get_quality_and_type(metrics['thd'])
        text = (
            f"{title} SUMMARY\n"
            "----------------------------------\n"
            f"Waveform Type  : {wave_type}\n"
            f"DC Offset Bias : {metrics['mean_val']} mV\n"
            f"True RMS Value : {metrics['rms_val']} mV\n"
            f"Peak-to-Peak   : {metrics['p2p']} mV\n\n"
            f"Dominant Freq  : {metrics['fund_freq']:.1f} Hz\n"
            f"T.H. Distortion: {metrics['thd']:.2f}% [{quality}]\n"
        )
        
        props = dict(boxstyle='round', facecolor=color, alpha=0.15, edgecolor=color)
        ax.text(0.02, 0.95, text, transform=ax.transAxes, fontsize=11, family='monospace', fontweight='bold',
                verticalalignment='top', bbox=props)

    # Time Arrays
    t_ms = np.arange(SAMPLE_COUNT) * (1 / FS) * 1000.0

    # -------------------------------------------------------------
    # 2. BLC Channel Layout
    # -------------------------------------------------------------
    ax_blc_time = fig.add_subplot(gs[1, 0])
    ax_blc_time.plot(t_ms, blc_trace, color='#2563eb', linewidth=2.5)
    ax_blc_time.set_title("BLC (Core Antenna) - Pure AC Waveform", fontsize=12)
    ax_blc_time.set_ylabel("AC Amplitude (mV)")
    ax_blc_time.grid(True, linestyle='--', alpha=0.7)
    add_metrics_box(ax_blc_time, blc_metrics, "BLC", "#2563eb")
    
    ax_blc_fft = fig.add_subplot(gs[1, 1])
    markerline, stemlines, baseline = ax_blc_fft.stem(blc_metrics['xf'], blc_metrics['mags'], basefmt=" ")
    plt.setp(stemlines, 'color', '#2563eb', 'linewidth', 2)
    plt.setp(markerline, 'color', '#2563eb')
    ax_blc_fft.set_title("BLC - Frequency Harmonics (FFT)", fontsize=12)
    ax_blc_fft.set_ylabel("Magnitude")
    ax_blc_fft.set_xlim(0, max(500, blc_metrics['fund_freq'] * 5)) 
    
    # -------------------------------------------------------------
    # 3. ALC Channel Layout
    # -------------------------------------------------------------
    ax_alc_time = fig.add_subplot(gs[2, 0])
    ax_alc_time.plot(t_ms, alc_trace, color='#dc2626', linewidth=2.5)
    ax_alc_time.set_title("ALC (After Fillter) - Pure AC Waveform", fontsize=12)
    ax_alc_time.set_xlabel("Time (ms)")
    ax_alc_time.set_ylabel("AC Amplitude (mV)")
    ax_alc_time.grid(True, linestyle='--', alpha=0.7)
    add_metrics_box(ax_alc_time, alc_metrics, "ALC", "#dc2626")

    ax_alc_fft = fig.add_subplot(gs[2, 1])
    markerline2, stemlines2, baseline2 = ax_alc_fft.stem(alc_metrics['xf'], alc_metrics['mags'], basefmt=" ")
    plt.setp(stemlines2, 'color', '#dc2626', 'linewidth', 2)
    plt.setp(markerline2, 'color', '#dc2626')
    ax_alc_fft.set_title("ALC - Frequency Harmonics (FFT)", fontsize=12)
    ax_alc_fft.set_xlabel("Frequency (Hz)")
    ax_alc_fft.set_ylabel("Magnitude")
    ax_alc_fft.set_xlim(0, max(500, alc_metrics['fund_freq'] * 5)) 
    
    plt.tight_layout()
    plt.show()


async def main():
    print(f"Scanning for BLE device named '{DEVICE_NAME}'...")
    devices = await BleakScanner.discover(timeout=5.0)
    
    target_device = next((d for d in devices if d.name == DEVICE_NAME), None)
    if not target_device:
        print("Device not found! Please check power, proximity, and advertisement.")
        return
        
    print(f"Found {DEVICE_NAME} [{target_device.address}]. Connecting...")
    
    async with BleakClient(target_device) as client:
        print("Connected successfully!")
        
        await client.start_notify(TX_UUID, handle_notification)
        
        print(f"Acquiring {DATASETS_NEEDED} sample buffers. Please wait...")
        for i in range(DATASETS_NEEDED):
            dataset_ready_event.clear()
            await client.write_gatt_char(RX_UUID, b"R", response=False)
            
            try:
                await asyncio.wait_for(dataset_ready_event.wait(), timeout=10.0)
            except asyncio.TimeoutError:
                break
                
            await asyncio.sleep(0.5)

        await client.stop_notify(TX_UUID)
        
    if len(global_datasets) > 0:
        print("\nProcessing single purest timeframe...")
        best_blc_trace, blc_metrics = extract_best_sample(global_datasets, 'blc')
        best_alc_trace, alc_metrics = extract_best_sample(global_datasets, 'alc')
        
        print("Generating Matplotlib Dashboard UI... Close the graphics window to end the script.")
        render_industrial_dashboard(best_blc_trace, blc_metrics, best_alc_trace, alc_metrics)
    else:
        print("No datasets successfully collected. Transmission failed.")

if __name__ == "__main__":
    asyncio.run(main())
