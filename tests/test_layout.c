#include "../src/hjp_layout.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>

int main(void) {
    HjpTrack tracks[] = {{100, 150, 1}, {100, 1000, 3}};
    double widths[64] = {777};
    assert(hjp_layout_widths(408, 8, tracks, 2, widths));
    assert(fabs(widths[0] - 150) < 1e-8 && fabs(widths[1] - 250) < 1e-8);
    widths[0] = 777;
    assert(!hjp_layout_widths(207, 8, tracks, 2, widths));
    assert(widths[0] == 777);
    assert(!hjp_layout_widths(NAN, 8, tracks, 2, widths));
    assert(!hjp_layout_widths(400, -1, tracks, 2, widths));
    tracks[0].weight = 0;
    assert(hjp_layout_widths(408, 8, tracks, 2, widths));
    assert(widths[0] == 100 && widths[1] == 300);
    tracks[0].maximum = 99;
    assert(!hjp_layout_widths(408, 8, tracks, 2, widths));
    uint32_t random = 17;
    for (int iteration = 0; iteration < 10000; iteration++) {
        HjpTrack t[16];
        size_t n = (size_t)(iteration % 16 + 1);
        double available = (n - 1) * 8;
        for (size_t i = 0; i < n; i++) {
            random = random * 1664525u + 1013904223u;
            t[i] = (HjpTrack){random % 200, 0, (random >> 16) % 5};
            t[i].maximum = t[i].minimum + (random >> 8) % 300;
            available += t[i].minimum;
        }
        available += random % 2000;
        assert(hjp_layout_widths(available, 8, t, n, widths));
        double total = (n - 1) * 8;
        for (size_t i = 0; i < n; i++) {
            assert(widths[i] >= t[i].minimum && widths[i] <= t[i].maximum + 1e-8);
            if (!t[i].weight) assert(widths[i] == t[i].minimum);
            total += widths[i];
        }
        assert(total <= available + 1e-7);
    }
    puts("layout passed: limits, weights, no overflow, invalid inputs, 10000 generated layouts");
}
