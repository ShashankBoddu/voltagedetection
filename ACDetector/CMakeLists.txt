cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ACDetector)

target_sources(app PRIVATE 
    src/main.c
    src/adc_lib/adc_backend.c
    src/ble_lib/ble_backend.c
    src/range_lib/range_backend.c
    src/storage_lib/storage_backend.c
)

# Link against MCUboot bootutil library to find headers
zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
