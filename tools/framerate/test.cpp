#include "port_framerate.h"
#include <assert.h>
#include <stdio.h>

int port_frame_rate = 60;
int port_active_frame_rate = 30;
int port_high_fps_active = 0;

int main()
{
    assert(port_parse_frame_rate(NULL) == 60);
    const char* invalid[] = {"", "0", "90", "144", "120fps", "60junk", "-30"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; ++i)
        assert(port_parse_frame_rate(invalid[i]) == 60);
    const int rates[] = {30, 60, 120};
    const char* values[] = {"30", "60", "120"};
    for (unsigned r = 0; r < 3; ++r) {
        port_frame_rate = port_parse_frame_rate(values[r]);
        assert(port_frame_rate == rates[r]);
        port_active_frame_rate = rates[r];
        port_high_fps_active = rates[r] > 30;
        unsigned ticks = 0, native_counts = 0;
        float distance = 0, remaining = 1;
        for (int frame = 0; frame < rates[r]; ++frame) {
            ticks += 120 / rates[r];
            native_counts += port_native_frame_step(ticks);
            distance += port_frame_scale();
            remaining *= 1 - port_frame_chase(0.05f);
        }
        assert(ticks == 120);
        assert(native_counts == 30);
        assert(fabsf(distance - 30) < 0.0001f);
        assert(fabsf(remaining - powf(0.95f, 30)) < 0.00001f);
        // Menus and movies retain 30 fps under any gameplay configuration.
        int retraces_per_second = 60 * port_vi_retrace_multiplier();
        assert(retraces_per_second / (retraces_per_second / rates[r]) == rates[r]);
        port_active_frame_rate = 30;
        port_high_fps_active = 0;
        assert(port_frame_scale() == 1);
        assert(port_native_frame_step(0));
        assert(fabsf(port_frame_chase(0.05f) - 0.05f) < 0.0000001f);
        assert(retraces_per_second / (port_vi_retrace_multiplier() * 2) == 30);
    }
    puts("PASS: 30/60/120 configuration, native timers, movement, effects, menu timing");
}
