#include <stdlib.h>
#include "jit/frontend/sh4/sh4_frontend.h"
#include "jit/frontend/sh4/sh4_disasm.h"
#include "jit/frontend/sh4/sh4_fallback.h"
#include "jit/frontend/sh4/sh4_guest.h"
#include "jit/frontend/sh4/sh4_translate.h"
#include "jit/ir/ir.h"
#include "jit/jit.h"
#include "jit/jit_frontend.h"
#include "jit/jit_guest.h"

/*
 * fsca estimate lookup table, used by the jit and interpreter
 */
uint32_t sh4_fsca_table[0x20000] = {
#include "jit/frontend/sh4/sh4_fsca.inc"
};

struct sh4_frontend {
  struct jit_frontend;
};

static const struct jit_opdef *sh4_frontend_lookup_op(struct jit_frontend *base,
                                                      const void *instr) {
  return sh4_get_opdef(*(const uint16_t *)instr);
}

static int sh4_frontend_is_terminator(struct jit_opdef *def) {
  /* stop emitting once a branch is hit */
  if (def->flags & SH4_FLAG_STORE_PC) {
    return 1;
  }

  /* if fpscr changed, stop as the compile-time assumptions may be invalid.
     fschg and frchg only toggle bits whose new state is known here, so
     translation can carry on with that state */
  if (def->flags & SH4_FLAG_STORE_FPSCR) {
    if (def->op == SH4_OP_FSCHG || def->op == SH4_OP_FRCHG) {
      return 0;
    }
    return 1;
  }

  return 0;
}

/* a conditional branch doesn't end the block while there are few of them.
   it's a side exit that leaves when taken, and translation carries on down the
   untaken path, so values stay in registers */
#define SH4_MAX_SIDE_EXITS 4

static int sh4_frontend_is_side_exit(struct jit_opdef *def) {
  return def->op == SH4_OP_BT || def->op == SH4_OP_BF ||
         def->op == SH4_OP_BTS || def->op == SH4_OP_BFS;
}

/* past a side exit we may not be in the fpscr mode the block was compiled
   for, or in code at all, so these can't translate */
static int sh4_frontend_can_speculate(struct jit_opdef *def) {
  return def->op != SH4_OP_TRAPA && def->op != SH4_OP_FMAC &&
         def->op != SH4_OP_FCNVDS && def->op != SH4_OP_FCNVSD;
}

static int sh4_frontend_is_idle_loop(struct sh4_frontend *frontend,
                                     uint32_t begin_addr) {
  struct sh4_guest *guest = (struct sh4_guest *)frontend->guest;

  /* look ahead to see if the current basic block is an idle loop */
  static int IDLE_MASK = SH4_FLAG_LOAD | SH4_FLAG_COND | SH4_FLAG_CMP;
  int idle_loop = 1;
  int all_flags = 0;
  int offset = 0;

  while (1) {
    uint32_t addr = begin_addr + offset;
    uint16_t data = guest->r16(guest->mem, addr);
    struct jit_opdef *def = sh4_get_opdef(data);

    offset += 2;
    all_flags |= def->flags;

    if (def->flags & SH4_FLAG_DELAYED) {
      uint32_t delay_addr = begin_addr + offset;
      uint16_t delay_data = guest->r16(guest->mem, delay_addr);
      struct jit_opdef *delay_def = sh4_get_opdef(delay_data);

      offset += 2;
      all_flags |= delay_def->flags;
    }

    if (sh4_frontend_is_terminator(def)) {
      /* if the block doesn't contain the required flags, disqualify */
      idle_loop &= (all_flags & IDLE_MASK) == IDLE_MASK;

      /* if the branch isn't a short back edge, disqualify */
      if (def->flags & SH4_FLAG_STORE_PC) {
        union sh4_instr instr = {data};

        int branch_type;
        uint32_t branch_addr;
        uint32_t next_addr;
        sh4_branch_info(addr, instr, &branch_type, &branch_addr, &next_addr);

        idle_loop &= (begin_addr - branch_addr) <= 32;
      }

      break;
    }
  }

  return idle_loop;
}

/* look further for idle loops, across blocks and through calls: follow the
   code and if it comes back without storing, reading only fixed addresses and
   writing only values it can work out again, it's waiting on something
   outside (irq, hardware, timer) which won't change before the next sched
   event. then the rest of the slice can go */
struct idle_reg {
  /* same every time round, and if so is the value known */
  int fix;
  int kn;
  uint32_t val;
  /* if not fixed because it came from ram idle_wait's loop stores to, the
     word it was read from */
  uint32_t src;
  /* only known as what ram holds right now */
  int rd;
};

#define IDLE_WALK_STEPS 48
#define IDLE_WALK_FORKS 4
#define IDLE_WALK_CALLS 4
#define IDLE_WALK_STORES 8
/* more than the longest slice, 1 ms of sh4 cycles */
#define SH4_IDLE_SKIP_CYCLES (1 << 20)
/* what an idle_wait loop is charged, in times the cycles it takes */
#define IDLE_WAIT_SCALE 8

/* words idle_wait's loop stores to, noted on the first pass and checked on
   the second */
struct idle_stores {
  uint32_t w[IDLE_WALK_STORES];
  int n;
  int chk;
};

struct idle_walk {
  struct idle_reg r[16];
  struct idle_reg pr;
  int nrd;
  int ncond;
  /* compares/tests of values with a known origin, other than a ram count */
  int ntest;
  /* t, if the compare that set it was of values known for good */
  int t_known;
  int t;
  /* regs written on the way */
  uint32_t wr;
  /* where the calls on the way return to, last made last */
  uint32_t ret[IDLE_WALK_CALLS];
  int ncalls;
  /* cycles taken */
  int cyc;
  /* idle_wait: regs in smask start with seed and must have it again at the
     end, and keep gets the regs that do */
  const uint32_t *seed;
  uint32_t smask;
  uint32_t *keep;
  struct idle_stores *st;
};

static int idle_is_ram(uint32_t addr) {
  return (addr & 0x1c000000) == 0x0c000000;
}

/* word at addr, same through all the mirrors */
static uint32_t idle_word(uint32_t addr) {
  return (addr & 0x00fffffc) | 0x0c000000;
}

static int idle_stored(struct idle_stores *st, uint32_t addr) {
  int k;

  for (k = 0; k < st->n; k++) {
    if (st->w[k] == idle_word(addr)) {
      return 1;
    }
  }
  return 0;
}

/* read base+disp into to if the address is fixed */
static int idle_load(struct sh4_guest *guest, struct idle_walk *w,
                     struct idle_reg *to, struct idle_reg base, uint32_t disp,
                     int is_long) {
  if (!base.fix) {
    return 0;
  }
  uint32_t addr = base.val + disp;
  /* what idle_wait's loop stores changes each time round */
  struct idle_stores *st = w->st;
  if (st && st->chk && st->n &&
      (!base.kn || (idle_is_ram(addr) && idle_stored(st, addr)))) {
    *to = (struct idle_reg){0, 0, 0, base.kn ? idle_word(addr) : 0};
    return 1;
  }
  w->nrd++;
  to->fix = 1;
  to->kn = is_long && base.kn && idle_is_ram(addr);
  to->val = to->kn ? guest->r32(guest->mem, addr) : 0;
  to->src = 0;
  to->rd = 1;
  return 1;
}

/* reads through r[m] (plus disp) into r[n] */
static int idle_read(struct sh4_guest *guest, struct idle_walk *w, int n,
                     int m, uint32_t disp, int is_long) {
  w->wr |= 1u << n;
  return idle_load(guest, w, &w->r[n], w->r[m], disp, is_long);
}

/* store v at r[n]+disp. only idle_wait's loop does this, and only to ram,
   either the same place each time or back where it came from */
static int idle_store(struct idle_walk *w, int n, uint32_t disp,
                      struct idle_reg v) {
  struct idle_stores *st = w->st;
  uint32_t addr = w->r[n].val + disp;
  if (!st || !w->r[n].kn || !idle_is_ram(addr) ||
      (!v.fix && v.src != idle_word(addr))) {
    return 0;
  }
  if (idle_stored(st, addr)) {
    return 1;
  }
  if (st->chk || st->n == IDLE_WALK_STORES) {
    return 0;
  }
  st->w[st->n++] = idle_word(addr);
  return 1;
}

/* the same, pushing v */
static int idle_push(struct idle_walk *w, int n, struct idle_reg v) {
  w->wr |= 1u << n;
  w->r[n].val -= 4;
  return idle_store(w, n, 0, v);
}

/* compare/test. a ram count can only be whether it ran out; a loop counting
   to anything else isn't waiting. the rest are what it waits on */
static int idle_test(struct idle_walk *w, union sh4_instr i, int op) {
  int n = i.def.rn, m = i.def.rm, zero = 0, t = -1;
  switch (op) {
    case SH4_OP_CMPEQI:
      n = m = 0;
      zero = !i.imm.imm;
      t = w->r[0].val == (uint32_t)(int8_t)i.imm.imm;
      break;
    case SH4_OP_TSTI:
      n = m = 0;
      t = !(w->r[0].val & i.imm.imm);
      break;
    case SH4_OP_CMPEQ:
      t = w->r[n].val == w->r[m].val;
      break;
    case SH4_OP_CMPPZ:
    case SH4_OP_CMPPL:
      m = n;
      zero = 1;
      break;
    case SH4_OP_TST:
      zero = n == m;
      t = !(w->r[n].val & w->r[m].val);
      break;
  }
  /* known both sides, the branch can only go one way */
  w->t_known = t >= 0 && w->r[n].kn && w->r[m].kn && !w->r[n].rd &&
               !w->r[m].rd;
  w->t = t;
  if (!w->r[n].src && !w->r[m].src) {
    w->ntest += w->r[n].fix && w->r[m].fix;
    return 1;
  }
  return zero;
}

/* one non-branch instruction, if an idle loop could have it */
static int idle_step(struct sh4_guest *guest, struct idle_walk *w,
                     uint32_t addr) {
  union sh4_instr i = {guest->r16(guest->mem, addr)};
  struct jit_opdef *def = sh4_get_opdef(i.raw);
  int n = i.def.rn, m = i.def.rm;
  struct idle_reg *rn = &w->r[n];

  w->cyc += def->cycles;

  switch (def->op) {
    case SH4_OP_NOP:
      return 1;
    case SH4_OP_CMPEQI:
    case SH4_OP_CMPEQ:
    case SH4_OP_CMPHS:
    case SH4_OP_CMPGE:
    case SH4_OP_CMPHI:
    case SH4_OP_CMPGT:
    case SH4_OP_CMPPZ:
    case SH4_OP_CMPPL:
    case SH4_OP_TST:
    case SH4_OP_TSTI:
      return idle_test(w, i, def->op);
    case SH4_OP_MOVI:
      w->wr |= 1u << n;
      w->r[n] = (struct idle_reg){1, 1, (uint32_t)(int8_t)i.imm.imm};
      return 1;
    case SH4_OP_MOVLL_PCR: {
      uint32_t lit = (addr & ~3u) + 4 + i.disp_8.disp * 4;
      w->wr |= 1u << n;
      w->r[n] = (struct idle_reg){1, 1, guest->r32(guest->mem, lit)};
      return 1;
    }
    case SH4_OP_MOVWL_PCR: {
      uint32_t lit = addr + 4 + i.disp_8.disp * 2;
      w->wr |= 1u << n;
      int16_t v = (int16_t)guest->r16(guest->mem, lit);
      w->r[n] = (struct idle_reg){1, 1, (uint32_t)v};
      return 1;
    }
    case SH4_OP_MOV:
      w->wr |= 1u << n;
      w->r[n] = w->r[m];
      return 1;
    case SH4_OP_MOVBL_IND:
    case SH4_OP_MOVWL_IND:
      return idle_read(guest, w, n, m, 0, 0);
    case SH4_OP_MOVLL_IND:
      return idle_read(guest, w, n, m, 0, 1);
    case SH4_OP_MOVBL_OFF:
      return idle_read(guest, w, 0, m, i.def.disp, 0);
    case SH4_OP_MOVWL_OFF:
      return idle_read(guest, w, 0, m, i.def.disp * 2, 0);
    case SH4_OP_MOVLL_OFF:
      return idle_read(guest, w, n, m, i.def.disp * 4, 1);
    case SH4_OP_MOVBL_GBR:
    case SH4_OP_MOVWL_GBR:
    case SH4_OP_MOVLL_GBR:
      /* gbr can't change, nothing here writes it */
      w->wr |= 1;
      return idle_load(guest, w, &w->r[0], (struct idle_reg){1, 0, 0}, 0, 0);
    case SH4_OP_ANDI:
    case SH4_OP_ORI:
    case SH4_OP_XORI:
      w->wr |= 1;
      w->r[0].kn = 0;
      return 1;
    case SH4_OP_ADD:
    case SH4_OP_SUB:
      /* idle_wait moves sp and follows calls through tables */
      w->wr |= 1u << n;
      rn->fix &= w->r[m].fix;
      rn->kn &= w->r[m].kn && w->seed;
      rn->rd |= w->r[m].rd;
      rn->val += def->op == SH4_OP_ADD ? w->r[m].val : 0u - w->r[m].val;
      return 1;
    case SH4_OP_ADDI:
      w->wr |= 1u << n;
      rn->kn &= w->seed != NULL;
      rn->val += (int8_t)i.imm.imm;
      return 1;
    case SH4_OP_SHLL:
    case SH4_OP_SHLL2:
      w->wr |= 1u << n;
      w->t_known = 0;
      rn->kn &= w->seed != NULL;
      rn->val <<= def->op == SH4_OP_SHLL ? 1 : 2;
      return 1;
    case SH4_OP_AND:
    case SH4_OP_OR:
    case SH4_OP_XOR:
      /* fixed if both sides are */
      w->wr |= 1u << n;
      rn->fix &= w->r[m].fix;
      rn->kn = 0;
      return 1;
    case SH4_OP_NOT:
    case SH4_OP_NEG:
    case SH4_OP_EXTSB:
    case SH4_OP_EXTSW:
    case SH4_OP_EXTUB:
    case SH4_OP_EXTUW:
    case SH4_OP_SWAPB:
    case SH4_OP_SWAPW:
      w->wr |= 1u << n;
      rn->fix = w->r[m].fix;
      rn->kn = 0;
      rn->src = w->r[m].src;
      return 1;
    case SH4_OP_DT:
    case SH4_OP_SHLR:
    case SH4_OP_SHAL:
    case SH4_OP_SHAR:
    case SH4_OP_SHLR2:
    case SH4_OP_SHLL8:
    case SH4_OP_SHLR8:
    case SH4_OP_SHLL16:
    case SH4_OP_SHLR16:
      w->wr |= 1u << n;
      w->t_known = 0;
      rn->kn = 0;
      return 1;
    case SH4_OP_MOVT:
      /* t can be left over from last time */
      w->wr |= 1u << n;
      *rn = (struct idle_reg){0, 0, 0, 0};
      return 1;
    case SH4_OP_MOVBS_IND:
    case SH4_OP_MOVWS_IND:
    case SH4_OP_MOVLS_IND:
      return idle_store(w, n, 0, w->r[m]);
    case SH4_OP_MOVLS_OFF:
      return idle_store(w, n, i.def.disp * 4, w->r[m]);
    case SH4_OP_MOVLS_DEC:
      return idle_push(w, n, w->r[m]);
    case SH4_OP_STSMPR:
      return idle_push(w, n, w->pr);
    case SH4_OP_MOVLL_INC:
      /* pop, through a fixed sp */
      if (!w->seed || !w->r[m].kn || !idle_read(guest, w, n, m, 0, 1)) {
        return 0;
      }
      if (n != m) {
        w->wr |= 1u << m;
        w->r[m].val += 4;
      }
      return 1;
    case SH4_OP_LDSMPR:
      if (!w->seed || !rn->kn || !idle_load(guest, w, &w->pr, *rn, 0, 1)) {
        return 0;
      }
      w->wr |= 1u << n;
      rn->val += 4;
      return 1;
    default:
      return 0;
  }
}

static int idle_walk(struct sh4_guest *guest, uint32_t begin_addr,
                     uint32_t addr, struct idle_walk w, int steps, int forks) {
  for (; steps > 0; steps--) {
    if (addr == begin_addr && steps < IDLE_WALK_STEPS) {
      uint32_t fixed = 0, kept = ~w.wr & 0xffff;
      for (int r = 0; r < 16; r++) {
        /* a read from ram the loop stores to changes every time, but can
           only be compared or stored back */
        fixed |= (uint32_t)(w.r[r].fix || w.r[r].src) << r;
        if (w.seed && w.r[r].kn && w.r[r].val == w.seed[r]) {
          kept |= 1u << r;
        }
      }
      if (w.keep) {
        *w.keep = kept;
      }
      int idle = w.nrd && w.ncond && (!w.seed || w.ntest) &&
                 !(w.wr & ~fixed) && !(w.smask & ~kept);
      return idle ? w.cyc : 0;
    }

    /* skip from the lowest block, so the loop doesn't skip once per block */
    if (!w.ncalls && addr < begin_addr) {
      return 0;
    }

    union sh4_instr i = {guest->r16(guest->mem, addr)};
    struct jit_opdef *def = sh4_get_opdef(i.raw);

    if (!(def->flags & SH4_FLAG_STORE_PC)) {
      if (!idle_step(guest, &w, addr)) {
        return 0;
      }
      addr += 2;
      continue;
    }

    w.cyc += def->cycles;

    int branch_type;
    uint32_t branch_addr = 0, next_addr = 0;
    sh4_branch_info(addr, i, &branch_type, &branch_addr, &next_addr);

    switch (def->op) {
      case SH4_OP_BF:
      case SH4_OP_BT:
      case SH4_OP_BFS:
      case SH4_OP_BTS: {
        /* t as the branch sees it, before the delay slot */
        int t_known = w.t_known;
        int taken = w.t == (def->op == SH4_OP_BT || def->op == SH4_OP_BTS);
        if ((def->flags & SH4_FLAG_DELAYED) && !idle_step(guest, &w, addr + 2)) {
          return 0;
        }
        if (t_known) {
          addr = taken ? branch_addr : next_addr;
          continue;
        }
        if (!forks) {
          return 0;
        }
        w.ncond = 1;
        int cycles = idle_walk(guest, begin_addr, branch_addr, w, steps - 1,
                               forks - 1);
        return cycles ? cycles
                      : idle_walk(guest, begin_addr, next_addr, w, steps - 1,
                                  forks - 1);
      }
      case SH4_OP_BRA:
        break;
      case SH4_OP_BSR:
      case SH4_OP_JSR:
        /* idle_wait follows calls further in */
        if (w.ncalls == (w.seed ? IDLE_WALK_CALLS : 1)) {
          return 0;
        }
        if (def->op == SH4_OP_JSR) {
          struct idle_reg to = w.r[i.def.rn];
          if (!to.kn || !idle_is_ram(to.val)) {
            return 0;
          }
          branch_addr = to.val;
        }
        w.ret[w.ncalls++] = addr + 4;
        w.pr = (struct idle_reg){1, 1, addr + 4};
        break;
      case SH4_OP_RTS:
        if (!w.ncalls) {
          return 0;
        }
        branch_addr = w.ret[--w.ncalls];
        break;
      default:
        return 0;
    }

    /* delay slot first */
    if (!idle_step(guest, &w, addr + 2)) {
      return 0;
    }
    addr = branch_addr;
  }

  return 0;
}

/* wider again, for loops that wait the same way but with regs set before
   them (sp, a call target), calls further in, and stores to ram: stack frames
   or a count to stop at. skipping those could leave a count running for good,
   so they're charged extra cycles instead */
static int idle_wait(struct sh4_guest *guest, struct sh4_context *ctx,
                     uint32_t begin_addr) {
  /* first with all regs as they are, to see what comes back unchanged and
     where it stores, then with only those. a read from a stored word only
     looks changed once the stores are known, so if any do, once more */
  struct idle_stores stores = {0};
  /* bit 16 is left set until the loop is seen to come back */
  uint32_t kept = 0x1ffff;

  for (int pass = 0; pass < 3; pass++) {
    struct idle_walk w = {0};
    w.seed = ctx->r;
    w.smask = kept & 0xffff;
    w.keep = &kept;
    w.st = &stores;
    stores.chk = pass > 0;
    for (int r = 0; r < 16; r++) {
      if (w.smask & (1u << r)) {
        w.r[r] = (struct idle_reg){1, 1, ctx->r[r]};
      }
    }
    int cycles = idle_walk(guest, begin_addr, begin_addr, w, IDLE_WALK_STEPS,
                           IDLE_WALK_FORKS);
    if (pass && (cycles || !(w.smask & ~kept))) {
      return cycles * (IDLE_WAIT_SCALE - 1);
    }
    /* the next pass goes the same way, only stricter, so if this one didn't
       come back neither will it. most blocks stop here */
    if (kept >> 16) {
      return 0;
    }
  }

  return 0;
}

static void sh4_frontend_dump_code(struct jit_frontend *base,
                                   uint32_t begin_addr, int size,
                                   FILE *output) {
  struct sh4_frontend *frontend = (struct sh4_frontend *)base;
  struct jit_guest *guest = frontend->guest;

  int offset = 0;
  char buffer[128];

  fprintf(output, "#==--------------------------------------------------==#\n");
  fprintf(output, "# sh4\n");
  fprintf(output, "#==--------------------------------------------------==#\n");

  while (offset < size) {
    uint32_t addr = begin_addr + offset;
    uint16_t data = guest->r16(guest->mem, addr);
    union sh4_instr instr = {data};
    struct jit_opdef *def = sh4_get_opdef(data);

    sh4_format(addr, instr, buffer, sizeof(buffer));
    fprintf(output, "# %s\n", buffer);

    offset += 2;

    if (def->flags & SH4_FLAG_DELAYED) {
      uint32_t delay_addr = begin_addr + offset;
      uint16_t delay_data = guest->r16(guest->mem, delay_addr);
      union sh4_instr delay_instr = {delay_data};

      sh4_format(delay_addr, delay_instr, buffer, sizeof(buffer));
      fprintf(output, "# %s\n", buffer);

      offset += 2;
    }
  }
}

static void sh4_frontend_translate_code(struct jit_frontend *base,
                                        uint32_t begin_addr, int size,
                                        struct ir *ir) {
  struct sh4_frontend *frontend = (struct sh4_frontend *)base;
  struct sh4_guest *guest = (struct sh4_guest *)frontend->guest;
  struct sh4_context *ctx = (struct sh4_context *)guest->ctx;

  int offset = 0;
  int use_fpscr = 0;
  int was_delay = 0;

  /* append inital block */
  struct ir_block *block = ir_append_block(ir);

  /* generate code specialized for the current fpscr state */
  int flags = 0;
  if (ctx->fpscr & PR_MASK) {
    flags |= SH4_DOUBLE_PR;
  }
  if (ctx->fpscr & SZ_MASK) {
    flags |= SH4_DOUBLE_SZ;
  }

  /* cheap idle skip. in an idle loop, the block is just spinning, waiting for
     an interrupt such as vblank before it'll exit. scale the block's number of
     cycles in order to yield execution faster, enabling the interrupt to
     actually be generated */
  int idle_loop = sh4_frontend_is_idle_loop(frontend, begin_addr);
  int cycle_scale = idle_loop ? 8 : 1;

  /* a loop that can only be waiting gives up the rest of its time slice: its
     first block costs more cycles than any slice has */
  struct idle_walk walk = {0};
  int idle_cycles = 0;
  if (idle_walk(guest, begin_addr, begin_addr, walk, IDLE_WALK_STEPS,
                IDLE_WALK_FORKS)) {
    idle_cycles = SH4_IDLE_SKIP_CYCLES;
  } else if (!idle_loop) {
    idle_cycles = idle_wait(guest, ctx, begin_addr);
  }

  while (offset < size) {
    /* if a branch instruction / delay slot was just emitted, rewind and emit
       the delay slot as its own instruction */
    if (was_delay) {
      offset -= 2;
      was_delay = 0;
    }

    uint32_t addr = begin_addr + offset;
    uint16_t data = guest->r16(guest->mem, addr);
    union sh4_instr instr = {data};
    struct jit_opdef *def = sh4_get_opdef(data);

    use_fpscr |= (def->flags & SH4_FLAG_USE_FPSCR) == SH4_FLAG_USE_FPSCR;

    /* emit meta information for the current guest instruction. this info is
       essential to the jit, and is used to map guest instructions to host
       addresses for branching and fastmem access */
    int cycles = def->cycles * cycle_scale;
    if (offset == 0) {
      cycles += idle_cycles;
    }
    ir_source_info(ir, addr, cycles);

    /* the pc is normally only written to the context at the end of the block,
       sync now for any instruction which needs to read the correct pc */
    if (def->flags & SH4_FLAG_LOAD_PC) {
      ir_store_context(ir, offsetof(struct sh4_context, pc),
                       ir_alloc_i32(ir, addr));
    }

    /* emit the instruction's translation if available */
    sh4_translate_cb cb = sh4_get_translator(data);

    if (cb) {
      /* if the instruction has a delay slot, delay_point is assigned where the
         slot's translation should be emitted */
      struct ir_insert_point delay_point;
      cb(guest, ir, addr, instr, flags, &delay_point);

      /* fschg flips the transfer size mode, and the rest of the block is
         compiled for the new one */
      if (def->op == SH4_OP_FSCHG) {
        flags ^= SH4_DOUBLE_SZ;
      }

      offset += 2;

      if (def->flags & SH4_FLAG_DELAYED) {
        uint32_t delay_addr = begin_addr + offset;
        uint32_t delay_data = guest->r16(guest->mem, delay_addr);
        union sh4_instr delay_instr = {delay_data};
        struct jit_opdef *delay_def = sh4_get_opdef(delay_data);

        use_fpscr |=
            (delay_def->flags & SH4_FLAG_USE_FPSCR) == SH4_FLAG_USE_FPSCR;

        /* move insert point back to the middle of the preceding instruction */
        struct ir_insert_point original = ir_get_insert_point(ir);
        ir_set_insert_point(ir, &delay_point);

        if (delay_def->flags & SH4_FLAG_LOAD_PC) {
          ir_store_context(ir, offsetof(struct sh4_context, pc),
                           ir_alloc_i32(ir, delay_addr));
        }

        /* emit the delay slot's translation if available */
        sh4_translate_cb delay_cb = sh4_get_translator(delay_data);

        if (delay_cb) {
          delay_cb(guest, ir, delay_addr, delay_instr, flags, NULL);
        } else {
          ir_fallback(ir, delay_def->fallback, delay_addr, delay_data);
        }

        /* an fschg in the delay slot flips the mode too, for the rest of a
           block carrying on past a side exit */
        if (delay_def->op == SH4_OP_FSCHG) {
          flags ^= SH4_DOUBLE_SZ;
        }

        /* restore insert point */
        ir_set_insert_point(ir, &original);

        offset += 2;
        was_delay = 1;
      }

      /* a conditional branch the block carries on past is a side exit, which
         leaves for whichever target isn't next. only the idle loop, the first
         part of the block, has its cycles scaled */
      if (sh4_frontend_is_side_exit(def) && offset < size) {
        struct ir_instr *branch =
            list_last_entry(&block->instrs, struct ir_instr, it);
        CHECK_EQ(branch->op, OP_BRANCH_COND);

        int set = branch->arg[1]->i32 == begin_addr + offset;
        ir_set_current_instr(ir, branch);
        ir_exit_cond(ir, branch->arg[2], branch->arg[set ? 0 : 1], set);
        ir_remove_instr(ir, branch);

        was_delay = 0;
        cycle_scale = 1;
      }
    } else {
      ir_fallback(ir, def->fallback, addr, data);

      offset += 2;

      /* don't emit a fallback for the delay slot, the original fallback will
         execute it */
      if (def->flags & SH4_FLAG_DELAYED) {
        offset += 2;
        was_delay = 1;
      }
    }

    /* there are 3 possible block endings:

       1.) the block terminates due to an unconditional branch; nothing needs to
           be done

       2.) the block terminates due to an instruction which doesn't set the pc;
           an unconditional branch to the next address needs to be added

       3.) the block terminates due to an instruction which sets the pc but is
           not a branch (e.g. an invalid instruction trap); nothing needs to be
           done dispatch will always implicitly branch to the next pc */
    int store_pc = (def->flags & SH4_FLAG_STORE_PC) == SH4_FLAG_STORE_PC;
    int end_of_block = sh4_frontend_is_terminator(def) || offset >= size;

    if (end_of_block) {
      if (!store_pc) {
        struct ir_block *tail_block =
            list_last_entry(&ir->blocks, struct ir_block, it);
        struct ir_instr *tail_instr =
            list_last_entry(&tail_block->instrs, struct ir_instr, it);
        ir_set_current_instr(ir, tail_instr);

        uint32_t next_addr = begin_addr + offset;
        ir_branch(ir, ir_alloc_i32(ir, next_addr));
      }
    }
  }

  /* if the block makes optimizations based on the fpscr state, assert that the
     run-time fpscr state matches the compile-time state. like flycast, release
     builds trust code to keep the state it was compiled with, the check costs
     every such block a load and compare on its way in and only ever traps */
#ifdef NDEBUG
  (void)use_fpscr;
#else
  if (use_fpscr) {
    /* insert after the first guest marker */
    struct ir_instr *after = NULL;
    list_for_each_entry(instr, &block->instrs, struct ir_instr, it) {
      if (instr->op == OP_SOURCE_INFO) {
        after = instr;
        break;
      }
    }
    ir_set_current_instr(ir, after);

    struct ir_value *actual =
        ir_load_context(ir, offsetof(struct sh4_context, fpscr), VALUE_I32);
    actual = ir_and(ir, actual, ir_alloc_i32(ir, PR_MASK | SZ_MASK));
    struct ir_value *expected =
        ir_alloc_i32(ir, ctx->fpscr & (PR_MASK | SZ_MASK));
    ir_assert_eq(ir, actual, expected);
  }
#endif
}

static void sh4_frontend_analyze_code(struct jit_frontend *base,
                                      uint32_t begin_addr, int *size) {
  struct sh4_frontend *frontend = (struct sh4_frontend *)base;
  struct sh4_guest *guest = (struct sh4_guest *)frontend->guest;

  int exits = 0;
  int exit_size = 0;

  *size = 0;

  while (1) {
    uint32_t addr = begin_addr + *size;
    uint16_t data = guest->r16(guest->mem, addr);
    struct jit_opdef *def = sh4_get_opdef(data);
    int speculate = sh4_frontend_can_speculate(def);
    int side_exit = sh4_frontend_is_side_exit(def);

    *size += 2;

    if (def->flags & SH4_FLAG_DELAYED) {
      uint32_t delay_addr = begin_addr + *size;
      uint16_t delay_data = guest->r16(guest->mem, delay_addr);
      struct jit_opdef *delay_def = sh4_get_opdef(delay_data);

      *size += 2;

      speculate &= sh4_frontend_can_speculate(delay_def) &&
                   !(delay_def->flags & (SH4_FLAG_DELAYED | SH4_FLAG_STORE_PC));

      /* the block can't carry on past a delay slot which would end it, such
         as one loading fpscr */
      side_exit &= !sh4_frontend_is_terminator(delay_def);

      /* delay slots can't have another delay slot */
      CHECK(exits || !(delay_def->flags & SH4_FLAG_DELAYED));
    }

    /* past a side exit, end the block at the exit rather than translate what
       can't be */
    if (exits && !speculate) {
      *size = exit_size;
      break;
    }

    if (side_exit && exits < SH4_MAX_SIDE_EXITS) {
      exits++;
      exit_size = *size;
      continue;
    }

    if (sh4_frontend_is_terminator(def)) {
      break;
    }
  }
}

static void sh4_frontend_destroy(struct jit_frontend *base) {
  struct sh4_frontend *frontend = (struct sh4_frontend *)base;

  free(frontend);
}

struct jit_frontend *sh4_frontend_create(struct jit_guest *guest) {
  struct sh4_frontend *frontend = calloc(1, sizeof(struct sh4_frontend));

  frontend->guest = guest;
  frontend->destroy = &sh4_frontend_destroy;
  frontend->analyze_code = &sh4_frontend_analyze_code;
  frontend->translate_code = &sh4_frontend_translate_code;
  frontend->dump_code = &sh4_frontend_dump_code;
  frontend->lookup_op = &sh4_frontend_lookup_op;

  return (struct jit_frontend *)frontend;
}
