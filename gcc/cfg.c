/* Control flow graph manipulation code for GNU compiler.
   Copyright (C) 1987, 1988, 1992, 1993, 1994, 1995, 1996, 1997, 1998,
   1999, 2000, 2001 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to the Free
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

/* This file contains low level functions to manipulate with CFG and analyze it.
   All other modules should not transform the datastructure directly and use
   abstraction instead.  The file is supposed to be ordered bottom-up.

   Available functionality:
     - Initialization/deallocation
	 init_flow, clear_edges
     - CFG aware instruction chain manipulation
	 delete_insn, delete_insn_chain
     - Basic block manipulation
	 create_basic_block, flow_delete_block, split_block, merge_blocks_nomove
     - Infrastructure to determine quickly basic block for instruction.
	 compute_bb_for_insn, update_bb_for_insn, set_block_for_insn,
	 set_block_for_new_insns
     - Edge manipulation
	 make_edge, make_single_succ_edge, cached_make_edge, remove_edge
	 - Low level edge redirection (without updating instruction chain)
	     redirect_edge_succ, redirect_edge_succ_nodup, redirect_edge_pred
	 - High level edge redirection (with updating and optimizing instruction
	   chain)
	     block_label, redirect_edge_and_branch,
	     redirect_edge_and_branch_force, tidy_fallthru_edge, force_nonfallthru
      - Edge splitting and commiting to edges
	  split_edge, insert_insn_on_edge, commit_edge_insertions
      - Dumpipng and debugging
	  dump_flow_info, debug_flow_info, dump_edge_info, dump_bb, debug_bb,
	  debug_bb_n, print_rtl_with_bb
      - Consistency checking
	  verify_flow_info
      - CFG updating after constant propagation
	  purge_dead_edges, purge_all_dead_edges
 */

#include "config.h"
#include "system.h"
#include "tree.h"
#include "rtl.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "regs.h"
#include "flags.h"
#include "output.h"
#include "function.h"
#include "except.h"
#include "toplev.h"
#include "tm_p.h"
#include "obstack.h"


/* Stubs in case we haven't got a return insn.  */
#ifndef HAVE_return
#define HAVE_return 0
#define gen_return() NULL_RTX
#endif

/* The obstack on which the flow graph components are allocated.  */

struct obstack flow_obstack;
static char *flow_firstobj;

/* Number of basic blocks in the current function.  */

int n_basic_blocks;

/* Number of edges in the current function.  */

int n_edges;

/* First edge in the deleted edges chain.  */

edge first_deleted_edge;

/* The basic block array.  */

varray_type basic_block_info;

/* The special entry and exit blocks.  */

struct basic_block_def entry_exit_blocks[2]
= {{NULL,			/* head */
    NULL,			/* end */
    NULL,			/* head_tree */
    NULL,			/* end_tree */
    NULL,			/* pred */
    NULL,			/* succ */
    NULL,			/* local_set */
    NULL,			/* cond_local_set */
    NULL,			/* global_live_at_start */
    NULL,			/* global_live_at_end */
    NULL,			/* aux */
    ENTRY_BLOCK,		/* index */
    0,				/* loop_depth */
    0,				/* count */
    0,				/* frequency */
    0				/* flags */
  },
  {
    NULL,			/* head */
    NULL,			/* end */
    NULL,			/* head_tree */
    NULL,			/* end_tree */
    NULL,			/* pred */
    NULL,			/* succ */
    NULL,			/* local_set */
    NULL,			/* cond_local_set */
    NULL,			/* global_live_at_start */
    NULL,			/* global_live_at_end */
    NULL,			/* aux */
    EXIT_BLOCK,			/* index */
    0,				/* loop_depth */
    0,				/* count */
    0,				/* frequency */
    0				/* flags */
  }
};

/* The basic block structure for every insn, indexed by uid.  */

varray_type basic_block_for_insn;

/* The labels mentioned in non-jump rtl.  Valid during find_basic_blocks.  */
/* ??? Should probably be using LABEL_NUSES instead.  It would take a
   bit of surgery to be able to use or co-opt the routines in jump.  */

rtx label_value_list;
rtx tail_recursion_label_list;

void debug_flow_info			PARAMS ((void));
static int can_delete_note_p		PARAMS ((rtx));
static int can_delete_label_p		PARAMS ((rtx));
static void commit_one_edge_insertion	PARAMS ((edge));
static bool try_redirect_by_replacing_jump PARAMS ((edge, basic_block));
static rtx last_loop_beg_note		PARAMS ((rtx));
static bool back_edge_of_syntactic_loop_p PARAMS ((basic_block, basic_block));
static basic_block force_nonfallthru_and_redirect PARAMS ((edge, basic_block));

/* Called once at intialization time.  */

void
init_flow ()
{
  static int initialized;

  first_deleted_edge = 0;
  n_edges = 0;

  if (!initialized)
    {
      gcc_obstack_init (&flow_obstack);
      flow_firstobj = (char *) obstack_alloc (&flow_obstack, 0);
      initialized = 1;
    }
  else
    {
      obstack_free (&flow_obstack, flow_firstobj);
      flow_firstobj = (char *) obstack_alloc (&flow_obstack, 0);
    }
}

/* Free the memory associated with the edge structures.  */

void
clear_edges ()
{
  int i;

  for (i = 0; i < n_basic_blocks; ++i)
    {
      basic_block bb = BASIC_BLOCK (i);

      while (bb->succ)
	remove_edge (bb->succ);
    }

  while (ENTRY_BLOCK_PTR->succ)
    remove_edge (ENTRY_BLOCK_PTR->succ);

  if (n_edges)
    abort ();
}

/* Return true if NOTE is not one of the ones that must be kept paired,
   so that we may simply delete them.  */

static int
can_delete_note_p (note)
     rtx note;
{
  return (NOTE_LINE_NUMBER (note) == NOTE_INSN_DELETED
	  || NOTE_LINE_NUMBER (note) == NOTE_INSN_BASIC_BLOCK);
}

/* True if a given label can be deleted.  */

static int
can_delete_label_p (label)
     rtx label;
{
  rtx x;

  if (LABEL_PRESERVE_P (label))
    return 0;

  for (x = forced_labels; x; x = XEXP (x, 1))
    if (label == XEXP (x, 0))
      return 0;
  for (x = label_value_list; x; x = XEXP (x, 1))
    if (label == XEXP (x, 0))
      return 0;
  for (x = exception_handler_labels; x; x = XEXP (x, 1))
    if (label == XEXP (x, 0))
      return 0;

  /* User declared labels must be preserved.  */
  if (LABEL_NAME (label) != 0)
    return 0;

  return 1;
}

/* Delete INSN by patching it out.  Return the next insn.  */

rtx
delete_insn (insn)
     rtx insn;
{
  rtx next = NEXT_INSN (insn);
  rtx note;
  bool really_delete = true;

  if (GET_CODE (insn) == CODE_LABEL)
    {
      /* Some labels can't be directly removed from the INSN chain, as they
         might be references via variables, constant pool etc. 
         Convert them to the special NOTE_INSN_DELETED_LABEL note.  */
      if (! can_delete_label_p (insn))
	{
	  const char *name = LABEL_NAME (insn);

	  really_delete = false;
	  PUT_CODE (insn, NOTE);
	  NOTE_LINE_NUMBER (insn) = NOTE_INSN_DELETED_LABEL;
	  NOTE_SOURCE_FILE (insn) = name;
	}
      remove_node_from_expr_list (insn, &nonlocal_goto_handler_labels);
    }

  if (really_delete)
    {
      remove_insn (insn);
      INSN_DELETED_P (insn) = 1;
    }

  /* If deleting a jump, decrement the use count of the label.  Deleting
     the label itself should happen in the normal course of block merging.  */
  if (GET_CODE (insn) == JUMP_INSN
      && JUMP_LABEL (insn)
      && GET_CODE (JUMP_LABEL (insn)) == CODE_LABEL)
    LABEL_NUSES (JUMP_LABEL (insn))--;

  /* Also if deleting an insn that references a label.  */
  else if ((note = find_reg_note (insn, REG_LABEL, NULL_RTX)) != NULL_RTX
	   && GET_CODE (XEXP (note, 0)) == CODE_LABEL)
    LABEL_NUSES (XEXP (note, 0))--;

  if (GET_CODE (insn) == JUMP_INSN
      && (GET_CODE (PATTERN (insn)) == ADDR_VEC
	  || GET_CODE (PATTERN (insn)) == ADDR_DIFF_VEC))
    {
      rtx pat = PATTERN (insn);
      int diff_vec_p = GET_CODE (PATTERN (insn)) == ADDR_DIFF_VEC;
      int len = XVECLEN (pat, diff_vec_p);
      int i;

      for (i = 0; i < len; i++)
	LABEL_NUSES (XEXP (XVECEXP (pat, diff_vec_p, i), 0))--;
    }

  return next;
}

/* Unlink a chain of insns between START and FINISH, leaving notes
   that must be paired.  */

void
delete_insn_chain (start, finish)
     rtx start, finish;
{
  /* Unchain the insns one by one.  It would be quicker to delete all
     of these with a single unchaining, rather than one at a time, but
     we need to keep the NOTE's.  */

  rtx next;

  while (1)
    {
      next = NEXT_INSN (start);
      if (GET_CODE (start) == NOTE && !can_delete_note_p (start))
	;
      else
	next = delete_insn (start);

      if (start == finish)
	break;
      start = next;
    }
}

/* Create a new basic block consisting of the instructions between
   HEAD and END inclusive.  This function is designed to allow fast
   BB construction - reuses the note and basic block struct
   in BB_NOTE, if any and do not grow BASIC_BLOCK chain and should
   be used directly only by CFG construction code.
   END can be NULL in to create new empty basic block before HEAD.
   Both END and HEAD can be NULL to create basic block at the end of
   INSN chain.  */

basic_block
create_basic_block_structure (index, head, end, bb_note)
     int index;
     rtx head, end, bb_note;
{
  basic_block bb;

  if (bb_note
      && ! RTX_INTEGRATED_P (bb_note)
      && (bb = NOTE_BASIC_BLOCK (bb_note)) != NULL
      && bb->aux == NULL)
    {
      /* If we found an existing note, thread it back onto the chain.  */

      rtx after;

      if (GET_CODE (head) == CODE_LABEL)
	after = head;
      else
	{
	  after = PREV_INSN (head);
	  head = bb_note;
	}

      if (after != bb_note && NEXT_INSN (after) != bb_note)
	reorder_insns (bb_note, bb_note, after);
    }
  else
    {
      /* Otherwise we must create a note and a basic block structure.
	 Since we allow basic block structs in rtl, give the struct
	 the same lifetime by allocating it off the function obstack
	 rather than using malloc.  */

      bb = (basic_block) obstack_alloc (&flow_obstack, sizeof (*bb));
      memset (bb, 0, sizeof (*bb));

      if (!head && !end)
	{
	  head = end = bb_note = emit_note_after (NOTE_INSN_BASIC_BLOCK,
						  get_last_insn ());
	}
      else if (GET_CODE (head) == CODE_LABEL && end)
	{
	  bb_note = emit_note_after (NOTE_INSN_BASIC_BLOCK, head);
	  if (head == end)
	    end = bb_note;
	}
      else
	{
	  bb_note = emit_note_before (NOTE_INSN_BASIC_BLOCK, head);
	  head = bb_note;
	  if (!end)
	    end = head;
	}
      NOTE_BASIC_BLOCK (bb_note) = bb;
    }

  /* Always include the bb note in the block.  */
  if (NEXT_INSN (end) == bb_note)
    end = bb_note;

  bb->head = head;
  bb->end = end;
  bb->index = index;
  BASIC_BLOCK (index) = bb;
  if (basic_block_for_insn)
    update_bb_for_insn (bb);

  /* Tag the block so that we know it has been used when considering
     other basic block notes.  */
  bb->aux = bb;

  return bb;
}

/* Create new basic block consisting of instructions in between HEAD and
   END and place it to the BB chain at possition INDEX.
   END can be NULL in to create new empty basic block before HEAD.
   Both END and HEAD can be NULL to create basic block at the end of
   INSN chain.  */

basic_block
create_basic_block (index, head, end)
     int index;
     rtx head, end;
{
  basic_block bb;
  int i;

  /* Place the new block just after the block being split.  */
  VARRAY_GROW (basic_block_info, ++n_basic_blocks);

  /* Some parts of the compiler expect blocks to be number in
     sequential order so insert the new block immediately after the
     block being split..  */
  for (i = n_basic_blocks - 1; i > index; --i)
    {
      basic_block tmp = BASIC_BLOCK (i - 1);
      BASIC_BLOCK (i) = tmp;
      tmp->index = i;
    }

  bb = create_basic_block_structure (index, head, end, NULL);
  bb->aux = NULL;
  return bb;
}

/* Remove block B from the basic block array and compact behind it.  */

void
expunge_block (b)
     basic_block b;
{
  int i, n = n_basic_blocks;

  for (i = b->index; i + 1 < n; ++i)
    {
      basic_block x = BASIC_BLOCK (i + 1);
      BASIC_BLOCK (i) = x;
      x->index = i;
    }

  /* Invalidate data to make bughunting easier.  */
  memset (b, 0, sizeof (*b));
  b->index = -3;
  basic_block_info->num_elements--;
  n_basic_blocks--;
}

/* Delete the insns in a (non-live) block.  We physically delete every
   non-deleted-note insn, and update the flow graph appropriately.

   Return nonzero if we deleted an exception handler.  */

/* ??? Preserving all such notes strikes me as wrong.  It would be nice
   to post-process the stream to remove empty blocks, loops, ranges, etc.  */

int
flow_delete_block (b)
     basic_block b;
{
  int deleted_handler = 0;
  rtx insn, end, tmp;

  /* If the head of this block is a CODE_LABEL, then it might be the
     label for an exception handler which can't be reached.

     We need to remove the label from the exception_handler_label list
     and remove the associated NOTE_INSN_EH_REGION_BEG and
     NOTE_INSN_EH_REGION_END notes.  */

  insn = b->head;

  never_reached_warning (insn);

  if (GET_CODE (insn) == CODE_LABEL)
    maybe_remove_eh_handler (insn);

  /* Include any jump table following the basic block.  */
  end = b->end;
  if (GET_CODE (end) == JUMP_INSN
      && (tmp = JUMP_LABEL (end)) != NULL_RTX
      && (tmp = NEXT_INSN (tmp)) != NULL_RTX
      && GET_CODE (tmp) == JUMP_INSN
      && (GET_CODE (PATTERN (tmp)) == ADDR_VEC
	  || GET_CODE (PATTERN (tmp)) == ADDR_DIFF_VEC))
    end = tmp;

  /* Include any barrier that may follow the basic block.  */
  tmp = next_nonnote_insn (end);
  if (tmp && GET_CODE (tmp) == BARRIER)
    end = tmp;

  /* Selectively delete the entire chain.  */
  b->head = NULL;
  delete_insn_chain (insn, end);

  /* Remove the edges into and out of this block.  Note that there may
     indeed be edges in, if we are removing an unreachable loop.  */
  while (b->pred != NULL)
    remove_edge (b->pred);
  while (b->succ != NULL)
    remove_edge (b->succ);

  b->pred = NULL;
  b->succ = NULL;

  /* Remove the basic block from the array, and compact behind it.  */
  expunge_block (b);

  return deleted_handler;
}

/* Records the basic block struct in BB_FOR_INSN, for every instruction
   indexed by INSN_UID.  MAX is the size of the array.  */

void
compute_bb_for_insn (max)
     int max;
{
  int i;

  if (basic_block_for_insn)
    VARRAY_FREE (basic_block_for_insn);
  VARRAY_BB_INIT (basic_block_for_insn, max, "basic_block_for_insn");

  for (i = 0; i < n_basic_blocks; ++i)
    {
      basic_block bb = BASIC_BLOCK (i);
      rtx insn, end;

      end = bb->end;
      insn = bb->head;
      while (1)
	{
	  int uid = INSN_UID (insn);
	  if (uid < max)
	    VARRAY_BB (basic_block_for_insn, uid) = bb;
	  if (insn == end)
	    break;
	  insn = NEXT_INSN (insn);
	}
    }
}

/* Release the basic_block_for_insn array.  */

void
free_bb_for_insn ()
{
  if (basic_block_for_insn)
    VARRAY_FREE (basic_block_for_insn);
  basic_block_for_insn = 0;
}

/* Update insns block within BB.  */

void
update_bb_for_insn (bb)
     basic_block bb;
{
  rtx insn;

  if (! basic_block_for_insn)
    return;

  for (insn = bb->head; ; insn = NEXT_INSN (insn))
    {
      set_block_for_insn (insn, bb);

      if (insn == bb->end)
	break;
    }
}

/* Record INSN's block as BB.  */

void
set_block_for_insn (insn, bb)
     rtx insn;
     basic_block bb;
{
  size_t uid = INSN_UID (insn);
  if (uid >= basic_block_for_insn->num_elements)
    {
      int new_size;

      /* Add one-eighth the size so we don't keep calling xrealloc.  */
      new_size = uid + (uid + 7) / 8;

      VARRAY_GROW (basic_block_for_insn, new_size);
    }
  VARRAY_BB (basic_block_for_insn, uid) = bb;
}

/* When a new insn has been inserted into an existing block, it will
   sometimes emit more than a single insn. This routine will set the
   block number for the specified insn, and look backwards in the insn
   chain to see if there are any other uninitialized insns immediately
   previous to this one, and set the block number for them too.  */

void
set_block_for_new_insns (insn, bb)
     rtx insn;
     basic_block bb;
{
  set_block_for_insn (insn, bb);

  /* Scan the previous instructions setting the block number until we find
     an instruction that has the block number set, or we find a note
     of any kind.  */
  for (insn = PREV_INSN (insn); insn != NULL_RTX; insn = PREV_INSN (insn))
    {
      if (GET_CODE (insn) == NOTE)
	break;
      if ((unsigned) INSN_UID (insn) >= basic_block_for_insn->num_elements
	  || BLOCK_FOR_INSN (insn) == 0)
	set_block_for_insn (insn, bb);
      else
	break;
    }
}

/* Create an edge connecting SRC and DST with FLAGS optionally using
   edge cache CACHE.  Return the new edge, NULL if already exist. */

edge
cached_make_edge (edge_cache, src, dst, flags)
     sbitmap *edge_cache;
     basic_block src, dst;
     int flags;
{
  int use_edge_cache;
  edge e;

  /* Don't bother with edge cache for ENTRY or EXIT; there aren't that
     many edges to them, and we didn't allocate memory for it.  */
  use_edge_cache = (edge_cache
		    && src != ENTRY_BLOCK_PTR
		    && dst != EXIT_BLOCK_PTR);

  /* Make sure we don't add duplicate edges.  */
  switch (use_edge_cache)
    {
    default:
      /* Quick test for non-existance of the edge.  */
      if (! TEST_BIT (edge_cache[src->index], dst->index))
	break;

      /* The edge exists; early exit if no work to do.  */
      if (flags == 0)
	return NULL;

      /* FALLTHRU */
    case 0:
      for (e = src->succ; e; e = e->succ_next)
	if (e->dest == dst)
	  {
	    e->flags |= flags;
	    return NULL;
	  }
      break;
    }

  if (first_deleted_edge)
    {
      e = first_deleted_edge;
      first_deleted_edge = e->succ_next;
    }
  else
    {
      e = (edge) obstack_alloc (&flow_obstack, sizeof (*e));
      memset (e, 0, sizeof (*e));
    }
  n_edges++;

  e->succ_next = src->succ;
  e->pred_next = dst->pred;
  e->src = src;
  e->dest = dst;
  e->flags = flags;

  src->succ = e;
  dst->pred = e;

  if (use_edge_cache)
    SET_BIT (edge_cache[src->index], dst->index);

  return e;
}

/* Create an edge connecting SRC and DEST with flags FLAGS.  Return newly
   created edge or NULL if already exist.  */

edge
make_edge (src, dest, flags)
     basic_block src, dest;
     int flags;
{
  return cached_make_edge (NULL, src, dest, flags);
}

/* Create an edge connecting SRC to DEST and set probability by knowling
   that it is the single edge leaving SRC.  */

edge
make_single_succ_edge (src, dest, flags)
     basic_block src, dest;
     int flags;
{
  edge e = make_edge (src, dest, flags);

  e->probability = REG_BR_PROB_BASE;
  e->count = src->count;
  return e;
}

/* This function will remove an edge from the flow graph.  */

void
remove_edge (e)
     edge e;
{
  edge last_pred = NULL;
  edge last_succ = NULL;
  edge tmp;
  basic_block src, dest;
  src = e->src;
  dest = e->dest;
  for (tmp = src->succ; tmp && tmp != e; tmp = tmp->succ_next)
    last_succ = tmp;

  if (!tmp)
    abort ();
  if (last_succ)
    last_succ->succ_next = e->succ_next;
  else
    src->succ = e->succ_next;

  for (tmp = dest->pred; tmp && tmp != e; tmp = tmp->pred_next)
    last_pred = tmp;

  if (!tmp)
    abort ();
  if (last_pred)
    last_pred->pred_next = e->pred_next;
  else
    dest->pred = e->pred_next;

  n_edges--;
  memset (e, 0, sizeof (*e));
  e->succ_next = first_deleted_edge;
  first_deleted_edge = e;
}

/* Redirect an edge's successor from one block to another.  */

void
redirect_edge_succ (e, new_succ)
     edge e;
     basic_block new_succ;
{
  edge *pe;

  /* Disconnect the edge from the old successor block.  */
  for (pe = &e->dest->pred; *pe != e; pe = &(*pe)->pred_next)
    continue;
  *pe = (*pe)->pred_next;

  /* Reconnect the edge to the new successor block.  */
  e->pred_next = new_succ->pred;
  new_succ->pred = e;
  e->dest = new_succ;
}

/* Like previous but avoid possible dupplicate edge.  */

edge
redirect_edge_succ_nodup (e, new_succ)
     edge e;
     basic_block new_succ;
{
  edge s;
  /* Check whether the edge is already present.  */
  for (s = e->src->succ; s; s = s->succ_next)
    if (s->dest == new_succ && s != e)
      break;
  if (s)
    {
      s->flags |= e->flags;
      s->probability += e->probability;
      s->count += e->count;
      remove_edge (e);
      e = s;
    }
  else
    redirect_edge_succ (e, new_succ);
  return e;
}

/* Redirect an edge's predecessor from one block to another.  */

void
redirect_edge_pred (e, new_pred)
     edge e;
     basic_block new_pred;
{
  edge *pe;

  /* Disconnect the edge from the old predecessor block.  */
  for (pe = &e->src->succ; *pe != e; pe = &(*pe)->succ_next)
    continue;
  *pe = (*pe)->succ_next;

  /* Reconnect the edge to the new predecessor block.  */
  e->succ_next = new_pred->succ;
  new_pred->succ = e;
  e->src = new_pred;
}

/* Split a block BB after insn INSN creating a new fallthru edge.
   Return the new edge.  Note that to keep other parts of the compiler happy,
   this function renumbers all the basic blocks so that the new
   one has a number one greater than the block split.  */

edge
split_block (bb, insn)
     basic_block bb;
     rtx insn;
{
  basic_block new_bb;
  edge new_edge;
  edge e;

  /* There is no point splitting the block after its end.  */
  if (bb->end == insn)
    return 0;

  /* Create the new basic block.  */
  new_bb = create_basic_block (bb->index + 1, NEXT_INSN (insn), bb->end);
  new_bb->count = bb->count;
  new_bb->frequency = bb->frequency;
  new_bb->loop_depth = bb->loop_depth;
  bb->end = insn;

  /* Redirect the outgoing edges.  */
  new_bb->succ = bb->succ;
  bb->succ = NULL;
  for (e = new_bb->succ; e; e = e->succ_next)
    e->src = new_bb;

  new_edge = make_single_succ_edge (bb, new_bb, EDGE_FALLTHRU);

  if (bb->global_live_at_start)
    {
      new_bb->global_live_at_start = OBSTACK_ALLOC_REG_SET (&flow_obstack);
      new_bb->global_live_at_end = OBSTACK_ALLOC_REG_SET (&flow_obstack);
      COPY_REG_SET (new_bb->global_live_at_end, bb->global_live_at_end);

      /* We now have to calculate which registers are live at the end
	 of the split basic block and at the start of the new basic
	 block.  Start with those registers that are known to be live
	 at the end of the original basic block and get
	 propagate_block to determine which registers are live.  */
      COPY_REG_SET (new_bb->global_live_at_start, bb->global_live_at_end);
      propagate_block (new_bb, new_bb->global_live_at_start, NULL, NULL, 0);
      COPY_REG_SET (bb->global_live_at_end,
		    new_bb->global_live_at_start);
    }

  return new_edge;
}

/* Blocks A and B are to be merged into a single block A.  The insns
   are already contiguous, hence `nomove'.  */

void
merge_blocks_nomove (a, b)
     basic_block a, b;
{
  edge e;
  rtx b_head, b_end, a_end;
  rtx del_first = NULL_RTX, del_last = NULL_RTX;
  int b_empty = 0;

  /* If there was a CODE_LABEL beginning B, delete it.  */
  b_head = b->head;
  b_end = b->end;
  if (GET_CODE (b_head) == CODE_LABEL)
    {
      /* Detect basic blocks with nothing but a label.  This can happen
	 in particular at the end of a function.  */
      if (b_head == b_end)
	b_empty = 1;
      del_first = del_last = b_head;
      b_head = NEXT_INSN (b_head);
    }

  /* Delete the basic block note.  */
  if (NOTE_INSN_BASIC_BLOCK_P (b_head))
    {
      if (b_head == b_end)
	b_empty = 1;
      if (! del_last)
	del_first = b_head;
      del_last = b_head;
      b_head = NEXT_INSN (b_head);
    }

  /* If there was a jump out of A, delete it.  */
  a_end = a->end;
  if (GET_CODE (a_end) == JUMP_INSN)
    {
      rtx prev;

      for (prev = PREV_INSN (a_end); ; prev = PREV_INSN (prev))
	if (GET_CODE (prev) != NOTE
	    || NOTE_LINE_NUMBER (prev) == NOTE_INSN_BASIC_BLOCK
	    || prev == a->head)
	  break;

      del_first = a_end;

#ifdef HAVE_cc0
      /* If this was a conditional jump, we need to also delete
	 the insn that set cc0.  */
      if (only_sets_cc0_p (prev))
	{
	  rtx tmp = prev;
	  prev = prev_nonnote_insn (prev);
	  if (!prev)
	    prev = a->head;
	  del_first = tmp;
	}
#endif

      a_end = PREV_INSN (del_first);
    }
  else if (GET_CODE (NEXT_INSN (a_end)) == BARRIER)
    del_first = NEXT_INSN (a_end);

  /* Normally there should only be one successor of A and that is B, but
     partway though the merge of blocks for conditional_execution we'll
     be merging a TEST block with THEN and ELSE successors.  Free the
     whole lot of them and hope the caller knows what they're doing.  */
  while (a->succ)
    remove_edge (a->succ);

  /* Adjust the edges out of B for the new owner.  */
  for (e = b->succ; e; e = e->succ_next)
    e->src = a;
  a->succ = b->succ;

  /* B hasn't quite yet ceased to exist.  Attempt to prevent mishap.  */
  b->pred = b->succ = NULL;

  expunge_block (b);

  /* Delete everything marked above as well as crap that might be
     hanging out between the two blocks.  */
  delete_insn_chain (del_first, del_last);

  /* Reassociate the insns of B with A.  */
  if (!b_empty)
    {
      rtx x = a_end;
      if (basic_block_for_insn)
	{
	  BLOCK_FOR_INSN (x) = a;
	  while (x != b_end)
	    {
	      x = NEXT_INSN (x);
	      BLOCK_FOR_INSN (x) = a;
	    }
	}
      a_end = b_end;
    }
  a->end = a_end;
}

/* Return label in the head of basic block.  Create one if it doesn't exist.  */

rtx
block_label (block)
     basic_block block;
{
  if (block == EXIT_BLOCK_PTR)
    return NULL_RTX;
  if (GET_CODE (block->head) != CODE_LABEL)
    {
      block->head = emit_label_before (gen_label_rtx (), block->head);
      if (basic_block_for_insn)
	set_block_for_insn (block->head, block);
    }
  return block->head;
}

/* Attempt to perform edge redirection by replacing possibly complex jump
   instruction by unconditional jump or removing jump completely.
   This can apply only if all edges now point to the same block.

   The parameters and return values are equivalent to redirect_edge_and_branch.
 */

static bool
try_redirect_by_replacing_jump (e, target)
     edge e;
     basic_block target;
{
  basic_block src = e->src;
  rtx insn = src->end, kill_from;
  edge tmp;
  rtx set;
  int fallthru = 0;

  /* Verify that all targets will be TARGET.  */
  for (tmp = src->succ; tmp; tmp = tmp->succ_next)
    if (tmp->dest != target && tmp != e)
      break;
  if (tmp || !onlyjump_p (insn))
    return false;

  /* Avoid removing branch with side effects.  */
  set = single_set (insn);
  if (!set || side_effects_p (set))
    return false;

  /* In case we zap a conditional jump, we'll need to kill
     the cc0 setter too.  */
  kill_from = insn;
#ifdef HAVE_cc0
  if (reg_mentioned_p (cc0_rtx, PATTERN (insn)))
    kill_from = PREV_INSN (insn);
#endif

  /* See if we can create the fallthru edge.  */
  if (can_fallthru (src, target))
    {
      if (rtl_dump_file)
	fprintf (rtl_dump_file, "Removing jump %i.\n", INSN_UID (insn));
      fallthru = 1;

      /* Selectivly unlink whole insn chain.  */
      delete_insn_chain (kill_from, PREV_INSN (target->head));
    }
  /* If this already is simplejump, redirect it.  */
  else if (simplejump_p (insn))
    {
      if (e->dest == target)
	return false;
      if (rtl_dump_file)
	fprintf (rtl_dump_file, "Redirecting jump %i from %i to %i.\n",
		 INSN_UID (insn), e->dest->index, target->index);
      redirect_jump (insn, block_label (target), 0);
    }
  /* Or replace possibly complicated jump insn by simple jump insn.  */
  else
    {
      rtx target_label = block_label (target);
      rtx barrier;

      emit_jump_insn_after (gen_jump (target_label), kill_from);
      JUMP_LABEL (src->end) = target_label;
      LABEL_NUSES (target_label)++;
      if (rtl_dump_file)
	fprintf (rtl_dump_file, "Replacing insn %i by jump %i\n",
		 INSN_UID (insn), INSN_UID (src->end));

      delete_insn_chain (kill_from, insn);

      barrier = next_nonnote_insn (src->end);
      if (!barrier || GET_CODE (barrier) != BARRIER)
	emit_barrier_after (src->end);
    }

  /* Keep only one edge out and set proper flags.  */
  while (src->succ->succ_next)
    remove_edge (src->succ);
  e = src->succ;
  if (fallthru)
    e->flags = EDGE_FALLTHRU;
  else
    e->flags = 0;
  e->probability = REG_BR_PROB_BASE;
  e->count = src->count;

  /* We don't want a block to end on a line-number note since that has
     the potential of changing the code between -g and not -g.  */
  while (GET_CODE (e->src->end) == NOTE
	 && NOTE_LINE_NUMBER (e->src->end) >= 0)
    delete_insn (e->src->end);

  if (e->dest != target)
    redirect_edge_succ (e, target);
  return true;
}

/* Return last loop_beg note appearing after INSN, before start of next
   basic block.  Return INSN if there are no such notes.

   When emmiting jump to redirect an fallthru edge, it should always
   appear after the LOOP_BEG notes, as loop optimizer expect loop to
   eighter start by fallthru edge or jump following the LOOP_BEG note
   jumping to the loop exit test.  */

static rtx
last_loop_beg_note (insn)
     rtx insn;
{
  rtx last = insn;
  insn = NEXT_INSN (insn);
  while (insn && GET_CODE (insn) == NOTE
	 && NOTE_LINE_NUMBER (insn) != NOTE_INSN_BASIC_BLOCK)
    {
      if (NOTE_LINE_NUMBER (insn) == NOTE_INSN_LOOP_BEG)
	last = insn;
      insn = NEXT_INSN (insn);
    }
  return last;
}

/* Attempt to change code to redirect edge E to TARGET.
   Don't do that on expense of adding new instructions or reordering
   basic blocks.

   Function can be also called with edge destionation equivalent to the
   TARGET.  Then it should try the simplifications and do nothing if
   none is possible.

   Return true if transformation suceeded.  We still return flase in case
   E already destinated TARGET and we didn't managed to simplify instruction
   stream.  */

bool
redirect_edge_and_branch (e, target)
     edge e;
     basic_block target;
{
  rtx tmp;
  rtx old_label = e->dest->head;
  basic_block src = e->src;
  rtx insn = src->end;

  if (e->flags & EDGE_COMPLEX)
    return false;

  if (try_redirect_by_replacing_jump (e, target))
    return true;
  /* Do this fast path late, as we want above code to simplify for cases
     where called on single edge leaving basic block containing nontrivial
     jump insn.  */
  else if (e->dest == target)
    return false;

  /* We can only redirect non-fallthru edges of jump insn.  */
  if (e->flags & EDGE_FALLTHRU)
    return false;
  if (GET_CODE (insn) != JUMP_INSN)
    return false;

  /* Recognize a tablejump and adjust all matching cases.  */
  if ((tmp = JUMP_LABEL (insn)) != NULL_RTX
      && (tmp = NEXT_INSN (tmp)) != NULL_RTX
      && GET_CODE (tmp) == JUMP_INSN
      && (GET_CODE (PATTERN (tmp)) == ADDR_VEC
	  || GET_CODE (PATTERN (tmp)) == ADDR_DIFF_VEC))
    {
      rtvec vec;
      int j;
      rtx new_label = block_label (target);

      if (GET_CODE (PATTERN (tmp)) == ADDR_VEC)
	vec = XVEC (PATTERN (tmp), 0);
      else
	vec = XVEC (PATTERN (tmp), 1);

      for (j = GET_NUM_ELEM (vec) - 1; j >= 0; --j)
	if (XEXP (RTVEC_ELT (vec, j), 0) == old_label)
	  {
	    RTVEC_ELT (vec, j) = gen_rtx_LABEL_REF (Pmode, new_label);
	    --LABEL_NUSES (old_label);
	    ++LABEL_NUSES (new_label);
	  }

      /* Handle casesi dispatch insns */
      if ((tmp = single_set (insn)) != NULL
	  && SET_DEST (tmp) == pc_rtx
	  && GET_CODE (SET_SRC (tmp)) == IF_THEN_ELSE
	  && GET_CODE (XEXP (SET_SRC (tmp), 2)) == LABEL_REF
	  && XEXP (XEXP (SET_SRC (tmp), 2), 0) == old_label)
	{
	  XEXP (SET_SRC (tmp), 2) = gen_rtx_LABEL_REF (VOIDmode,
						       new_label);
	  --LABEL_NUSES (old_label);
	  ++LABEL_NUSES (new_label);
	}
    }
  else
    {
      /* ?? We may play the games with moving the named labels from
	 one basic block to the other in case only one computed_jump is
	 available.  */
      if (computed_jump_p (insn))
	return false;

      /* A return instruction can't be redirected.  */
      if (returnjump_p (insn))
	return false;

      /* If the insn doesn't go where we think, we're confused.  */
      if (JUMP_LABEL (insn) != old_label)
	abort ();
      redirect_jump (insn, block_label (target), 0);
    }

  if (rtl_dump_file)
    fprintf (rtl_dump_file, "Edge %i->%i redirected to %i\n",
	     e->src->index, e->dest->index, target->index);
  if (e->dest != target)
    redirect_edge_succ_nodup (e, target);
  return true;
}

/* Like force_nonfallthru bellow, but additionally performs redirection
   Used by redirect_edge_and_branch_force.  */

static basic_block
force_nonfallthru_and_redirect (e, target)
     edge e;
     basic_block target;
{
  basic_block jump_block, new_bb = NULL;
  rtx note;
  edge new_edge;

  if (e->flags & EDGE_ABNORMAL)
    abort ();
  if (!(e->flags & EDGE_FALLTHRU))
    abort ();
  if (e->src->succ->succ_next)
    {
      /* Create the new structures.  */
      note = last_loop_beg_note (e->src->end);
      jump_block = create_basic_block (e->src->index + 1, NEXT_INSN (note), NULL);
      jump_block->count = e->count;
      jump_block->frequency = EDGE_FREQUENCY (e);
      jump_block->loop_depth = target->loop_depth;

      if (target->global_live_at_start)
	{
	  jump_block->global_live_at_start =
	    OBSTACK_ALLOC_REG_SET (&flow_obstack);
	  jump_block->global_live_at_end =
	    OBSTACK_ALLOC_REG_SET (&flow_obstack);
	  COPY_REG_SET (jump_block->global_live_at_start,
			target->global_live_at_start);
	  COPY_REG_SET (jump_block->global_live_at_end,
			target->global_live_at_start);
	}

      /* Wire edge in.  */
      new_edge = make_edge (e->src, jump_block, EDGE_FALLTHRU);
      new_edge->probability = e->probability;
      new_edge->count = e->count;

      /* Redirect old edge.  */
      redirect_edge_pred (e, jump_block);
      e->probability = REG_BR_PROB_BASE;

      new_bb = jump_block;
    }
  else
    jump_block = e->src;
  e->flags &= ~EDGE_FALLTHRU;
  if (target == EXIT_BLOCK_PTR)
    {
      if (HAVE_return)
	emit_jump_insn_after (gen_return (), jump_block->end);
      else
	abort ();
    }
  else
    {
      rtx label = block_label (target);
      emit_jump_insn_after (gen_jump (label), jump_block->end);
      JUMP_LABEL (jump_block->end) = label;
      LABEL_NUSES (label)++;
    }
  emit_barrier_after (jump_block->end);
  redirect_edge_succ_nodup (e, target);

  return new_bb;
}

/* Edge E is assumed to be fallthru edge.  Emit needed jump instruction
   (and possibly create new basic block) to make edge non-fallthru.
   Return newly created BB or NULL if none.  */
basic_block
force_nonfallthru (e)
     edge e;
{
  return force_nonfallthru_and_redirect (e, e->dest);
}

/* Redirect edge even at the expense of creating new jump insn or
   basic block.  Return new basic block if created, NULL otherwise.
   Abort if converison is impossible.  */

basic_block
redirect_edge_and_branch_force (e, target)
     edge e;
     basic_block target;
{
  basic_block new_bb;

  if (redirect_edge_and_branch (e, target))
    return NULL;
  if (e->dest == target)
    return NULL;

  /* In case the edge redirection failed, try to force it to be non-fallthru
     and redirect newly created simplejump.  */
  new_bb = force_nonfallthru_and_redirect (e, target);
  return new_bb;
}

/* The given edge should potentially be a fallthru edge.  If that is in
   fact true, delete the jump and barriers that are in the way.  */

void
tidy_fallthru_edge (e, b, c)
     edge e;
     basic_block b, c;
{
  rtx q;

  /* ??? In a late-running flow pass, other folks may have deleted basic
     blocks by nopping out blocks, leaving multiple BARRIERs between here
     and the target label. They ought to be chastized and fixed.

     We can also wind up with a sequence of undeletable labels between
     one block and the next.

     So search through a sequence of barriers, labels, and notes for
     the head of block C and assert that we really do fall through.  */

  if (next_real_insn (b->end) != next_real_insn (PREV_INSN (c->head)))
    return;

  /* Remove what will soon cease being the jump insn from the source block.
     If block B consisted only of this single jump, turn it into a deleted
     note.  */
  q = b->end;
  if (GET_CODE (q) == JUMP_INSN
      && onlyjump_p (q)
      && (any_uncondjump_p (q)
	  || (b->succ == e && e->succ_next == NULL)))
    {
#ifdef HAVE_cc0
      /* If this was a conditional jump, we need to also delete
	 the insn that set cc0.  */
      if (any_condjump_p (q) && only_sets_cc0_p (PREV_INSN (q)))
	q = PREV_INSN (q);
#endif

      q = PREV_INSN (q);

      /* We don't want a block to end on a line-number note since that has
	 the potential of changing the code between -g and not -g.  */
      while (GET_CODE (q) == NOTE && NOTE_LINE_NUMBER (q) >= 0)
	q = PREV_INSN (q);
    }

  /* Selectively unlink the sequence.  */
  if (q != PREV_INSN (c->head))
    delete_insn_chain (NEXT_INSN (q), PREV_INSN (c->head));

  e->flags |= EDGE_FALLTHRU;
}

/* Fix up edges that now fall through, or rather should now fall through
   but previously required a jump around now deleted blocks.  Simplify
   the search by only examining blocks numerically adjacent, since this
   is how find_basic_blocks created them.  */

void
tidy_fallthru_edges ()
{
  int i;

  for (i = 1; i < n_basic_blocks; ++i)
    {
      basic_block b = BASIC_BLOCK (i - 1);
      basic_block c = BASIC_BLOCK (i);
      edge s;

      /* We care about simple conditional or unconditional jumps with
	 a single successor.

	 If we had a conditional branch to the next instruction when
	 find_basic_blocks was called, then there will only be one
	 out edge for the block which ended with the conditional
	 branch (since we do not create duplicate edges).

	 Furthermore, the edge will be marked as a fallthru because we
	 merge the flags for the duplicate edges.  So we do not want to
	 check that the edge is not a FALLTHRU edge.  */
      if ((s = b->succ) != NULL
	  && ! (s->flags & EDGE_COMPLEX)
	  && s->succ_next == NULL
	  && s->dest == c
	  /* If the jump insn has side effects, we can't tidy the edge.  */
	  && (GET_CODE (b->end) != JUMP_INSN
	      || onlyjump_p (b->end)))
	tidy_fallthru_edge (s, b, c);
    }
}

/* Helper function for split_edge.  Return true in case edge BB2 to BB1
   is back edge of syntactic loop.  */

static bool
back_edge_of_syntactic_loop_p (bb1, bb2)
	basic_block bb1, bb2;
{
  rtx insn;
  int count = 0;

  if (bb1->index > bb2->index)
    return false;

  if (bb1->index == bb2->index)
    return true;

  for (insn = bb1->end; insn != bb2->head && count >= 0;
       insn = NEXT_INSN (insn))
    if (GET_CODE (insn) == NOTE)
      {
	if (NOTE_LINE_NUMBER (insn) == NOTE_INSN_LOOP_BEG)
	  count++;
	if (NOTE_LINE_NUMBER (insn) == NOTE_INSN_LOOP_END)
	  count--;
      }

  return count >= 0;
}

/* Split a (typically critical) edge.  Return the new block.
   Abort on abnormal edges.

   ??? The code generally expects to be called on critical edges.
   The case of a block ending in an unconditional jump to a
   block with multiple predecessors is not handled optimally.  */

basic_block
split_edge (edge_in)
     edge edge_in;
{
  basic_block bb;
  edge edge_out;
  rtx before;

  /* Abnormal edges cannot be split.  */
  if ((edge_in->flags & EDGE_ABNORMAL) != 0)
    abort ();

  /* We are going to place the new block in front of edge destination.
     Avoid existence of fallthru predecesors.  */
  if ((edge_in->flags & EDGE_FALLTHRU) == 0)
    {
      edge e;
      for (e = edge_in->dest->pred; e; e = e->pred_next)
	if (e->flags & EDGE_FALLTHRU)
	  break;

      if (e)
	force_nonfallthru (e);
    }

  /* Create the basic block note.

     Where we place the note can have a noticable impact on the generated
     code.  Consider this cfg:

		        E
			|
			0
		       / \
		   +->1-->2--->E
                   |  |
		   +--+

      If we need to insert an insn on the edge from block 0 to block 1,
      we want to ensure the instructions we insert are outside of any
      loop notes that physically sit between block 0 and block 1.  Otherwise
      we confuse the loop optimizer into thinking the loop is a phony.  */

  if (edge_in->dest != EXIT_BLOCK_PTR
      && PREV_INSN (edge_in->dest->head)
      && GET_CODE (PREV_INSN (edge_in->dest->head)) == NOTE
      && NOTE_LINE_NUMBER (PREV_INSN (edge_in->dest->head)) == NOTE_INSN_LOOP_BEG
      && !back_edge_of_syntactic_loop_p (edge_in->dest, edge_in->src))
    before = PREV_INSN (edge_in->dest->head);
  else if (edge_in->dest != EXIT_BLOCK_PTR)
    before = edge_in->dest->head;
  else
    before = NULL_RTX;

  bb = create_basic_block (edge_in->dest == EXIT_BLOCK_PTR ? n_basic_blocks
			   : edge_in->dest->index, before, NULL);
  bb->count = edge_in->count;
  bb->frequency = EDGE_FREQUENCY (edge_in);

  /* ??? This info is likely going to be out of date very soon.  */
  if (edge_in->dest->global_live_at_start)
    {
      bb->global_live_at_start = OBSTACK_ALLOC_REG_SET (&flow_obstack);
      bb->global_live_at_end = OBSTACK_ALLOC_REG_SET (&flow_obstack);
      COPY_REG_SET (bb->global_live_at_start, edge_in->dest->global_live_at_start);
      COPY_REG_SET (bb->global_live_at_end, edge_in->dest->global_live_at_start);
    }

  edge_out = make_single_succ_edge (bb, edge_in->dest, EDGE_FALLTHRU);

  /* For non-fallthry edges, we must adjust the predecessor's
     jump instruction to target our new block.  */
  if ((edge_in->flags & EDGE_FALLTHRU) == 0)
    {
      if (!redirect_edge_and_branch (edge_in, bb))
	abort ();
    }
  else
    redirect_edge_succ (edge_in, bb);

  return bb;
}

/* Queue instructions for insertion on an edge between two basic blocks.
   The new instructions and basic blocks (if any) will not appear in the
   CFG until commit_edge_insertions is called.  */

void
insert_insn_on_edge (pattern, e)
     rtx pattern;
     edge e;
{
  /* We cannot insert instructions on an abnormal critical edge.
     It will be easier to find the culprit if we die now.  */
  if ((e->flags & EDGE_ABNORMAL) && EDGE_CRITICAL_P (e))
    abort ();

  if (e->insns == NULL_RTX)
    start_sequence ();
  else
    push_to_sequence (e->insns);

  emit_insn (pattern);

  e->insns = get_insns ();
  end_sequence ();
}

/* Update the CFG for the instructions queued on edge E.  */

static void
commit_one_edge_insertion (e)
     edge e;
{
  rtx before = NULL_RTX, after = NULL_RTX, insns, tmp, last;
  basic_block bb;

  /* Pull the insns off the edge now since the edge might go away.  */
  insns = e->insns;
  e->insns = NULL_RTX;

  /* Figure out where to put these things.  If the destination has
     one predecessor, insert there.  Except for the exit block.  */
  if (e->dest->pred->pred_next == NULL
      && e->dest != EXIT_BLOCK_PTR)
    {
      bb = e->dest;

      /* Get the location correct wrt a code label, and "nice" wrt
	 a basic block note, and before everything else.  */
      tmp = bb->head;
      if (GET_CODE (tmp) == CODE_LABEL)
	tmp = NEXT_INSN (tmp);
      if (NOTE_INSN_BASIC_BLOCK_P (tmp))
	tmp = NEXT_INSN (tmp);
      if (tmp == bb->head)
	before = tmp;
      else
	after = PREV_INSN (tmp);
    }

  /* If the source has one successor and the edge is not abnormal,
     insert there.  Except for the entry block.  */
  else if ((e->flags & EDGE_ABNORMAL) == 0
	   && e->src->succ->succ_next == NULL
	   && e->src != ENTRY_BLOCK_PTR)
    {
      bb = e->src;
      /* It is possible to have a non-simple jump here.  Consider a target
	 where some forms of unconditional jumps clobber a register.  This
	 happens on the fr30 for example.

	 We know this block has a single successor, so we can just emit
	 the queued insns before the jump.  */
      if (GET_CODE (bb->end) == JUMP_INSN)
	{
	  before = bb->end;
	  while (GET_CODE (PREV_INSN (before)) == NOTE
		 && NOTE_LINE_NUMBER (PREV_INSN (before)) == NOTE_INSN_LOOP_BEG)
	    before = PREV_INSN (before);
	}
      else
	{
	  /* We'd better be fallthru, or we've lost track of what's what.  */
	  if ((e->flags & EDGE_FALLTHRU) == 0)
	    abort ();

	  after = bb->end;
	}
    }

  /* Otherwise we must split the edge.  */
  else
    {
      bb = split_edge (e);
      after = bb->end;
    }

  /* Now that we've found the spot, do the insertion.  */

  if (before)
    {
      emit_insns_before (insns, before);
      last = prev_nonnote_insn (before);
    }
  else
    last = emit_insns_after (insns, after);

  if (returnjump_p (last))
    {
      /* ??? Remove all outgoing edges from BB and add one for EXIT.
         This is not currently a problem because this only happens
	 for the (single) epilogue, which already has a fallthru edge
	 to EXIT.  */

      e = bb->succ;
      if (e->dest != EXIT_BLOCK_PTR
	  || e->succ_next != NULL
	  || (e->flags & EDGE_FALLTHRU) == 0)
	abort ();
      e->flags &= ~EDGE_FALLTHRU;

      emit_barrier_after (last);

      if (before)
	delete_insn (before);
    }
  else if (GET_CODE (last) == JUMP_INSN)
    abort ();
  find_sub_basic_blocks (bb);
}

/* Update the CFG for all queued instructions.  */

void
commit_edge_insertions ()
{
  int i;
  basic_block bb;

#ifdef ENABLE_CHECKING
  verify_flow_info ();
#endif

  i = -1;
  bb = ENTRY_BLOCK_PTR;
  while (1)
    {
      edge e, next;

      for (e = bb->succ; e; e = next)
	{
	  next = e->succ_next;
	  if (e->insns)
	    commit_one_edge_insertion (e);
	}

      if (++i >= n_basic_blocks)
	break;
      bb = BASIC_BLOCK (i);
    }
}

void
dump_flow_info (file)
     FILE *file;
{
  register int i;
  static const char * const reg_class_names[] = REG_CLASS_NAMES;

  fprintf (file, "%d registers.\n", max_regno);
  for (i = FIRST_PSEUDO_REGISTER; i < max_regno; i++)
    if (REG_N_REFS (i))
      {
	enum reg_class class, altclass;
	fprintf (file, "\nRegister %d used %d times across %d insns",
		 i, REG_N_REFS (i), REG_LIVE_LENGTH (i));
	if (REG_BASIC_BLOCK (i) >= 0)
	  fprintf (file, " in block %d", REG_BASIC_BLOCK (i));
	if (REG_N_SETS (i))
	  fprintf (file, "; set %d time%s", REG_N_SETS (i),
		   (REG_N_SETS (i) == 1) ? "" : "s");
	if (REG_USERVAR_P (regno_reg_rtx[i]))
	  fprintf (file, "; user var");
	if (REG_N_DEATHS (i) != 1)
	  fprintf (file, "; dies in %d places", REG_N_DEATHS (i));
	if (REG_N_CALLS_CROSSED (i) == 1)
	  fprintf (file, "; crosses 1 call");
	else if (REG_N_CALLS_CROSSED (i))
	  fprintf (file, "; crosses %d calls", REG_N_CALLS_CROSSED (i));
	if (PSEUDO_REGNO_BYTES (i) != UNITS_PER_WORD)
	  fprintf (file, "; %d bytes", PSEUDO_REGNO_BYTES (i));
	class = reg_preferred_class (i);
	altclass = reg_alternate_class (i);
	if (class != GENERAL_REGS || altclass != ALL_REGS)
	  {
	    if (altclass == ALL_REGS || class == ALL_REGS)
	      fprintf (file, "; pref %s", reg_class_names[(int) class]);
	    else if (altclass == NO_REGS)
	      fprintf (file, "; %s or none", reg_class_names[(int) class]);
	    else
	      fprintf (file, "; pref %s, else %s",
		       reg_class_names[(int) class],
		       reg_class_names[(int) altclass]);
	  }
	if (REG_POINTER (regno_reg_rtx[i]))
	  fprintf (file, "; pointer");
	fprintf (file, ".\n");
      }

  fprintf (file, "\n%d basic blocks, %d edges.\n", n_basic_blocks, n_edges);
  for (i = 0; i < n_basic_blocks; i++)
    {
      register basic_block bb = BASIC_BLOCK (i);
      register edge e;

      fprintf (file, "\nBasic block %d: first insn %d, last %d, loop_depth %d, count ",
	       i, INSN_UID (bb->head), INSN_UID (bb->end), bb->loop_depth);
      fprintf (file, HOST_WIDEST_INT_PRINT_DEC, (HOST_WIDEST_INT) bb->count);
      fprintf (file, ", freq %i.\n", bb->frequency);

      fprintf (file, "Predecessors: ");
      for (e = bb->pred; e; e = e->pred_next)
	dump_edge_info (file, e, 0);

      fprintf (file, "\nSuccessors: ");
      for (e = bb->succ; e; e = e->succ_next)
	dump_edge_info (file, e, 1);

      fprintf (file, "\nRegisters live at start:");
      dump_regset (bb->global_live_at_start, file);

      fprintf (file, "\nRegisters live at end:");
      dump_regset (bb->global_live_at_end, file);

      putc ('\n', file);
    }

  putc ('\n', file);
}

void
debug_flow_info ()
{
  dump_flow_info (stderr);
}

void
dump_edge_info (file, e, do_succ)
     FILE *file;
     edge e;
     int do_succ;
{
  basic_block side = (do_succ ? e->dest : e->src);

  if (side == ENTRY_BLOCK_PTR)
    fputs (" ENTRY", file);
  else if (side == EXIT_BLOCK_PTR)
    fputs (" EXIT", file);
  else
    fprintf (file, " %d", side->index);

  if (e->probability)
    fprintf (file, " [%.1f%%] ", e->probability * 100.0 / REG_BR_PROB_BASE);

  if (e->count)
    {
      fprintf (file, " count:");
      fprintf (file, HOST_WIDEST_INT_PRINT_DEC, (HOST_WIDEST_INT) e->count);
    }

  if (e->flags)
    {
      static const char * const bitnames[] = {
	"fallthru", "ab", "abcall", "eh", "fake", "dfs_back"
      };
      int comma = 0;
      int i, flags = e->flags;

      fputc (' ', file);
      fputc ('(', file);
      for (i = 0; flags; i++)
	if (flags & (1 << i))
	  {
	    flags &= ~(1 << i);

	    if (comma)
	      fputc (',', file);
	    if (i < (int) ARRAY_SIZE (bitnames))
	      fputs (bitnames[i], file);
	    else
	      fprintf (file, "%d", i);
	    comma = 1;
	  }
      fputc (')', file);
    }
}

/* Print out one basic block with live information at start and end.  */

void
dump_bb (bb, outf)
     basic_block bb;
     FILE *outf;
{
  rtx insn;
  rtx last;
  edge e;

  fprintf (outf, ";; Basic block %d, loop depth %d, count ",
	   bb->index, bb->loop_depth);
  fprintf (outf, HOST_WIDEST_INT_PRINT_DEC, (HOST_WIDEST_INT) bb->count);
  putc ('\n', outf);

  fputs (";; Predecessors: ", outf);
  for (e = bb->pred; e; e = e->pred_next)
    dump_edge_info (outf, e, 0);
  putc ('\n', outf);

  fputs (";; Registers live at start:", outf);
  dump_regset (bb->global_live_at_start, outf);
  putc ('\n', outf);

  for (insn = bb->head, last = NEXT_INSN (bb->end);
       insn != last;
       insn = NEXT_INSN (insn))
    print_rtl_single (outf, insn);

  fputs (";; Registers live at end:", outf);
  dump_regset (bb->global_live_at_end, outf);
  putc ('\n', outf);

  fputs (";; Successors: ", outf);
  for (e = bb->succ; e; e = e->succ_next)
    dump_edge_info (outf, e, 1);
  putc ('\n', outf);
}

void
debug_bb (bb)
     basic_block bb;
{
  dump_bb (bb, stderr);
}

void
debug_bb_n (n)
     int n;
{
  dump_bb (BASIC_BLOCK (n), stderr);
}

/* Like print_rtl, but also print out live information for the start of each
   basic block.  */

void
print_rtl_with_bb (outf, rtx_first)
     FILE *outf;
     rtx rtx_first;
{
  register rtx tmp_rtx;

  if (rtx_first == 0)
    fprintf (outf, "(nil)\n");
  else
    {
      int i;
      enum bb_state { NOT_IN_BB, IN_ONE_BB, IN_MULTIPLE_BB };
      int max_uid = get_max_uid ();
      basic_block *start = (basic_block *)
	xcalloc (max_uid, sizeof (basic_block));
      basic_block *end = (basic_block *)
	xcalloc (max_uid, sizeof (basic_block));
      enum bb_state *in_bb_p = (enum bb_state *)
	xcalloc (max_uid, sizeof (enum bb_state));

      for (i = n_basic_blocks - 1; i >= 0; i--)
	{
	  basic_block bb = BASIC_BLOCK (i);
	  rtx x;

	  start[INSN_UID (bb->head)] = bb;
	  end[INSN_UID (bb->end)] = bb;
	  for (x = bb->head; x != NULL_RTX; x = NEXT_INSN (x))
	    {
	      enum bb_state state = IN_MULTIPLE_BB;
	      if (in_bb_p[INSN_UID (x)] == NOT_IN_BB)
		state = IN_ONE_BB;
	      in_bb_p[INSN_UID (x)] = state;

	      if (x == bb->end)
		break;
	    }
	}

      for (tmp_rtx = rtx_first; NULL != tmp_rtx; tmp_rtx = NEXT_INSN (tmp_rtx))
	{
	  int did_output;
	  basic_block bb;

	  if ((bb = start[INSN_UID (tmp_rtx)]) != NULL)
	    {
	      fprintf (outf, ";; Start of basic block %d, registers live:",
		       bb->index);
	      dump_regset (bb->global_live_at_start, outf);
	      putc ('\n', outf);
	    }

	  if (in_bb_p[INSN_UID (tmp_rtx)] == NOT_IN_BB
	      && GET_CODE (tmp_rtx) != NOTE
	      && GET_CODE (tmp_rtx) != BARRIER)
	    fprintf (outf, ";; Insn is not within a basic block\n");
	  else if (in_bb_p[INSN_UID (tmp_rtx)] == IN_MULTIPLE_BB)
	    fprintf (outf, ";; Insn is in multiple basic blocks\n");

	  did_output = print_rtl_single (outf, tmp_rtx);

	  if ((bb = end[INSN_UID (tmp_rtx)]) != NULL)
	    {
	      fprintf (outf, ";; End of basic block %d, registers live:\n",
		       bb->index);
	      dump_regset (bb->global_live_at_end, outf);
	      putc ('\n', outf);
	    }

	  if (did_output)
	    putc ('\n', outf);
	}

      free (start);
      free (end);
      free (in_bb_p);
    }

  if (current_function_epilogue_delay_list != 0)
    {
      fprintf (outf, "\n;; Insns in epilogue delay list:\n\n");
      for (tmp_rtx = current_function_epilogue_delay_list; tmp_rtx != 0;
	   tmp_rtx = XEXP (tmp_rtx, 1))
	print_rtl_single (outf, XEXP (tmp_rtx, 0));
    }
}

/* Verify the CFG consistency.  This function check some CFG invariants and
   aborts when something is wrong.  Hope that this function will help to
   convert many optimization passes to preserve CFG consistent.

   Currently it does following checks:

   - test head/end pointers
   - overlapping of basic blocks
   - edge list correctness
   - headers of basic blocks (the NOTE_INSN_BASIC_BLOCK note)
   - tails of basic blocks (ensure that boundary is necesary)
   - scans body of the basic block for JUMP_INSN, CODE_LABEL
     and NOTE_INSN_BASIC_BLOCK
   - check that all insns are in the basic blocks
   (except the switch handling code, barriers and notes)
   - check that all returns are followed by barriers

   In future it can be extended check a lot of other stuff as well
   (reachability of basic blocks, life information, etc. etc.).  */

void
verify_flow_info ()
{
  const int max_uid = get_max_uid ();
  const rtx rtx_first = get_insns ();
  rtx last_head = get_last_insn ();
  basic_block *bb_info, *last_visited;
  size_t *edge_checksum;
  rtx x;
  int i, last_bb_num_seen, num_bb_notes, err = 0;

  bb_info = (basic_block *) xcalloc (max_uid, sizeof (basic_block));
  last_visited = (basic_block *) xcalloc (n_basic_blocks + 2,
					  sizeof (basic_block));
  edge_checksum = (size_t *) xcalloc (n_basic_blocks + 2, sizeof (size_t));

  for (i = n_basic_blocks - 1; i >= 0; i--)
    {
      basic_block bb = BASIC_BLOCK (i);
      rtx head = bb->head;
      rtx end = bb->end;

      /* Verify the end of the basic block is in the INSN chain.  */
      for (x = last_head; x != NULL_RTX; x = PREV_INSN (x))
	if (x == end)
	  break;
      if (!x)
	{
	  error ("End insn %d for block %d not found in the insn stream.",
		 INSN_UID (end), bb->index);
	  err = 1;
	}

      /* Work backwards from the end to the head of the basic block
	 to verify the head is in the RTL chain.  */
      for (; x != NULL_RTX; x = PREV_INSN (x))
	{
	  /* While walking over the insn chain, verify insns appear
	     in only one basic block and initialize the BB_INFO array
	     used by other passes.  */
	  if (bb_info[INSN_UID (x)] != NULL)
	    {
	      error ("Insn %d is in multiple basic blocks (%d and %d)",
		     INSN_UID (x), bb->index, bb_info[INSN_UID (x)]->index);
	      err = 1;
	    }
	  bb_info[INSN_UID (x)] = bb;

	  if (x == head)
	    break;
	}
      if (!x)
	{
	  error ("Head insn %d for block %d not found in the insn stream.",
		 INSN_UID (head), bb->index);
	  err = 1;
	}

      last_head = x;
    }

  /* Now check the basic blocks (boundaries etc.) */
  for (i = n_basic_blocks - 1; i >= 0; i--)
    {
      basic_block bb = BASIC_BLOCK (i);
      int has_fallthru = 0;
      edge e;

      e = bb->succ;
      while (e)
	{
	  if (last_visited [e->dest->index + 2] == bb)
	    {
	      error ("verify_flow_info: Duplicate edge %i->%i",
		     e->src->index, e->dest->index);
	      err = 1;
	    }
	  last_visited [e->dest->index + 2] = bb;

	  if (e->flags & EDGE_FALLTHRU)
	    has_fallthru = 1;

	  if ((e->flags & EDGE_FALLTHRU)
	      && e->src != ENTRY_BLOCK_PTR
	      && e->dest != EXIT_BLOCK_PTR)
	    {
	      rtx insn;
	      if (e->src->index + 1 != e->dest->index)
		{
		    error ("verify_flow_info: Incorrect blocks for fallthru %i->%i",
			   e->src->index, e->dest->index);
		    err = 1;
		}
	      else
		for (insn = NEXT_INSN (e->src->end); insn != e->dest->head;
		     insn = NEXT_INSN (insn))
		  if (GET_CODE (insn) == BARRIER || INSN_P (insn))
		    {
		      error ("verify_flow_info: Incorrect fallthru %i->%i",
			     e->src->index, e->dest->index);
		      fatal_insn ("Wrong insn in the fallthru edge", insn);
		      err = 1;
		    }
	    }
	  if (e->src != bb)
	    {
	      error ("verify_flow_info: Basic block %d succ edge is corrupted",
		     bb->index);
	      fprintf (stderr, "Predecessor: ");
	      dump_edge_info (stderr, e, 0);
	      fprintf (stderr, "\nSuccessor: ");
	      dump_edge_info (stderr, e, 1);
	      fprintf (stderr, "\n");
	      err = 1;
	    }
	  edge_checksum[e->dest->index + 2] += (size_t) e;
	  e = e->succ_next;
	}
      if (!has_fallthru)
	{
	  rtx insn = bb->end;

	  /* Ensure existence of barrier in BB with no fallthru edges.  */
	  for (insn = bb->end; GET_CODE (insn) != BARRIER;
	       insn = NEXT_INSN (insn))
	    if (!insn
		|| (GET_CODE (insn) == NOTE
		    && NOTE_LINE_NUMBER (insn) == NOTE_INSN_BASIC_BLOCK))
		{
		  error ("Missing barrier after block %i", bb->index);
		  err = 1;
		}
	}

      e = bb->pred;
      while (e)
	{
	  if (e->dest != bb)
	    {
	      error ("Basic block %d pred edge is corrupted", bb->index);
	      fputs ("Predecessor: ", stderr);
	      dump_edge_info (stderr, e, 0);
	      fputs ("\nSuccessor: ", stderr);
	      dump_edge_info (stderr, e, 1);
	      fputc ('\n', stderr);
	      err = 1;
	    }
	  edge_checksum[e->dest->index + 2] -= (size_t) e;
	  e = e->pred_next;
	}
       for (x = bb->head; x != NEXT_INSN (bb->end); x = NEXT_INSN (x))
	 if (basic_block_for_insn && BLOCK_FOR_INSN (x) != bb)
	   {
	     debug_rtx (x);
	     if (! BLOCK_FOR_INSN (x))
	       error ("Insn %d is inside basic block %d but block_for_insn is NULL",
		      INSN_UID (x), bb->index);
	     else
	       error ("Insn %d is inside basic block %d but block_for_insn is %i",
		      INSN_UID (x), bb->index, BLOCK_FOR_INSN (x)->index);
	     err = 1;
	   }

      /* OK pointers are correct.  Now check the header of basic
         block.  It ought to contain optional CODE_LABEL followed
	 by NOTE_BASIC_BLOCK.  */
      x = bb->head;
      if (GET_CODE (x) == CODE_LABEL)
	{
	  if (bb->end == x)
	    {
	      error ("NOTE_INSN_BASIC_BLOCK is missing for block %d",
		     bb->index);
	      err = 1;
	    }
	  x = NEXT_INSN (x);
	}
      if (!NOTE_INSN_BASIC_BLOCK_P (x) || NOTE_BASIC_BLOCK (x) != bb)
	{
	  error ("NOTE_INSN_BASIC_BLOCK is missing for block %d",
		 bb->index);
	  err = 1;
	}

      if (bb->end == x)
	{
	  /* Do checks for empty blocks here */
	}
      else
	{
	  x = NEXT_INSN (x);
	  while (x)
	    {
	      if (NOTE_INSN_BASIC_BLOCK_P (x))
		{
		  error ("NOTE_INSN_BASIC_BLOCK %d in the middle of basic block %d",
			 INSN_UID (x), bb->index);
		  err = 1;
		}

	      if (x == bb->end)
		break;

	      if (GET_CODE (x) == JUMP_INSN
		  || GET_CODE (x) == CODE_LABEL
		  || GET_CODE (x) == BARRIER)
		{
		  error ("In basic block %d:", bb->index);
		  fatal_insn ("Flow control insn inside a basic block", x);
		}

	      x = NEXT_INSN (x);
	    }
	}
    }

  /* Complete edge checksumming for ENTRY and EXIT.  */
  {
    edge e;
    for (e = ENTRY_BLOCK_PTR->succ; e ; e = e->succ_next)
      edge_checksum[e->dest->index + 2] += (size_t) e;
    for (e = EXIT_BLOCK_PTR->pred; e ; e = e->pred_next)
      edge_checksum[e->dest->index + 2] -= (size_t) e;
  }

  for (i = -2; i < n_basic_blocks; ++i)
    if (edge_checksum[i + 2])
      {
	error ("Basic block %i edge lists are corrupted", i);
	err = 1;
      }

  last_bb_num_seen = -1;
  num_bb_notes = 0;
  x = rtx_first;
  while (x)
    {
      if (NOTE_INSN_BASIC_BLOCK_P (x))
	{
	  basic_block bb = NOTE_BASIC_BLOCK (x);
	  num_bb_notes++;
	  if (bb->index != last_bb_num_seen + 1)
	    internal_error ("Basic blocks not numbered consecutively.");

	  last_bb_num_seen = bb->index;
	}

      if (!bb_info[INSN_UID (x)])
	{
	  switch (GET_CODE (x))
	    {
	    case BARRIER:
	    case NOTE:
	      break;

	    case CODE_LABEL:
	      /* An addr_vec is placed outside any block block.  */
	      if (NEXT_INSN (x)
		  && GET_CODE (NEXT_INSN (x)) == JUMP_INSN
		  && (GET_CODE (PATTERN (NEXT_INSN (x))) == ADDR_DIFF_VEC
		      || GET_CODE (PATTERN (NEXT_INSN (x))) == ADDR_VEC))
		{
		  x = NEXT_INSN (x);
		}

	      /* But in any case, non-deletable labels can appear anywhere.  */
	      break;

	    default:
	      fatal_insn ("Insn outside basic block", x);
	    }
	}

      if (INSN_P (x)
	  && GET_CODE (x) == JUMP_INSN
	  && returnjump_p (x) && ! condjump_p (x)
	  && ! (NEXT_INSN (x) && GET_CODE (NEXT_INSN (x)) == BARRIER))
	    fatal_insn ("Return not followed by barrier", x);

      x = NEXT_INSN (x);
    }

  if (num_bb_notes != n_basic_blocks)
    internal_error
      ("number of bb notes in insn chain (%d) != n_basic_blocks (%d)",
       num_bb_notes, n_basic_blocks);

  if (err)
    internal_error ("verify_flow_info failed.");

  /* Clean up.  */
  free (bb_info);
  free (last_visited);
  free (edge_checksum);
}

/* Assume that the preceeding pass has possibly eliminated jump instructions
   or converted the unconditional jumps.  Eliminate the edges from CFG.
   Return true if any edges are eliminated.  */

bool
purge_dead_edges (bb)
     basic_block bb;
{
  edge e, next;
  rtx insn = bb->end;
  bool purged = false;

  if (GET_CODE (insn) == JUMP_INSN && !simplejump_p (insn))
    return false;
  if (GET_CODE (insn) == JUMP_INSN)
    {
      rtx note;
      edge b,f;
      /* We do care only about conditional jumps and simplejumps.  */
      if (!any_condjump_p (insn)
	  && !returnjump_p (insn)
	  && !simplejump_p (insn))
	return false;
      for (e = bb->succ; e; e = next)
	{
	  next = e->succ_next;

	  /* Check purposes we can have edge.  */
	  if ((e->flags & EDGE_FALLTHRU)
	      && any_condjump_p (insn))
	    continue;
	  if (e->dest != EXIT_BLOCK_PTR
	      && e->dest->head == JUMP_LABEL (insn))
	    continue;
	  if (e->dest == EXIT_BLOCK_PTR
	      && returnjump_p (insn))
	    continue;
	  purged = true;
	  remove_edge (e);
	}
      if (!bb->succ || !purged)
	return false;
      if (rtl_dump_file)
	fprintf (rtl_dump_file, "Purged edges from bb %i\n", bb->index);
      if (!optimize)
	return purged;

      /* Redistribute probabilities.  */
      if (!bb->succ->succ_next)
	{
	  bb->succ->probability = REG_BR_PROB_BASE;
	  bb->succ->count = bb->count;
        }
      else
	{
	  note = find_reg_note (insn, REG_BR_PROB, NULL);
	  if (!note)
	    return purged;
	  b = BRANCH_EDGE (bb);
	  f = FALLTHRU_EDGE (bb);
	  b->probability = INTVAL (XEXP (note, 0));
	  f->probability = REG_BR_PROB_BASE - b->probability;
	  b->count = bb->count * b->probability / REG_BR_PROB_BASE;
	  f->count = bb->count * f->probability / REG_BR_PROB_BASE;
	}
      return purged;
    }

  /* Cleanup abnormal edges caused by throwing insns that have been
     eliminated.  */
  if (! can_throw_internal (bb->end))
    for (e = bb->succ; e; e = next)
      {
	next = e->succ_next;
	if (e->flags & EDGE_EH)
	  {
	    remove_edge (e);
	    purged = true;
	  }
      }

  /* If we don't see a jump insn, we don't know exactly why the block would
     have been broken at this point.  Look for a simple, non-fallthru edge,
     as these are only created by conditional branches.  If we find such an
     edge we know that there used to be a jump here and can then safely
     remove all non-fallthru edges.  */
  for (e = bb->succ; e && (e->flags & (EDGE_COMPLEX | EDGE_FALLTHRU));
       e = e->succ_next);
  if (!e)
    return purged;
  for (e = bb->succ; e; e = next)
    {
      next = e->succ_next;
      if (!(e->flags & EDGE_FALLTHRU))
	remove_edge (e), purged = true;
    }
  if (!bb->succ || bb->succ->succ_next)
    abort ();
  bb->succ->probability = REG_BR_PROB_BASE;
  bb->succ->count = bb->count;

  if (rtl_dump_file)
    fprintf (rtl_dump_file, "Purged non-fallthru edges from bb %i\n",
	     bb->index);
  return purged;
}

/* Search all basic blocks for potentionally dead edges and purge them.

   Return true ifif some edge has been elliminated.
 */

bool
purge_all_dead_edges ()
{
  int i, purged = false;
  for (i = 0; i < n_basic_blocks; i++)
    purged |= purge_dead_edges (BASIC_BLOCK (i));
  return purged;
}
