#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "core/ftrace.h"

int ftrace_on;
int ftrace_fine;
__thread struct ftrace_tls ftrace_tls;

#if defined(__linux__)

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct ft_record {
  uint64_t t;
  uint32_t tid;
  uint16_t id;
  uint8_t phase;
  uint8_t pad;
  int64_t arg;
};

#define FT_RING_SIZE (1 << 16)
#define FT_MAX_RINGS 32

struct ft_ring {
  struct ft_record recs[FT_RING_SIZE];
  uint32_t tid;
  /* head written by the owning thread, tail by the writer */
  uint64_t head;
  uint64_t tail;
  uint64_t dropped;
};

static struct ft_ring *rings[FT_MAX_RINGS];
static int num_rings;
static __thread struct ft_ring *my_ring;
static pthread_mutex_t ft_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t ft_writer;
static int ft_quit;
static FILE *ft_file;
static char ft_names_path[1024];
static int ft_names_dirty;

static const void *ptr_keys[FT_MAX_ZONES];
static int ptr_zones[FT_MAX_ZONES];
static int next_zone = FT_ZONE_LAST;

static uint64_t tick_freq;

#define FT_NAME(name) #name,
static const char *event_names[] = {FTRACE_EVENTS(FT_NAME)};
static const char *zone_names[] = {FTRACE_ZONES(FT_NAME)};
#undef FT_NAME

static char thread_names[FT_MAX_RINGS][32];

uint64_t ftrace_ticks_slow() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static uint64_t ft_now() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int64_t ticks_to_ns(uint64_t ticks) {
  return (int64_t)((__uint128_t)ticks * 1000000000ull / tick_freq);
}

static struct ft_ring *ft_get_ring() {
  if (my_ring) {
    return my_ring;
  }
  struct ft_ring *ring = calloc(1, sizeof(*ring));
  ring->tid = (uint32_t)syscall(SYS_gettid);
  pthread_mutex_lock(&ft_mutex);
  if (num_rings < FT_MAX_RINGS) {
    rings[num_rings++] = ring;
  }
  pthread_mutex_unlock(&ft_mutex);
  my_ring = ring;
  return ring;
}

static void ft_push(struct ft_ring *ring, uint64_t t, int id, int phase,
                    int64_t arg) {
  uint64_t head = ring->head;
  uint64_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
  if (head - tail >= FT_RING_SIZE) {
    ring->dropped++;
    return;
  }
  struct ft_record *r = &ring->recs[head & (FT_RING_SIZE - 1)];
  r->t = t;
  r->tid = ring->tid;
  r->id = (uint16_t)id;
  r->phase = (uint8_t)phase;
  r->pad = 0;
  r->arg = arg;
  __atomic_store_n(&ring->head, head + 1, __ATOMIC_RELEASE);
}

void ftrace_emit(int id, int phase, int64_t arg) {
  ft_push(ft_get_ring(), ft_now(), id, phase, arg);
}

void ftrace_thread(const char *name) {
  if (!ftrace_on) {
    return;
  }
  struct ft_ring *ring = ft_get_ring();
  pthread_mutex_lock(&ft_mutex);
  for (int i = 0; i < num_rings; i++) {
    if (rings[i] == ring) {
      snprintf(thread_names[i], sizeof(thread_names[i]), "%s", name);
    }
  }
  ft_names_dirty = 1;
  pthread_mutex_unlock(&ft_mutex);
  ftrace_emit(FT_thread_name, FT_INSTANT, 0);
}

int ftrace_ptr_zone(const void *fn) {
  unsigned h = (unsigned)(((uintptr_t)fn >> 2) * 2654435761u) % FT_MAX_ZONES;
  for (int i = 0; i < FT_MAX_ZONES; i++) {
    unsigned slot = (h + i) % FT_MAX_ZONES;
    const void *key = __atomic_load_n(&ptr_keys[slot], __ATOMIC_ACQUIRE);
    if (key == fn) {
      return ptr_zones[slot];
    }
    if (!key) {
      break;
    }
  }

  pthread_mutex_lock(&ft_mutex);
  int zone = 0;
  for (int i = 0; i < FT_MAX_ZONES; i++) {
    unsigned slot = (h + i) % FT_MAX_ZONES;
    if (ptr_keys[slot] == fn) {
      zone = ptr_zones[slot];
      break;
    }
    if (!ptr_keys[slot]) {
      zone = next_zone < FT_MAX_ZONES ? next_zone++ : FT_z_sched;
      ptr_zones[slot] = zone;
      __atomic_store_n(&ptr_keys[slot], fn, __ATOMIC_RELEASE);
      ft_names_dirty = 1;
      break;
    }
  }
  pthread_mutex_unlock(&ft_mutex);
  return zone;
}

void ftrace_flush() {
  if (!ftrace_on) {
    return;
  }
  struct ft_ring *ring = ft_get_ring();
  struct ftrace_tls *t = &ftrace_tls;
  uint64_t now = ft_now();

  /* charge the open zone up to now, so a flush inside one is exact */
  if (t->depth > 0 && t->depth <= FT_MAX_DEPTH) {
    uint64_t ticks = ftrace_ticks();
    t->ticks[t->stack[t->depth - 1] - FT_ZONE_FIRST] += ticks - t->last;
    t->last = ticks;
  }

  for (int i = 0; i < next_zone - FT_ZONE_FIRST; i++) {
    if (!t->count[i] && !t->ticks[i]) {
      continue;
    }
    ft_push(ring, now, FT_ZONE_FIRST + i, FT_ZONE_NS, ticks_to_ns(t->ticks[i]));
    ft_push(ring, now, FT_ZONE_FIRST + i, FT_ZONE_N, t->count[i]);
    t->ticks[i] = 0;
    t->count[i] = 0;
  }

  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  ft_push(ring, now, FT_cpu_ns, FT_COUNTER,
          (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec);
  struct rusage ru;
  getrusage(RUSAGE_THREAD, &ru);
  ft_push(ring, now, FT_nvcsw, FT_COUNTER, ru.ru_nvcsw);
  ft_push(ring, now, FT_nivcsw, FT_COUNTER, ru.ru_nivcsw);
  ft_push(ring, now, FT_minflt, FT_COUNTER, ru.ru_minflt);
  ft_push(ring, now, FT_majflt, FT_COUNTER, ru.ru_majflt);
  ft_push(ring, now, FT_cpu, FT_COUNTER, sched_getcpu());
  if (ring->dropped) {
    ft_push(ring, now, FT_dropped, FT_COUNTER, (int64_t)ring->dropped);
  }
}

static void ft_write_names() {
  FILE *fp = fopen(ft_names_path, "w");
  if (!fp) {
    return;
  }
  for (int i = 0; i < FT_NUM_EVENTS; i++) {
    fprintf(fp, "%d %s\n", i, event_names[i]);
  }
  for (int i = 0; i < FT_ZONE_LAST - FT_ZONE_FIRST - 1; i++) {
    fprintf(fp, "%d %s\n", FT_ZONE_FIRST + 1 + i, zone_names[i]);
  }
  for (int i = 0; i < FT_MAX_ZONES; i++) {
    if (ptr_keys[i]) {
      fprintf(fp, "%d ptr:%p\n", ptr_zones[i], ptr_keys[i]);
    }
  }
  for (int i = 0; i < num_rings; i++) {
    if (thread_names[i][0]) {
      fprintf(fp, "thread %u %s\n", rings[i]->tid, thread_names[i]);
    }
  }
  fprintf(fp, "tick_freq %llu\n", (unsigned long long)tick_freq);
  fclose(fp);
}

static void ft_drain() {
  pthread_mutex_lock(&ft_mutex);
  int n = num_rings;
  if (ft_names_dirty) {
    ft_names_dirty = 0;
    ft_write_names();
  }
  pthread_mutex_unlock(&ft_mutex);

  for (int i = 0; i < n; i++) {
    struct ft_ring *ring = rings[i];
    uint64_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
    uint64_t tail = ring->tail;
    while (tail < head) {
      uint64_t idx = tail & (FT_RING_SIZE - 1);
      uint64_t run = head - tail;
      if (run > FT_RING_SIZE - idx) {
        run = FT_RING_SIZE - idx;
      }
      fwrite(&ring->recs[idx], sizeof(struct ft_record), run, ft_file);
      tail += run;
    }
    __atomic_store_n(&ring->tail, tail, __ATOMIC_RELEASE);
  }
  fflush(ft_file);
}

static void *ft_writer_thread(void *data) {
  (void)data;
  while (!__atomic_load_n(&ft_quit, __ATOMIC_ACQUIRE)) {
    struct timespec ts = {0, 100 * 1000 * 1000};
    nanosleep(&ts, NULL);
    ft_drain();
  }
  ft_drain();
  return NULL;
}

void ftrace_init() {
  const char *path = getenv("NODOSUMI_FTRACE");
  if (!path || ftrace_on) {
    return;
  }
  ft_file = fopen(path, "wb");
  if (!ft_file) {
    return;
  }
  snprintf(ft_names_path, sizeof(ft_names_path), "%s.names", path);

#if defined(__aarch64__)
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(tick_freq));
#else
  tick_freq = 1000000000ull;
#endif

  ft_names_dirty = 1;
  ftrace_on = 1;
  ftrace_fine = getenv("NODOSUMI_FTRACE_FINE") != NULL;

  /* lets the analysis turn ptr: zones into symbols */
  Dl_info info;
  if (dladdr((void *)&ftrace_init, &info)) {
    ftrace_emit(FT_core_base, FT_INSTANT, (int64_t)(uintptr_t)info.dli_fbase);
  }

  pthread_create(&ft_writer, NULL, &ft_writer_thread, NULL);
}

void ftrace_shutdown() {
  if (!ftrace_on) {
    return;
  }
  __atomic_store_n(&ft_quit, 1, __ATOMIC_RELEASE);
  pthread_join(ft_writer, NULL);
  fclose(ft_file);
  ft_file = NULL;
  ftrace_on = 0;
  ftrace_fine = 0;
}

#else

void ftrace_init() {}
void ftrace_shutdown() {}
void ftrace_thread(const char *name) {}
void ftrace_emit(int id, int phase, int64_t arg) {}
int ftrace_ptr_zone(const void *fn) {
  return FT_z_sched;
}
void ftrace_flush() {}

#endif
