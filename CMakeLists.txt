cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(telemetry_aggregator)

target_compile_definitions(app PRIVATE 
    CONFIG_NEXT_LOAD_SPIKE_MIN_INTERVAL_MS=500
    CONFIG_NEXT_LOAD_SPIKE_MAX_INTERVAL_MS=3000
    CONFIG_LOAD_SPIKE_MIN_DURATION_MS=10
    CONFIG_LOAD_SPIKE_MAX_DURATION_MS=100
)

target_sources(app PRIVATE src/main.c)
