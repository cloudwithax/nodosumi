#include "jit/frontend/armv3/armv3_frontend.h"
#include "core/math.h"
#include "jit/frontend/armv3/armv3_context.h"
#include "jit/frontend/armv3/armv3_disasm.h"
#include "jit/frontend/armv3/armv3_fallback.h"
#include "jit/frontend/armv3/armv3_guest.h"
#include "jit/ir/ir.h"
#include "jit/jit.h"
#include "jit/jit_guest.h"

struct armv3_frontend {
  struct jit_frontend;
};

static const struct jit_opdef *armv3_frontend_lookup_op(
    struct jit_frontend *base, const void *instr) {
  return armv3_get_opdef(*(const uint32_t *)instr);
}

static void armv3_frontend_dump_code(struct jit_frontend *base,
                                     uint32_t begin_addr, int size,
                                     FILE *output) {
  struct armv3_frontend *frontend = (struct armv3_frontend *)base;
  struct jit_guest *guest = frontend->guest;

  int offset = 0;
  char buffer[128];

  fprintf(output, "#==--------------------------------------------------==#\n");
  fprintf(output, "# armv3\n");
  fprintf(output, "#==--------------------------------------------------==#\n");

  while (offset < size) {
    uint32_t addr = begin_addr + offset;
    uint32_t data = guest->r32(guest->mem, addr);

    armv3_format(addr, data, buffer, sizeof(buffer));
    fprintf(output, "# %s\n", buffer);

    offset += 4;
  }
}

/*
 * the most common instructions are translated to ir, matching what their
 * fallbacks do. everything else still calls the fallback. translated code
 * leaves the pc alone, so the instructions that end a block branch themselves
 */
static struct ir_value *armv3_load_reg(struct ir *ir, uint32_t addr, int n) {
  if (n == 15) {
    /* account for instruction prefetching if loading the pc */
    return ir_alloc_i32(ir, addr + 8);
  }

  return ir_load_context(ir, offsetof(struct armv3_context, r[n]), VALUE_I32);
}

static void armv3_store_reg(struct ir *ir, int n, struct ir_value *v) {
  ir_store_context(ir, offsetof(struct armv3_context, r[n]), v);
}

/* moves an i8 condition to a psr bit */
static struct ir_value *armv3_flag(struct ir *ir, struct ir_value *cond,
                                   int bit) {
  return ir_shli(ir, ir_zext(ir, cond, VALUE_I32), bit);
}

/* returns an i8 that's set when cond, which isn't al or nv, passes */
static struct ir_value *armv3_translate_cond(struct ir *ir, uint32_t cond) {
  struct ir_value *sr = armv3_load_reg(ir, 0, CPSR);
  struct ir_value *v = sr;
  uint32_t mask = N_MASK;
  uint32_t set = 0;

  /* each even condition passes when v's masked bits equal set, and the odd
     condition after it when they don't */
  switch (cond & ~1) {
    case COND_EQ:
      mask = set = Z_MASK;
      break;
    case COND_CS:
      mask = set = C_MASK;
      break;
    case COND_MI:
      mask = set = N_MASK;
      break;
    case COND_VS:
      mask = set = V_MASK;
      break;
    case COND_HI:
      mask = C_MASK | Z_MASK;
      set = C_MASK;
      break;
    case COND_GE:
      /* n == v */
      v = ir_xor(ir, sr, ir_shli(ir, sr, N_BIT - V_BIT));
      break;
    case COND_GT:
      /* z clear and n == v */
      v = ir_or(ir, ir_shli(ir, sr, N_BIT - Z_BIT),
                ir_xor(ir, sr, ir_shli(ir, sr, N_BIT - V_BIT)));
      break;
  }

  v = ir_and(ir, v, ir_alloc_i32(ir, mask));

  if (cond & 1) {
    return ir_cmp_ne(ir, v, ir_alloc_i32(ir, set));
  }
  return ir_cmp_eq(ir, v, ir_alloc_i32(ir, set));
}

/* only shifts by an immediate 1-31, or lsl #0, are translated */
static int armv3_simple_shift(uint32_t shift) {
  enum armv3_shift_source src;
  enum armv3_shift_type type;
  uint32_t n;
  armv3_disasm_shift(shift, &src, &type, &n);

  return src == SHIFT_IMM && type != SHIFT_RRX && n < 32;
}

/* sets carry, if it isn't NULL, to the shifter's carry out. it's left NULL
   when the shift leaves c alone */
static struct ir_value *armv3_translate_shift(struct ir *ir, uint32_t addr,
                                              uint32_t rm, uint32_t shift,
                                              struct ir_value **carry) {
  enum armv3_shift_source src;
  enum armv3_shift_type type;
  uint32_t n;
  armv3_disasm_shift(shift, &src, &type, &n);

  struct ir_value *v = armv3_load_reg(ir, addr, rm);
  struct ir_value *res = NULL;
  int carry_bit = n - 1;

  if (!n) {
    return v;
  }

  switch (type) {
    case SHIFT_LSL:
      res = ir_shli(ir, v, n);
      carry_bit = 32 - n;
      break;
    case SHIFT_LSR:
      res = ir_lshri(ir, v, n);
      break;
    case SHIFT_ASR:
      res = ir_ashri(ir, v, n);
      break;
    default:
      res = ir_rori(ir, v, n);
      break;
  }

  if (carry) {
    *carry = ir_and(ir, ir_lshri(ir, v, carry_bit), ir_alloc_i32(ir, 1));
  }

  return res;
}

static int armv3_can_translate(union armv3_instr i, int op) {
  uint32_t cond = i.raw >> 28;

  if (cond == COND_NV) {
    return 0;
  }

  switch (op) {
    case ARMV3_OP_B:
    case ARMV3_OP_BL:
      return 1;

    case ARMV3_OP_AND:
    case ARMV3_OP_EOR:
    case ARMV3_OP_SUB:
    case ARMV3_OP_RSB:
    case ARMV3_OP_ADD:
    case ARMV3_OP_TST:
    case ARMV3_OP_TEQ:
    case ARMV3_OP_CMP:
    case ARMV3_OP_CMN:
    case ARMV3_OP_ORR:
    case ARMV3_OP_MOV:
    case ARMV3_OP_BIC:
    case ARMV3_OP_MVN:
      return i.data.rd != 15 &&
             (i.data.i || armv3_simple_shift(i.data_reg.shift));

    case ARMV3_OP_MRS:
      return cond == COND_AL && i.mrs.rd != 15;

    case ARMV3_OP_LDR:
    case ARMV3_OP_STR:
      return cond == COND_AL && i.xfr.rd != 15 &&
             !(i.xfr.rn == 15 && (i.xfr.w || !i.xfr.p)) &&
             (!i.xfr.i || armv3_simple_shift(i.xfr_reg.shift));

    case ARMV3_OP_LDM:
    case ARMV3_OP_STM:
      return cond == COND_AL && !i.blk.s && i.blk.rn != 15 && i.blk.rlist;

    default:
      return 0;
  }
}

static void armv3_translate_branch(struct ir *ir, uint32_t addr,
                                   union armv3_instr i, int op) {
  uint32_t cond = i.raw >> 28;
  struct ir_value *target =
      ir_alloc_i32(ir, addr + 8 + armv3_disasm_offset(i.branch.offset));
  struct ir_value *next = ir_alloc_i32(ir, addr + 4);

  if (cond == COND_AL) {
    if (op == ARMV3_OP_BL) {
      armv3_store_reg(ir, 14, next);
    }
    ir_branch(ir, target);
    return;
  }

  struct ir_value *passed = armv3_translate_cond(ir, cond);

  if (op == ARMV3_OP_BL) {
    struct ir_value *lr = armv3_load_reg(ir, addr, 14);
    armv3_store_reg(ir, 14, ir_select(ir, passed, next, lr));
  }

  ir_branch_cond(ir, passed, target, next);
}

static void armv3_translate_data(struct ir *ir, uint32_t addr,
                                 union armv3_instr i, int op) {
  uint32_t cond = i.raw >> 28;
  struct ir_value *passed = NULL;

  if (cond != COND_AL) {
    passed = armv3_translate_cond(ir, cond);
  }

  /* op2, with the shifter's carry out, which is NULL when c is left alone */
  struct ir_value *lhs = NULL;
  struct ir_value *rhs = NULL;
  struct ir_value *carry = NULL;

  if (i.data.i) {
    uint32_t n = i.data_imm.rot << 1;
    uint32_t imm = i.data_imm.imm;

    if (n) {
      imm = (imm >> n) | (imm << (32 - n));
      carry = ir_alloc_i32(ir, imm >> 31);
    }

    rhs = ir_alloc_i32(ir, imm);
  } else {
    rhs = armv3_translate_shift(ir, addr, i.data_reg.rm, i.data_reg.shift,
                                &carry);
  }

  if (op != ARMV3_OP_MOV && op != ARMV3_OP_MVN) {
    lhs = armv3_load_reg(ir, addr, i.data.rn);
  }

  if (op == ARMV3_OP_RSB) {
    struct ir_value *tmp = lhs;
    lhs = rhs;
    rhs = tmp;
  }

  struct ir_value *res = NULL;
  int sub = 0;
  int add = 0;

  switch (op) {
    case ARMV3_OP_AND:
    case ARMV3_OP_TST:
      res = ir_and(ir, lhs, rhs);
      break;
    case ARMV3_OP_EOR:
    case ARMV3_OP_TEQ:
      res = ir_xor(ir, lhs, rhs);
      break;
    case ARMV3_OP_SUB:
    case ARMV3_OP_RSB:
    case ARMV3_OP_CMP:
      res = ir_sub(ir, lhs, rhs);
      sub = 1;
      break;
    case ARMV3_OP_ADD:
    case ARMV3_OP_CMN:
      res = ir_add(ir, lhs, rhs);
      add = 1;
      break;
    case ARMV3_OP_ORR:
      res = ir_or(ir, lhs, rhs);
      break;
    case ARMV3_OP_MOV:
      res = rhs;
      break;
    case ARMV3_OP_BIC:
      res = ir_and(ir, lhs, ir_not(ir, rhs));
      break;
    case ARMV3_OP_MVN:
      res = ir_not(ir, rhs);
      break;
  }

  if (op != ARMV3_OP_TST && op != ARMV3_OP_TEQ && op != ARMV3_OP_CMP &&
      op != ARMV3_OP_CMN) {
    struct ir_value *v = res;
    if (passed) {
      v = ir_select(ir, passed, res, armv3_load_reg(ir, addr, i.data.rd));
    }
    armv3_store_reg(ir, i.data.rd, v);
  }

  if (!i.data.s) {
    return;
  }

  struct ir_value *sr = armv3_load_reg(ir, addr, CPSR);
  uint32_t keep = ~(N_MASK | Z_MASK | C_MASK | V_MASK);
  struct ir_value *flags = ir_and(ir, res, ir_alloc_i32(ir, N_MASK));
  flags = ir_or(ir, flags,
                armv3_flag(ir, ir_cmp_eq(ir, res, ir_alloc_i32(ir, 0)), Z_BIT));

  if (sub || add) {
    /* sub's c is set when there's no borrow, add's when there's a carry out.
       v is set when the result's sign is wrong */
    struct ir_value *c =
        sub ? ir_cmp_uge(ir, lhs, rhs) : ir_cmp_ult(ir, res, lhs);
    struct ir_value *v =
        sub ? ir_and(ir, ir_xor(ir, lhs, rhs), ir_xor(ir, res, lhs))
            : ir_and(ir, ir_xor(ir, res, lhs), ir_xor(ir, res, rhs));
    flags = ir_or(ir, flags, armv3_flag(ir, c, C_BIT));
    flags = ir_or(ir, flags, ir_and(ir, ir_lshri(ir, v, N_BIT - V_BIT),
                                    ir_alloc_i32(ir, V_MASK)));
  } else if (carry) {
    /* logical ops clear v */
    flags = ir_or(ir, flags, ir_shli(ir, carry, C_BIT));
  } else {
    keep |= C_MASK;
  }

  struct ir_value *new_sr =
      ir_or(ir, ir_and(ir, sr, ir_alloc_i32(ir, keep)), flags);
  if (passed) {
    new_sr = ir_select(ir, passed, new_sr, sr);
  }
  armv3_store_reg(ir, CPSR, new_sr);
}

static void armv3_translate_mrs(struct ir *ir, uint32_t addr,
                                union armv3_instr i) {
  armv3_store_reg(ir, i.mrs.rd,
                  armv3_load_reg(ir, addr, i.mrs.src_psr ? SPSR : CPSR));

  /* mrs ends the block */
  ir_branch(ir, ir_alloc_i32(ir, addr + 4));
}

static void armv3_translate_xfr(struct ir *ir, uint32_t addr,
                                union armv3_instr i) {
  struct ir_value *offset = NULL;

  if (i.xfr.i) {
    offset = armv3_translate_shift(ir, addr, i.xfr_reg.rm, i.xfr_reg.shift,
                                   NULL);
  } else {
    offset = ir_alloc_i32(ir, i.xfr_imm.imm);
  }

  struct ir_value *base = armv3_load_reg(ir, addr, i.xfr.rn);
  struct ir_value *final =
      i.xfr.u ? ir_add(ir, base, offset) : ir_sub(ir, base, offset);
  struct ir_value *ea = i.xfr.p ? final : base;

  /* writeback is applied before memory is read, and post-indexing always
     writes back */
  if (i.xfr.w || !i.xfr.p) {
    armv3_store_reg(ir, i.xfr.rn, final);
  }

  if (i.xfr.l) {
    struct ir_value *data = NULL;
    if (i.xfr.b) {
      data = ir_zext(ir, ir_load_guest(ir, ea, VALUE_I8), VALUE_I32);
    } else {
      data = ir_load_guest(ir, ea, VALUE_I32);
    }
    armv3_store_reg(ir, i.xfr.rd, data);
  } else {
    struct ir_value *data = armv3_load_reg(ir, addr, i.xfr.rd);
    if (i.xfr.b) {
      ir_store_guest(ir, ea, ir_trunc(ir, data, VALUE_I8));
    } else {
      ir_store_guest(ir, ea, data);
    }
  }
}

static void armv3_translate_blk(struct ir *ir, uint32_t addr,
                                union armv3_instr i) {
  struct ir_value *base = armv3_load_reg(ir, addr, i.blk.rn);
  struct ir_value *size = ir_alloc_i32(ir, popcnt32(i.blk.rlist) * 4);
  struct ir_value *final =
      i.blk.u ? ir_add(ir, base, size) : ir_sub(ir, base, size);
  struct ir_value *pc = NULL;
  int offset = 0;
  int wrote = 0;

  /* a load's writeback is applied before memory is read */
  if (i.blk.l && i.blk.w) {
    armv3_store_reg(ir, i.blk.rn, final);
  }

  for (int bit = 0; bit < 16; bit++) {
    int reg = i.blk.u ? bit : 15 - bit;

    if (!(i.blk.rlist & (1 << reg))) {
      continue;
    }

    if (i.blk.p) {
      offset += 4;
    }

    struct ir_value *ea = base;
    if (offset) {
      struct ir_value *n = ir_alloc_i32(ir, offset);
      ea = i.blk.u ? ir_add(ir, base, n) : ir_sub(ir, base, n);
    }

    if (i.blk.l) {
      struct ir_value *data = ir_load_guest(ir, ea, VALUE_I32);
      if (reg == 15) {
        pc = data;
      } else {
        armv3_store_reg(ir, reg, data);
      }
    } else {
      /* the pc is stored 12 bytes ahead */
      struct ir_value *data = reg == 15 ? ir_alloc_i32(ir, addr + 12)
                                        : armv3_load_reg(ir, addr, reg);
      ir_store_guest(ir, ea, data);

      /* a store's writeback is applied after the first register is stored */
      if (i.blk.w && !wrote) {
        armv3_store_reg(ir, i.blk.rn, final);
        wrote = 1;
      }
    }

    if (!i.blk.p) {
      offset += 4;
    }
  }

  /* transferring the pc ends the block */
  if (i.blk.rlist & (1 << 15)) {
    ir_branch(ir, pc ? pc : ir_alloc_i32(ir, addr + 4));
  }
}

static void armv3_translate_instr(struct ir *ir, uint32_t addr,
                                  union armv3_instr i, int op) {
  switch (op) {
    case ARMV3_OP_B:
    case ARMV3_OP_BL:
      armv3_translate_branch(ir, addr, i, op);
      break;
    case ARMV3_OP_MRS:
      armv3_translate_mrs(ir, addr, i);
      break;
    case ARMV3_OP_LDR:
    case ARMV3_OP_STR:
      armv3_translate_xfr(ir, addr, i);
      break;
    case ARMV3_OP_LDM:
    case ARMV3_OP_STM:
      armv3_translate_blk(ir, addr, i);
      break;
    default:
      armv3_translate_data(ir, addr, i, op);
      break;
  }
}

static void armv3_frontend_translate_code(struct jit_frontend *base,
                                          uint32_t begin_addr, int size,
                                          struct ir *ir) {
  struct armv3_frontend *frontend = (struct armv3_frontend *)base;
  struct armv3_guest *guest = (struct armv3_guest *)frontend->guest;

  int offset = 0;

  while (offset < size) {
    uint32_t addr = begin_addr + offset;
    uint32_t data = guest->r32(guest->mem, addr);
    union armv3_instr i = {data};
    int op = armv3_get_op(data);
    struct jit_opdef *def = armv3_get_opdef(data);

    ir_source_info(ir, addr, 12);

    /* fallbacks take less of the ir buffer, so a long block is finished with
       them once it's half full */
    if (ir->used < ir->capacity / 2 && armv3_can_translate(i, op)) {
      armv3_translate_instr(ir, addr, i, op);
    } else {
      ir_fallback(ir, def->fallback, addr, data);
    }

    offset += 4;
  }
}

static void armv3_frontend_analyze_code(struct jit_frontend *base,
                                        uint32_t begin_addr, int *size) {
  struct armv3_frontend *frontend = (struct armv3_frontend *)base;
  struct armv3_guest *guest = (struct armv3_guest *)frontend->guest;

  *size = 0;

  while (1) {
    uint32_t addr = begin_addr + *size;
    uint32_t data = guest->r32(guest->mem, addr);
    union armv3_instr i = {data};
    struct jit_opdef *def = armv3_get_opdef(i.raw);

    *size += 4;

    /* stop emitting when pc is changed */
    int mov_to_pc = 0;

    mov_to_pc |= (def->flags & FLAG_SET_PC);
    mov_to_pc |= (def->flags & FLAG_DATA) && i.data.rd == 15;
    mov_to_pc |= (def->flags & FLAG_PSR);
    mov_to_pc |= (def->flags & FLAG_XFR) && i.xfr.rd == 15;
    mov_to_pc |= (def->flags & FLAG_BLK) && (i.blk.rlist & (1 << 15));
    mov_to_pc |= (def->flags & FLAG_SWI);

    if (mov_to_pc) {
      break;
    }
  }
}

void armv3_frontend_destroy(struct jit_frontend *base) {
  struct armv3_frontend *frontend = (struct armv3_frontend *)base;

  free(frontend);
}

struct jit_frontend *armv3_frontend_create(struct jit_guest *guest) {
  struct armv3_frontend *frontend = calloc(1, sizeof(struct armv3_frontend));

  frontend->guest = guest;
  frontend->destroy = &armv3_frontend_destroy;
  frontend->analyze_code = &armv3_frontend_analyze_code;
  frontend->translate_code = &armv3_frontend_translate_code;
  frontend->dump_code = &armv3_frontend_dump_code;
  frontend->lookup_op = &armv3_frontend_lookup_op;

  return (struct jit_frontend *)frontend;
}
