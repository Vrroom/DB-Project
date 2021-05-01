#include "postgres.h"
#include "optimizer/parallel_worker.h"
#include "optimizer/parallel_utils.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include <pthread.h>

/**
 * Postgres is written assuming a single threaded process space.
 * This sort of throws the spanner into our works. To implement
 * the no-shared algorithm as a proof of concept, we use a 
 * mutex lock to run each worker thread sequentially. Of course,
 * we don't get any parallelism benefit anymore. 
 */
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; 

int ptr_less (const void * a, const void * b){
	List * one = (List *) lfirst(*(ListCell **) a);
	List * two = (List *) lfirst(*(ListCell **) b);
	return list_length(one) - list_length(two);
}

/**
 * Generate the power set for {q1, q2} which
 * respects the constraints in constr. The power 
 * sets are essentially permissible intermediate
 * join results. 
 *
 * Hence if q1 < q2 is a constraint, then no
 * intermediate join result can have just q2 and 
 * not q1. For such a constraint, {q2} is eliminated 
 * from the power set of {q1, q2}.
 *
 * constr, q1 and q2 are not modified.
 */
List * constrained_power_set(List * constr, int q1, int q2){
	List * cps = NIL;
	bool include_q1 = true;
	bool include_q2 = true;
	for(int i = 0; i < list_length(constr); i++){
		List * ci = (List *) list_nth(constr, i);
		if(list_nth_int(ci, 1) == q1)
			include_q1 = false;
		else if(list_nth_int(ci, 1) == q2)
			include_q2 = false;
	}
	if(include_q1){
		List * arr = NIL;
		arr = lappend_int(arr, q1);
		cps = lappend(cps, arr);
	}
	if(include_q2){
		List * arr = NIL;
		arr = lappend_int(arr, q2);
		cps = lappend(cps, arr);
	}
	List * arr = NIL;
	arr = lappend_int(arr, q1);
	arr = lappend_int(arr, q2);
	cps = lappend(cps, arr);

	return cps;
}

// changed for bushy tree joins
List * constrained_power_set_b(List * constr, int q1, int q2, int q3){
	List * cps = NIL;
	// size 1 elems of power(S) and {1, 2} in S
	List * arr1 = NIL;
	arr1 = lappend_int(arr1, q1);
	cps = lappend(cps, arr1);
	List * arr2 = NIL;
	arr2 = lappend_int(arr2, q2);
	cps = lappend(cps, arr2);
	List * arr3 = NIL;
	arr3 = lappend_int(arr3, q3);
	cps = lappend(cps, arr3);

	List * arr12 = NIL;
	arr12 = lappend_int(arr12, q1);
	arr12 = lappend_int(arr12, q2);
	cps = lappend(cps, arr12);

	bool include_q1q3 = true;
	bool include_q2q3 = true;
	for(int i = 0; i < list_length(constr); i++){
		List * ci = (List *) list_nth(constr, i);
		if(list_nth_int(ci, 1) == q1)
			include_q1q3 = false;
		else if(list_nth_int(ci, 1) == q2)
			include_q2q3 = false;
	}
	if(include_q1q3){
		List * arr = NIL;
		arr = lappend_int(arr, q1);
		arr = lappend_int(arr, q3);
		cps = lappend(cps, arr);
	}
	if(include_q2q3){
		List * arr = NIL;
		arr = lappend_int(arr, q2);
		arr = lappend_int(arr, q3);
		cps = lappend(cps, arr);
	}

	// size 3 {1, 2, 3} in S
	List * arr123 = NIL;
	arr123 = lappend_int(arr123, q1);
	arr123 = lappend_int(arr123, q2);
	arr123 = lappend_int(arr123, q3);
	cps = lappend(cps, arr123);
	return cps;
}

/**
 * Find the constraints on join order for left deep plans 
 * for the worker with given part_id. 
 *
 * Returns the part constraints as a list of two tuples. 
 * Each tuple is of the form (a, b). Here, a and b are 
 * indices of the tables in the query. For example, if 
 * the query is :
 *
 * 		SELECT * from table1, table2, table3, table4, table5.
 *
 * Then a and b are in the range 0-4, indexing these 5 tables. 
 * The constraint (a, b) says that the ath table will be joined 
 * before bth table. 
 *
 * part_id is in the range [0, n_workers). The bits of the part_id
 * are used to generate the constraints for the worker. Pairs of
 * tables are oriented based on the bits in part_id, starting 
 * from the least significant bit. 
 *
 * For example, if n_workers=4, and part_id is 2, then:
 *
 * 0th Bit = 0 : Constraint is (table1, table2)
 * 1st Bit = 1 : Constraint is (table4, table3)
 */
List * part_constraints(int levels_needed, int part_id, int n_workers){
	List * pc = NIL;
	for(int i = 0; (1 << i) < n_workers; i++){
		int select = part_id & (1 << i);
		int q1, q2;
		if(select > 0){
			q1 = 2*i + 1;
			q2 = 2*i;
		}else{
			q1 = 2*i;
			q2 = 2*i + 1;
		}
		List * arr = NIL;
		arr = lappend_int(arr, q1);
		arr = lappend_int(arr, q2);
		pc = lappend(pc, arr);
	}
	return pc;
}

/**
 * Find the constraints on join order for bushy plans 
 * for the worker with given part_id. 
 *
 * Returns the part constraints as a list of three tuples. 
 * Each tuple is of the form (a, b, c). Here, a, b and c are 
 * indices of the tables in the query. For example, if 
 * the query is :
 *
 * 		SELECT * from table1, table2, table3, table4, table5, table6;
 *
 * Then a, b and c are in the range 0-5, indexing these 6 tables. 
 * The constraint (a, b) says that the ath table will be joined 
 * with cth table before the result is joined with the bth table. 
 *
 * part_id is in the range [0, n_workers). The bits of the part_id
 * are used to generate the constraints for the worker. Pairs of
 * tables are oriented based on the bits in part_id, starting 
 * from the least significant bit. 
 *
 * For example, if n_workers=4, and part_id is 2, then:
 *
 * 0th Bit = 0 : Constraint is (table1, table2, table3)
 * 1st Bit = 1 : Constraint is (table5, table4, table6)
 */

List * part_constraints_b(int levels_needed, int part_id, int n_workers){
	List * pc = NIL;
	for(int i = 0; (1 << i) < n_workers; i++){
		int select = part_id & (1 << i);
		int q1, q2, q3;
		if(select > 0){
			q1 = 3*i + 1;
			q2 = 3*i;
			q3 = 3*i + 2;
		}else{
			q1 = 3*i;
			q2 = 3*i + 1;
			q3 = 3*i + 2;
		}
		List * arr = NIL;
		arr = lappend_int(arr, q1);
		arr = lappend_int(arr, q2);
		arr = lappend_int(arr, q3);
		pc = lappend(pc, arr);
	}
	return pc;
}

/**
 * Generate list of intermediate join results which 
 * are consistent with the constraints.
 *
 * Left deep plans can be ordered from left to right 
 * where join will happen in this left to right order.
 *
 * Each join will give rise to an intermediate join 
 * result. These join results has to respect constraints 
 * of the form q1 < q2, given in the constr list. Such a 
 * constraint states that q2 can't be part of an intermediate 
 * join result without q1 already being part of the join result.
 *
 */
List * adm_join_results(int levels_needed, List * constr){
	List * join_res = NIL;
	for(int i = 0; 2*i + 1 < levels_needed; i++){
		int q1 = 2*i;
		int q2 = 2*i + 1;
		List * cps = constrained_power_set(constr, q1, q2);
		join_res = cartesian_product(join_res, cps);
	}
	return join_res;
}

/**
 * Generate list of intermediate join results which 
 * are consistent with the constraints for bushy plans.
 *
 * I'm not sure of the exact logic here but the overall
 * plan can be understood from above. This was written by 
 * Adwait Godbole.
 */
List * adm_join_results_b(int levels_needed, List * constr){
	List * join_res = NIL;
	for(int i = 0; 3*i + 2 < levels_needed; i++){
		int q1 = 3*i;
		int q2 = 3*i + 1;
		int q3 = 3*i + 2;
		List * cps = constrained_power_set_b(constr, q1, q2, q3);
		join_res = cartesian_product(join_res, cps);
	}
	return join_res;
}


/**
 * Compute the best score for each intermediate subset of
 * joined tables using dynamic programming. 
 * 
 * P : DP Table. A bitmap is used to index subsets of joined 
 * tables. Since an int is used for the bitmap, we can handle 
 * joins of atmost 32 tables.
 */
void try_splits(PlannerInfo *root, List * sub_rels, List * constr, RelOptInfo ** P, int levels_needed){

	// Marks those sub_rels which can't be placed on the right
	// in an admissible join set.
	bool * valid = (bool *) palloc(levels_needed*sizeof(bool));

	// Marks those sub_rels which are passed in the input.
	bool * present = (bool *) palloc(levels_needed*sizeof(bool));

	int bitmap = 0;

	// Initialize the two arrays.
	for(int i = 0; i < levels_needed; i++){
		valid[i] = true;
		present[i] = false;
	}

	for(int i = 0; i < list_length(sub_rels); i++){
		int num = list_nth_int(sub_rels, i);

		// Fill in the bitmap representing this sub_rel
		bitmap |= (1 << num);

		// Set those tables which are present in this sub_rel
		present[num] = true;
	}

	// Scan the constraints list.
	// If there is a constraint such that both the LHS and the
	// RHS are present in the sub_rel, then mark the LHS
	// as invalid, because it can't appear on the right in
	// an admissible join order.
	for(int i = 0; i < list_length(constr); i++){

		List * ci = (List *) list_nth(constr, i);

		int q1 = list_nth_int(ci, 0);
		int q2 = list_nth_int(ci, 1);

		if(present[q1] && present[q2]){

			valid[q1] = false;

		}
	}
	// Search the space of left deep joins by partitioning
	// this sub_rel into left tree and singleton right.
	for(int i = 0; i < list_length(sub_rels); i++){

		// The table to keep on the right if possible.
		int u = list_nth_int(sub_rels, i);
		if(valid[u]){

			int l_bitmp = bitmap & ~(1 << u);

			// Pattern is copied from geqo_eval.c
			RelOptInfo * l_splt = P[l_bitmp];
			RelOptInfo * r_splt = P[1 << u];
			elog(LOG, "Trying to make_join_rel");
			RelOptInfo * join_rel = make_join_rel(root, l_splt, r_splt);
			elog(LOG, "Made join_rel");
			if(join_rel){
				elog(LOG, "Setting cheapest join_rel");
				generate_partitionwise_join_paths(root, join_rel);

				if(list_length(sub_rels) != levels_needed)
					generate_gather_paths(root, join_rel, false);

				set_cheapest(join_rel);
				elog(LOG, "Set cheapest join_rel");
			}
			// Set the better result in the DP Table
			// Lower cost is better.
			if(P[bitmap] == NULL){
				P[bitmap] = join_rel;
			}else if(P[bitmap]->cheapest_total_path->total_cost > join_rel->cheapest_total_path->total_cost){
				P[bitmap] = join_rel;
			}
		}
	}

	pfree(valid);
	pfree(present);
}


/**
 * Adwait Godbole's implementation for splits in a bushy plan.
 */
void try_splits_b(PlannerInfo * root, List * sub_rels, List * constr, RelOptInfo ** P, int n){
	List * A = NIL;

	bool * present = (bool *) palloc(n*sizeof(bool));

	int bitmap = 0;

	for(int i = 0; i < n; i++){
		present[i] = false;
	}
	for(int i = 0; i < list_length(sub_rels); i++){
		int num = list_nth_int(sub_rels, i);
		bitmap |= (1 << num);
		present[num] = true;
	}
	for (int i = 0; 3*i+2 < n; i++)
	{
		if(i < list_length(constr)){
			List * ithentry = (List *) list_nth(constr, i);
			int q1 = list_nth_int(ithentry, 0);
			int q2 = list_nth_int(ithentry, 1);
			int q3 = list_nth_int(ithentry, 2);
			bool q1pres = present[q1];
			bool q2pres = present[q2];
			bool q3pres = present[q3];

			List * Spower = NIL;

			if (q3pres)
			{
				List * arr3 = NIL;
				arr3 = lappend_int(arr3, q3);
				Spower = lappend(Spower, arr3);
				if (q2pres)
				{
					List * arr2 = NIL;
					arr2 = lappend_int(arr2, q2);
					Spower = lappend(Spower, arr2);
					if (q1pres)
					{
						List * arr12 = NIL;
						arr12 = lappend_int(arr12, q1);
						arr12 = lappend_int(arr12, q2);
						List * arr13 = NIL;
						arr13 = lappend_int(arr13, q1);
						arr13 = lappend_int(arr13, q3);
						List * arr123 = NIL;
						arr123 = lappend_int(arr123, q1);
						arr123 = lappend_int(arr123, q2);
						arr123 = lappend_int(arr123, q3);
						Spower = lappend(Spower, arr12);
						Spower = lappend(Spower, arr13);
						Spower = lappend(Spower, arr123);
					}
				}
				else{
					if (q1pres)
					{
						List * arr1 = NIL;
						arr1 = lappend_int(arr1, q1);
						List * arr13 = NIL;
						arr13 = lappend_int(arr13, q1);
						arr13 = lappend_int(arr13, q3);
						Spower = lappend(Spower, arr1);
						Spower = lappend(Spower, arr13);
					}
				}
			}
			else{
				if (q2pres)
				{
					List * arr2 = NIL;
					arr2 = lappend_int(arr2, q2);
					Spower = lappend(Spower, arr2);
					if (q1pres)
					{
						List * arr1 = NIL;
						arr1 = lappend_int(arr1, q1);
						List * arr12 = NIL;
						arr12 = lappend_int(arr12, q1);
						arr12 = lappend_int(arr12, q2);
						Spower = lappend(Spower, arr12);
						Spower = lappend(Spower, arr1);
					}
				}
				else{
					if (q1pres)
					{
						List * arr1 = NIL;
						arr1 = lappend_int(arr1, q1);
						Spower = lappend(Spower, arr1);
					}
				}
			}
			A = cartesian_product(A, Spower);
		}else{
			int q1 = 3*i;
			int q2 = 3*i+1;
			int q3 = 3*i+2;
			bool q1pres = present[q1];
			bool q2pres = present[q2];
			bool q3pres = present[q3];

			List * Spower = NIL;


			if (q3pres)
			{
				List * arr3 = NIL;
				arr3 = lappend_int(arr3, q3);
				Spower = lappend(Spower, arr3);
				if (q2pres)
				{
					List * arr2 = NIL;
					arr2 = lappend_int(arr2, q2);
					Spower = lappend(Spower, arr2);
					List * arr23 = NIL;
					arr23 = lappend_int(arr23, q2);
					arr23 = lappend_int(arr23, q3);
					Spower = lappend(Spower, arr23);
					if (q1pres)
					{
						List * arr1 = NIL;
						arr1 = lappend_int(arr1, q1);
						Spower = lappend(Spower, arr1);
						List * arr12 = NIL;
						arr12 = lappend_int(arr12, q1);
						arr12 = lappend_int(arr12, q2);
						List * arr13 = NIL;
						arr13 = lappend_int(arr13, q1);
						arr13 = lappend_int(arr13, q3);
						List * arr123 = NIL;
						arr123 = lappend_int(arr123, q1);
						arr123 = lappend_int(arr123, q2);
						arr123 = lappend_int(arr123, q3);
						Spower = lappend(Spower, arr12);
						Spower = lappend(Spower, arr13);
						Spower = lappend(Spower, arr123);
					}
				}
				else{
					if (q1pres)
					{
						List * arr1 = NIL;
						arr1 = lappend_int(arr1, q1);
						List * arr13 = NIL;
						arr13 = lappend_int(arr13, q1);
						arr13 = lappend_int(arr13, q3);
						Spower = lappend(Spower, arr1);
						Spower = lappend(Spower, arr13);
					}
				}
			}
			else{
				if (q2pres)
				{
					List * arr2 = NIL;
					arr2 = lappend_int(arr2, q2);
					Spower = lappend(Spower, arr2);
					if (q1pres)
					{
						List * arr1 = NIL;
						arr1 = lappend_int(arr1, q1);
						List * arr12 = NIL;
						arr12 = lappend_int(arr12, q1);
						arr12 = lappend_int(arr12, q2);
						Spower = lappend(Spower, arr12);
						Spower = lappend(Spower, arr1);
					}
				}
				else{
					if (q1pres)
					{
						List * arr1 = NIL;
						arr1 = lappend_int(arr1, q1);
						Spower = lappend(Spower, arr1);
					}
				}
			}
			A = cartesian_product(A, Spower);
		}
	}

	for (int i = 0; i < list_length(A); i++)
	{
		List * L = (List *) list_nth(A, i);
		int bitmapl = 0;
		int bitmapr = 0;
		for (int j = 0; j < list_length(L); j++)
		{
			int num = list_nth_int(L, j);
			bitmapl |= (1 << num);

		}
		if (bitmapl == 0 || bitmapl == bitmap)
		{
			continue;
		}
		bitmapr = bitmap - bitmapl;

		RelOptInfo * l_splt = P[bitmapl];
		RelOptInfo * r_splt = P[bitmapr];
		RelOptInfo * join_rel = make_join_rel(root, l_splt, r_splt);

		if(join_rel){
			generate_partitionwise_join_paths(root, join_rel);

			if(list_length(sub_rels) != n)
				generate_gather_paths(root, join_rel, false);

			set_cheapest(join_rel);
		}

		if(P[bitmap] == NULL){
			P[bitmap] = join_rel;
		}else if(P[bitmap]->cheapest_total_path->total_cost > join_rel->cheapest_total_path->total_cost){
			P[bitmap] = join_rel;
		}
	}
	pfree(present);
}

/**
 * Each worker computes the optimal plan in 
 * its own partitioned space of plans. 
 *
 * The space of all join plans is broken down and
 * indexed by a partition id, known as the part_id. 
 * Each plan in this subspace is explored in a 
 * bottom-up fashion. The optimal sub-plans are stored in 
 * a DP Table. These are used to construct the plans for larger
 * and larger subsets of the set of query tables until a plan 
 * is devised for the entire set.
 *
 */
void * worker(void * data){
	pthread_mutex_lock(&mutex);
	WorkerData * wi = (WorkerData *) data;
	PlannerInfo * root = wi->root;
	List * initial_rels = wi->initial_rels;
	int levels_needed = wi->levels_needed;
	int part_id = wi->part_id;
	int n_workers = wi->n_workers;
	int p_type = wi->p_type;

	// Get the relevant constraints for this worker using part_id.
	List * constr;

	// Given the set of constraints, populate this array of arrays
	// with the possible intermediate results.
	List * join_res;

	if(p_type == 2){
		elog(LOG, "Optimizing for left-deep plans");
		constr =  part_constraints(levels_needed, part_id, n_workers);
		join_res = adm_join_results(levels_needed, constr);
	}else if (p_type == 3){
		elog(LOG, "Optimizing for bushy plans");
		constr = part_constraints_b(levels_needed, part_id, n_workers);
		join_res = adm_join_results_b(levels_needed, constr);
	}else{
		printf("error : invalid p_type\n");
	}
	// This is our DP Table which is indexed by a subset bitmap.
	// It contains the best RelOptInfo struct
	// (the one with the cheapest total path) for this level.
	RelOptInfo ** P = (RelOptInfo **) palloc((1 << levels_needed) * sizeof(RelOptInfo *));
	// Initialize DP Table.
	for(int i = 0; i < (1 << levels_needed); i++){
		P[i] = NULL;
	}

	// For singleton subsets, just fill with the ith initial_rels.
	for(int i = 0; i < levels_needed; i++){
		// This is the only way I found to set values in an array.
		P[1 << i] = (RelOptInfo *) list_nth(initial_rels, i);
	}
	elog(LOG, "Initialized DP Table");
	// Sort the join_res 2D array on size, in ascending order.
	List * sorted = list_qsort(join_res, ptr_less);
	list_free(join_res);
	join_res = sorted;

	if(p_type == 2){
		for(int i = 0; i < list_length(join_res); i++){
			List * q = list_nth(join_res, i);

			// For non-singleton admissible subset,
			// try splits.
			if(list_length(q) > 1){
				elog(LOG, "Try splitting admissible join result sets - %d", i); 
				try_splits(root, q, constr, P, levels_needed);
			}
		}
	}else if (p_type == 3){
		for(int i = 0; i < list_length(join_res); i++){
			List * q = list_nth(join_res, i);

			// For non-singleton admissible subset,
			// try splits.
			if(list_length(q) > 1){
				elog(LOG, "Try splitting admissible join result sets - %d", i); 
				try_splits_b(root, q, constr, P, levels_needed);
			}
		}
	}else{
		printf("error : invalid p_type\n");
	}


	// The RelOptInfo which represents the entire set.
	RelOptInfo * best = (RelOptInfo *) palloc(sizeof(RelOptInfo));

	// Copy the best solution and free the DP Table.
	*best= *(P[(1 << levels_needed) - 1]);
	pfree(P);

	list_free_deep(join_res);
	list_free_deep(constr);
	WorkerOutput * opt = (WorkerOutput *) palloc(sizeof(WorkerOutput));
	opt->optimal = best;
	opt->root = root;
	pthread_mutex_unlock(&mutex);
	return opt;
}


