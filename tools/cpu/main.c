/* cpu only run + state hash. boots a disc, ticks it N times with no render
 * backend (dc_start_render stays NULL, but the ta timer still runs), then
 * hashes the saved state. for checking a jit backend against the interpreter.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/filesystem.h"
#include "core/state.h"
#include "core/time.h"
#include "core/profiler.h"
extern prof_token_t COUNTER_sh4_instrs, COUNTER_ta_renders;
#include "guest/dreamcast.h"
#include "guest/sh4/sh4.h"
#include "jit/frontend/sh4/sh4_guest.h"
#include "jit/frontend/sh4/sh4_disasm.h"
#include "guest/memory.h"
#include "options.h"
#include "jit/jit.h"
#include "stats.h"

static volatile int g_frames;
static volatile int g_loop;
static volatile int g_fn;

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s game.chd frames [state_out] [sysdir]\n", argv[0]);
    return 1;
  }

  if (argc > 4) {
    fs_set_appdir(argv[4]);
  }

  /* /tmp/perf-<pid>.map for perf */
  if (getenv("NODOSUMI_PERF")) {
    OPTION_perf = 1;
  }

  struct dreamcast *dc = dc_create();
  /* CPU_DUMP dumps each block's ir/code under the sysdir */
  if (getenv("CPU_DUMP")) {
    dc->sh4->jit->dump_code = 1;
  }

  if (!dc_load(dc, argv[1])) {
    fprintf(stderr, "dc_load failed\n");
    return 1;
  }

  const char *sp = getenv("CPU_STATE");
  if (sp) {
    FILE *sf = fopen(sp, "rb");
    fseek(sf, 0, SEEK_END);
    long ss = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    uint8_t *sb = malloc(ss);
    fread(sb, 1, ss, sf);
    fclose(sf);
    fprintf(stderr, "state %ld bytes, want %zu, load=%d\n", ss, dc_state_size(dc),
            dc_load_state(dc, sb, ss));
  }

  g_frames = atoi(argv[2]);
  const char *tns = getenv("CPU_TICK_NS");
  int64_t tick_ns = tns ? atoll(tns) : (NS_PER_SEC / 60);
  int trace = getenv("CPU_TRACE") != NULL;
  /* CPU_TIMES: average ms per tick, printed every 60 */
  int times = getenv("CPU_TIMES") != NULL;
  int64_t t0 = time_nanoseconds();
  for (g_loop = 0; g_loop < g_frames; g_loop++) {
    int i = g_loop;
    dc_tick(dc, tick_ns);
    if (times && (i % 60) == 59) {
      int64_t t1 = time_nanoseconds();
      fprintf(stderr, "ticks %d-%d %.2f ms\n", i - 59, i, (t1 - t0) / 60e6);
      t0 = t1;
    }
    if (getenv("CPU_CTX")) {
      struct sh4_context *c = &dc->sh4->ctx;
      printf("tick=%d pc=%08x fpul=%08x sr=%08x pr=%08x", i, c->pc, c->fpul, c->sr, c->pr);
      for (int r = 0; r < 16; r++) printf(" r%d=%08x", r, c->r[r]);
      printf("\n");
    } else if (trace) {
      printf("tick=%d pc=%08x\n", i, dc->sh4->ctx.pc);
    }
  }

  const char *fns = getenv("CPU_FINE_NS");
  const char *fn = getenv("CPU_FINE_N");
  if (fns && fn) {
    int64_t fnsv = atoll(fns);
    g_fn = atoi(fn);
    for (g_loop = 0; g_loop < g_fn; g_loop++) {
      int i = g_loop;
      dc_tick(dc, fnsv);
      struct sh4_context *c = &dc->sh4->ctx;
      if (getenv("CPU_CTX")) {
        printf("fine=%d pc=%08x pr=%08x sr=%08x fp=%08x gbr=%08x vbr=%08x",
               i, c->pc, c->pr, c->sr, c->fpscr, c->gbr, c->vbr);
        for (int r = 0; r < 16; r++) printf(" r%d=%08x", r, c->r[r]);
        printf("\n");
      } else {
        printf("fine=%d pc=%08x\n", i, c->pc);
      }
    }
  }

  const char *dp = getenv("CPU_DISASM_PC");
  if (dp) {
    uint32_t pc = (uint32_t)strtoul(dp, NULL, 16);
    uint8_t *ram = mem_ram(dc->mem, pc - 0x8c000000);
    for (int k = -12; k < 48; k++) {
      uint32_t a = pc + (uint32_t)(k * 2);
      uint16_t raw = *(uint16_t *)(mem_ram(dc->mem, a - 0x8c000000));
      union sh4_instr i;
      i.raw = raw;
      char buf[64];
      sh4_format(a, i, buf, sizeof(buf));
      printf("%08x: %04x  %s\n", a, raw, buf);
    }
    return 0;
  }

  size_t size = dc_state_size(dc);
  uint8_t *buf = malloc(size);
  if (!dc_save_state(dc, buf, size)) {
    fprintf(stderr, "dc_save_state failed\n");
    return 1;
  }

  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < size; i++) {
    h ^= buf[i];
    h *= 1099511628211ULL;
  }
  printf("sh4=%lld renders=%lld ", (long long)prof_counter_load_current(COUNTER_sh4_instrs), (long long)prof_counter_load_current(COUNTER_ta_renders));
  printf("frames=%d size=%zu hash=%016llx\n", (int)g_frames, size,
         (unsigned long long)h);
  if (getenv("CPU_TIMES")) {
    long long n = prof_counter_load_current(COUNTER_jit_blocks);
    double ms = prof_counter_load_current(COUNTER_jit_compile_ns) / 1e6;
    fprintf(stderr, "jit blocks=%lld compile=%.1fms per=%.1fus\n", n, ms,
            n ? ms * 1000 / n : 0.0);
  }

  if (argc > 3) {
    FILE *f = fopen(argv[3], "wb");
    fwrite(buf, 1, size, f);
    fclose(f);
  }

  dc_destroy(dc);
  return 0;
}
