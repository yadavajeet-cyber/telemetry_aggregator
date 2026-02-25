#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include <zephyr/logging/log.h>
#include <math.h>

LOG_MODULE_REGISTER(telemetry, LOG_LEVEL_WRN);

/* ========== Data Structures ========== */

struct telemetry_frame {
    uint32_t frame_id;
    int64_t  timestamp;
    uint32_t uptime;
    int      latest_sensor_value;
    uint32_t sensor_avg_last_200ms;
    bool     degraded;
};

struct sensor_data {
    int64_t timestamp;
    int sensor_value;
};

struct uptime_data {
    int64_t timestamp;
    uint32_t uptime;
};

/* Sensor averaging buffer */
#define SENSOR_AVG_BUFFER_SIZE 20
struct sensor_avg_buffer {
    int sensor_values[SENSOR_AVG_BUFFER_SIZE];
    int64_t timestamps[SENSOR_AVG_BUFFER_SIZE];
    int index;
    int count;
};

/* ========== Constants ========== */

#define TELEMETRY_FRAME_RATE_MS     200  /* 5 Hz */
#define SYNTHETIC_SENSOR_RATE_MS    50   /* 20 Hz */
#define UPTIME_RATE_MS              1000 /* 1 Hz */

#define SENSOR_QUEUE_SIZE           10
#define UPTIME_QUEUE_SIZE           2

#define PRIO_AGGREGATOR             5  /* Lower number = higher priority. Aggregator has to be high priority to meet deadlines. */
#define PRIO_PRODUCER               7
#define PRIO_LOAD_SPIKE             10

#define TRIGGER_SYNTHETIC_SENSOR    1
#define TRIGGER_UPTIME              2

/* ========== Global Variables ========== */

/* Message queues for inter-thread communication */
K_MSGQ_DEFINE(sensor_msgq, sizeof(struct sensor_data), SENSOR_QUEUE_SIZE, 4);
K_MSGQ_DEFINE(uptime_msgq, sizeof(struct uptime_data), UPTIME_QUEUE_SIZE, 4);
K_MSGQ_DEFINE(trigger_msgq, sizeof(uint8_t), SENSOR_QUEUE_SIZE + UPTIME_QUEUE_SIZE, 4); /* Queue for timer-triggered work submissions */


/* Timers for periodic operations */
K_TIMER_DEFINE(uptime_timer, NULL, NULL);
K_TIMER_DEFINE(synthetic_sensor_timer, NULL, NULL);

/* Work items for deferred processing */
K_WORK_DEFINE(uptime_work, NULL);
K_WORK_DEFINE(synthetic_sensor_work, NULL);

/* Thread stacks */
K_THREAD_STACK_DEFINE(telemetry_aggregator_stack, 2048);
K_THREAD_STACK_DEFINE(producer_stack, 1024);
K_THREAD_STACK_DEFINE(load_spike_generator_stack, 1024);

/* Thread control blocks */
struct k_thread telemetry_aggregator_thread;
struct k_thread producer_thread;
struct k_thread load_spike_generator_thread;

/* System state */
static uint32_t frame_counter = 0;
static int64_t system_start_time = 0;

/* ========== Utility Functions ========== */

static inline int64_t get_current_timestamp_ms(void)
{
    return k_uptime_get();
}

static bool is_data_fresh(int64_t data_timestamp, int64_t current_time, int64_t timeout_ms)
{
    return (current_time - data_timestamp) <= timeout_ms;
}

static void add_sensor_to_avg_buffer(struct sensor_avg_buffer *buffer, struct sensor_data data)
{
    buffer->sensor_values[buffer->index] = data.sensor_value;
    buffer->timestamps[buffer->index] = data.timestamp;
    
    buffer->index = (buffer->index + 1) % SENSOR_AVG_BUFFER_SIZE;
    if (buffer->count < SENSOR_AVG_BUFFER_SIZE) {
        buffer->count++;
    }
}

static uint32_t calculate_sensor_avg_200ms(struct sensor_avg_buffer *buffer)
{
    int64_t cutoff_time = get_current_timestamp_ms() - 200;
    int sum = 0;
    int count = 0;
    
    for (int i = 0; i < buffer->count; i++) {
        if (buffer->timestamps[i] >= cutoff_time) {
            sum += buffer->sensor_values[i];
            count++;
        }
    }
    
    return count > 0 ? (uint32_t)(sum / count) : 0;
}

/* ========== Work Handler Functions ========== */

static void synthetic_sensor_work_handler(struct k_work *work)
{
    uint8_t trigger_id = TRIGGER_SYNTHETIC_SENSOR;

    if (k_msgq_put(&trigger_msgq, &trigger_id, K_NO_WAIT) != 0) {
        LOG_WRN("Sensor Trigger queue full, dropping data");
    }
}

static void uptime_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    uint8_t trigger_id = TRIGGER_UPTIME;

    if (k_msgq_put(&trigger_msgq, &trigger_id, K_NO_WAIT) != 0) {
        LOG_WRN("Uptime Trigger queue full, dropping data");
    }
}

/*
 * Generates synthetic sensor data in a sine wave pattern with added random noise to simulate real-world sensor behavior.
 * The data is timestamped and put into the sensor message queue for the aggregator thread to process.
 */
static void synthetic_sensor_data(struct sensor_data *data)
{
    if(data == NULL) {
        return;
    }

    data->timestamp = get_current_timestamp_ms();
    
    /* Generate synthetic sensor data (sine wave with noise) */
    static uint32_t counter = 0;
    double angle = (counter * 2.0 * 3.14159) / 100.0;  /* Complete cycle every 5 seconds at 20Hz */
    int base_value = (int)(50.0 * sin(angle)) + 50;    /* 0-100 range */
    int noise = (sys_rand32_get() % 21) - 10;          /* ±10 noise */
    data->sensor_value = base_value + noise;
    
    if (data->sensor_value < 0) {
        data->sensor_value = 0;
    }

    if (data->sensor_value > 100) {
        data->sensor_value = 100;
    }
    
    counter++;
}

/* ========== Timer Callbacks ========== */

static void uptime_timer_callback(struct k_timer *timer)
{
    k_work_submit(&uptime_work);
}

static void synthetic_sensor_timer_callback(struct k_timer *timer)
{
    k_work_submit(&synthetic_sensor_work);
}

/*
 * The telemetry aggregator thread is responsible for collecting data from the producer thread, 
 * generating telemetry frames at a fixed rate, and outputting them.
 *
 * It uses a timer to ensure strict periodic wakeups every 200ms to maintain the telemetry frame rate. 
 * The thread also checks for data freshness and logs any missed deadlines or degraded conditions in the generated frames.
 */
static void telemetry_aggregator_thread_func(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    
    bool frame_deadline_met;
    int64_t current_frame_time;
    int64_t last_frame_time;
    bool sensor_valid;
    bool degraded;

    struct uptime_data uptime_msg;
    struct sensor_data sensor_msg;

    struct sensor_avg_buffer avg_buffer = {0};

    struct k_timer telemetry_timer;
    k_timer_init(&telemetry_timer, NULL, NULL);
    k_timer_start(&telemetry_timer, K_MSEC(TELEMETRY_FRAME_RATE_MS), K_MSEC(TELEMETRY_FRAME_RATE_MS));

    LOG_INF("Aggregator thread started");
    
    last_frame_time = get_current_timestamp_ms();
    
    while (1) {
        /* Wait for telemetry timer */
        k_timer_status_sync(&telemetry_timer);  // strict periodic wake

        frame_deadline_met = true;
        current_frame_time = get_current_timestamp_ms();
        /* Detect and log missed deadlines (if any) */
        if (current_frame_time - last_frame_time > TELEMETRY_FRAME_RATE_MS + 10) {
            frame_deadline_met = false;
            LOG_WRN("Frame deadline missed by %lld ms", 
                    current_frame_time - last_frame_time - TELEMETRY_FRAME_RATE_MS);
        }
        
        /* Process all available data */
        while (k_msgq_get(&uptime_msgq, &uptime_msg, K_NO_WAIT) == 0) {
            /* keep draining the queue to get the latest uptime message */
        }
        
        sensor_valid = false;
        while (k_msgq_get(&sensor_msgq, &sensor_msg, K_NO_WAIT) == 0) {
            sensor_valid = true;
            add_sensor_to_avg_buffer(&avg_buffer, sensor_msg);
        }
        
        /* Generate telemetry frame */
        struct telemetry_frame frame = {0};
        frame.frame_id = ++frame_counter;
        frame.timestamp = get_current_timestamp_ms();
        
        degraded = false;
        
        /* Check uptime data freshness */
        if (is_data_fresh(uptime_msg.timestamp, frame.timestamp, 2*UPTIME_RATE_MS + 10)) {
            frame.uptime = uptime_msg.uptime;
        } else {
            frame.uptime = (uint32_t)((frame.timestamp - system_start_time) / 1000);
            degraded = true;
        }

        /* Check sensor data freshness */
        if (sensor_valid && 
            is_data_fresh(sensor_msg.timestamp, frame.timestamp, SYNTHETIC_SENSOR_RATE_MS + 10)) {
            frame.latest_sensor_value = sensor_msg.sensor_value;
        } else {
            frame.latest_sensor_value = -1;  /* Invalid marker */
            degraded = true;
        }
        
        frame.sensor_avg_last_200ms = calculate_sensor_avg_200ms(&avg_buffer);
        frame.degraded = degraded || !frame_deadline_met;

        /* Output frame */
        printk("FRAME %u | ts=%lld | up=%u | sensor=%d | avg=%u | degraded=%d\n",
               frame.frame_id, frame.timestamp, frame.uptime,
               frame.latest_sensor_value, frame.sensor_avg_last_200ms,
               frame.degraded ? 1 : 0);
        
        last_frame_time = frame.timestamp;
    }
    
    LOG_INF("Aggregator thread stopped");
}

/*
 * Producer thread simulates data generation for uptime and synthetic sensor at their respective rates.
 * It uses timers to trigger strict periodic wakeups for data generation.
 * 
 * Uptime data is generated every 1 second, while synthetic sensor data is generated every 50ms.
 * Synthetic sensor data: sine wave with added random noise to simulate real-world sensor behavior.
 */
static void producer_thread_func(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    struct uptime_data uptime_msg;
    struct sensor_data sensor_msg;
    uint8_t trigger_id;

    LOG_INF("Producer thread started");
    
    /* Start producer timers */
    k_timer_start(&uptime_timer, K_MSEC(UPTIME_RATE_MS), K_MSEC(UPTIME_RATE_MS));
    k_timer_start(&synthetic_sensor_timer, K_MSEC(SYNTHETIC_SENSOR_RATE_MS), K_MSEC(SYNTHETIC_SENSOR_RATE_MS));
    
    while (1) {
        /* Wait for timer-triggered event */
        k_msgq_get(&trigger_msgq, &trigger_id, K_FOREVER);

        if (trigger_id == TRIGGER_SYNTHETIC_SENSOR) {
            synthetic_sensor_data(&sensor_msg);
            if (k_msgq_put(&sensor_msgq, &sensor_msg, K_NO_WAIT) != 0) {
                LOG_WRN("Sensor queue full, dropping data");
            }
        } else if (trigger_id == TRIGGER_UPTIME) {
            uptime_msg.timestamp = get_current_timestamp_ms();
            uptime_msg.uptime = (uint32_t)((uptime_msg.timestamp - system_start_time) / 1000);
            if (k_msgq_put(&uptime_msgq, &uptime_msg, K_NO_WAIT) != 0) {
                LOG_WRN("Uptime queue full, dropping data");
            }
        }
    }
    
    LOG_INF("Producer thread stopped");
}

/*
 * Load spike generator thread simulates CPU load spikes at random intervals to test the aggregator's ability
 * to handle scheduling pressure and maintain frame deadlines. 
 *
 * The load pattern is designed to be realistic with intermittent bursts of busy work followed by idle periods, 
 * which can help identify potential issues in the aggregator's scheduling and data processing logic under varying load conditions.
 */
static void load_spike_generator_thread_func(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);
    
    LOG_INF("Load simulation thread started");

    uint32_t next_load_spike_interval;
    uint32_t load_spike_burst_duration;
    
    while (1) {
        // idle time before next spike (reduce cpu load)
        next_load_spike_interval = CONFIG_NEXT_LOAD_SPIKE_MIN_INTERVAL_MS + 
                                  (sys_rand32_get() % (CONFIG_NEXT_LOAD_SPIKE_MAX_INTERVAL_MS - CONFIG_NEXT_LOAD_SPIKE_MIN_INTERVAL_MS));
        k_sleep(K_MSEC(next_load_spike_interval));

        // Simulate scheduling pressure by introducing busy spike time (generate cpu load)
        load_spike_burst_duration = CONFIG_LOAD_SPIKE_MIN_DURATION_MS + 
                             (sys_rand32_get() % (CONFIG_LOAD_SPIKE_MAX_DURATION_MS - CONFIG_LOAD_SPIKE_MIN_DURATION_MS));

        LOG_DBG("After interval of %u ms, executing load spike for %u ms", next_load_spike_interval, load_spike_burst_duration);
        
        /* Simulate CPU-intensive work */
        int64_t load_spike_start = get_current_timestamp_ms();
        volatile uint32_t dummy = 0;
        
        while ((get_current_timestamp_ms() - load_spike_start) < load_spike_burst_duration) {
            /* Perform some meaningless calculations to keep the CPU busy */
            for (int i = 0; i < 1000; i++) {
                dummy += sys_rand32_get();
            }
            /* Small yield to prevent complete system lockup */
            if ((dummy % 10000) == 0) {
                k_yield();
            }
        }
        
        LOG_DBG("Load spike completed");
    }
    
    LOG_INF("Load Spike Generator stopped");
}

/* 
 * Initializes the work handlers for uptime and synthetic sensor. 
 */
static void init_workers(void)
{
    k_work_init(&uptime_work, uptime_work_handler);
    k_work_init(&synthetic_sensor_work, synthetic_sensor_work_handler);
    /* No need to initialize a handler for Telemetry and Load work because:
     * Telemetry work is handled directly in the aggregator thread by using strict periodic wake
     * Load work is handled directly in the load spike thread */
}

/* 
 * Initializes the timers for uptime data and synthetic sensor data. 
 * Each timer is associated with its respective callback function that submits work(just a trigger message) to the appropriate work queue.
 */
static void init_timers(void)
{
    k_timer_init(&uptime_timer, uptime_timer_callback, NULL);
    k_timer_init(&synthetic_sensor_timer, synthetic_sensor_timer_callback, NULL);
    /* No need to initialize a timer for telemetry and load because:
     * telemetry frame is handled directly in the aggregator thread by using strict periodic wake
     * load spikes are irregular activity with intermittent burst, strict deadline is not required (not even for logging) */
}

/* 
 * Main function initializes the system, starts the telemetry aggregator thread, and simulates data production and load spikes. 
 * The main thread also periodically prints status updates about the number of frames generated and system uptime.
 */
int main(void)
{
    printk("Zephyr Telemetry Aggregator Starting...\n");
    
    system_start_time = get_current_timestamp_ms();

    init_workers();

    /* Telemetry thread (priority 5) which aggregates data from the producer thread */
    k_thread_create(&telemetry_aggregator_thread, telemetry_aggregator_stack,
                    K_THREAD_STACK_SIZEOF(telemetry_aggregator_stack),
                    telemetry_aggregator_thread_func, NULL, NULL, NULL,
                    PRIO_AGGREGATOR, 0, K_NO_WAIT);
                    
    /* Data production thread (priority 7) that generates uptime and synthetic sensor data at their respective rates. */
    k_thread_create(&producer_thread, producer_stack,
                    K_THREAD_STACK_SIZEOF(producer_stack),
                    producer_thread_func, NULL, NULL, NULL,
                    PRIO_PRODUCER, 0, K_NO_WAIT);

    /* Load spike generator thread (priority 10) that simulates CPU load spikes at random intervals to test 
     * the aggregator's ability to handle scheduling pressure and maintain frame deadlines. */
    k_thread_create(&load_spike_generator_thread, load_spike_generator_stack,
                    K_THREAD_STACK_SIZEOF(load_spike_generator_stack),
                    load_spike_generator_thread_func, NULL, NULL, NULL,
                    PRIO_LOAD_SPIKE, 0, K_NO_WAIT);
    
    init_timers();
    
    printk("Aggregating telemetry data...\n");
    
    /* Main thread becomes monitoring thread */
    int64_t last_status_time = get_current_timestamp_ms();
    
    while (1) {
        k_sleep(K_MSEC(5000));
        
        int64_t current_time = get_current_timestamp_ms();
        if (current_time - last_status_time >= 10000) { /* Status every 10 seconds */
            printk("--- STATUS: Total frames generated %u, system uptime %lld s ---\n",
                   frame_counter, (current_time - system_start_time) / 1000);
            last_status_time = current_time;
        }
    }
    
    printk("Zephyr Telemetry Aggregator Stopped\n");
    return 0;
}
