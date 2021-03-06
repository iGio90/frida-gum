/*
 * Copyright (C) 2014-2019 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gummipsrelocator.h"

#include "gummemory.h"

#define GUM_MAX_INPUT_INSN_COUNT (100)

typedef struct _GumCodeGenCtx GumCodeGenCtx;

struct _GumCodeGenCtx
{
  const cs_insn * insn;
  cs_mips * detail;

  const cs_insn * delay_slot_insn;
  cs_mips * delay_slot_detail;

  GumMipsWriter * output;
};

static gboolean gum_mips_has_delay_slot (const cs_insn * insn);

GumMipsRelocator *
gum_mips_relocator_new (gconstpointer input_code,
                        GumMipsWriter * output)
{
  GumMipsRelocator * relocator;

  relocator = g_slice_new (GumMipsRelocator);

  gum_mips_relocator_init (relocator, input_code, output);

  return relocator;
}

GumMipsRelocator *
gum_mips_relocator_ref (GumMipsRelocator * relocator)
{
  g_atomic_int_inc (&relocator->ref_count);

  return relocator;
}

void
gum_mips_relocator_unref (GumMipsRelocator * relocator)
{
  if (g_atomic_int_dec_and_test (&relocator->ref_count))
  {
    gum_mips_relocator_clear (relocator);

    g_slice_free (GumMipsRelocator, relocator);
  }
}

void
gum_mips_relocator_init (GumMipsRelocator * relocator,
                         gconstpointer input_code,
                         GumMipsWriter * output)
{
  relocator->ref_count = 1;

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
  cs_open (CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN,
      &relocator->capstone);
#else
  cs_open (CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_BIG_ENDIAN,
      &relocator->capstone);
#endif
  cs_option (relocator->capstone, CS_OPT_DETAIL, CS_OPT_ON);
  relocator->input_insns = g_new0 (cs_insn *, GUM_MAX_INPUT_INSN_COUNT);

  relocator->output = NULL;

  gum_mips_relocator_reset (relocator, input_code, output);
}

void
gum_mips_relocator_clear (GumMipsRelocator * relocator)
{
  guint i;

  gum_mips_relocator_reset (relocator, NULL, NULL);

  for (i = 0; i != GUM_MAX_INPUT_INSN_COUNT; i++)
  {
    cs_insn * insn = relocator->input_insns[i];
    if (insn != NULL)
    {
      cs_free (insn, 1);
      relocator->input_insns[i] = NULL;
    }
  }
  g_free (relocator->input_insns);

  cs_close (&relocator->capstone);
}

void
gum_mips_relocator_reset (GumMipsRelocator * relocator,
                          gconstpointer input_code,
                          GumMipsWriter * output)
{
  relocator->input_start = input_code;
  relocator->input_cur = input_code;
  relocator->input_pc = GUM_ADDRESS (input_code);

  if (output != NULL)
    gum_mips_writer_ref (output);
  if (relocator->output != NULL)
    gum_mips_writer_unref (relocator->output);
  relocator->output = output;

  relocator->inpos = 0;
  relocator->outpos = 0;

  relocator->eob = FALSE;
  relocator->eoi = FALSE;
  relocator->delay_slot_pending = FALSE;
}

static guint
gum_mips_relocator_inpos (GumMipsRelocator * self)
{
  return self->inpos % GUM_MAX_INPUT_INSN_COUNT;
}

static guint
gum_mips_relocator_outpos (GumMipsRelocator * self)
{
  return self->outpos % GUM_MAX_INPUT_INSN_COUNT;
}

static void
gum_mips_relocator_increment_inpos (GumMipsRelocator * self)
{
  self->inpos++;
  g_assert (self->inpos > self->outpos);
}

static void
gum_mips_relocator_increment_outpos (GumMipsRelocator * self)
{
  self->outpos++;
  g_assert (self->outpos <= self->inpos);
}

guint
gum_mips_relocator_read_one (GumMipsRelocator * self,
                             const cs_insn ** instruction)
{
  cs_insn ** insn_ptr, * insn;
  const uint8_t * code;
  size_t size;
  uint64_t address;

  if (self->eoi && !self->delay_slot_pending)
    return 0;

  insn_ptr = &self->input_insns[gum_mips_relocator_inpos (self)];

  if (*insn_ptr == NULL)
    *insn_ptr = cs_malloc (self->capstone);

  code = self->input_cur;
  size = 4;
  address = self->input_pc;
  insn = *insn_ptr;

  if (!cs_disasm_iter (self->capstone, &code, &size, &address, insn))
    return 0;

  switch (insn->id)
  {
    case MIPS_INS_J:
      self->eob = TRUE;
      self->eoi = TRUE;
      self->delay_slot_pending = TRUE;
      break;
    case MIPS_INS_JR:
      self->eob = TRUE;
      self->eoi = TRUE;
      self->delay_slot_pending = TRUE;
      break;
    case MIPS_INS_BGEZAL:
    case MIPS_INS_BGEZALL:
    case MIPS_INS_BLTZAL:
    case MIPS_INS_BLTZALL:
    case MIPS_INS_JAL:
    case MIPS_INS_JALR:
      self->eob = TRUE;
      self->eoi = FALSE;
      self->delay_slot_pending = TRUE;
      break;
    case MIPS_INS_BEQ:
    case MIPS_INS_BEQL:
    case MIPS_INS_BGEZ:
    case MIPS_INS_BGEZL:
    case MIPS_INS_BGTZ:
    case MIPS_INS_BGTZL:
    case MIPS_INS_BLEZ:
    case MIPS_INS_BLEZL:
    case MIPS_INS_BLTZ:
    case MIPS_INS_BLTZL:
    case MIPS_INS_BNE:
    case MIPS_INS_BNEL:
      self->eob = TRUE;
      self->eoi = FALSE;
      self->delay_slot_pending = TRUE;
      break;
    default:
      self->eob = FALSE;
      if (self->delay_slot_pending)
        self->delay_slot_pending = FALSE;
      break;
  }

  gum_mips_relocator_increment_inpos (self);

  if (instruction != NULL)
    *instruction = insn;

  self->input_cur += insn->size;
  self->input_pc += insn->size;

  return self->input_cur - self->input_start;
}

cs_insn *
gum_mips_relocator_peek_next_write_insn (GumMipsRelocator * self)
{
  if (self->outpos == self->inpos)
    return NULL;

  return self->input_insns[gum_mips_relocator_outpos (self)];
}

gpointer
gum_mips_relocator_peek_next_write_source (GumMipsRelocator * self)
{
  cs_insn * next;

  next = gum_mips_relocator_peek_next_write_insn (self);
  if (next == NULL)
    return NULL;

  return GSIZE_TO_POINTER (next->address);
}

void
gum_mips_relocator_skip_one (GumMipsRelocator * self)
{
  gum_mips_relocator_peek_next_write_insn (self);
  gum_mips_relocator_increment_outpos (self);
}

gboolean
gum_mips_relocator_write_one (GumMipsRelocator * self)
{
  const cs_insn * insn;
  const cs_insn * delay_slot_insn = NULL;
  GumCodeGenCtx ctx;
  gboolean rewritten;

  if ((insn = gum_mips_relocator_peek_next_write_insn (self)) == NULL)
    return FALSE;
  gum_mips_relocator_increment_outpos (self);
  ctx.insn = insn;
  ctx.detail = &ctx.insn->detail->mips;
  ctx.output = self->output;

  if (gum_mips_has_delay_slot (insn))
  {
    delay_slot_insn = gum_mips_relocator_peek_next_write_insn (self);
    if (delay_slot_insn == NULL)
      return FALSE;
    gum_mips_relocator_increment_outpos (self);
    ctx.delay_slot_insn = delay_slot_insn;
    ctx.delay_slot_detail = &ctx.delay_slot_insn->detail->mips;
  }
  else
  {
    ctx.delay_slot_insn = NULL;
    ctx.delay_slot_detail = NULL;
  }

  switch (insn->id)
  {
    default:
      rewritten = FALSE;
      break;
  }

  if (!rewritten)
  {
    gum_mips_writer_put_bytes (ctx.output, insn->bytes, insn->size);

    if (delay_slot_insn != NULL)
    {
      gum_mips_writer_put_bytes (ctx.output, delay_slot_insn->bytes,
          delay_slot_insn->size);
    }
  }

  return TRUE;
}

void
gum_mips_relocator_write_all (GumMipsRelocator * self)
{
  guint count = 0;

  while (gum_mips_relocator_write_one (self))
    count++;

  g_assert (count > 0);
}

gboolean
gum_mips_relocator_eob (GumMipsRelocator * self)
{
  return self->eob || self->delay_slot_pending;
}

gboolean
gum_mips_relocator_eoi (GumMipsRelocator * self)
{
  return self->eoi || self->delay_slot_pending;
}

gboolean
gum_mips_relocator_can_relocate (gpointer address,
                                 guint min_bytes,
                                 GumRelocationScenario scenario,
                                 guint * maximum,
                                 mips_reg * available_scratch_reg)
{
  guint n = 0;
  guint8 * buf;
  GumMipsWriter cw;
  GumMipsRelocator rl;
  guint reloc_bytes;

  buf = g_alloca (3 * min_bytes);
  gum_mips_writer_init (&cw, buf);

  gum_mips_relocator_init (&rl, address, &cw);

  do
  {
    const cs_insn * insn;
    gboolean safe_to_relocate_further;

    reloc_bytes = gum_mips_relocator_read_one (&rl, &insn);
    if (reloc_bytes == 0)
      break;

    n = reloc_bytes;

    if (scenario == GUM_SCENARIO_ONLINE)
    {
      switch (insn->id)
      {
        case MIPS_INS_JAL:
        case MIPS_INS_JALR:
        case MIPS_INS_SYSCALL:
          safe_to_relocate_further = FALSE;
          break;
        default:
          safe_to_relocate_further = TRUE;
          break;
      }
    }
    else
    {
      safe_to_relocate_further = TRUE;
    }

    if (!safe_to_relocate_further)
      break;
  }
  while (reloc_bytes < min_bytes || rl.delay_slot_pending);

  if (!rl.eoi)
  {
    csh capstone;
    cs_insn * insn;
    size_t count, i;
    gboolean eoi;

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    cs_open (CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN, &capstone);
#else
    cs_open (CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_BIG_ENDIAN, &capstone);
#endif
    cs_option (capstone, CS_OPT_DETAIL, CS_OPT_ON);

    count = cs_disasm (capstone, rl.input_cur, 1024, rl.input_pc, 0, &insn);
    g_assert (insn != NULL);

    eoi = FALSE;
    for (i = 0; i != count && !eoi; i++)
    {
      cs_mips * d = &insn[i].detail->mips;

      switch (insn[i].id)
      {
        case MIPS_INS_J:
        {
          cs_mips_op * op;
          gssize target, offset;

          op = &d->operands[0];

          g_assert (op->type == MIPS_OP_IMM);
          target =
              (gssize) (GPOINTER_TO_SIZE (insn[i].address & 0xf0000000)) |
              (op->imm << 2);
          offset = target - (gssize) GPOINTER_TO_SIZE (address);
          if (offset > 0 && offset < (gssize) n)
            n = offset;
          eoi = TRUE;
          break;
        }
        case MIPS_INS_BEQ:
        case MIPS_INS_BEQL:
        case MIPS_INS_BGEZ:
        case MIPS_INS_BGEZL:
        case MIPS_INS_BGTZ:
        case MIPS_INS_BGTZL:
        case MIPS_INS_BLEZ:
        case MIPS_INS_BLEZL:
        case MIPS_INS_BLTZ:
        case MIPS_INS_BLTZL:
        case MIPS_INS_BNE:
        case MIPS_INS_BNEL:
        {
          cs_mips_op * op;
          gssize target, offset;

          op = d->op_count == 3 ? &d->operands[2] : &d->operands[1];

          g_assert (op->type == MIPS_OP_IMM);
          target = (gssize) insn->address +
              (op->imm & 0x8000 ? (0xffff0000 + op->imm) << 2 : op->imm << 2);
          offset =
              target - (gssize) GPOINTER_TO_SIZE (address);
          if (offset > 0 && offset < (gssize) n)
            n = offset;
          break;
        }
        case MIPS_INS_JR:
          eoi = TRUE;
          break;
        default:
          break;
      }
    }

    cs_free (insn, count);

    cs_close (&capstone);
  }

  if (available_scratch_reg != NULL)
  {
    gboolean at_used;
    guint insn_index;

    at_used = FALSE;

    for (insn_index = 0; insn_index != n / 4; insn_index++)
    {
      const cs_insn * insn = rl.input_insns[insn_index];
      const cs_mips * info = &insn->detail->mips;
      uint8_t op_index;

      for (op_index = 0; op_index != info->op_count; op_index++)
      {
        const cs_mips_op * op = &info->operands[op_index];

        if (op->type == MIPS_OP_REG)
        {
          at_used |= op->reg == MIPS_REG_AT;
        }
      }
    }

    if (!at_used)
      *available_scratch_reg = MIPS_REG_AT;
    else
      *available_scratch_reg = MIPS_REG_INVALID;
  }

  gum_mips_relocator_clear (&rl);

  gum_mips_writer_clear (&cw);

  if (maximum != NULL)
    *maximum = n;

  return n >= min_bytes;
}

guint
gum_mips_relocator_relocate (gpointer from,
                             guint min_bytes,
                             gpointer to)
{
  GumMipsWriter cw;
  GumMipsRelocator rl;
  guint reloc_bytes;

  gum_mips_writer_init (&cw, to);

  gum_mips_relocator_init (&rl, from, &cw);

  do
  {
    reloc_bytes = gum_mips_relocator_read_one (&rl, NULL);
    g_assert (reloc_bytes != 0);
  }
  while (reloc_bytes < min_bytes || rl.delay_slot_pending);

  gum_mips_relocator_write_all (&rl);

  gum_mips_relocator_clear (&rl);
  gum_mips_writer_clear (&cw);

  return reloc_bytes;
}

static gboolean
gum_mips_has_delay_slot (const cs_insn * insn)
{
  switch (insn->id)
  {
    case MIPS_INS_J:
    case MIPS_INS_BGEZAL:
    case MIPS_INS_BGEZALL:
    case MIPS_INS_BLTZAL:
    case MIPS_INS_BLTZALL:
    case MIPS_INS_JAL:
    case MIPS_INS_JALR:
    case MIPS_INS_BEQ:
    case MIPS_INS_BEQL:
    case MIPS_INS_BGEZ:
    case MIPS_INS_BGEZL:
    case MIPS_INS_BGTZ:
    case MIPS_INS_BGTZL:
    case MIPS_INS_BLEZ:
    case MIPS_INS_BLEZL:
    case MIPS_INS_BLTZ:
    case MIPS_INS_BLTZL:
    case MIPS_INS_BNE:
    case MIPS_INS_BNEL:
      return TRUE;
    default:
      return FALSE;
  }
}
