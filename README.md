# Telemetry Aggregator Application

## High-Level Architecture Overview

The Telemetry Aggregator is a Zephyr RTOS-based application that simulates real-time telemetry data collection and aggregation. It consists of three main threads that independently work to produce data, aggregate and report data at a fixed but strict 5 Hz rate (200ms intervals), and introduce intermittent load spikes.

The system generates synthetic sensor data (sine wave with noise) at a fixed 20 Hz rate (50msec intervals) and system uptime information at a fixed 1 Hz rate (1sec intervals), aggregates them into structured frames, and outputs them via console. A load simulation thread introduces CPU pressure to test the system's robustness under varying conditions. The load spikes are configurable via CMakeList.txt where the minimum and maximum load cycle intervals and also the minimum and maximum spike durations can be configured.

Example load configuration:

    CONFIG_NEXT_LOAD_SPIKE_MIN_INTERVAL_MS=500
    CONFIG_NEXT_LOAD_SPIKE_MAX_INTERVAL_MS=3000
    CONFIG_LOAD_SPIKE_MIN_DURATION_MS=10
    CONFIG_LOAD_SPIKE_MAX_DURATION_MS=100

### Control Flow

Uptime/Sensor Timers → k_work_submit() → Work Handler -> k_msgq_put(trigger) -> Producer Thread

Producer Thread → Message Queues → Aggregator Thread

Aggregator Thread → Strict periodic wait → k_msgq_get() -> Strucuture Frame -> Report

Load Spike Thread → Load Interval Sleep → Intermittent Busy Wait

## Thread Model and Priority Rationale

The telemetry application uses three threads with priorities chosen to ensure real-time performance:

**Telemetry Aggregator Thread (Priority 5)**: Highest priority thread responsible for maintaining strict 200ms frame deadlines. This priority ensures the aggregator can preempt other threads to meet timing requirements.

**Producer Thread (Priority 7)**: Generates synthetic sensor data (20 Hz) and uptime data (1 Hz). Fixed data rate is achieved using strict timers. Lower priority than aggregator but higher than load generator to ensure data production doesn't starve the aggregator. It waits for timer-triggered sensor and uptime events and effectively sleeping while waiting for data messages rather than blocking indefintely.

**Load Spike Generator Thread (Priority 10)**: Lowest priority thread that simulates random CPU load spikes. This simulates scheduling pressure which allows testing of system behavior under load while ensuring critical telemetry operations take precedence.

Priority assignment follows real-time principles: critical timing-sensitive operations get highest priority, followed by data producers, with testing/simulator threads at lowest priority.

## Data Ownership Model

Data ownership follows a producer-consumer pattern with clear boundaries ensuring asynchronus delivery:

**Producer Thread Owns**: Synthetic sensor data generation and uptime calculation. Data is timestamped and placed in message queues.

**Aggregator Thread Owns**: Telemetry frame assembly, data validation, averaging sensor calculations, and report the assembled frame to console. The aggregator drains queues to get the latest available data from producer.

**Shared Resources**: Message queues (`sensor_msgq`, `uptime_msgq`) act as ownership transfer points between producer and aggregator.

**Sensor Average Buffer**: Owned by the aggregator thread, used for maintaining a rolling 200ms average of sensor values.

Data freshness is validated at consumption time, with invalid/stale data marked as degraded in the telemetry frame.

## Backpressure or Overload Handling Policy

The system implements several backpressure mechanisms to handle overload conditions:

**Message Queue Limits**: All message queues are bounded. Sensor queue (10 items), uptime queue (2 items), trigger queue (12 items). When queues are full, new data is dropped with warning logs.

**Non-Blocking Queues**: All message queue operations use `K_NO_WAIT`, allowing threads to continue execution even if queues are full.

**Data Freshness Checks**: Aggregator validates data age (sensor: <60ms, uptime: <2010ms). Any stale data is reported as degraded.

**Deadline Monitoring**: Aggregator detects missed 200ms frame deadlines and logs warnings, setting degradation flags.

**Load Simulation**: Controlled CPU spikes with yields prevent complete system lockup during overload testing.

This design prioritizes the system stability over perfect data delivery, ensuring the aggregator continues operating even under extreme load.

## Known Limitations

**Load Spike Configuration**: Ideal configuration is using custom Kconfig with default values and overriding them through prj.conf. For the simplicity, both load spike interval and load spike duration are configured directly using CMakeList file. Respective minimum and maximum values can be configured directly from CMakeLists.

**Synthetic Data Only**: Uses generated data rather than real sensors.

**Fixed Frame Rate**: Fixed 5 Hz output as its not configurable.

**Memory Constraints**: Small queue sizes may lead to data loss under high load (when configured).

**Console Output Bottleneck**: Printk-based output may become a performance bottleneck at high frame rates.

**No Persistence**: Telemetry data is not stored or transmitted, only printed to console.

**Load Simulation Simplicity**: CPU load spikes use busy-wait loops intermittently. Sleep used may drift as interval and spike durations are not strictly timed. This can result into not strictly timed logging of the load spikes.
