cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(HC4067intterface)

target_sources(app PRIVATE src/main.c src/hc4067.c)
