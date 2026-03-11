/*
 * Copyright © 2014 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "util/dag.h"
#include "util/u_math.h"

#include "ir3.h"
#include "ir3_compiler.h"

#if MESA_DEBUG
#define SCHED_DEBUG (ir3_shader_debug & IR3_DBG_SCHEDMSGS)
#else
#define SCHED_DEBUG 0
#endif
#define d(fmt, ...)                                                            \
   do {                                                                        \
      if (SCHED_DEBUG) {                                                       \
         mesa_logi("SCHED: " fmt, ##__VA_ARGS__);                              \
      }                                                                        \
   } while (0)

#define di(instr, fmt, ...)                                                    \
   do {                                                                        \
      if (SCHED_DEBUG) {                                                       \
         struct log_stream *stream = mesa_log_streami();                       \
         mesa_log_stream_printf(stream, "SCHED: " fmt ": ", ##__VA_ARGS__);    \
         ir3_print_instr_stream(stream, instr);                                \
         mesa_log_stream_destroy(stream);                                      \
      }                                                                        \
   } while (0)

struct ir3_sched_ctx {
   struct ir3_compiler *compiler;
   struct ir3_block *block;
   struct dag *dag;

   struct list_head unscheduled_list;
   struct ir3_instruction *scheduled;
   struct ir3_instruction *addr0;
   struct ir3_instruction *addr1;
   unsigned addr0_uses;
   unsigned addr1_uses;

   struct ir3_instruction *split;

   int remaining_kills;
   int remaining_tex;

   bool error;

   unsigned ip;

   int sy_delay;
   int ss_delay;

   int sy_index, first_outstanding_sy_index;
   int ss_index, first_outstanding_ss_index;
};

struct ir3_sched_node {
   struct dag_node dag;
   struct ir3_instruction *instr;

   unsigned delay;
   unsigned max_delay;

   unsigned sy_index;
   unsigned ss_index;

   unsigned earliest_ip;

   struct ir3_instruction *collect;
   bool partially_live;

   bool kill_path;
   bool output;
};

#define foreach_sched_node(__n, __list)                                        \
   list_for_each_entry (struct ir3_sched_node, __n, __list, dag.link)

static void sched_node_init(struct ir3_sched_ctx *ctx,
                            struct ir3_instruction *instr);
static void sched_node_add_dep(struct ir3_sched_ctx *ctx,
                               struct ir3_instruction *instr,
                               struct ir3_instruction *src, int i);

/* Объявления функций */
static int nearest_use(struct ir3_instruction *instr);
static int live_effect(struct ir3_instruction *instr);
static unsigned node_delay(struct ir3_sched_ctx *ctx, struct ir3_sched_node *n);
static void dump_state(struct ir3_sched_ctx *ctx);

static bool
is_scheduled(struct ir3_instruction *instr)
{
   return !!(instr->flags & IR3_INSTR_MARK);
}

static bool
sched_check_src_cond(struct ir3_instruction *instr,
                     bool (*cond)(struct ir3_instruction *,
                                  struct ir3_instruction *,
                                  struct ir3_sched_ctx *),
                     struct ir3_sched_ctx *ctx)
{
   foreach_ssa_src (src, instr) {
      if ((src->opc == OPC_META_SPLIT) || (src->opc == OPC_META_COLLECT)) {
         if (sched_check_src_cond(src, cond, ctx))
            return true;
      } else {
         if (cond(src, instr, ctx))
            return true;
      }
   }

   return false;
}

static bool
is_outstanding_sy(struct ir3_instruction *instr, struct ir3_instruction *use,
                  struct ir3_sched_ctx *ctx)
{
   if (!is_sy_producer(instr))
      return false;

   if (instr->block != ctx->block)
      return true;

   struct ir3_sched_node *n = instr->data;
   return n->sy_index >= ctx->first_outstanding_sy_index;
}

static bool
is_outstanding_ss(struct ir3_instruction *instr, struct ir3_instruction *use,
                  struct ir3_sched_ctx *ctx)
{
   if (!needs_ss(ctx->compiler, instr, use))
      return false;

   if (instr->block != ctx->block)
      return true;

   struct ir3_sched_node *n = instr->data;
   return n->ss_index >= ctx->first_outstanding_ss_index;
}

static unsigned
cycle_count(struct ir3_instruction *instr)
{
   if (instr->opc == OPC_META_COLLECT) {
      unsigned n = 0;
      foreach_src (src, instr) {
         if (src->flags & (IR3_REG_IMMED | IR3_REG_CONST))
            n++;
      }
      return n;
   } else if (is_meta(instr)) {
      return 0;
   } else {
      return 1;
   }
}

static void
schedule(struct ir3_sched_ctx *ctx, struct ir3_instruction *instr)
{
   assert(ctx->block == instr->block);

   list_delinit(&instr->node);

   if (writes_addr0(instr)) {
      assert(ctx->addr0 == NULL);
      ctx->addr0 = instr;
      ctx->addr0_uses = instr->uses->entries;
   }

   if (writes_addr1(instr)) {
      assert(ctx->addr1 == NULL);
      ctx->addr1 = instr;
      ctx->addr1_uses = instr->uses->entries;
   }

   if (reads_addr0(instr)) {
      assert(instr->address->def->instr == ctx->addr0);
      assert(ctx->addr0_uses > 0);

      if (--ctx->addr0_uses == 0) {
         ctx->addr0 = NULL;
      }
   }

   if (reads_addr1(instr)) {
      assert(instr->address->def->instr == ctx->addr1);
      assert(ctx->addr1_uses > 0);

      if (--ctx->addr1_uses == 0) {
         ctx->addr1 = NULL;
      }
   }

   instr->flags |= IR3_INSTR_MARK;

   di(instr, "schedule");

   list_addtail(&instr->node, &instr->block->instr_list);
   ctx->scheduled = instr;

   if (is_kill_or_demote(instr)) {
      assert(ctx->remaining_kills > 0);
      ctx->remaining_kills--;
   }

   struct ir3_sched_node *n = instr->data;

   if (n->collect) {
      foreach_ssa_src (src, n->collect) {
         if (src->block != instr->block)
            continue;
         struct ir3_sched_node *sn = src->data;
         sn->partially_live = true;
      }
   }

   bool counts_for_delay = is_alu(instr) || is_flow(instr);

   unsigned delay_cycles = counts_for_delay ? 1 + instr->repeat : 0;

   ctx->ip = MAX2(ctx->ip, n->earliest_ip) + delay_cycles;

   util_dynarray_foreach (&n->dag.edges, struct dag_edge, edge) {
      unsigned delay = (unsigned)(uintptr_t)edge->data;
      struct ir3_sched_node *child =
         container_of(edge->child, struct ir3_sched_node, dag);
      child->earliest_ip = MAX2(child->earliest_ip, ctx->ip + delay);
   }

   dag_prune_head(ctx->dag, &n->dag);

   unsigned cycles = cycle_count(instr);

   if (is_ss_producer(instr)) {
      ctx->ss_delay = soft_ss_delay(instr);
      n->ss_index = ctx->ss_index++;
   } else if (!is_meta(instr) &&
              sched_check_src_cond(instr, is_outstanding_ss, ctx)) {
      ctx->ss_delay = 0;
      ctx->first_outstanding_ss_index = ctx->ss_index;
   } else if (ctx->ss_delay > 0) {
      ctx->ss_delay -= MIN2(cycles, ctx->ss_delay);
   }

   if (is_sy_producer(instr)) {
      ctx->sy_delay = soft_sy_delay(instr, ctx->block->shader);
      assert(ctx->remaining_tex > 0);
      ctx->remaining_tex--;
      n->sy_index = ctx->sy_index++;
   } else if (!is_meta(instr) &&
              sched_check_src_cond(instr, is_outstanding_sy, ctx)) {
      ctx->sy_delay = 0;
      ctx->first_outstanding_sy_index = ctx->sy_index;
   } else if (ctx->sy_delay > 0) {
      ctx->sy_delay -= MIN2(cycles, ctx->sy_delay);
   }
}

struct ir3_sched_notes {
   bool blocked_kill;
   bool addr0_conflict, addr1_conflict;
};

/* Определение базовых функций до их использования */
static unsigned
node_delay(struct ir3_sched_ctx *ctx, struct ir3_sched_node *n)
{
   return MAX2(n->earliest_ip, ctx->ip) - ctx->ip;
}

static int
nearest_use(struct ir3_instruction *instr)
{
    unsigned nearest = ~0;
    foreach_ssa_use (use, instr)
        if (!is_scheduled(use))
            nearest = MIN2(nearest, use->ip);

    if (is_input(instr))
        nearest = nearest / 3;

    if (is_tex(instr))
        nearest = nearest * 2 / 3;

    return nearest;
}

static unsigned
new_regs(struct ir3_instruction *instr)
{
   unsigned regs = 0;

   foreach_dst (dst, instr) {
      if (!is_dest_gpr(dst))
         continue;
      regs += reg_elems(dst);
   }

   return regs;
}

static bool
is_only_nonscheduled_use(struct ir3_instruction *instr,
                         struct ir3_instruction *use)
{
   foreach_ssa_use (other_use, instr) {
      if (other_use != use && !is_scheduled(other_use))
         return false;
   }

   return true;
}

static int
live_effect(struct ir3_instruction *instr)
{
    struct ir3_sched_node *n = instr->data;
    int new_live =
        (n->partially_live || !instr->uses || instr->uses->entries == 0)
            ? 0
            : new_regs(instr);
    int freed_live = 0;

    if (n->collect)
        new_live = new_live * n->collect->srcs_count / 2;

    foreach_ssa_src_n (src, n, instr) {
        if (__is_false_dep(instr, n))
            continue;

        if (instr->block != src->block)
            continue;

        if (is_only_nonscheduled_use(src, instr))
            freed_live += new_regs(src);
    }

    return new_live - freed_live;
}

static bool
should_skip(struct ir3_sched_ctx *ctx, struct ir3_instruction *instr)
{
    if (ctx->remaining_kills && (is_tex(instr) || is_mem(instr))) {

        struct ir3_sched_node *n = instr->data;

        if (n->kill_path)
            return false;

        unsigned dist = (unsigned)nearest_use(instr);
        if (dist > 48)
            return true;

        int live = live_effect(instr);
        if (live > 6)
            return true;

        return false;
    }

    return false;
}

static bool
could_sched(struct ir3_sched_ctx *ctx,
            struct ir3_instruction *instr, struct ir3_instruction *src)
{
   foreach_ssa_src (other_src, instr) {
      if ((src != other_src) && !is_scheduled(other_src)) {
         return false;
      }
   }

   if (instr->block != src->block)
      return false;

   return !should_skip(ctx, instr);
}

static bool
check_instr(struct ir3_sched_ctx *ctx, struct ir3_sched_notes *notes,
            struct ir3_instruction *instr)
{
   assert(!is_scheduled(instr));

   if (instr == ctx->split) {
      return false;
   }

   if (should_skip(ctx, instr))
       return false;

   if (writes_addr0(instr)) {
      struct ir3 *ir = instr->block->shader;
      bool ready = false;
      for (unsigned i = 0; (i < ir->a0_users_count) && !ready; i++) {
         struct ir3_instruction *indirect = ir->a0_users[i];
         if (!indirect)
            continue;
         if (indirect->address->def != instr->dsts[0])
            continue;
         ready = could_sched(ctx, indirect, instr);
      }

      if (!ready)
         return false;
   }

   if (writes_addr1(instr)) {
      struct ir3 *ir = instr->block->shader;
      bool ready = false;
      for (unsigned i = 0; (i < ir->a1_users_count) && !ready; i++) {
         struct ir3_instruction *indirect = ir->a1_users[i];
         if (!indirect)
            continue;
         if (indirect->address->def != instr->dsts[0])
            continue;
         ready = could_sched(ctx, indirect, instr);
      }

      if (!ready)
         return false;
   }

   if (writes_addr0(instr) && ctx->addr0) {
      assert(ctx->addr0 != instr);
      notes->addr0_conflict = true;
      return false;
   }

   if (writes_addr1(instr) && ctx->addr1) {
      assert(ctx->addr1 != instr);
      notes->addr1_conflict = true;
      return false;
   }

   if (is_kill_or_demote(instr)) {
      struct ir3 *ir = instr->block->shader;

      for (unsigned i = 0; i < ir->baryfs_count; i++) {
         struct ir3_instruction *baryf = ir->baryfs[i];
         if (baryf->flags & IR3_INSTR_UNUSED)
            continue;
         if (!is_scheduled(baryf)) {
            notes->blocked_kill = true;
            return false;
         }
      }
   }

   return true;
}

static bool
should_defer(struct ir3_sched_ctx *ctx, struct ir3_instruction *instr)
{
    if (ctx->ss_delay) {
        if (sched_check_src_cond(instr, is_outstanding_ss, ctx)) {
            int outstanding_ss = ctx->ss_index - ctx->first_outstanding_ss_index;
            if (outstanding_ss < 16)
                return true;
        }
    }

    if (ctx->sy_delay && ctx->remaining_tex) {
        if (sched_check_src_cond(instr, is_outstanding_sy, ctx)) {
            int outstanding_sy = ctx->sy_index - ctx->first_outstanding_sy_index;
            if (outstanding_sy < 16)
                return true;
        }
    }

    if (is_sy_producer(instr)) {
        int outstanding = ctx->sy_index - ctx->first_outstanding_sy_index;

        if (outstanding >= 16)
            return true;

        unsigned dist = (unsigned)nearest_use(instr);
        if (dist > 64 && outstanding >= 10)
            return true;

        int live = live_effect(instr);
        if (live > 8 && outstanding >= 8)
            return true;
    }

    {
        int outstanding_ss = ctx->ss_index - ctx->first_outstanding_ss_index;
        if (outstanding_ss >= 16 && is_ss_producer(instr))
            return true;
    }

    return false;
}

static struct ir3_sched_node *choose_instr_inc(struct ir3_sched_ctx *ctx,
                                               struct ir3_sched_notes *notes,
                                               bool defer, bool avoid_output);

static struct ir3_sched_node *
choose_instr_tex_prio(struct ir3_sched_ctx *ctx, struct ir3_sched_notes *notes)
{
    struct ir3_sched_node *chosen = NULL;
    unsigned chosen_distance = ~0;

    foreach_sched_node (n, &ctx->dag->heads) {
        if (!is_tex(n->instr))
            continue;

        if (!check_instr(ctx, notes, n->instr))
            continue;

        unsigned d = node_delay(ctx, n);
        if (d > 0)
            continue;

        unsigned distance = nearest_use(n->instr);

        if (!chosen || distance < chosen_distance) {
            chosen = n;
            chosen_distance = distance;
        }
    }

    if (chosen) {
        di(chosen->instr, "tex_prio: chose (early texture)");
        return chosen;
    }

    return NULL;
}

enum choose_instr_dec_rank {
   DEC_NEUTRAL,
   DEC_NEUTRAL_READY,
   DEC_FREED,
   DEC_FREED_READY,
};

static const char *
dec_rank_name(enum choose_instr_dec_rank rank)
{
   switch (rank) {
   case DEC_NEUTRAL:
      return "neutral";
   case DEC_NEUTRAL_READY:
      return "neutral+ready";
   case DEC_FREED:
      return "freed";
   case DEC_FREED_READY:
      return "freed+ready";
   default:
      return NULL;
   }
}

static struct ir3_sched_node *
choose_instr_dec(struct ir3_sched_ctx *ctx, struct ir3_sched_notes *notes,
                 bool defer)
{
   const char *mode = defer ? "-d" : "";
   struct ir3_sched_node *chosen = NULL;
   enum choose_instr_dec_rank chosen_rank = DEC_NEUTRAL;

   foreach_sched_node (n, &ctx->dag->heads) {
      if (defer && should_defer(ctx, n->instr))
         continue;

      unsigned d = node_delay(ctx, n);

      int live = live_effect(n->instr);
      if (live > 0)
         continue;

      if (!check_instr(ctx, notes, n->instr))
         continue;

      enum choose_instr_dec_rank rank;
      if (live < 0) {
         if (d == 0)
            rank = DEC_FREED_READY;
         else
            rank = DEC_FREED;
      } else {
         if (d == 0)
            rank = DEC_NEUTRAL_READY;
         else
            rank = DEC_NEUTRAL;
      }

      if (!chosen || rank > chosen_rank ||
          (rank == chosen_rank && chosen->max_delay < n->max_delay)) {
         chosen = n;
         chosen_rank = rank;
      }
   }

   if (chosen) {
      di(chosen->instr, "dec%s: chose (%s)", mode, dec_rank_name(chosen_rank));
      return chosen;
   }

   return choose_instr_inc(ctx, notes, defer, true);
}

enum choose_instr_inc_rank {
   INC_DISTANCE,
   INC_DISTANCE_READY,
};

static const char *
inc_rank_name(enum choose_instr_inc_rank rank)
{
   switch (rank) {
   case INC_DISTANCE:
      return "distance";
   case INC_DISTANCE_READY:
      return "distance+ready";
   default:
      return NULL;
   }
}

static struct ir3_sched_node *
choose_instr_inc(struct ir3_sched_ctx *ctx, struct ir3_sched_notes *notes,
                 bool defer, bool avoid_output)
{
   const char *mode = defer ? "-d" : "";
   struct ir3_sched_node *chosen = NULL;
   enum choose_instr_inc_rank chosen_rank = INC_DISTANCE;

   unsigned chosen_distance = 0;

   foreach_sched_node (n, &ctx->dag->heads) {
      if (avoid_output && n->output)
         continue;

      if (defer && should_defer(ctx, n->instr))
         continue;

      if (!check_instr(ctx, notes, n->instr))
         continue;

      unsigned d = node_delay(ctx, n);

      enum choose_instr_inc_rank rank;
      if (d == 0)
         rank = INC_DISTANCE_READY;
      else
         rank = INC_DISTANCE;

      unsigned distance = nearest_use(n->instr);

      if (!chosen || rank > chosen_rank ||
          (rank == chosen_rank && distance < chosen_distance)) {
         chosen = n;
         chosen_distance = distance;
         chosen_rank = rank;
      }
   }

   if (chosen) {
      di(chosen->instr, "inc%s: chose (%s)", mode, inc_rank_name(chosen_rank));
      return chosen;
   }

   return NULL;
}

static struct ir3_sched_node *
choose_instr_prio(struct ir3_sched_ctx *ctx, struct ir3_sched_notes *notes)
{
   struct ir3_sched_node *chosen = NULL;

   foreach_sched_node (n, &ctx->dag->heads) {
      if (!is_meta(n->instr) || n->instr->opc == OPC_META_COLLECT)
         continue;

      if (!chosen || (chosen->max_delay < n->max_delay))
         chosen = n;
   }

   if (chosen) {
      di(chosen->instr, "prio: chose (meta)");
      return chosen;
   }

   return NULL;
}

static void
dump_state(struct ir3_sched_ctx *ctx)
{
   if (!SCHED_DEBUG)
      return;

   foreach_sched_node (n, &ctx->dag->heads) {
      di(n->instr, "maxdel=%3d le=%d del=%u ", n->max_delay,
         live_effect(n->instr), node_delay(ctx, n));

      util_dynarray_foreach (&n->dag.edges, struct dag_edge, edge) {
         struct ir3_sched_node *child = (struct ir3_sched_node *)edge->child;

         di(child->instr, " -> (%d parents) ", child->dag.parent_count);
      }
   }
}

static struct ir3_instruction *
choose_instr(struct ir3_sched_ctx *ctx, struct ir3_sched_notes *notes)
{
    struct ir3_sched_node *chosen;

    dump_state(ctx);

    chosen = choose_instr_prio(ctx, notes);
    if (chosen)
        return chosen->instr;

    chosen = choose_instr_tex_prio(ctx, notes);
    if (chosen)
        return chosen->instr;

    chosen = choose_instr_dec(ctx, notes, true);
    if (chosen)
        return chosen->instr;

    chosen = choose_instr_dec(ctx, notes, false);
    if (chosen)
        return chosen->instr;

    chosen = choose_instr_inc(ctx, notes, false, false);
    if (chosen)
        return chosen->instr;

    return NULL;
}

static struct ir3_instruction *
split_instr(struct ir3_sched_ctx *ctx, struct ir3_instruction *orig_instr)
{
   struct ir3_instruction *new_instr = ir3_instr_clone(orig_instr);
   di(new_instr, "split instruction");
   sched_node_init(ctx, new_instr);
   return new_instr;
}

static struct ir3_instruction *
split_addr(struct ir3_sched_ctx *ctx, struct ir3_instruction **addr,
           struct ir3_instruction **users, unsigned users_count)
{
   struct ir3_instruction *new_addr = NULL;
   unsigned i;

   assert(*addr);

   for (i = 0; i < users_count; i++) {
      struct ir3_instruction *indirect = users[i];

      if (!indirect)
         continue;

      if (is_scheduled(indirect))
         continue;

      if (indirect->address->def == (*addr)->dsts[0]) {
         if (!new_addr) {
            new_addr = split_instr(ctx, *addr);
            new_addr->flags &= ~IR3_INSTR_MARK;
            new_addr->uses = _mesa_pointer_set_create(ctx);
         }
         indirect->address->def = new_addr->dsts[0];
         _mesa_set_add(new_addr->uses, indirect);
         sched_node_add_dep(ctx, indirect, new_addr, 0);
         di(indirect, "new address");
      }
   }

   *addr = NULL;

   return new_addr;
}

static void
sched_node_init(struct ir3_sched_ctx *ctx, struct ir3_instruction *instr)
{
   struct ir3_sched_node *n = rzalloc(ctx->dag, struct ir3_sched_node);

   dag_init_node(ctx->dag, &n->dag);

   n->instr = instr;
   instr->data = n;
}

static void
sched_node_add_dep(struct ir3_sched_ctx *ctx,
                   struct ir3_instruction *instr, struct ir3_instruction *src,
                   int i)
{
   if (src->block != instr->block)
      return;

   if (src->flags & IR3_INSTR_UNUSED) {
      assert(__is_false_dep(instr, i));
      return;
   }

   struct ir3_sched_node *n = instr->data;
   struct ir3_sched_node *sn = src->data;

   if (instr->opc == OPC_META_COLLECT)
      sn->collect = instr;

   unsigned d_soft = ir3_delayslots(ctx->compiler, src, instr, i, true);
   unsigned d = ir3_delayslots(ctx->compiler, src, instr, i, false);

   dag_add_edge_max_data(&sn->dag, &n->dag, (uintptr_t)d);

   n->delay = MAX2(n->delay, d_soft);
}

static void
mark_kill_path(struct ir3_instruction *instr)
{
   struct ir3_sched_node *n = instr->data;

   if (n->kill_path) {
      return;
   }

   n->kill_path = true;

   foreach_ssa_src (src, instr) {
      if (src->block != instr->block)
         continue;
      mark_kill_path(src);
   }
}

static bool
is_output_collect(struct ir3_instruction *instr)
{
   if (instr->opc != OPC_META_COLLECT)
      return false;

   foreach_ssa_use (use, instr) {
      if (use->opc != OPC_END && use->opc != OPC_CHMASK)
         return false;
   }

   return true;
}

static bool
is_output_only(struct ir3_instruction *instr)
{
   foreach_ssa_use (use, instr)
      if (!is_output_collect(use))
         return false;

   return true;
}

static void
sched_node_add_deps(struct ir3_sched_ctx *ctx, struct ir3_instruction *instr)
{
   if (instr->opc == OPC_META_PHI)
      return;

   foreach_ssa_src_n (src, i, instr) {
      sched_node_add_dep(ctx, instr, src, i);
   }

   if (is_kill_or_demote(instr) || is_input(instr)) {
      mark_kill_path(instr);
   }

   if (is_output_only(instr)) {
      struct ir3_sched_node *n = instr->data;
      n->output = true;
   }
}

static void
sched_dag_max_delay_cb(struct dag_node *node, void *state)
{
   struct ir3_sched_node *n = (struct ir3_sched_node *)node;
   uint32_t max_delay = 0;

   util_dynarray_foreach (&n->dag.edges, struct dag_edge, edge) {
      struct ir3_sched_node *child = (struct ir3_sched_node *)edge->child;
      max_delay = MAX2(child->max_delay, max_delay);
   }

   n->max_delay = MAX2(n->max_delay, max_delay + n->delay);
}

#ifndef NDEBUG
static void
sched_dag_validate_cb(const struct dag_node *node, void *data)
{
   struct ir3_sched_node *n = (struct ir3_sched_node *)node;

   ir3_print_instr(n->instr);
}
#endif

static void
sched_dag_init(struct ir3_sched_ctx *ctx)
{
   ctx->dag = dag_create(ctx);

   foreach_instr (instr, &ctx->unscheduled_list)
      sched_node_init(ctx, instr);

#ifndef NDEBUG
   dag_validate(ctx->dag, sched_dag_validate_cb, NULL);
#endif

   foreach_instr (instr, &ctx->unscheduled_list)
      sched_node_add_deps(ctx, instr);

   dag_traverse_bottom_up(ctx->dag, sched_dag_max_delay_cb, NULL);
}

static void
sched_dag_destroy(struct ir3_sched_ctx *ctx)
{
   ralloc_free(ctx->dag);
   ctx->dag = NULL;
}

static void
sched_block(struct ir3_sched_ctx *ctx, struct ir3_block *block)
{
   ctx->block = block;

   ctx->addr0 = NULL;
   ctx->addr1 = NULL;
   ctx->sy_delay = 0;
   ctx->ss_delay = 0;
   ctx->sy_index = ctx->first_outstanding_sy_index = 0;
   ctx->ss_index = ctx->first_outstanding_ss_index = 0;

   struct ir3_instruction *terminator = ir3_block_take_terminator(block);

   list_replace(&block->instr_list, &ctx->unscheduled_list);
   list_inithead(&block->instr_list);

   sched_dag_init(ctx);

   ctx->remaining_kills = 0;
   ctx->remaining_tex = 0;
   foreach_instr_safe (instr, &ctx->unscheduled_list) {
      if (is_kill_or_demote(instr))
         ctx->remaining_kills++;
      if (is_sy_producer(instr))
         ctx->remaining_tex++;
   }

   foreach_instr_safe (instr, &ctx->unscheduled_list)
      if (instr->opc == OPC_META_INPUT || instr->opc == OPC_META_PHI)
         schedule(ctx, instr);

   foreach_instr_safe (instr, &ctx->unscheduled_list)
      if (instr->opc == OPC_META_TEX_PREFETCH)
         schedule(ctx, instr);

   foreach_instr_safe (instr, &ctx->unscheduled_list)
      if (instr->opc == OPC_PUSH_CONSTS_LOAD_MACRO)
         schedule(ctx, instr);

   while (!list_is_empty(&ctx->unscheduled_list)) {
      struct ir3_sched_notes notes = {0};
      struct ir3_instruction *instr;

      instr = choose_instr(ctx, &notes);
      if (instr) {
         unsigned delay = node_delay(ctx, instr->data);
         d("delay=%u", delay);

         assert(delay <= 6);

         schedule(ctx, instr);

         ctx->split = NULL;
      } else {
         struct ir3_instruction *new_instr = NULL;
         struct ir3 *ir = block->shader;

         if (notes.addr0_conflict) {
            new_instr =
               split_addr(ctx, &ctx->addr0, ir->a0_users, ir->a0_users_count);
         } else if (notes.addr1_conflict) {
            new_instr =
               split_addr(ctx, &ctx->addr1, ir->a1_users, ir->a1_users_count);
         } else {
            d("unscheduled_list:");
            foreach_instr (instr, &ctx->unscheduled_list)
               di(instr, "unscheduled: ");
            assert(0);
            ctx->error = true;
            return;
         }

         if (new_instr) {
            list_delinit(&new_instr->node);
            list_addtail(&new_instr->node, &ctx->unscheduled_list);
         }

         ctx->split = new_instr;
      }
   }

   sched_dag_destroy(ctx);

   if (terminator)
      list_addtail(&terminator->node, &block->instr_list);
}

int
ir3_sched(struct ir3 *ir)
{
   struct ir3_sched_ctx *ctx = rzalloc(NULL, struct ir3_sched_ctx);

   ctx->compiler = ir->compiler;

   foreach_block (block, &ir->block_list) {
      foreach_instr (instr, &block->instr_list) {
         instr->data = NULL;
      }
   }

   ir3_count_instructions_sched(ir);
   ir3_clear_mark(ir);
   ir3_find_ssa_uses(ir, ctx, false);

   foreach_block (block, &ir->block_list) {
      sched_block(ctx, block);
   }

   int ret = ctx->error ? -1 : 0;

   ralloc_free(ctx);

   return ret;
}

static unsigned
get_array_id(struct ir3_instruction *instr)
{
   foreach_dst (dst, instr)
      if (dst->flags & IR3_REG_ARRAY)
         return dst->array.id;
   foreach_src (src, instr)
      if (src->flags & IR3_REG_ARRAY)
         return src->array.id;

   UNREACHABLE("this was unexpected");
}

static bool
depends_on(struct ir3_instruction *instr, struct ir3_instruction *prior)
{
   if (((instr->barrier_class & IR3_BARRIER_EVERYTHING) &&
        prior->barrier_class) ||
       ((prior->barrier_class & IR3_BARRIER_EVERYTHING) &&
        instr->barrier_class))
      return true;

   if (instr->barrier_class & prior->barrier_conflict) {
      if (!(instr->barrier_class &
            ~(IR3_BARRIER_ARRAY_R | IR3_BARRIER_ARRAY_W))) {
         if (get_array_id(instr) != get_array_id(prior)) {
            return false;
         }
      }

      return true;
   }

   return false;
}

static void
add_barrier_deps(struct ir3_block *block, struct ir3_instruction *instr)
{
   struct list_head *prev = instr->node.prev;
   struct list_head *next = instr->node.next;

   while (prev != &block->instr_list) {
      struct ir3_instruction *pi =
         list_entry(prev, struct ir3_instruction, node);

      prev = prev->prev;

      if (is_meta(pi))
         continue;

      if (instr->barrier_class == pi->barrier_class) {
         ir3_instr_add_dep(instr, pi);
         break;
      }

      if (depends_on(instr, pi))
         ir3_instr_add_dep(instr, pi);
   }

   while (next != &block->instr_list) {
      struct ir3_instruction *ni =
         list_entry(next, struct ir3_instruction, node);

      next = next->next;

      if (is_meta(ni))
         continue;

      if (instr->barrier_class == ni->barrier_class) {
         ir3_instr_add_dep(ni, instr);
         break;
      }

      if (depends_on(ni, instr))
         ir3_instr_add_dep(ni, instr);
   }
}

static bool
add_const_deps(struct ir3_block *block, struct ir3_instruction *stc)
{
   bool progress = false;
   unsigned const_start = stc->cat6.dst_offset;
   unsigned const_end = const_start + stc->cat6.iim_val;

   foreach_instr_from (instr, stc, &block->instr_list) {
      foreach_src (src, instr) {
         if (!(src->flags & IR3_REG_CONST)) {
            continue;
         }

         if (src->num >= const_start && src->num < const_end) {
            ir3_instr_add_dep(instr, stc);
            progress = true;
         }
      }
   }

   return progress;
}

bool
ir3_sched_add_deps(struct ir3 *ir)
{
   bool progress = false;

   foreach_block (block, &ir->block_list) {
      foreach_instr (instr, &block->instr_list) {
         if (instr->barrier_class) {
            add_barrier_deps(block, instr);
            progress = true;
         }

         if (instr->opc == OPC_STC) {
            progress |= add_const_deps(block, instr);
         }
      }
   }

   return progress;
}