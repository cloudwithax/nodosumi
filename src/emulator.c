#include <time.h>
/*
 * client code for the dreamcast machine
 *
 * acts as a middle man between the dreamcast guest and local host. the host
 * interface provides callbacks for user input events, window resize events,
 * etc. that need to be passed to the dreamcast, while the dreamcast interface
 * provides callbacks that push frames of video and audio data to be presented
 * on the host
 *
 * this code encapsulates the logic that would otherwise need to be duplicated
 * for each of the multiple host implementations (sdl, libretro, etc.)
 */

#include "emulator.h"
#include <stdio.h>
#include <stdlib.h>
#include "core/memory.h"
#include "core/thread.h"
#include "core/time.h"
#include "file/trace.h"
#include "guest/aica/aica.h"
#include "guest/arm7/arm7.h"
#include "guest/bios/bios.h"
#include "guest/dreamcast.h"
#include "guest/gdrom/gdrom.h"
#include "guest/holly/holly.h"
#include "guest/maple/maple.h"
#include "guest/memory.h"
#include "guest/pvr/pvr.h"
#include "guest/pvr/ta.h"
#include "guest/pvr/tr.h"
#include "guest/scheduler.h"
#include "guest/sh4/sh4.h"
#include "host/host.h"
#include "imgui.h"
#include "options.h"
#include "render/render_backend.h"
#include "stats.h"

enum {
  ASPECT_RATIO_STRETCH,
  ASPECT_RATIO_16BY9,
  ASPECT_RATIO_4BY3,
};

/* emulation thread state */
enum {
  EMU_SHUTDOWN,
  EMU_WAITING,
  EMU_RUNFRAME,
  EMU_DRAWFRAME,
  EMU_ENDFRAME,
};

enum {
  EMU_SOURCE_NONE,
  EMU_SOURCE_CTX,
  EMU_SOURCE_PXL,
};

struct emu_framebuffer {
  uint8_t data[PVR_FRAMEBUFFER_SIZE];
  int width;
  int height;
};

struct emu_texture {
  struct tr_texture;
  struct emu *emu;
  struct list_node free_it;
  struct rb_node live_it;

  struct memory_watch *texture_watch;
  struct memory_watch *palette_watch;
  struct list_node modified_it;
  int modified;
  int written;
  /* in the live tree. a stale watch can mark a free entry modified, so dirty
     alone doesn't say */
  int live;
};

/* rt output, standing in for the texture it was rendered to */
struct emu_target {
  struct tr_texture tex;
  uint32_t addr;
};

struct emu {
  struct host *host;
  struct render_backend *r;

  struct dreamcast *dc;
  int aspect_ratio;

  /* when running with multiple threads, the dreamcast emulation is ran on a
     secondary thread, in order to allow rendering to be done asynchronously on
     the main thread. the original hardware rendered asynchronously, so many
     games use this time to perform additional cpu work. on some games this
     upwards of doubles the performance */
  int multi_threaded;

  /* emulation thread synchronization primitives */
  volatile int state;
  volatile unsigned frame;
  thread_t run_thread;
  mutex_t req_mutex;
  cond_t req_cond;
  mutex_t res_mutex;
  cond_t res_cond;

  /* latest video state pushed by the dreamcast */
  volatile int vid_disabled;
  volatile int vid_source;
  /* double buffered so the last frame can render while the cpu runs the next */
  struct tr_context vid_rc[2];
  int vid_pending;
  int vid_src;
  int vid_off;
  struct emu_framebuffer vid_fb;

  /* latest context submitted to emu_start_render */
  struct ta_context *pending_ctx;
  /* video thread is converting without res_mutex */
  int converting;
  /* set when the video thread won't take another context until this frame
     ends, so the emu thread shouldn't wait on one */
  int no_take;
  /* a frame is running whose end emu_end_frame hasn't waited for */
  int started;

  /* texture cache. the dreamcast interface calls into us when new contexts are
     available to be rendered. parsing the contexts, uploading their textures to
     the render backend, and managing the texture cache is our responsibility */
  struct emu_texture textures[8192];
  struct list free_textures;
  struct rb_tree live_textures;
  /* last live texture found, keyed by hash */
  struct emu_texture *recent[1024];

  /* textures for the current context are uploaded to the render backend by the
     video thread in parallel to the emulation thread executing. normally, this
     is safe as the real hardware also rendered asynchronously. unfortunately,
     some games will be naughty and modify a texture before receiving the end of
     render interrupt. in order to avoid race conditions around accessing the
     texture's dirty state, textures are not immediately marked dirty by the
     emulation thread when modified. instead, they are added to this modified
     list which will be processed the next time the threads are synchronized */
  struct list modified_textures;

  /* evicted on the emu thread, destroyed by the video thread */
  texture_handle_t evicted[8];
  int num_evicted;
  /* where the next eviction pass starts */
  int next_evict;

  /* 8 rt targets, oldest replaced first. enough in practice */
  struct emu_target targets[8];
  int next_target;

  /* debugging */
  struct trace_writer *trace_writer;
};

/*
 * texture cache
 */
static int emu_texture_cmp(const struct rb_node *rb_lhs,
                           const struct rb_node *rb_rhs) {
  const struct emu_texture *lhs =
      rb_entry(rb_lhs, const struct emu_texture, live_it);
  tr_texture_key_t lhs_key = tr_texture_key(lhs->tsp, lhs->tcw);

  const struct emu_texture *rhs =
      rb_entry(rb_rhs, const struct emu_texture, live_it);
  tr_texture_key_t rhs_key = tr_texture_key(rhs->tsp, rhs->tcw);

  if (lhs_key < rhs_key) {
    return -1;
  } else if (lhs_key > rhs_key) {
    return 1;
  } else {
    return 0;
  }
}

static struct rb_callbacks emu_texture_cb = {&emu_texture_cmp, NULL, NULL};

static void emu_dirty_textures(struct emu *emu) {
  LOG_INFO("emu_dirty_textures");

  struct rb_node *it = rb_first(&emu->live_textures);

  while (it) {
    struct rb_node *next = rb_next(it);
    struct emu_texture *tex = rb_entry(it, struct emu_texture, live_it);

    tex->dirty = 1;

    it = next;
  }
}

static void emu_flush_modified(struct emu *emu,
                                        const struct ta_context *ctx) {
  list_for_each_entry(tex, &emu->modified_textures, struct emu_texture,
                      modified_it) {
    /* palette ram is one page, so a write to any entry trips every paletted
       texture's watch. keep it if its own entries didn't change */
    int same =
        !tex->written && tex->handle && tex->palette &&
        tr_palette_hash(tex->palette, tex->palette_size, ctx->palette_fmt) ==
            tex->palette_hash;
    if (!same) {
      tex->dirty = 1;
    }
    tex->modified = 0;
    tex->written = 0;
  }

  list_clear(&emu->modified_textures);
}

static void emu_texture_modified(const struct exception_state *ex, void *data) {
  struct emu_texture *tex = data;
  tex->written = 1;
  tex->texture_watch = NULL;

  if (!tex->modified) {
    list_add(&tex->emu->modified_textures, &tex->modified_it);
    tex->modified = 1;
  }
}

static void emu_palette_modified(const struct exception_state *ex, void *data) {
  struct emu_texture *tex = data;
  tex->palette_watch = NULL;

  if (!tex->modified) {
    list_add(&tex->emu->modified_textures, &tex->modified_it);
    tex->modified = 1;
  }
}

static void emu_free_texture(struct emu *emu, struct emu_texture *tex) {
  /* remove from live tree */
  rb_unlink(&emu->live_textures, &tex->live_it, &emu_texture_cb);
  memset(emu->recent, 0, sizeof(emu->recent));
  tex->live = 0;

  /* add back to free list */
  list_add(&emu->free_textures, &tex->free_it);
}

static struct emu_texture *emu_alloc_texture(struct emu *emu, union tsp tsp,
                                             union tcw tcw) {
  /* remove from free list */
  struct emu_texture *tex =
      list_first_entry(&emu->free_textures, struct emu_texture, free_it);
  CHECK_NOTNULL(tex);
  list_remove(&emu->free_textures, &tex->free_it);

  /* reset tex */
  memset(tex, 0, sizeof(*tex));
  tex->emu = emu;
  tex->tsp = tsp;
  tex->tcw = tcw;
  tex->live = 1;

  /* add to live tree */
  rb_insert(&emu->live_textures, &tex->live_it, &emu_texture_cb);

  return tex;
}

/* a texture written over sits in the cache until the game asks for it again,
   which it may never do, so free ones not asked for in a while. a slice each
   frame, while the video thread isn't converting */
static void emu_evict_textures(struct emu *emu) {
  int i;

  for (i = 0; i < 64 && emu->num_evicted < ARRAY_SIZE(emu->evicted); i++) {
    struct emu_texture *tex = &emu->textures[emu->next_evict];
    emu->next_evict = (emu->next_evict + 1) % ARRAY_SIZE(emu->textures);

    /* a set watch can't go, its page would fault with no handler */
    if (tex->live && tex->dirty && !tex->texture_watch &&
        !tex->palette_watch && emu->frame - tex->frame > 120) {
      if (tex->handle) {
        emu->evicted[emu->num_evicted++] = tex->handle;
      }
      emu_free_texture(emu, tex);
    }
  }
}

static struct tr_texture *emu_find_texture(void *userdata, union tsp tsp,
                                           union tcw tcw) {
  struct emu *emu = userdata;

  struct emu_texture search;
  search.tsp = tsp;
  search.tcw = tcw;

  struct emu_texture *tex =
      rb_find_entry(&emu->live_textures, &search, struct emu_texture, live_it,
                    &emu_texture_cb);
  return (struct tr_texture *)tex;
}

/* rt output never hits texture memory, so a lookup there finds the target
   until the guest writes over it */
static struct tr_texture *emu_find_target(void *userdata,
                                                  union tsp tsp,
                                                  union tcw tcw) {
  struct emu *emu = userdata;
  uint32_t addr = tcw.texture_addr << 3;

  for (int i = 0; i < ARRAY_SIZE(emu->targets); i++) {
    struct emu_target *target = &emu->targets[i];

    if (target->addr == addr && target->tex.handle &&
        target->tex.width == ta_texture_width(tsp, tcw) &&
        target->tex.height == ta_texture_height(tsp, tcw) &&
        pvr_test_rtt(emu->dc->pvr, addr)) {
      return &target->tex;
    }
  }

  return emu_find_texture(userdata, tsp, tcw);
}

static void emu_render_target(struct emu *emu, const struct ta_context *ctx,
                              const struct tr_context *rc) {
  /* replace the one at this address, else the oldest */
  struct emu_target *target = NULL;
  int i;

  for (i = 0; i < ARRAY_SIZE(emu->targets) && !target; i++) {
    if (emu->targets[i].tex.handle && emu->targets[i].addr == ctx->rtt_addr) {
      target = &emu->targets[i];
    }
  }
  if (!target) {
    target = &emu->targets[emu->next_target];
    emu->next_target = (emu->next_target + 1) % ARRAY_SIZE(emu->targets);
  }

  /* a new texture each time, the context may sample the old one */
  int w = ctx->video_width;
  int h = ctx->video_height;
  int alpha = ctx->rtt_fmt == 2 || ctx->rtt_fmt == 3 || ctx->rtt_fmt == 6;
  r_destroy_texture(emu->r, target->tex.handle);
  target->addr = ctx->rtt_addr;
  target->tex.width = w;
  target->tex.height = h;
  target->tex.handle =
      r_create_texture(emu->r, alpha ? PXL_RGBA : PXL_RGB, FILTER_BILINEAR,
                       WRAP_REPEAT, WRAP_REPEAT, 0, w, h, NULL);

  r_begin_target(emu->r, target->tex.handle, w, h);
  tr_render_context(emu->r, rc);
  r_end_target(emu->r);
}

static void emu_register_source(struct emu *emu, union tsp tsp,
                                        union tcw tcw) {
  tr_texture_key_t key = tr_texture_key(tsp, tcw);
  struct emu_texture **recent =
      &emu->recent[(key * 0x9e3779b97f4a7c15ull) >> 54];
  struct emu_texture *entry = *recent;

  if (!entry || entry->tsp.full != tsp.full || entry->tcw.full != tcw.full) {
    entry = (struct emu_texture *)emu_find_texture(emu, tsp, tcw);

    if (!entry) {
      entry = emu_alloc_texture(emu, tsp, tcw);
      entry->dirty = 1;
    }

    *recent = entry;
  }

  /* mark valid for the pending frame */
  int first = entry->frame != emu->frame;
  entry->frame = emu->frame;

  /* set texture address */
  if (!entry->texture || !entry->palette) {
    ta_texture_info(emu->dc->ta, tsp, tcw, &entry->texture,
                    &entry->texture_size, &entry->palette,
                    &entry->palette_size);
  }

#ifdef NDEBUG
  /* add write callback in order to invalidate on future writes. the callback
     address will be page aligned, therefore it will be triggered falsely in
     some cases. over invalidate in these cases */
  if (!entry->texture_watch) {
    entry->texture_watch = add_single_write_watch(
        entry->texture, entry->texture_size, &emu_texture_modified, entry);
  }

  if (entry->palette && !entry->palette_watch) {
    entry->palette_watch = add_single_write_watch(
        entry->palette, entry->palette_size, &emu_palette_modified, entry);
  }
#endif

  if (emu->trace_writer && entry->dirty && first) {
    trace_writer_insert_texture(emu->trace_writer, tsp, tcw, entry->frame,
                                entry->palette, entry->palette_size,
                                entry->texture, entry->texture_size);
  }
}

static void emu_register_sources(struct emu *emu,
                                         struct ta_context *ctx) {
  const uint8_t *data = ctx->params;
  const uint8_t *end = ctx->params + ctx->size;
  int vert_type = 0;
  /* runs of polys usually share a texture, so skip repeats of the last */
  int have_last = 0;
  union tsp last_tsp = {0};
  union tcw last_tcw = {0};

  if (ctx->bg_isp.texture) {
    emu_register_source(emu, ctx->bg_tsp, ctx->bg_tcw);
  }

  if (ctx->num_tex_runs >= 0) {
    for (int i = 0; i < ctx->num_tex_runs; i++) {
      emu_register_source(emu, ctx->tex_tsp[i], ctx->tex_tcw[i]);
    }
    return;
  }

  int list_type = TA_NUM_LISTS;

  while (data < end) {
    union pcw pcw = ta_pcw_in_list(*(union pcw *)data, list_type);

    if (ta_pcw_list_type_valid(pcw, list_type)) {
      list_type = pcw.list_type;
    }

    switch (pcw.para_type) {
      case TA_PARAM_END_OF_LIST:
        list_type = TA_NUM_LISTS;
        break;

      case TA_PARAM_POLY_OR_VOL:
      case TA_PARAM_SPRITE: {
        const union poly_param *param = (const union poly_param *)data;

        vert_type = ta_vert_type(pcw);

        if (param->type0.pcw.texture &&
            !(have_last && param->type0.tsp.full == last_tsp.full &&
              param->type0.tcw.full == last_tcw.full)) {
          emu_register_source(emu, param->type0.tsp, param->type0.tcw);
          have_last = 1;
          last_tsp = param->type0.tsp;
          last_tcw = param->type0.tcw;
        }
      } break;

      default:
        break;
    }

    data += ta_param_size(pcw, vert_type);
  }
}

/*
 * trace recording
 */
static void emu_stop_tracing(struct emu *emu) {
  if (!emu->trace_writer) {
    return;
  }

  trace_writer_close(emu->trace_writer);
  emu->trace_writer = NULL;

  LOG_INFO("end tracing");
}

static void emu_start_tracing(struct emu *emu) {
  if (emu->trace_writer) {
    return;
  }

  char filename[PATH_MAX];
  get_next_trace_filename(filename, sizeof(filename));

  emu->trace_writer = trace_writer_open(filename);

  if (!emu->trace_writer) {
    LOG_INFO("failed to start tracing");
    return;
  }

  /* clear texture cache in order to generate insert events for all
     textures referenced while tracing */
  emu_dirty_textures(emu);

  LOG_INFO("begin tracing to %s", filename);
}

/*
 * dreamcast guest interface
 */
static void emu_vblank_in(void *userdata, int vid_disabled) {
  struct emu *emu = userdata;

  if (emu->multi_threaded) {
    mutex_lock(emu->res_mutex);
  }

  emu->state = EMU_DRAWFRAME;
  emu->vid_disabled = vid_disabled;

  if (emu->multi_threaded) {
    cond_signal(emu->res_cond);
    mutex_unlock(emu->res_mutex);
  }
}

static void emu_vblank_out(void *userdata) {
  struct emu *emu = userdata;

  if (emu->multi_threaded) {
    mutex_lock(emu->res_mutex);
  }

  emu->state = EMU_ENDFRAME;

  if (emu->multi_threaded) {
    cond_signal(emu->res_cond);
    mutex_unlock(emu->res_mutex);
  }
}

static int g_stats_enabled = -1;
static int64_t g_stat_render0;
static int64_t g_stat_render1;
static int64_t g_stat_vblank;
static int64_t g_stat_post;
static int64_t g_stat_conv;
static int64_t g_stat_stall;
static int64_t g_stat_skip;

static void emu_finish_render(void *userdata) {
  struct emu *emu = userdata;

  if (emu->multi_threaded) {
    /* ideally, the video thread has parsed the pending context, uploaded its
       textures, etc. during the estimated render time. however, if it hasn't
       finished, the emulation thread must be paused to avoid altering
       the yet-to-be-uploaded texture memory */
    int64_t t0 = g_stats_enabled > 0 ? time_nanoseconds() : 0;
    mutex_lock(emu->res_mutex);

    /* the context can only be converted until its render finishes, so wait for
       the video thread to take it rather than skip the frame */
    while (emu->converting || (emu->pending_ctx && !emu->no_take)) {
      cond_wait(emu->res_cond, emu->res_mutex);
    }
    if (g_stats_enabled > 0) {
      g_stat_stall += time_nanoseconds() - t0;
    }

    /* if pending_ctx is non-NULL here, a frame is being skipped */
    if (emu->pending_ctx) {
      g_stat_skip++;
    }
    emu->pending_ctx = NULL;
    cond_signal(emu->res_cond);

    mutex_unlock(emu->res_mutex);
  }
}

static void emu_start_render(void *userdata, struct ta_context *ctx) {
  struct emu *emu = userdata;

  /* incement internal frame number. this frame number is assigned to the each
     texture source registered to assert synchronization between the emulator
     and video thread is working as expected */
  emu->frame++;

  /* now that the video thread is sure to not be accessing the texture data,
     mark any textures dirty that were invalidated by a memory watch */
  emu_flush_modified(emu, ctx);

  /* register the source of each texture referenced by the context with the
     tile renderer. note, uploading the texture to the render backend happens
     lazily while converting the context. this registration just lets the
     backend know where the texture's source data is */
  emu_register_sources(emu, ctx);

  if (emu->trace_writer) {
    trace_writer_render_context(emu->trace_writer, ctx);
  }

  if (emu->multi_threaded) {
    /* save off context and notify video thread that it's available */
    mutex_lock(emu->res_mutex);
    if (g_stats_enabled > 0) {
      g_stat_post = time_nanoseconds();
    }

    while (emu->converting) {
      cond_wait(emu->res_cond, emu->res_mutex);
    }

    emu_evict_textures(emu);
    emu->pending_ctx = ctx;
    cond_signal(emu->res_cond);

    mutex_unlock(emu->res_mutex);
  } else {
    emu_evict_textures(emu);
    emu->pending_ctx = ctx;
  }
}

static void emu_push_pixels(void *userdata, const uint8_t *data, int w, int h) {
  struct emu *emu = userdata;

  memcpy(emu->vid_fb.data, data, w * h * 4);
  emu->vid_fb.width = w;
  emu->vid_fb.height = h;

  emu->vid_source = EMU_SOURCE_PXL;
}

static void emu_push_audio(void *userdata, const int16_t *data, int frames) {
  struct emu *emu = userdata;
  audio_push(emu->host, data, frames);
}

/*
 * options
 */
static void emu_set_aspect_ratio(struct emu *emu, const char *new_ratio) {
  int i;

  for (i = 0; i < NUM_ASPECT_RATIOS; i++) {
    const char *aspect_ratio = ASPECT_RATIOS[i];

    if (!strcmp(aspect_ratio, new_ratio)) {
      break;
    }
  }

  /* force to stretch if the new ratio isn't a valid one */
  if (i == NUM_ASPECT_RATIOS) {
    i = ASPECT_RATIO_STRETCH;
  }

  /* update persistent option as well as this session's aspect ratio */
  strncpy(OPTION_aspect, ASPECT_RATIOS[i], sizeof(OPTION_aspect));
  emu->aspect_ratio = i;
}

/*
 * frame running logic
 */
static void emu_run_until_vblank(struct emu *emu);

static int64_t g_stat_ecpu;
static int64_t g_stat_ewall;

static int64_t thread_cpu_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *emu_run_thread(void *data) {
  struct emu *emu = data;

  while (1) {
    /* wait for video thread to request a frame to be ran */
    mutex_lock(emu->req_mutex);

    while (emu->state == EMU_WAITING) {
      cond_wait(emu->req_cond, emu->req_mutex);
    }

    if (emu->state == EMU_SHUTDOWN) {
      mutex_unlock(emu->req_mutex);
      break;
    }

    int64_t c0 = thread_cpu_ns(), w0 = time_nanoseconds();
    emu_run_until_vblank(emu);
    g_stat_ecpu = thread_cpu_ns() - c0;
    g_stat_ewall = time_nanoseconds() - w0;

    emu->state = EMU_WAITING;
    /* wake emu_sync */
    cond_signal(emu->req_cond);

    mutex_unlock(emu->req_mutex);
  }

  return NULL;
}

static void emu_run_until_vblank(struct emu *emu) {
  const int64_t step = HZ_TO_NANO(1000);

  emu->state = EMU_RUNFRAME;

  while (emu->state == EMU_RUNFRAME || emu->state == EMU_DRAWFRAME) {
    dc_tick(emu->dc, step);
  }
}

static int64_t g_stat_ta;
static int64_t g_stat_sh4;
static int64_t g_stat_arm7;
static int64_t g_stat_jit;
static int64_t g_stat_flush;
static int64_t g_stat_compile;

static void emu_start_frame(struct emu *emu) {
  /* fresh source for the next frame */
  emu->vid_source = EMU_SOURCE_NONE;

  if (emu->multi_threaded) {
    mutex_lock(emu->req_mutex);

    CHECK_EQ(emu->state, EMU_WAITING);
    emu->no_take = 0;
    emu->state = EMU_RUNFRAME;
    cond_signal(emu->req_cond);

    mutex_unlock(emu->req_mutex);
  }

  emu->started = 1;
}

/* res_mutex is held. in mt mode drop it while converting so the emu thread
   isn't stuck at vblank; start/finish_render both wait on converting */
static void emu_convert_pending(struct emu *emu) {
  struct ta_context *ctx = emu->pending_ctx;

  if (!ctx) {
    return;
  }

  emu->pending_ctx = NULL;

  if (emu->multi_threaded) {
    emu->converting = 1;
    mutex_unlock(emu->res_mutex);
  }

  for (int i = 0; i < emu->num_evicted; i++) {
    r_destroy_texture(emu->r, emu->evicted[i]);
  }
  emu->num_evicted = 0;

  int conv_idx = (emu->vid_pending >= 0) ? (1 - emu->vid_pending) : 0;
  int64_t c0 = g_stats_enabled ? time_nanoseconds() : 0;
  tr_convert_context(emu->r, emu, &emu_find_target, ctx,
                     &emu->vid_rc[conv_idx]);

  /* rt isn't presented, draw it into its texture now */
  int rtt = ctx->rtt;
  if (rtt) {
    emu_render_target(emu, ctx, &emu->vid_rc[conv_idx]);
  }

  if (g_stats_enabled) {
    g_stat_conv += time_nanoseconds() - c0;
  }

  if (emu->multi_threaded) {
    mutex_lock(emu->res_mutex);
    emu->converting = 0;
    cond_signal(emu->res_cond);
  }

  if (!rtt) {
    emu->vid_source = EMU_SOURCE_CTX;
    emu->vid_pending = conv_idx;
  }
}

void emu_sync(struct emu *emu) {
  /* the emu thread holds req_mutex during a frame. don't let
     emu_finish_render wait on a context nobody will take */
  if (emu->multi_threaded) {
    mutex_lock(emu->res_mutex);
    int no_take = emu->no_take;
    emu->no_take = 1;
    cond_signal(emu->res_cond);
    mutex_unlock(emu->res_mutex);

    /* lock free isn't enough, a just-started frame may not be taken yet */
    mutex_lock(emu->req_mutex);
    while (emu->state != EMU_WAITING) {
      cond_wait(emu->req_cond, emu->req_mutex);
    }
    mutex_unlock(emu->req_mutex);

    mutex_lock(emu->res_mutex);
    emu->no_take = no_take;
    mutex_unlock(emu->res_mutex);
  }
}

int emu_begin_frame(struct emu *emu) {
  if (g_stats_enabled < 0) {
    g_stats_enabled = getenv("NODOSUMI_STATS") != NULL;
  }

  prof_counter_add(COUNTER_frames, 1);

  if (OPTION_aspect_dirty) {
    emu_set_aspect_ratio(emu, OPTION_aspect);
    OPTION_aspect_dirty = 0;
  }

  if (!dc_running(emu->dc)) {
    r_clear(emu->r);
    /* since the host times itself based off of our audio output, it's important
       to pump out silent audio frames even when not running the dreamcast, else
       the host will render the ui completely unthrottled  */
    uint32_t silence[AICA_SAMPLE_FREQ / 60] = {0};
    audio_push(emu->host, (int16_t *)silence, ARRAY_SIZE(silence));
    return 1;
  }

  int width = r_width(emu->r);
  int height = r_height(emu->r);
  int frame_width;
  int frame_height;
  int frame_x;
  int frame_y;

  if (emu->aspect_ratio == ASPECT_RATIO_STRETCH) {
    frame_height = height;
    frame_width = width;
    frame_x = 0;
    frame_y = 0;
  } else if (emu->aspect_ratio == ASPECT_RATIO_16BY9) {
    frame_width = width;
    frame_height = (int)(frame_width * (9.0f / 16.0f));
    frame_x = 0;
    frame_y = (int)((height - frame_height) / 2.0f);
  } else if (emu->aspect_ratio == ASPECT_RATIO_4BY3) {
    frame_height = height;
    frame_width = (int)(frame_height * (4.0f / 3.0f));
    frame_x = (int)((width - frame_width) / 2.0f);
    frame_y = 0;
  } else {
    LOG_FATAL("emu_begin_frame unexpected aspect ratio %d", emu->aspect_ratio);
  }

  r_viewport(emu->r, frame_x, frame_y, frame_width, frame_height);

  /* grab the last frame */
  int psrc = emu->vid_src;
  int pdis = emu->vid_off;
  int pidx = emu->vid_pending;
  emu->vid_src = EMU_SOURCE_NONE;

  /* games can go many vblanks without a new image. keep the last one
     instead of flashing black. video-disable still gives a black frame */
  int updated = psrc != EMU_SOURCE_NONE || pdis;
  if (updated) {
    r_clear(emu->r);
  }

  if (g_stats_enabled) {
    g_stat_render0 = time_nanoseconds();
    g_stat_ta = prof_counter_load_current(COUNTER_ta_renders);
    g_stat_sh4 = prof_counter_load_current(COUNTER_sh4_instrs);
    g_stat_arm7 = prof_counter_load_current(COUNTER_arm7_instrs);
    g_stat_jit = prof_counter_load_current(COUNTER_jit_blocks);
    g_stat_flush = prof_counter_load_current(COUNTER_jit_flushes);
    g_stat_compile = prof_counter_load_current(COUNTER_jit_compile_ns);
  }

  /* the sw pixel path shares vid_fb with the emu thread, so draw it before
     the next frame starts */
  if (psrc == EMU_SOURCE_PXL && !pdis) {
    r_draw_pixels(emu->r, emu->vid_fb.data, 0, 0, emu->vid_fb.width,
                  emu->vid_fb.height);
  }

  /* start the emu thread first so the render below overlaps it. usually
     emu_end_frame started it already and it ran between calls */
  if (!emu->started) {
    emu_start_frame(emu);
  } else if (emu->multi_threaded) {
    /* take a context the running frame submitted, so it doesn't block at
       end of render */
    mutex_lock(emu->res_mutex);
    emu_convert_pending(emu);
    mutex_unlock(emu->res_mutex);
  }

  /* render last frame's context while the cpu runs this one */
  if (psrc == EMU_SOURCE_CTX && !pdis) {
    tr_render_context(emu->r, &emu->vid_rc[pidx]);
  }

  /* textures replaced by those are never drawn again */
  r_delete_dead_textures(emu->r);
  r_make_mipmaps(emu->r);

  if (g_stats_enabled) {
    g_stat_render1 = time_nanoseconds();
  }
  return updated;
}

void emu_end_frame(struct emu *emu) {
  if (!dc_running(emu->dc)) {
    return;
  }

  if (!emu->multi_threaded) {
    emu_run_until_vblank(emu);
  }

  /* take contexts submitted up to vblank_out. the emu thread waits in
     emu_finish_render for each one, so don't stop at vblank_in and block on
     req_mutex while one from vblank is still waiting. anything after
     vblank_out is dropped */
  if (emu->multi_threaded) {
    mutex_lock(emu->res_mutex);

    while (emu->state == EMU_RUNFRAME || emu->state == EMU_DRAWFRAME) {
      if (emu->pending_ctx) {
        emu_convert_pending(emu);
      } else {
        cond_wait(emu->res_cond, emu->res_mutex);
      }
    }
  }

  emu_convert_pending(emu);

  if (emu->multi_threaded) {
    emu->no_take = 1;
    mutex_unlock(emu->res_mutex);
  }

  /* what to render next call */
  if (emu->vid_source == EMU_SOURCE_PXL) {
    emu->vid_src = EMU_SOURCE_PXL;
  } else if (emu->vid_source == EMU_SOURCE_CTX) {
    emu->vid_src = EMU_SOURCE_CTX;
  } else {
    emu->vid_src = EMU_SOURCE_NONE;
  }
  emu->vid_off = emu->vid_disabled;
  emu->started = 0;

  /* run the next frame while the frontend is between calls. the pixel path's
     vid_fb is drawn in emu_begin_frame, so that one has to wait */
  if (emu->multi_threaded && emu->vid_src != EMU_SOURCE_PXL) {
    emu_start_frame(emu);
  }

  if (g_stats_enabled) {
    g_stat_vblank = time_nanoseconds();
    fprintf(stderr,
            "STAT frame=%lld pre=%.2fms render=%.2fms ta=%lld sh4=%lld "
            "arm7=%lld jit=%lld flush=%lld compile=%.2fms post=%.2fms "
            "conv=%.2fms stall=%.2fms ecpu=%.2fms ewall=%.2fms skip=%lld "
            "surfs=%d verts=%d\n",
            (long long)prof_counter_load_current(COUNTER_frames),
            (double)(g_stat_vblank - g_stat_render0) / 1e6,
            (double)(g_stat_render1 - g_stat_render0) / 1e6,
            (long long)(prof_counter_load_current(COUNTER_ta_renders) - g_stat_ta),
            (long long)(prof_counter_load_current(COUNTER_sh4_instrs) - g_stat_sh4),
            (long long)(prof_counter_load_current(COUNTER_arm7_instrs) -
                        g_stat_arm7),
            (long long)(prof_counter_load_current(COUNTER_jit_blocks) - g_stat_jit),
            (long long)(prof_counter_load_current(COUNTER_jit_flushes) -
                        g_stat_flush),
            (double)(prof_counter_load_current(COUNTER_jit_compile_ns) -
                     g_stat_compile) /
                1e6,
            (double)(g_stat_post - g_stat_render0) / 1e6,
            (double)g_stat_conv / 1e6, (double)g_stat_stall / 1e6,
            (double)g_stat_ecpu / 1e6, (double)g_stat_ewall / 1e6,
            (long long)g_stat_skip,
            emu->vid_pending >= 0 ? emu->vid_rc[emu->vid_pending].lists[0].num_surfs +
                emu->vid_rc[emu->vid_pending].lists[1].num_surfs +
                emu->vid_rc[emu->vid_pending].lists[2].num_surfs : 0,
            emu->vid_pending >= 0 ? emu->vid_rc[emu->vid_pending].num_verts : 0);
    g_stat_post = g_stat_render0;
    g_stat_conv = 0;
    g_stat_stall = 0;
  }
}

void emu_render_frame(struct emu *emu) {
  emu_begin_frame(emu);
  emu_end_frame(emu);
}

void emu_debug_menu(struct emu *emu) {
#ifdef HAVE_IMGUI
  /* ensure the emulation thread isn't still executing a previous frame */
  if (emu->multi_threaded) {
    mutex_lock(emu->req_mutex);
    CHECK_EQ(emu->state, EMU_WAITING);
    mutex_unlock(emu->req_mutex);
  }

  if (igBeginMainMenuBar()) {
    if (igBeginMenu("EMU", 1)) {
      if (igMenuItem("clear texture cache", NULL, 0, 1)) {
        emu_dirty_textures(emu);
      }
      if (!emu->trace_writer && igMenuItem("start trace", NULL, 0, 1)) {
        emu_start_tracing(emu);
      }
      if (emu->trace_writer && igMenuItem("stop trace", NULL, 1, 1)) {
        emu_stop_tracing(emu);
      }
      igEndMenu();
    }

    igEndMainMenuBar();
  }

  holly_debug_menu(emu->dc->holly);
  aica_debug_menu(emu->dc->aica);
  arm7_debug_menu(emu->dc->arm7);
  sh4_debug_menu(emu->dc->sh4);

  /* add status */
  if (igBeginMainMenuBar()) {
    char status[128];
    int frames = (int)prof_counter_load(COUNTER_frames);
    int ta_renders = (int)prof_counter_load(COUNTER_ta_renders);
    int pvr_vblanks = (int)prof_counter_load(COUNTER_pvr_vblanks);
    int sh4_instrs = (int)(prof_counter_load(COUNTER_sh4_instrs) / 1000000.0f);
    int arm7_instrs =
        (int)(prof_counter_load(COUNTER_arm7_instrs) / 1000000.0f);

    snprintf(status, sizeof(status), "FPS %3d RPS %3d VBS %3d SH4 %4d ARM %d",
             frames, ta_renders, pvr_vblanks, sh4_instrs, arm7_instrs);

    /* right align */
    struct ImVec2 content;
    struct ImVec2 size;
    igGetContentRegionMax(&content);
    igCalcTextSize(&size, status, NULL, 0, 0.0f);
    igSetCursorPosX(content.x - size.x);
    igText(status);

    igEndMainMenuBar();
  }
#endif
}

int emu_load(struct emu *emu, const char *path) {
  return dc_load(emu->dc, path);
}

void emu_open_tray(struct emu *emu) {
  emu_sync(emu);
  gdrom_open_tray(emu->dc->gdrom);
}

/* puts the disc at path in the drive, spinning it up as a closed lid would,
   or straight in, as for a state that was saved with it in */
int emu_close_tray(struct emu *emu, const char *path, int spin_up) {
  struct disc *disc = disc_create(path, 1);
  if (!disc) {
    return 0;
  }

  emu_sync(emu);
  if (spin_up) {
    gdrom_close_tray(emu->dc->gdrom, disc);
  } else {
    gdrom_set_disc(emu->dc->gdrom, disc);
  }
  return 1;
}

int emu_keydown(struct emu *emu, int port, int key, int16_t value) {
  if (key >= K_CONT_C && key <= K_CONT_RTRIG) {
    dc_input(emu->dc, port, key - K_CONT_C, value);
  }

  return 0;
}

uint8_t *emu_ram(struct emu *emu) {
  return mem_ram(emu->dc->mem, 0);
}

size_t emu_ram_size() {
  return 16 * 1024 * 1024;
}

size_t emu_state_size(struct emu *emu) {
  /* the size is found by saving into nothing, and saving moves the line
     timer, so it can't run alongside a frame either */
  emu_sync(emu);
  return dc_state_size(emu->dc);
}

int emu_save_state(struct emu *emu, void *data, size_t size) {
  emu_sync(emu);
  return dc_save_state(emu->dc, data, size);
}

int emu_load_state(struct emu *emu, const void *data, size_t size) {
  emu_sync(emu);
  return dc_load_state(emu->dc, data, size);
}

void emu_vid_destroyed(struct emu *emu) {
  rb_for_each_entry_safe(tex, &emu->live_textures, struct emu_texture,
                         live_it) {
    r_destroy_texture(emu->r, tex->handle);
    emu_free_texture(emu, tex);
  }

  for (int i = 0; i < emu->num_evicted; i++) {
    r_destroy_texture(emu->r, emu->evicted[i]);
  }
  emu->num_evicted = 0;

  for (int i = 0; i < ARRAY_SIZE(emu->targets); i++) {
    r_destroy_texture(emu->r, emu->targets[i].tex.handle);
  }
  memset(emu->targets, 0, sizeof(emu->targets));

  emu->r = NULL;
}

void emu_vid_created(struct emu *emu, struct render_backend *r) {
  emu->r = r;
}

void emu_destroy(struct emu *emu) {
  /* shutdown the emulation thread */
  if (emu->multi_threaded) {
    emu_sync(emu);

    mutex_lock(emu->req_mutex);
    emu->state = EMU_SHUTDOWN;
    cond_signal(emu->req_cond);
    mutex_unlock(emu->req_mutex);

    void *result;
    thread_join(emu->run_thread, &result);

    mutex_destroy(emu->req_mutex);
    cond_destroy(emu->req_cond);
    mutex_destroy(emu->res_mutex);
    cond_destroy(emu->res_cond);
  }

  emu_stop_tracing(emu);
  emu_vid_destroyed(emu);
  dc_destroy(emu->dc);
  free(emu);
}

struct emu *emu_create(struct host *host) {
  struct emu *emu = calloc(1, sizeof(struct emu));

  emu->host = host;
  emu->vid_pending = -1;

  /* create dreamcast, bind client callbacks */
  emu->dc = dc_create();
  emu->dc->userdata = emu;
  emu->dc->push_audio = &emu_push_audio;
  emu->dc->push_pixels = &emu_push_pixels;
  emu->dc->start_render = &emu_start_render;
  emu->dc->finish_render = &emu_finish_render;
  emu->dc->vblank_in = &emu_vblank_in;
  emu->dc->vblank_out = &emu_vblank_out;

  /* add all textures to free list by default */
  for (int i = 0; i < ARRAY_SIZE(emu->textures); i++) {
    struct emu_texture *tex = &emu->textures[i];
    list_add(&emu->free_textures, &tex->free_it);
  }

  /* enable the cpu / gpu to be emulated in parallel */
  emu->multi_threaded = 1;

  if (emu->multi_threaded) {
    emu->state = EMU_WAITING;
    emu->req_mutex = mutex_create();
    emu->req_cond = cond_create();
    emu->res_mutex = mutex_create();
    emu->res_cond = cond_create();

    emu->run_thread = thread_create(&emu_run_thread, NULL, emu);
    CHECK_NOTNULL(emu->run_thread);
  }

  /* set initial aspect ratio */
  emu_set_aspect_ratio(emu, OPTION_aspect);

  return emu;
}
