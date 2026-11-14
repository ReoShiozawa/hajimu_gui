#ifndef HJP_LAYOUT_H
#define HJP_LAYOUT_H
#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <float.h>

typedef struct { double minimum, maximum, weight; } HjpTrack;

/* 最小幅を確保した後、上限に達していない列へ重み付きで残り幅を配る。 */
static bool hjp_layout_widths(double available, double gap, const HjpTrack *tracks,
                              size_t count, double *widths) {
    if (!tracks || !widths || !count || count > 64 || !isfinite(available) ||
        !isfinite(gap) || available < 0 || available > FLT_MAX || gap < 0 || gap > FLT_MAX) return false;
    double used = gap * (count - 1);
    for (size_t i = 0; i < count; i++) {
        HjpTrack t = tracks[i];
        if (!isfinite(t.minimum) || !isfinite(t.maximum) || !isfinite(t.weight) ||
            t.minimum < 0 || t.maximum < t.minimum || t.maximum > FLT_MAX ||
            t.weight < 0 || t.weight > FLT_MAX) return false;
        used += t.minimum;
    }
    if (used > available) return false;
    for (size_t i = 0; i < count; i++) widths[i] = tracks[i].minimum;
    double remaining = available - used;
    for (size_t pass = 0; pass <= count && remaining > 0; pass++) {
        double weights = 0;
        for (size_t i = 0; i < count; i++)
            if (widths[i] < tracks[i].maximum) weights += tracks[i].weight;
        if (weights == 0) break;
        double assigned = 0;
        for (size_t i = 0; i < count; i++) {
            double share = remaining * (tracks[i].weight / weights);
            double delta = fmin(share, tracks[i].maximum - widths[i]);
            widths[i] += delta;
            assigned += delta;
        }
        if (assigned <= 0) break;
        remaining = fmax(0, remaining - assigned);
    }
    return true;
}
#endif
