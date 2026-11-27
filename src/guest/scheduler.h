#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "core/time.h"

struct dreamcast;
struct timer;
struct scheduler;
struct state;

/* double, not float: the tmu hands the guest a 32-bit down counter, and float
   only has 24 bits of mantissa, so cycles past ~2^24 had their low 8 bits
   pinned to 0. the bios measures elapsed time with (0xff - tcnt) differences
   masked to 7 bits, so every delta read as 0 and timed waits never expired. */
#define HZ_TO_NANO(hz) (int64_t)((double)NS_PER_SEC / (double)(hz))
#define NANO_TO_CYCLES(ns, hz) \
  (int64_t)(((double)(ns) * (double)(hz)) / (double)NS_PER_SEC)
#define CYCLES_TO_NANO(cycles, hz) \
  (int64_t)(((double)(cycles) * (double)NS_PER_SEC) / (double)(hz))

typedef void (*timer_cb)(void *);

struct scheduler *sched_create(struct dreamcast *dc);
void sched_destroy(struct scheduler *sch);

void sched_tick(struct scheduler *sch, int64_t ns);

struct timer *sched_start_timer(struct scheduler *sch, timer_cb cb, void *data,
                                int64_t ns);
int64_t sched_remaining_time(struct scheduler *sch, struct timer *);
void sched_cancel_timer(struct scheduler *sch, struct timer *);

/* save state support: timers are serialized by their owning device as a
   (active, remaining) pair so no host pointers need to be persisted */
void sched_state_timer(struct scheduler *sch, struct state *s, int save,
                       struct timer *timer);

#endif
