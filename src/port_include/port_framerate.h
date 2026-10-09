#ifndef SMS_PORT_FRAMERATE_H
#define SMS_PORT_FRAMERATE_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
extern int port_frame_rate;
extern int port_active_frame_rate;
extern int port_high_fps_active;
}

// Configuration is independent of GPU performance and monitor refresh rate.
static inline int port_parse_frame_rate(const char* value)
{
    if (value && (!strcmp(value, "30") || !strcmp(value, "60") || !strcmp(value, "120")))
        return atoi(value);
    return 60;
}

static inline int port_vi_retrace_multiplier()
{
    return port_frame_rate == 120 ? 2 : 1;
}

static inline int port_native_frame_divisor()
{
    return port_active_frame_rate / 30;
}

static inline float port_frame_scale()
{
    return 30.0f / port_active_frame_rate;
}

// Custom rates authored as animation frames per native 30 Hz display frame.
// Rates already based on SMSGetAnmFrameRate, or advanced on movement ticks,
// must not pass through this conversion.
static inline float port_native_animation_rate(float rate)
{
    return rate * port_frame_scale();
}

// Movement stays at 120 ticks/s. Keep the existing 60 fps phase, and step
// integer frame counters on one of each four displayed frames at 120 fps.
static inline bool port_native_frame_step(unsigned int ticks)
{
    return !port_high_fps_active || (ticks & 3) == (unsigned int)(120 / port_active_frame_rate);
}

static inline float port_frame_chase(float rate)
{
    if (!port_high_fps_active)
        return rate;
    float remaining = sqrtf(1.0f - rate);
    if (port_active_frame_rate == 120)
        remaining = sqrtf(remaining);
    return 1.0f - remaining;
}

#endif
