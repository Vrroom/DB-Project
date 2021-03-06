#include "postgres.h"

#include <float.h>
#include <limits.h>
#include <math.h>

#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/parallel_eval.h"
#include "optimizer/parallel_tree.h"
#include "utils/memutils.h"


/* A "clump" of already-joined relations within construct_rel_based_on_plan */
typedef struct
{
	RelOptInfo *joinrel;		/* joinrel for the set of relations */
	int			size;			/* number of input relations in clump */
} Clump;

static List *
force_merge_clump(
	PlannerInfo *root, 
	int levels_needed,
	List *frelList,
	RelOptInfo * rel);

static List *
try_merge_clump(
	PlannerInfo *root, 
	int levels_needed,
	List *initial_rels,
	BinaryTree * bt);

static bool desirable_join(PlannerInfo *root,
			   RelOptInfo *outer_rel, RelOptInfo *inner_rel);


double parallel_eval (
		PlannerInfo * root, 
		int levels_needed,
		List * initial_rels,
		BinaryTree * bt)
{
	MemoryContext mycontext;
	MemoryContext oldcxt;
	RelOptInfo *joinrel;
	double		cost;
	int			savelength;
	struct HTAB *savehash;

	mycontext = AllocSetContextCreate(CurrentMemoryContext,
									  "PARALLEL",
									  ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(mycontext);
	savelength = list_length(root->join_rel_list);
	savehash = root->join_rel_hash;
	Assert(root->join_rel_level == NULL);
	root->join_rel_hash = NULL;
	/* construct the best path for the given combination of relations */
	joinrel = construct_rel_based_on_plan(root,
 		   levels_needed, initial_rels, bt);

	if (joinrel) {
		Path *best_path = joinrel->cheapest_total_path;
		cost = best_path->total_cost;
	} else cost = DBL_MAX;

	/*
	 * Restore join_rel_list to its former state, and put back original
	 * hashtable if any.
	 */
	root->join_rel_list = list_truncate(root->join_rel_list,
										savelength);
	root->join_rel_hash = savehash;
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(mycontext);

	return cost;
}

RelOptInfo *
construct_rel_based_on_plan(
	PlannerInfo *root, 
	int levels_needed,
	List * initial_rels,
	BinaryTree * bt)
{
	List * relList = NIL;

	/*
	 * Sometimes, a relation can't yet be joined to others due to heuristics
	 * or actual semantic restrictions. 
	 */
	relList = try_merge_clump(root, levels_needed, initial_rels, bt);
	if (list_length(relList) > 1) {
		/* Force-join the remaining clumps in some legal order */
		List	   *frelList;
		ListCell   *lc;
		frelList = NIL;
		foreach(lc, relList)
		{
			RelOptInfo *rel = (RelOptInfo *) lfirst(lc);
			frelList = force_merge_clump(root, levels_needed, frelList, rel);
		}
		relList = frelList;
	}
	/* Did we succeed in forming a single join relation? */
	if (list_length(relList) != 1)
		return NULL;
	return (RelOptInfo *) linitial(relList);
}

static List *
try_merge_clump(
	PlannerInfo *root, 
	int levels_needed,
	List *initial_rels,
	BinaryTree * bt)
{
	List * relList = NIL;
	if (is_leaf(bt)) {
		int relid = linitial_int(bt->relids);
		RelOptInfo * rel = list_nth(initial_rels, relid);
		relList = lappend(relList, rel);
	} else {
		List * list1 = try_merge_clump(root, levels_needed, 
				initial_rels, bt->left);
		List * list2 = try_merge_clump(root, levels_needed, 
				initial_rels, bt->right);
		/* 
 		 * It is possible that these are lists with more than one
 		 * element in either of them. If so, then just concat and
 		 * return the result. Else, take the singleton elements
 		 * in the lists and try to join them
 		 */
		if (list_length(list1) > 1 || list_length(list2) > 1) { 
			relList = list_concat(list1, list2);
		} else {
			Assert(list_length(list1) == 1 && list_length(list2) == 1);
			RelOptInfo * rel1 = linitial(list1);
			RelOptInfo * rel2 = linitial(list2);
			if (desirable_join(root, rel1, rel2)) {
				RelOptInfo *joinrel;
				joinrel = make_join_rel(root, rel1, rel2);
				if (joinrel) {
					generate_partitionwise_join_paths(root, joinrel);
					if (list_length(bt->relids) < levels_needed)
						generate_gather_paths(root, joinrel, false);
					/* Find and save the cheapest paths for this joinrel */
					set_cheapest(joinrel);
					relList = lappend(relList, joinrel);				
				} else relList = list_concat(list1, list2);
			} else relList = list_concat(list1, list2);
		}
	}
	return relList;
}

static List *
force_merge_clump(
	PlannerInfo *root, 
	int levels_needed, 
	List *frelList, 
	RelOptInfo *rel)
{
	ListCell   *prev;
	ListCell   *lc;
	/* Look for a clump that new_clump can join to */
	prev = NULL;
	foreach(lc, frelList)
	{
		RelOptInfo *old_rel = (RelOptInfo *) lfirst(lc);
		RelOptInfo *joinrel;
		joinrel = make_join_rel(root, old_rel, rel);
		/* Keep searching if join order is not valid */
		if (joinrel) {
			/* Create paths for partitionwise joins. */
			generate_partitionwise_join_paths(root, joinrel);
			if (bms_num_members(joinrel->relids) < levels_needed)
				generate_gather_paths(root, joinrel, false);
			/* Find and save the cheapest paths for this joinrel */
			set_cheapest(joinrel);
			/* Absorb new clump into old */
			old_rel = joinrel;
			/* Remove old_clump from list */
			frelList = list_delete_cell(frelList, lc, prev);
			/*
			 * Recursively try to merge the enlarged old_clump with
			 * others.  When no further merge is possible, we'll reinsert
			 * it into the list.
			 */
			return force_merge_clump(root, levels_needed, frelList, old_rel);
		}
		prev = lc;
	}
	int rel_size = bms_num_members(rel->relids);
	if (frelList == NIL || rel_size == 1)
		return lappend(frelList, rel);
	/* Check if it belongs at the front */
	lc = list_head(frelList);
	if (rel_size > bms_num_members(((RelOptInfo *) lfirst(lc))->relids))
		return lcons(rel, frelList);
	/* Else search for the place to insert it */
	for (;;)
	{
		ListCell   *nxt = lnext(lc);
		if (nxt == NULL || rel_size > bms_num_members(((RelOptInfo *) lfirst(nxt))->relids))
			break;				/* it belongs after 'lc', before 'nxt' */
		lc = nxt;
	}
	lappend_cell(frelList, lc, rel);
	return frelList;
}

/*
 * Heuristics for gimme_tree: do we want to join these two relations?
 */
static bool
desirable_join(PlannerInfo *root,
			   RelOptInfo *outer_rel, RelOptInfo *inner_rel)
{
	/*
	 * Join if there is an applicable join clause, or if there is a join order
	 * restriction forcing these rels to be joined.
	 */
	if (have_relevant_joinclause(root, outer_rel, inner_rel) ||
		have_join_order_restriction(root, outer_rel, inner_rel))
		return true;

	/* Otherwise postpone the join till later. */
	return false;
}
