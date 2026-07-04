# Set environment variables for nRF Connect SDK build
$env:ZEPHYR_TOOLCHAIN_VARIANT = "zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR = "C:\ncs\toolchains\66cdf9b75e\opt\zephyr-sdk"
$env:PATH = "C:\ncs\toolchains\66cdf9b75e\bin;C:\ncs\toolchains\66cdf9b75e\mingw64\bin;C:\ncs\toolchains\66cdf9b75e\opt\bin;C:\ncs\toolchains\66cdf9b75e\opt\bin\Scripts;C:\ncs\toolchains\66cdf9b75e\opt\zephyr-sdk\arm-zephyr-eabi\bin;" + $env:PATH
$env:PYTHONPATH = "C:\ncs\toolchains\66cdf9b75e\opt\bin;C:\ncs\toolchains\66cdf9b75e\opt\bin\Lib;C:\ncs\toolchains\66cdf9b75e\opt\bin\Lib\site-packages"

# Run west build
west build -p always -b WrieLessPotentialDetector_Nrf52832 build/ACDetector
