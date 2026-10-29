#ifndef FTRACE_H
#define FTRACE_H

/* frame tracer for profiling on device. off unless NODOSUMI_FTRACE=<path> is
   set, and then writes 24-byte records to <path> from a background thread:
     u64 t_ns (CLOCK_MONOTONIC), u32 tid, u16 id, u8 phase, u8 0, i64 arg
   with the names of the ids in <path>.names. zones are timed with the
   arch counter and summed per thread as exclusive time, then written out as
   FT_ZONE_NS / FT_ZONE_N records each time the thread calls ftrace_flush */

#include <stdint.h>

enum {
  FT_INSTANT = 0,
  FT_BEGIN = 1,
  FT_END = 2,
  FT_COUNTER = 3,
  FT_ZONE_NS = 4,
  FT_ZONE_N = 5,
};

/* timeline events */
#define FTRACE_EVENTS(X)                                       \
  X(thread_name)                                               \
  X(core_base)                                                 \
  X(dropped)                                                   \
  X(emu_frame)                                                 \
  X(emu_idle)                                                  \
  X(start_render)                                              \
  X(start_render_wait)                                         \
  X(finish_render_wait)                                        \
  X(ta_render)                                                 \
  X(ta_render_end)                                             \
  X(vblank_in)                                                 \
  X(vblank_out)                                                \
  X(skip)                                                      \
  X(retro_run)                                                 \
  X(begin_frame)                                               \
  X(end_frame)                                                 \
  X(end_frame_wait)                                            \
  X(convert)                                                   \
  X(render)                                                    \
  X(video_cb)                                                  \
  X(emu_sync)                                                  \
  X(vid_src)                                                   \
  X(verts)                                                     \
  X(surfs)                                                     \
  X(params)                                                    \
  X(textures_new)                                              \
  X(texture_bytes)                                             \
  X(gpu_ns)                                                    \
  X(gpu_done)                                                  \
  X(gpu_frame)                                                 \
  X(audio_frames)                                              \
  X(audio_queued)                                              \
  X(chd_decode)                                                \
  X(chd_miss)                                                  \
  X(chd_wait)                                                  \
  X(gd_read)                                                   \
  X(jit_compile)                                               \
  X(jit_flush)                                                 \
  X(jit_fastmem_fault)                                         \
  X(cpu_ns)                                                    \
  X(nvcsw)                                                     \
  X(nivcsw)                                                    \
  X(minflt)                                                    \
  X(majflt)                                                    \
  X(cpu)                                                       \
  X(sh4_instrs)                                                \
  X(arm7_instrs)                                               \
  X(guest_frame)                                               \
  X(bot_line)                                                 \
  X(gl_draws)                                                  \
  X(gl_progs)                                                  \
  X(gl_binds)                                                  \
  X(gl_states)                                                 \
  X(gl_uniforms)

/* zones, exclusive time per thread */
#define FTRACE_ZONES(X)                                        \
  X(z_emu)                                                     \
  X(z_sched)                                                   \
  X(z_sh4)                                                     \
  X(z_sh4_interp)                                              \
  X(z_jit_compile)                                             \
  X(z_jit_free)                                                \
  X(z_jit_invalidate)                                          \
  X(z_fastmem_fault)                                           \
  X(z_arm7)                                                    \
  X(z_aica)                                                    \
  X(z_ta_poly)                                                 \
  X(z_ta_tex)                                                  \
  X(z_ta_yuv)                                                  \
  X(z_ta_render)                                               \
  X(z_gdrom)                                                   \
  X(z_chd_read)                                                \
  X(z_chd_wait)                                                \
  X(z_dmac)                                                    \
  X(z_maple)                                                   \
  X(z_start_render)                                            \
  X(z_flush_modified)                                          \
  X(z_register_sources)                                        \
  X(z_evict)                                                   \
  X(z_start_render_wait)                                       \
  X(z_finish_render_wait)                                      \
  X(z_audio_push)                                              \
  X(z_retro_run)                                               \
  X(z_input)                                                   \
  X(z_audio_flush)                                             \
  X(z_res_lock)                                                \
  X(z_convert)                                                 \
  X(z_conv_parse)                                              \
  X(z_conv_tex_decode)                                         \
  X(z_conv_tex_upload)                                         \
  X(z_conv_tex_destroy)                                        \
  X(z_conv_sort)                                               \
  X(z_conv_indices)                                            \
  X(z_conv_map)                                                \
  X(z_conv_rtt)                                                \
  X(z_render)                                                  \
  X(z_render_upload)                                           \
  X(z_render_draw)                                             \
  X(z_dead_tex)                                                \
  X(z_mipmaps)                                                 \
  X(z_draw_pixels)                                             \
  X(z_video_cb)                                                \
  X(z_end_frame_wait)                                          \
  X(z_gpu_query)                                               \
  X(z_chd_decode)

#define FT_ENUM(name) FT_##name,
enum { FTRACE_EVENTS(FT_ENUM) FT_NUM_EVENTS };
#undef FT_ENUM

/* zone ids follow the events, timer callbacks get theirs at runtime after
   the static zones */
#define FT_ENUM(name) FT_##name,
enum {
  FT_ZONE_FIRST = 256,
  FTRACE_ZONES(FT_ENUM) FT_ZONE_LAST,
};
#undef FT_ENUM

#define FT_MAX_ZONES 512
#define FT_MAX_DEPTH 32

extern int ftrace_on;
/* NODOSUMI_FTRACE_FINE also times the zones hit thousands of times a frame */
extern int ftrace_fine;

void ftrace_init();
void ftrace_shutdown();
/* names the calling thread in the trace */
void ftrace_thread(const char *name);
void ftrace_emit(int id, int phase, int64_t arg);
/* zone id for a function pointer, named by its address */
int ftrace_ptr_zone(const void *fn);
/* writes the calling thread's zone sums plus its cpu time, context switches,
   page faults and cpu as counters, then clears the sums */
void ftrace_flush();

struct ftrace_tls {
  int depth;
  uint64_t last;
  uint16_t stack[FT_MAX_DEPTH];
  uint64_t ticks[FT_MAX_ZONES - FT_ZONE_FIRST];
  uint32_t count[FT_MAX_ZONES - FT_ZONE_FIRST];
};

extern __thread struct ftrace_tls ftrace_tls;

static inline uint64_t ftrace_ticks() {
#if defined(__aarch64__)
  uint64_t t;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
  return t;
#else
  uint64_t ftrace_ticks_slow();
  return ftrace_ticks_slow();
#endif
}

static inline void ftrace_zone_begin(int zone) {
  struct ftrace_tls *t = &ftrace_tls;
  uint64_t now = ftrace_ticks();
  if (t->depth > 0) {
    t->ticks[t->stack[t->depth - 1] - FT_ZONE_FIRST] += now - t->last;
  }
  t->last = now;
  if (t->depth < FT_MAX_DEPTH) {
    t->stack[t->depth] = (uint16_t)zone;
  }
  t->depth++;
}

static inline void ftrace_zone_end(int zone) {
  struct ftrace_tls *t = &ftrace_tls;
  uint64_t now = ftrace_ticks();
  if (t->depth > 0 && t->depth <= FT_MAX_DEPTH) {
    int top = t->stack[t->depth - 1] - FT_ZONE_FIRST;
    t->ticks[top] += now - t->last;
    t->count[top]++;
  }
  t->last = now;
  t->depth--;
  (void)zone;
}

#define FT_UNLIKELY(x) __builtin_expect(!!(x), 0)

#define FT_EVENT(id, phase, arg)                     \
  do {                                               \
    if (FT_UNLIKELY(ftrace_on)) {                    \
      ftrace_emit(FT_##id, FT_##phase, (int64_t)(arg)); \
    }                                                \
  } while (0)
#define FT_B(id, arg) FT_EVENT(id, BEGIN, arg)
#define FT_E(id, arg) FT_EVENT(id, END, arg)
#define FT_I(id, arg) FT_EVENT(id, INSTANT, arg)
#define FT_C(id, arg) FT_EVENT(id, COUNTER, arg)

#define FZ_B(zone)                        \
  do {                                    \
    if (FT_UNLIKELY(ftrace_on)) {         \
      ftrace_zone_begin(FT_##zone);       \
    }                                     \
  } while (0)
#define FZ_E(zone)                        \
  do {                                    \
    if (FT_UNLIKELY(ftrace_on)) {         \
      ftrace_zone_end(FT_##zone);         \
    }                                     \
  } while (0)
#define FZF_B(zone)                       \
  do {                                    \
    if (FT_UNLIKELY(ftrace_fine)) {       \
      ftrace_zone_begin(FT_##zone);       \
    }                                     \
  } while (0)
#define FZF_E(zone)                       \
  do {                                    \
    if (FT_UNLIKELY(ftrace_fine)) {       \
      ftrace_zone_end(FT_##zone);         \
    }                                     \
  } while (0)

#endif
