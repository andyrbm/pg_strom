/*
 * gpuhashjoin.c
 *
 * Hash-Join acceleration by GPU processors
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/relation.h"
#include "nodes/plannodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/subselect.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "storage/ipc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/pg_crc.h"
#include "utils/selfuncs.h"
#include "pg_strom.h"
#include "opencl_hashjoin.h"

/* static variables */
static add_hashjoin_path_hook_type	add_hashjoin_path_next;
static CustomPathMethods		gpuhashjoin_path_methods;
static CustomPlanMethods		gpuhashjoin_plan_methods;
static CustomPlanMethods		multihash_plan_methods;
static bool						enable_gpuhashjoin;

/*
 *                              (depth=0)
 * [GpuHashJoin] ---<outer>--- [relation scan to be joined]
 *    |
 * <inner>
 *    |    (depth=1)
 *    +-- [MultiHash] ---<outer>--- [relation scan to be hashed]
 *           |
 *        <inner>
 *           |    (depth=2)
 *           +-- [MultiHash] ---<outer>--- [relation scan to be hashed]
 *
 * The diagram above shows structure of GpuHashJoin which can have a hash-
 * table that contains multiple inner scans. GpuHashJoin always takes a
 * MultiHash node as inner relation to join it with outer relation, then
 * materialize them into a single pseudo relation view. A MultiHash node
 * has an outer relation to be hashed, and can optionally have another
 * MultiHash node to put multiple inner (small) relations on a hash-table.
 * A smallest set of GpuHashJoin is consists of an outer relation and
 * an inner MultiHash node. When third relation is added, it inject the
 * third relation on the inner-tree of GpuHashJoin. So, it means the
 * deepest MultiHash is the first relation to be joined with outer
 * relation, then second deepest one shall be joined, in case when order
 * of join needs to be paid attention.
 */
typedef struct
{
	CustomPath		cpath;
	Path		   *outerpath;	/* outer path (always one) */
	int				num_rels;	/* number of inner relations */
	Size			hashtable_size;	/* estimated hashtable size */
	struct {
		Path	   *scan_path;
		JoinType	jointype;
		List	   *hash_clause;
		List	   *qual_clause;
		List	   *host_clause;
		double		threshold_ratio;
		Size		chunk_size;	/* available size for each relation chunk */
		cl_uint		ntuples;	/* estimated number of tuples per chunk */
		cl_uint		nloops;		/* expected number of outer loops */
	} inners[FLEXIBLE_ARRAY_MEMBER];
} GpuHashJoinPath;

/*
 * source of pseudo tlist entries
 */
typedef struct
{
	Index		srcdepth;	/* source relation depth */
	AttrNumber	srcresno;	/* source resource number (>0) */
	AttrNumber	resno;		/* resource number of pseudo relation */
	char	   *resname;	/* name of this resource, if any */
	Oid			vartype;	/* type oid of the expression node */
	int32		vartypmod;	/* typmod value of the expression node */
	Oid			varcollid;	/* collation oid of the expression node */
	bool		ref_host;	/* true, if referenced in host expression */
	bool		ref_device;	/* true, if referenced in device expression */
	Expr	   *expr;		/* source Var or PlaceHolderVar node */
} vartrans_info;

typedef struct
{
	CustomPlan	cplan;
	/*
	 * outerPlan ... relation to be joined
	 * innerPlan ... MultiHash with multiple inner relations
	 */
	int			num_rels;		/* number of underlying MultiHash */
	const char *kernel_source;
	int			extra_flags;
	bool		outer_bulkload;	/* is outer can bulk loading? */
	List	   *join_types;		/* list of join types */
	List	   *hash_clauses;	/* list of hash_clause (List *) */
	List	   *qual_clauses;	/* list of qual_clause (List *) */
	List	   *host_clauses;	/* list of host_clause (List *) */

	List	   *used_params;	/* template for kparams */
	Bitmapset  *outer_attrefs;	/* bitmap of referenced outer columns */
	List	   *pscan_vartrans;	/* list of vartrans_info */
} GpuHashJoin;

typedef struct
{
	CustomPlan	cplan;
	/*
	 * outerPlan ... relation to be hashed
	 * innerPlan ... one another MultiHash, if any
	 */
	int			depth;		/* depth of this hash table */

	cl_uint		nslots;		/* width of hash slots */
	cl_uint		nloops;		/* expected number of outer loops */
	double		threshold_ratio;
	Size		hashtable_size;	/* estimated total hashtable size */

	List	   *hash_inner_keys;/* list of inner hash key expressions */
	List	   *hash_outer_keys;/* list of outer hash key expressions */
	/*
	 * NOTE: Any varno of the var-nodes in hash_inner_keys references
	 * OUTER_VAR, because this expression node is used to calculate
	 * hash-value of individual entries on construction of MultiHashNode
	 * during outer relation scan.
	 * On the other hands, any varno of the var-nodes in hash_outer_keys
	 * references INDEX_VAR with varattno on the pseudo tlist, because
	 * it is used for code generation.
	 */
} MultiHash;

/*
 * MultiHashNode - a data structure to be returned from MultiHash node;
 * that contains a pgstrom_multihash_tables object on shared memory
 * region and related tuplestore/tupleslot for each inner relations.
 */
typedef struct {
	Node		type;	/* T_Invalid */
	pgstrom_multihash_tables *mhtables;
	int				nrels;
} MultiHashNode;

typedef struct
{
	CustomPlanState	cps;
	List		   *join_types;
	List		   *hash_clauses;
	List		   *qual_clauses;
	List		   *host_clauses;

	pgstrom_multihash_tables *mhtables;

	int				pscan_nattrs;
	vartrans_info  *pscan_vartrans;
	TupleTableSlot *pscan_slot;
	TupleTableSlot *pscan_wider_slot;
	ProjectionInfo *pscan_projection;
	ProjectionInfo *pscan_wider_projection;

	/* average ratio to popurate result row */
	double			row_population_ratio;
	/* average number of tuples per page */
	double			ntups_per_page;

	/* state for outer scan */
	bool			outer_done;
	bool			outer_bulkload;
	TupleTableSlot *outer_overflow;

	pgstrom_queue  *mqueue;
	Datum			dprog_key;
	kern_parambuf  *kparams;

	pgstrom_gpuhashjoin *curr_ghjoin;
	cl_uint			curr_index;
	bool			curr_recheck;
	cl_int			num_running;
	dlist_head		ready_pscans;

	pgstrom_perfmon	pfm;
} GpuHashJoinState;

typedef struct {
	CustomPlanState	cps;
	int				depth;
	cl_uint			nslots;
	double			threshold_ratio;
	Size			chunk_size;
	Size			hashtable_size;
	TupleTableSlot *outer_overflow;
	bool			outer_done;
	kern_hashtable *curr_chunk;
	List		   *hash_keys;
	List		   *hash_keylen;
	List		   *hash_keybyval;
} MultiHashState;

/* declaration of static functions */
static void clserv_process_gpuhashjoin(pgstrom_message *message);

/*
 * path_is_gpuhashjoin - returns true, if supplied pathnode is gpuhashjoin
 */
static bool
path_is_gpuhashjoin(Path *pathnode)
{
	CustomPath *cpath = (CustomPath *) pathnode;

	if (!IsA(cpath, CustomPath))
		return false;
	if (cpath->methods != &gpuhashjoin_path_methods)
		return false;
	return true;
}

/*
 * path_is_mergeable_gpuhashjoin - returns true, if supplied pathnode is
 * gpuhashjoin that can be merged with one more inner scan.
 */
static bool
path_is_mergeable_gpuhashjoin(Path *pathnode)
{
	RelOptInfo		*rel = pathnode->parent;
	GpuHashJoinPath	*gpath;
	List	   *host_clause;
	ListCell   *cell;
	int			last;

	if (!path_is_gpuhashjoin(pathnode))
		return false;

	gpath = (GpuHashJoinPath *) pathnode;
	last = gpath->num_rels - 1;

	/*
	 * target-list must be simple var-nodes only
	 */
	foreach (cell, rel->reltargetlist)
	{
		Expr   *expr = lfirst(cell);

		if (!IsA(expr, Var))
			return false;
	}

	/*
	 * Only INNER JOIN is supported right now
	 */
	if (gpath->inners[last].jointype != JOIN_INNER)
		return false;

	/*
	 * Host qual should not contain volatile function except for
	 * the last inner relation
	 */
	host_clause = gpath->inners[last].host_clause;
	foreach (cell, host_clause)
	{
		RestrictInfo   *rinfo = lfirst(cell);

		Assert(IsA(rinfo, RestrictInfo));
		if (contain_volatile_functions((Node *)rinfo->clause))
			return false;
	}

	/*
	 * TODO: Is any other condition to be checked?
	 */
	return true;
}

/*
 * plan_is_multihash - returns true, if supplied plannode is multihash
 */
static bool
plan_is_multihash(Plan *plannode)
{
	CustomPlan *cplan = (CustomPlan *) plannode;

	if (!IsA(cplan, CustomPlan))
		return false;
	if (cplan->methods != &multihash_plan_methods)
		return false;
	return true;
}

/*
 * estimate_hashitem_size
 *
 * It estimates size of hashitem for GpuHashJoin
 */
static bool
estimate_hashtable_size(PlannerInfo *root,
						GpuHashJoinPath *gpath,
						Relids required_outer,
						JoinCostWorkspace *workspace)
{
	RelOptInfo *joinrel = gpath->cpath.path.parent;
	Size		hashtable_size;
	Size		threshold_size;
	cl_int		numbatches;
	bool		is_first = true;

	do {
		Size		largest_size;
		cl_int		i_largest;
		cl_int		i, nrels = gpath->num_rels;

		/* increment outer loop count to reduce size of hash table */
		if (!is_first)
			gpath->inners[i_largest].nloops++;
		numbatches = 1;
		largest_size = 0;
		i_largest = -1;

		hashtable_size = LONGALIGN(offsetof(kern_multihash,
											htable_offset[nrels]));
		for (i=0; i < nrels; i++)
		{
			Path	   *scan_path = gpath->inners[i].scan_path;
			RelOptInfo *scan_rel = scan_path->parent;
			cl_uint		ncols = list_length(scan_rel->reltargetlist);
			double		ntuples;
			Size		entry_size;
			Size		chunk_size;

			if (is_first)
				gpath->inners[i].nloops = 1;
			else
				numbatches *= gpath->inners[i].nloops;

			/* force a plausible relation size if no information.
			 * It expects 15% of margin to avoid unnecessary hash-
			 * table split
			 */
			ntuples = (Max(1.15 * scan_path->rows, 1000.0) /
					   (double) gpath->inners[i].nloops);

			/* estimate length of each hash entry */
			entry_size = (offsetof(kern_hashentry, htup) +
						  MAXALIGN(offsetof(HeapTupleHeaderData,
											t_bits[BITMAPLEN(ncols)])) +
						  MAXALIGN(scan_rel->width));
			/* estimate length of this chunk */
			chunk_size = (LONGALIGN(offsetof(kern_hashtable,
											 colmeta[ncols])) +
						  LONGALIGN(sizeof(cl_uint) * (Size)ntuples) +
						  LONGALIGN(entry_size * (Size)ntuples));
			chunk_size = STROMALIGN(chunk_size);
			if (largest_size < chunk_size)
			{
				largest_size = chunk_size;
				i_largest = i;
			}
			gpath->inners[i].chunk_size = chunk_size;
			gpath->inners[i].ntuples = (cl_uint) ntuples;

			/* expand estimated hashtable-size */
			hashtable_size += chunk_size;
		}
		/* also compute threshold_ratio */
		threshold_size = 0;
		for (i = nrels - 1; i >= 0; i--)
		{
			threshold_size += gpath->inners[i].chunk_size;
			gpath->inners[i].threshold_ratio
				= (double) threshold_size / (double) hashtable_size;
		}
		Assert(i_largest >= 0 && i_largest < nrels);
		is_first = false;

		/*
		 * NOTE: In case when extreme number of rows are expected,
		 * it does not make sense to split hash-tables because
		 * increasion of numbatches also increases the total cost
		 * by iteration of outer scan. In this case, the best
		 * strategy is to give up this path, instead of incredible
		 * number of numbatches!
		 */
		if (!add_path_precheck(joinrel,
							   workspace->startup_cost,
							   workspace->startup_cost +
							   workspace->run_cost * numbatches,
							   NULL, required_outer))
			return false;
	} while (hashtable_size > pgstrom_shmem_maxalloc());

	/*
	 * Update estimated hashtable_size, but ensure hashtable_size
	 * shall be allocated at least
	 */
	gpath->hashtable_size = Max(hashtable_size,
								pgstrom_chunk_size << 20);
	/*
	 * Update JoinCostWorkspace according to numbatches
	 */
	workspace->run_cost *= numbatches;
	workspace->total_cost = workspace->startup_cost + workspace->run_cost;

	return true;	/* ok */
}

/*
 * cost_gpuhashjoin
 *
 * cost estimation for GpuHashJoin
 */
static bool
cost_gpuhashjoin(PlannerInfo *root,
				 GpuHashJoinPath *gpath,
				 Relids required_outer,
				 JoinCostWorkspace *workspace)
{
	Path	   *outer_path = gpath->outerpath;
	Cost		startup_cost;
	Cost		run_cost;
	Cost		row_cost;
	double		num_rows;
	int			num_hash_clauses = 0;
	int			i;

	/* cost of source data */
	startup_cost = outer_path->startup_cost;
	run_cost = outer_path->total_cost - outer_path->startup_cost;
	for (i=0; i < gpath->num_rels; i++)
		startup_cost += gpath->inners[i].scan_path->total_cost;

	/*
	 * Cost of computing hash function: it is done by CPU right now,
	 * so we follow the logic in initial_cost_hashjoin().
	 */
	for (i=0; i < gpath->num_rels; i++)
	{
		num_hash_clauses += list_length(gpath->inners[i].hash_clause);
		num_rows = gpath->inners[i].scan_path->rows;
		startup_cost += (cpu_operator_cost *
						 list_length(gpath->inners[i].hash_clause) +
						 cpu_tuple_cost) * num_rows;
	}

	/* in addition, it takes cost to set up OpenCL device/program */
	startup_cost += pgstrom_gpu_setup_cost;

	/* on the other hands, its cost to run outer scan for joinning
	 * is much less than usual GPU hash join.
	 */
	num_rows = gpath->outerpath->rows;
	row_cost = pgstrom_gpu_operator_cost * num_hash_clauses;
	run_cost += row_cost * num_rows;

	/* setup join-cost-workspace */
	workspace->startup_cost = startup_cost;
	workspace->run_cost = run_cost;
	workspace->total_cost = startup_cost + run_cost;
	workspace->numbatches = 1;

	/*
	 * Estimation of hash table size and number of outer loops
	 * according to the split of hash tables.
	 * In case of estimated plan cost is too large to win the
	 * existing paths, it breaks to find out this path.
	 */
	if (!estimate_hashtable_size(root, gpath, required_outer, workspace))
		return false;

	return true;
}

/*
 * approx_tuple_count - copied from costsize.c but arguments are adjusted
 * according to GpuHashJoinPath.
 */
static double
approx_tuple_count(PlannerInfo *root, GpuHashJoinPath *gpath)
{
	Path	   *outer_path = gpath->outerpath;
	Selectivity selec = 1.0;
	double		tuples = outer_path->rows;
	int			i;

	for (i=0; i < gpath->num_rels; i++)
	{
		Path	   *inner_path = gpath->inners[i].scan_path;
		List	   *hash_clause = gpath->inners[i].hash_clause;
		List	   *qual_clause = gpath->inners[i].qual_clause;
		double		inner_tuples = inner_path->rows;
		SpecialJoinInfo sjinfo;
		ListCell   *cell;

		/* make up a SpecialJoinInfo for JOIN_INNER semantics. */
		sjinfo.type = T_SpecialJoinInfo;
		sjinfo.min_lefthand  = outer_path->parent->relids;
		sjinfo.min_righthand = inner_path->parent->relids;
		sjinfo.syn_lefthand  = outer_path->parent->relids;
		sjinfo.syn_righthand = inner_path->parent->relids;
		sjinfo.jointype = JOIN_INNER;
		/* we don't bother trying to make the remaining fields valid */
		sjinfo.lhs_strict = false;
		sjinfo.delay_upper_joins = false;
		sjinfo.join_quals = NIL;

		/* Get the approximate selectivity */
		foreach (cell, hash_clause)
		{
			Node   *qual = (Node *) lfirst(cell);

			/* Note that clause_selectivity can cache its result */
			selec *= clause_selectivity(root, qual, 0, JOIN_INNER, &sjinfo);
		}
		foreach (cell, qual_clause)
		{
			Node   *qual = (Node *) lfirst(cell);

			/* Note that clause_selectivity can cache its result */
			selec *= clause_selectivity(root, qual, 0, JOIN_INNER, &sjinfo);
		}
		/* Apply it to the input relation sizes */
		tuples *= selec * inner_tuples;
	}
	return clamp_row_est(tuples);
}

static void
final_cost_gpuhashjoin(PlannerInfo *root, GpuHashJoinPath *gpath,
					   JoinCostWorkspace *workspace)
{
	Path	   *path = &gpath->cpath.path;
	Cost		startup_cost = workspace->startup_cost;
	Cost		run_cost = workspace->run_cost;
	QualCost	hash_cost;
	QualCost	qual_cost;
	QualCost	host_cost;
	double		hashjointuples;
	int			i;

	/* Mark the path with correct row estimation */
	if (path->param_info)
		path->rows = path->param_info->ppi_rows;
	else
		path->rows = path->parent->rows;

	/* Compute cost of the hash, qual and host clauses */
	for (i=0; i < gpath->num_rels; i++)
	{
		List	   *hash_clause = gpath->inners[i].hash_clause;
		List	   *qual_clause = gpath->inners[i].qual_clause;
		List	   *host_clause = gpath->inners[i].host_clause;
		double		outer_path_rows = gpath->outerpath->rows;
		double		inner_path_rows = gpath->inners[i].scan_path->rows;
		Relids		inner_relids = gpath->inners[i].scan_path->parent->relids;
		Selectivity	innerbucketsize = 1.0;
		ListCell   *cell;

		/*
		 * Determine bucketsize fraction for inner relation.
		 * We use the smallest bucketsize estimated for any individual
		 * hashclause; this is undoubtedly conservative.
		 */
		foreach (cell, hash_clause)
		{
			RestrictInfo   *restrictinfo = (RestrictInfo *) lfirst(cell);
			Selectivity		thisbucketsize;
			double			virtualbuckets;
			Node		   *op_expr;

			Assert(IsA(restrictinfo, RestrictInfo));

			/* Right now, GpuHashJoin assumes all the inner record can
			 * be loaded into a single "multihash_tables" structure,
			 * so hash table is never divided and outer relation is
			 * rescanned.
			 * This assumption may change in the future implementation
			 */
			if (inner_path_rows < 1000.0)
				virtualbuckets = 1000.0;
			else
				virtualbuckets = inner_path_rows;

			/*
			 * First we have to figure out which side of the hashjoin clause
			 * is the inner side.
			 *
			 * Since we tend to visit the same clauses over and over when
			 * planning a large query, we cache the bucketsize estimate in the
			 * RestrictInfo node to avoid repeated lookups of statistics.
			 */
			if (bms_is_subset(restrictinfo->right_relids, inner_relids))
				op_expr = get_rightop(restrictinfo->clause);
			else
				op_expr = get_leftop(restrictinfo->clause);

			thisbucketsize = estimate_hash_bucketsize(root, op_expr,
													  virtualbuckets);
			if (innerbucketsize > thisbucketsize)
				innerbucketsize = thisbucketsize;
		}

		/*
		 * Pulls function cost of individual clauses
		 */
		cost_qual_eval(&hash_cost, hash_clause, root);
		cost_qual_eval(&qual_cost, qual_clause, root);
		cost_qual_eval(&host_cost, host_clause, root);
		/*
		 * Because cost_qual_eval returns cost value that assumes CPU
		 * execution, we need to adjust its ratio according to the score
		 * of GPU execution to CPU.
		 */
		hash_cost.per_tuple *= (pgstrom_gpu_operator_cost / cpu_operator_cost);
		qual_cost.per_tuple *= (pgstrom_gpu_operator_cost / cpu_operator_cost);

		/*
		 * The number of comparison according to hash_clauses and qual_clauses
		 * are the number of outer tuples, but right now PG-Strom does not
		 * support to divide hash table
		 */
		startup_cost += hash_cost.startup + qual_cost.startup;
		run_cost += ((hash_cost.per_tuple + qual_cost.per_tuple)
					 * outer_path_rows
					 * clamp_row_est(inner_path_rows * innerbucketsize) * 0.5);
	}

	/*
	 * Get approx # tuples passing the hashquals.  We use
	 * approx_tuple_count here because we need an estimate done with
	 * JOIN_INNER semantics.
	 */
	hashjointuples = approx_tuple_count(root, gpath);

	/*
	 * Also add cost for qualifiers to be run on host
	 */
	startup_cost += host_cost.startup;
	run_cost += (cpu_tuple_cost + host_cost.per_tuple) * hashjointuples;

	gpath->cpath.path.startup_cost = startup_cost;
	gpath->cpath.path.total_cost = startup_cost + run_cost;
}

/*
 * gpuhashjoin_add_path
 *
 * callback function invoked to check up GpuHashJoinPath.
 */
static void
gpuhashjoin_add_path(PlannerInfo *root,
					 RelOptInfo *joinrel,
					 JoinType jointype,
					 JoinCostWorkspace *core_workspace,
					 SpecialJoinInfo *sjinfo,
					 SemiAntiJoinFactors *semifactors,
					 Path *outer_path,
					 Path *inner_path,
					 List *restrict_clauses,
					 Relids required_outer,
					 List *hashclauses)
{
	GpuHashJoinPath	*gpath_new;
	ParamPathInfo  *ppinfo;
	List		   *hash_clause = NIL;
	List		   *qual_clause = NIL;
	List		   *host_clause = NIL;
	List		   *outer_clause;
	ListCell	   *cell;
	JoinCostWorkspace gpu_workspace;

	/* calls secondary module if exists */
	if (add_hashjoin_path_next)
		add_hashjoin_path_next(root,
							   joinrel,
							   jointype,
							   core_workspace,
							   sjinfo,
							   semifactors,
							   outer_path,
							   inner_path,
							   restrict_clauses,
							   required_outer,
							   hashclauses);

	/* nothing to do, if either PG-Strom or GpuHashJoin is not enabled */
	if (!pgstrom_enabled || !enable_gpuhashjoin)
		return;

	/*
	 * right now, only inner join is supported!
	 */
	if (jointype != JOIN_INNER)
		return;

	/*
	 * make a ParamPathInfo of this GpuHashJoin, according to the standard
	 * manner.
	 * XXX - needs to ensure whether it is actually harmless in case when
	 * multiple inner relations are planned to be cached.
	 */
	ppinfo = get_joinrel_parampathinfo(root,
									   joinrel,
									   outer_path,
									   inner_path,
									   sjinfo,
									   bms_copy(required_outer),
									   &restrict_clauses);

	/* reasonable portion of hash-clauses can be runnable on GPU */
	foreach (cell, restrict_clauses)
	{
		RestrictInfo   *rinfo = lfirst(cell);

		if (pgstrom_codegen_available_expression(rinfo->clause))
		{
			if (list_member_ptr(hashclauses, rinfo))
				hash_clause = lappend(hash_clause, rinfo);
			else
				qual_clause = lappend(qual_clause, rinfo);
		}
		else
			host_clause = lappend(host_clause, rinfo);
	}
	if (hash_clause == NIL)
		return;		/* no need to run it on GPU */

	/*
	 * creation of gpuhashjoin path, if no pull-up
	 */
	outer_clause = NIL;
	gpath_new = palloc0(offsetof(GpuHashJoinPath, inners[1]));
	gpath_new->cpath.path.type = T_CustomPath;
	gpath_new->cpath.path.pathtype = T_CustomPlan;
	gpath_new->cpath.path.parent = joinrel;
	gpath_new->cpath.path.param_info = ppinfo;
	gpath_new->cpath.path.pathkeys = NIL;
	/* other cost fields of Path shall be set later */
	gpath_new->cpath.methods = &gpuhashjoin_path_methods;
	gpath_new->num_rels = 1;
	gpath_new->outerpath = gpuscan_try_replace_seqscan_path(root, outer_path,
															&outer_clause);
	gpath_new->inners[0].scan_path = inner_path;
	gpath_new->inners[0].jointype = jointype;
	gpath_new->inners[0].hash_clause = hash_clause;
	gpath_new->inners[0].qual_clause = list_concat(qual_clause, outer_clause);
	gpath_new->inners[0].host_clause = host_clause;

	/* cost estimation and check availability */
	if (cost_gpuhashjoin(root, gpath_new,
						 required_outer,
						 &gpu_workspace))
	{
		if (add_path_precheck(joinrel,
							  gpu_workspace.startup_cost,
							  gpu_workspace.total_cost,
							  NULL, required_outer))
		{
			final_cost_gpuhashjoin(root, gpath_new, &gpu_workspace);
			add_path(joinrel, &gpath_new->cpath.path);
		}
	}

	/*
     * creation of gpuhashjoin path using sub-inner pull-up, if available
     */
	if (path_is_mergeable_gpuhashjoin(outer_path))
	{
		GpuHashJoinPath	   *gpath_sub = (GpuHashJoinPath *) outer_path;
		int		num_rels = gpath_sub->num_rels;

		outer_clause = NIL;
		gpath_new = palloc0(offsetof(GpuHashJoinPath, inners[num_rels + 1]));
		gpath_new->cpath.path.type = T_CustomPath;
		gpath_new->cpath.path.pathtype = T_CustomPlan;
		gpath_new->cpath.path.parent = joinrel;
		gpath_new->cpath.path.param_info = ppinfo;
		gpath_new->cpath.path.pathkeys = NIL;
		/* other cost fields of Path shall be set later */
		gpath_new->cpath.methods = &gpuhashjoin_path_methods;
		gpath_new->num_rels = gpath_sub->num_rels + 1;
		gpath_new->outerpath =
			gpuscan_try_replace_seqscan_path(root, gpath_sub->outerpath,
											 &outer_clause);
		memcpy(gpath_new->inners,
			   gpath_sub->inners,
			   offsetof(GpuHashJoinPath, inners[num_rels]) -
			   offsetof(GpuHashJoinPath, inners[0]));
		gpath_new->inners[num_rels].scan_path = inner_path;
		gpath_new->inners[num_rels].jointype = jointype;
		gpath_new->inners[num_rels].hash_clause = hash_clause;
		gpath_new->inners[num_rels].qual_clause = list_concat(qual_clause,
															  outer_clause);
		gpath_new->inners[num_rels].host_clause = host_clause;

		/* cost estimation and check availability */
		if (cost_gpuhashjoin(root, gpath_new,
							 required_outer,
							 &gpu_workspace))
		{
			if (add_path_precheck(joinrel,
								  gpu_workspace.startup_cost,
								  gpu_workspace.total_cost,
								  NULL, required_outer))
			{
				final_cost_gpuhashjoin(root, gpath_new, &gpu_workspace);
				add_path(joinrel, &gpath_new->cpath.path);
			}
		}
	}
}

/*
 * XXX - a workaround. once CustomPlan became based on CustomScan,
 * we can access Scan->scanrelid...
 */
typedef struct {
	CustomPlan	cplan;
    Index		scanrelid;
} GpuScanPlanDummy;

static bool
gpuhashjoin_use_bulkload(GpuHashJoin *ghjoin)
{
	Plan	   *outer_plan = outerPlan(ghjoin);
	Bitmapset  *outer_attrefs = NULL;
	Index		outer_scanrelid;
	ListCell   *lc;

	/*
	 * Only GpuScan support bulk-loading right now
	 */
	if (!pgstrom_plan_is_gpuscan(outer_plan))
		return false;
	outer_scanrelid = ((GpuScanPlanDummy *) outer_plan)->scanrelid;
	pull_varattnos((Node *)ghjoin->cplan.plan.targetlist,
				   outer_scanrelid,
				   &outer_attrefs);
	pull_varattnos((Node *)ghjoin->hash_clauses,
				   outer_scanrelid,
				   &outer_attrefs);
	pull_varattnos((Node *)ghjoin->qual_clauses,
				   outer_scanrelid,
				   &outer_attrefs);
	pull_varattnos((Node *)ghjoin->host_clauses,
				   outer_scanrelid,
				   &outer_attrefs);

	foreach (lc, outer_plan->targetlist)
	{
		TargetEntry	   *tle = lfirst(lc);
		int				x = tle->resno - FirstLowInvalidHeapAttributeNumber;

		if (!bms_is_member(x, outer_attrefs))
			continue;

		if (!IsA(tle->expr, Var) ||
			((Var *) tle->expr)->varattno != tle->resno)
			return false;
	}
	return true;
}

static CustomPlan *
gpuhashjoin_create_plan(PlannerInfo *root, CustomPath *best_path)
{
	GpuHashJoinPath *gpath = (GpuHashJoinPath *)best_path;
	GpuHashJoin		*ghjoin;
	Plan	   *prev_plan = NULL;
	List	   *join_types = NIL;
	List	   *hash_clauses = NIL;
	List	   *qual_clauses = NIL;
	List	   *host_clauses = NIL;
	int			i;


	ghjoin = palloc0(sizeof(GpuHashJoin));
	NodeSetTag(ghjoin, T_CustomPlan);
	ghjoin->cplan.methods = &gpuhashjoin_plan_methods;
	ghjoin->cplan.plan.targetlist
		= build_path_tlist(root, &gpath->cpath.path);
	ghjoin->cplan.plan.qual = NIL;	/* to be set later */
	outerPlan(ghjoin) = create_plan_recurse(root, gpath->outerpath);

	for (i=0; i < gpath->num_rels; i++)
	{
		MultiHash  *mhash;
		List	   *hash_clause = gpath->inners[i].hash_clause;
		List	   *qual_clause = gpath->inners[i].qual_clause;
		List	   *host_clause = gpath->inners[i].host_clause;
		Plan	   *scan_plan
			= create_plan_recurse(root, gpath->inners[i].scan_path);

		if (gpath->cpath.path.param_info)
		{
			hash_clause = (List *)
				replace_nestloop_params(root, (Node *) hash_clause);
			qual_clause = (List *)
				replace_nestloop_params(root, (Node *) qual_clause);
			host_clause = (List *)
				replace_nestloop_params(root, (Node *) host_clause);
		}
		/*
		 * Sort clauses into best execution order, even though it's
		 * uncertain whether it makes sense in GPU execution...
		 */
		hash_clause = order_qual_clauses(root, hash_clause);
		qual_clause = order_qual_clauses(root, qual_clause);
		host_clause = order_qual_clauses(root, host_clause);

		/* Get plan expression form */
		hash_clause = extract_actual_clauses(hash_clause, false);
		qual_clause = extract_actual_clauses(qual_clause, false);
		host_clause = extract_actual_clauses(host_clause, false);

		/* saved on the GpuHashJoin node */
		join_types = lappend_int(join_types, (int)gpath->inners[i].jointype);
		hash_clauses = lappend(hash_clauses, hash_clause);
		qual_clauses = lappend(qual_clauses, qual_clause);
		host_clauses = lappend(host_clauses, host_clause);

		/* Make a MultiHash node */
		mhash = palloc0(sizeof(MultiHash));
		NodeSetTag(mhash, T_CustomPlan);
		mhash->cplan.methods = &multihash_plan_methods;
		mhash->cplan.plan.startup_cost = scan_plan->total_cost;
		mhash->cplan.plan.total_cost = scan_plan->total_cost;
		mhash->cplan.plan.plan_rows = scan_plan->plan_rows;
		mhash->cplan.plan.plan_width = scan_plan->plan_width;
		mhash->cplan.plan.targetlist = scan_plan->targetlist;
		mhash->cplan.plan.qual = NIL;
		mhash->depth = i + 1;
		mhash->nslots = gpath->inners[i].ntuples;
		mhash->nloops = gpath->inners[i].nloops;
		mhash->threshold_ratio = gpath->inners[i].threshold_ratio;
		//mhash->chunk_size = gpath->inners[i].chunk_size;
		mhash->hashtable_size = gpath->hashtable_size;

		/* chain it under the GpuHashJoin */
		outerPlan(mhash) = scan_plan;
		if (prev_plan)
			innerPlan(prev_plan) = (Plan *) mhash;
		else
			innerPlan(ghjoin) = (Plan *) mhash;
		prev_plan = (Plan *) mhash;
	}
	ghjoin->num_rels = gpath->num_rels;
	ghjoin->join_types = join_types;
	ghjoin->hash_clauses = hash_clauses;
	ghjoin->qual_clauses = qual_clauses;
	ghjoin->host_clauses = host_clauses;
	ghjoin->outer_bulkload = gpuhashjoin_use_bulkload(ghjoin);

	return &ghjoin->cplan;
}

static void
gpuhashjoin_textout_path(StringInfo str, Node *node)
{
	GpuHashJoinPath *gpath = (GpuHashJoinPath *) node;
	char	   *temp;
	int			i;

	/* outerpath */
	temp = nodeToString(gpath->outerpath);
	appendStringInfo(str, " :outerpath %s", temp);

	/* num_rels */
	appendStringInfo(str, " :num_rels %d", gpath->num_rels);

	/* inners */
	appendStringInfo(str, " :num_rels (");
	for (i=0; i < gpath->num_rels; i++)
	{
		appendStringInfo(str, "{");
		/* path */
		temp = nodeToString(gpath->inners[i].scan_path);
		appendStringInfo(str, " :scan_path %s", temp);

		/* jointype */
		appendStringInfo(str, " :jointype %d",
						 (int)gpath->inners[i].jointype);

		/* hash_clause */
		temp = nodeToString(gpath->inners[i].hash_clause);
		appendStringInfo(str, " :hash_clause %s", temp);

		/* qual_clause */
		temp = nodeToString(gpath->inners[i].qual_clause);
		appendStringInfo(str, " :qual_clause %s", temp);

		/* host_clause */
		temp = nodeToString(gpath->inners[i].host_clause);
		appendStringInfo(str, " :host_clause %s", temp);

		appendStringInfo(str, "}");		
	}
	appendStringInfo(str, ")");
}

static void
gpuhashjoin_codegen_projection(StringInfo body,
							   GpuHashJoin *ghjoin,
							   codegen_context *context)
{
	Plan	   *plan = (Plan *) ghjoin;
	int			depth = 0;
	ListCell   *lc;

	/* materialize-mapping function */
	appendStringInfo(
		body,
		"\n"
		"static void\n"
		"gpuhashjoin_projection_mapping(cl_int dest_colidx,\n"
		"                               __private cl_uint *src_depth,\n"
		"                               __private cl_uint *src_colidx)\n"
		"{\n"
		"  switch (dest_colidx)\n"
		"  {\n");
	foreach (lc, ghjoin->pscan_vartrans)
	{
		vartrans_info  *vtrans = lfirst(lc);

		appendStringInfo(
			body,
			"  case %d:\n"
			"    *src_depth = %d;\n"
			"    *src_colidx = %d;\n"
			"    break;\n",
			vtrans->resno - 1,
			vtrans->srcdepth,
			vtrans->srcresno - 1);
	}
	appendStringInfo(
        body,
		"  default:\n"
		"    /* should not run here */\n"
		"    break;\n"
		"  }\n"
		"}\n"
		"\n");

	/* projection-datum function */
	appendStringInfo(
        body,
		"static void\n"
		"gpuhashjoin_projection_datum(__private cl_int *errcode,\n"
		"                             __global Datum *slot_values,\n"
		"                             __global cl_char *slot_isnull,\n"
		"                             cl_int depth,\n"
		"                             cl_int colidx,\n"
		"                             hostptr_t hostaddr,\n"
		"                             __global void *datum)\n"
		"{\n"
		"  switch (depth)\n"
		"  {\n");
	depth = 0;
    do {
        appendStringInfo(
            body,
            "  case %d:\n"
            "    switch (colidx)\n"
            "    {\n", depth);
        foreach (lc, ghjoin->pscan_vartrans)
        {
            vartrans_info  *vtrans = lfirst(lc);
            int16           typlen;
            bool            typbyval;

            if (vtrans->srcdepth != depth)
                continue;
			if (!vtrans->ref_host)
				continue;

            get_typlenbyval(vtrans->vartype, &typlen, &typbyval);
			if (typbyval)
			{
				char   *cl_type = NULL;

				appendStringInfo(
					body,
					"    case %d:\n"
					"      if (!datum)\n"
					"        slot_isnull[%d] = (cl_char) 1;\n"
					"      else\n"
					"      {\n"
					"        slot_isnull[%d] = (cl_char) 0;\n",
					vtrans->srcresno - 1,
                    vtrans->resno - 1,
                    vtrans->resno - 1);
				if (typlen == sizeof(cl_char))
					cl_type = "cl_char";
				else if (typlen == sizeof(cl_short))
					cl_type = "cl_short";
				else if (typlen == sizeof(cl_int))
					cl_type = "cl_int";
				else if (typlen == sizeof(cl_long))
					cl_type = "cl_long";

				if (cl_type)
				{
					appendStringInfo(
						body,
						"        slot_values[%d]"
						" = (Datum)(*((__global %s *) datum));\n",
						vtrans->resno - 1,
						cl_type);
				}
				else if (typlen < sizeof(Datum))
				{
					appendStringInfo(
						body,
						"        memcpy(&slot_values[%d], datum, %d);\n",
						vtrans->resno - 1,
						typlen);
				}
				else
					elog(ERROR, "Bug? unexpected type length (%d)", typlen);
				appendStringInfo(
					body,
					"      }\n"
					"      break;\n");
			}
			else
			{
				appendStringInfo(
					body,
					"    case %d:\n"
					"      if (!datum)\n"
					"        slot_isnull[%d] = (cl_char) 1;\n"
					"      else\n"
					"      {\n"
					"        slot_isnull[%d] = (cl_char) 0;\n"
					"        slot_values[%d] = (Datum) hostaddr;\n"
					"      }\n"
					"      break;\n",
					vtrans->srcresno - 1,
					vtrans->resno - 1,
					vtrans->resno - 1,
					vtrans->resno - 1);
			}
		}
        appendStringInfo(
            body,
            "    default: /* do nothing */ break;\n"
            "    }\n"
			"    break;\n");
        plan = innerPlan(plan);
        depth++;
    } while (plan);
    appendStringInfo(
        body,
        "  default: /* do nothing */ break;\n"
        "  }\n"
        "}\n");
}

static void
gpuhashjoin_codegen_recurse(StringInfo body,
							GpuHashJoin *ghjoin,
							MultiHash *mhash, int depth,
							codegen_context *context)
{
	MultiHash  *inner_hash = (MultiHash *) innerPlan(mhash);
	List	   *hash_clause = list_nth(ghjoin->hash_clauses, depth - 1);
	List	   *qual_clause = list_nth(ghjoin->qual_clauses, depth - 1);
	ListCell   *cell;
	char	   *clause;

	/*
	 * construct a hash-key in this nest-level
	 */
	appendStringInfo(body, "cl_uint hash_%u;\n\n", depth);
	appendStringInfo(body, "INIT_CRC32(hash_%u);\n", depth);
	foreach (cell, mhash->hash_outer_keys)
	{
		Node		   *expr = lfirst(cell);
		devtype_info   *dtype;
		char		   *temp;

		dtype = pgstrom_devtype_lookup(exprType(expr));
		Assert(dtype != NULL);
		temp = pgstrom_codegen_expression(expr, context);

		appendStringInfo(
			body,
			"hash_%u = pg_%s_hashkey(pg_crc32_table, hash_%u, %s);\n",
			depth, dtype->type_name, depth, temp);
		pfree(temp);
	}
	appendStringInfo(body, "FIN_CRC32(hash_%u);\n", depth);

	/*
	 * construct hash-table walking according to the hash-value
	 * calculated above
	 */
	appendStringInfo(
		body,
		"for (kentry_%d = KERN_HASH_FIRST_ENTRY(khtable_%d, hash_%d);\n"
		"     kentry_%d != NULL;\n"
		"     kentry_%d = KERN_HASH_NEXT_ENTRY(khtable_%d, kentry_%d))\n"
		"{\n",
		depth, depth, depth,
		depth,
		depth, depth, depth);

	/*
	 * construct variables that reference individual entries.
	 * (its value depends on the current entry, so it needs to be
	 * referenced within the loop)
	 */
	foreach (cell, ghjoin->pscan_vartrans)
	{
		vartrans_info  *vtrans = lfirst(cell);
		devtype_info   *dtype;

		if (!vtrans->ref_device || vtrans->srcdepth != depth)
			continue;
		dtype = pgstrom_devtype_lookup(vtrans->vartype);
		Assert(dtype != NULL);

		appendStringInfo(
			body,
			"pg_%s_t KVAR_%u = "
			"pg_%s_hashref(khtable_%d,kentry_%u,errcode,%u);\n",
			dtype->type_name,
			vtrans->resno,
			dtype->type_name,
			depth,
			depth,
			vtrans->srcresno - 1);
	}

	/*
	 * construct hash-key (and other qualifiers) comparison
	 */
	appendStringInfo(body,
					 "if (kentry_%d->hash == hash_%d",
					 depth, depth);
	foreach (cell, hash_clause)
	{
		clause = pgstrom_codegen_expression(lfirst(cell), context);
		appendStringInfo(body, " &&\n    EVAL(%s)", clause);
		pfree(clause);
	}

	foreach (cell, qual_clause)
	{
		clause = pgstrom_codegen_expression(lfirst(cell), context);
		appendStringInfo(body, " &&\n      EVAL(%s)", clause);
		pfree(clause);
	}
	appendStringInfo(body, ")\n{\n");

	/*
	 * If we have one more deeper hash-table, one nest level shall be added.
	 * Elsewhere, a code to put hash-join result and to increment the counter
	 * of matched items.
	 */
	if (inner_hash)
		gpuhashjoin_codegen_recurse(body, ghjoin,
									inner_hash, depth + 1,
									context);
	else
	{
		int		i;

		/*
		 * FIXME: needs to set negative value if host-recheck is needed
		 * (errcode: StromError_CpuReCheck)
		 */
		appendStringInfo(
			body,
			"n_matches++;\n"
			"if (rbuffer)\n"
			"{\n"
			"  rbuffer[0] = (cl_int)kds_index + 1;\n");	/* outer relation */
		for (i=1; i <= ghjoin->num_rels; i++)
			appendStringInfo(
				body,
				"  rbuffer[%d] = (cl_int)"
				"((uintptr_t)kentry_%d - (uintptr_t)khtable_%d);\n",
				i, i, i);
		appendStringInfo(
            body,
			"  rbuffer += %d;\n"
			"}\n",
			ghjoin->num_rels + 1);
	}
	appendStringInfo(body, "}\n");
	appendStringInfo(body, "}\n");
}

static char *
gpuhashjoin_codegen_type_declarations(codegen_context *context)
{
	StringInfoData	str;
	ListCell	   *cell;

	initStringInfo(&str);
	foreach (cell, context->type_defs)
	{
		devtype_info   *dtype = lfirst(cell);

		if (dtype->type_flags & DEVTYPE_IS_VARLENA)
		{
			appendStringInfo(&str,
							 "STROMCL_VARLENA_HASHKEY_TEMPLATE(%s)\n"
							 "STROMCL_VARLENA_HASHREF_TEMPLATE(%s)\n",
							 dtype->type_name,
							 dtype->type_name);
		}
		else
		{
			appendStringInfo(&str,
							 "STROMCL_SIMPLE_HASHKEY_TEMPLATE(%s,%s)\n"
							 "STROMCL_SIMPLE_HASHREF_TEMPLATE(%s,%s)\n",
							 dtype->type_name, dtype->type_base,
							 dtype->type_name, dtype->type_base);
		}
    }
    appendStringInfoChar(&str, '\n');

    return str.data;
}

static char *
gpuhashjoin_codegen(PlannerInfo *root,
					GpuHashJoin *ghjoin,
					codegen_context *context)
{
	StringInfoData	str;
	StringInfoData	decl;
	StringInfoData	body;
	ListCell	   *cell;
	int				depth;

	initStringInfo(&str);
	initStringInfo(&body);
	initStringInfo(&decl);
	pgstrom_init_codegen_context(context);

	/* declaration of gpuhashjoin_execute */
	appendStringInfo(
		&decl,
		"static cl_uint\n"
		"gpuhashjoin_execute(__private cl_int *errcode,\n"
		"                    __global kern_parambuf *kparams,\n"
		"                    __global kern_multihash *kmhash,\n"
		"                    __local cl_uint *pg_crc32_table,\n"
		"                    __global kern_data_store *kds,\n"
		"                    __global kern_data_store *ktoast,\n"
		"                    size_t kds_index,\n"
		"                    __global cl_int *rbuffer)\n"
		"{\n"
		);

	/* reference to each hash table */
	for (depth=1; depth <= ghjoin->num_rels; depth++)
	{
		appendStringInfo(
			&decl,
			"__global kern_hashtable *khtable_%d"
			" = KERN_HASHTABLE(kmhash,%d);\n",
			depth, depth);
	}
	/* variable for individual hash entries */
	for (depth=1; depth <= ghjoin->num_rels; depth++)
    {
        appendStringInfo(
            &decl,
            "__global kern_hashentry *kentry_%d;\n",
			depth);
	}

	/*
	 * declaration of variables that reference outer relations
	 */
	foreach (cell, ghjoin->pscan_vartrans)
	{
		vartrans_info  *vtrans = lfirst(cell);
		devtype_info   *dtype;

		if (vtrans->srcdepth != 0 || !vtrans->ref_device)
			continue;

		/* reference to the outer relation (kern_data_store) */
		dtype = pgstrom_devtype_lookup(vtrans->vartype);
		appendStringInfo(&body,
						 "pg_%s_t KVAR_%u = "
						 "pg_%s_vref(kds,ktoast,errcode,%u,kds_index);\n",
						 dtype->type_name,
						 vtrans->resno,
						 dtype->type_name,
						 vtrans->srcresno - 1);
	}
	/* misc variable definitions */
	appendStringInfo(&body,
					 "cl_int n_matches = 0;\n");

	/* nested loop for hash tables */
	gpuhashjoin_codegen_recurse(&body,
								ghjoin,
								(MultiHash *) innerPlan(ghjoin),
								1,
								context);

	/* end of gpuhashjoin_execute function */
	appendStringInfo(&body,
					 "return n_matches;\n"
					 "}\n");

	/* reference to kern_params */
	appendStringInfo(&decl, "%s",
					 pgstrom_codegen_param_declarations(context,
														context->param_refs));
	context->param_refs = NULL;

	/* integrate decl and body */
	appendStringInfo(&decl, "%s", body.data);

	/* also, gpuhashjoin_projection_datum() */
	gpuhashjoin_codegen_projection(&decl, ghjoin, context);

	/* put declarations of types/funcs/params */
	appendStringInfo(&str, "%s%s%s%s",
					 pgstrom_codegen_type_declarations(context),
					 gpuhashjoin_codegen_type_declarations(context),
					 pgstrom_codegen_func_declarations(context),
					 decl.data);

	/* include opencl_hashjoin.h */
	context->extra_flags |= DEVKERNEL_NEEDS_HASHJOIN;

	return str.data;
}

/*
 * build_pseudoscan_tlist
 *
 * GpuHashJoin performs like a scan-node that run on pseudo relation being
 * constructed with two source relations. Any (pseudo) columns in this
 * relation are, of course, reference to either inner or outer relation.
 */
typedef struct
{
	List	   *varlist;	/* list of either Var or PHV */
	List	   *varrefs;	/* list of variable reference flags */
	List	   *resnums;	/* list of pseudo resource number to be assigned */
	int			refmode;	/* bitmask to be put on the related varrefs */
} pscan_varlist_context;

static bool
build_pscan_varlist_walker(Node *node, pscan_varlist_context *context)
{
	ListCell   *lc1;
	ListCell   *lc2;
	ListCell   *lc3;

	if (!node)
		return false;
	if (IsA(node, Var) || IsA(node, PlaceHolderVar))
	{
		forthree (lc1, context->varlist,
				  lc2, context->varrefs,
				  lc3, context->resnums)
		{
			if (equal(node, lfirst(lc1)))
			{
				lfirst_int(lc2) |= context->refmode;
				return false;
			}
		}
		context->varlist = lappend(context->varlist, copyObject(node));
		context->varrefs = lappend_int(context->varrefs,
									   context->refmode);
		context->resnums = lappend_int(context->resnums,
									   list_length(context->resnums) + 1);
		return false;
	}
	return expression_tree_walker(node, build_pscan_varlist_walker,
								  (void *) context);
}

static List *
build_pseudo_scan_vartrans(GpuHashJoin *ghjoin)
{
	List	   *pscan_vartrans = NIL;
	List	   *pscan_varlist;
	List	   *pscan_varrefs;
	List	   *pscan_resnums;
	Plan	   *curr_plan;
	ListCell   *cell;
	int			depth;
	pscan_varlist_context context;

	/* check for top-level subplans */
	Assert(outerPlan(ghjoin) != NULL);
	Assert(plan_is_multihash(innerPlan(ghjoin)));

	/*
	 * build a pseudo-scan varlist/varhost - first of all, we pick up
	 * all the varnode (and place-holder) in the GpuHashJoin node and
	 * underlying MultiHash nodes.
	 */
	context.varlist = NIL;
	context.varrefs = NIL;
	context.resnums = NIL;
	context.refmode = 0x0001;	/* host referenced */
	curr_plan = &ghjoin->cplan.plan;
	build_pscan_varlist_walker((Node *)curr_plan->targetlist, &context);
	build_pscan_varlist_walker((Node *)curr_plan->qual, &context);
	build_pscan_varlist_walker((Node *)ghjoin->host_clauses, &context);
	context.refmode = 0x0002;	/* device referenced */
	build_pscan_varlist_walker((Node *)ghjoin->hash_clauses, &context);
	build_pscan_varlist_walker((Node *)ghjoin->qual_clauses, &context);
	pscan_varlist = context.varlist;
	pscan_varrefs = context.varrefs;
	pscan_resnums = context.resnums;

	/*
	 * Second, walks on the target list of outer relation of the GpuHashJoin
	 * and MultiHash nodes, to find out where is the source of the referenced
	 * variables.
	 */
	for (curr_plan = &ghjoin->cplan.plan, depth = 0;
		 curr_plan;
		 curr_plan = innerPlan(curr_plan), depth++)
	{
		Plan	   *outer_plan = outerPlan(curr_plan);
		List	   *temp_vartrans = NIL;
		ListCell   *lc1, *prev1 = NULL;
		ListCell   *lc2, *prev2 = NULL;
		ListCell   *lc3, *prev3 = NULL;
		int			num_device_vars = 0;

		Assert(depth==0 || plan_is_multihash(curr_plan));

		/*
		 * Construct a list of vartrans_info; It takes depth of source
		 * varnode, so we need to walk down the underlying inner relations.
		 */
	retry_from_head:
		forthree (lc1, pscan_varlist,
				  lc2, pscan_varrefs,
				  lc3, pscan_resnums)
		{
			Node   *node = lfirst(lc1);
			int		refmode = lfirst_int(lc2);
			int		resnum = lfirst_int(lc3);

			foreach (cell, outer_plan->targetlist)
			{
				TargetEntry	   *tle = lfirst(cell);

				if (equal(node, tle->expr))
				{
					vartrans_info *vtrans = palloc0(sizeof(vartrans_info));

					vtrans->srcdepth = depth;
					vtrans->srcresno = tle->resno;
					vtrans->resno = resnum;
					if (tle->resname)
						vtrans->resname = pstrdup(tle->resname);
					vtrans->vartype = exprType((Node *) tle->expr);
					vtrans->vartypmod = exprTypmod((Node *) tle->expr);
					vtrans->varcollid = exprCollation((Node *) tle->expr);
					if ((refmode & 0x0001) != 0)
						vtrans->ref_host = true;
					if ((refmode & 0x0002) != 0)
					{
						vtrans->ref_device = true;
						num_device_vars++;
					}
					vtrans->expr = copyObject(node);
					temp_vartrans = lappend(temp_vartrans, vtrans);
					/* remove this varnode; no longer needed */
					pscan_varlist = list_delete_cell(pscan_varlist,
													 lc1, prev1);
					pscan_varrefs = list_delete_cell(pscan_varrefs,
													 lc2, prev2);
					pscan_resnums = list_delete_cell(pscan_resnums,
													 lc3, prev3);
					if (prev1 == NULL)
						goto retry_from_head;
					lc1 = prev1;
					lc2 = prev2;
					lc3 = prev3;
					break;
				}
			}
			prev1 = lc1;
			prev2 = lc2;
			prev3 = lc3;
		}
		pscan_vartrans = list_concat(pscan_vartrans, temp_vartrans);
	}
	Assert(list_length(pscan_varlist) == 0);
#ifdef USE_ASSERT_CHECKING
	/*
	 * sanity checks - all the host referenced variables has to heve
	 * smaller resource number than device only variables, to keep
	 * consistent pseudo scan view.
	 */
	{
		int		max_resno_host = 0;
		int		min_resno_device = 0;

		foreach (cell, pscan_vartrans)
		{
			vartrans_info  *vtrans = lfirst(cell);

			if (vtrans->ref_host &&
				(!max_resno_host || max_resno_host < vtrans->resno))
				max_resno_host = vtrans->resno;
			if (!vtrans->ref_host &&
				(!min_resno_device || min_resno_device > vtrans->resno))
				min_resno_device = vtrans->resno;
		}
		Assert(!max_resno_host ||
			   !min_resno_device ||
			   max_resno_host < min_resno_device);
	}
#endif
	return pscan_vartrans;
}

static inline void
dump_pseudo_scan_vartrans(List *pscan_vartrans)
{
#if 1
	ListCell   *cell;
	int			index = 0;

	foreach (cell, pscan_vartrans)
	{
		vartrans_info  *vtrans = lfirst(cell);

		elog(INFO, "vtrans[%d] {srcdepth=%u srcresno=%d resno=%d resname='%s' vartype=%u vartypmod=%d varcollid=%u ref_host=%s ref_device=%s expr=%s}",
			 index,
			 vtrans->srcdepth,
			 vtrans->srcresno,
			 vtrans->resno,
			 vtrans->resname,
			 vtrans->vartype,
			 vtrans->vartypmod,
			 vtrans->varcollid,
			 vtrans->ref_host ? "true" : "false",
			 vtrans->ref_device ? "true" : "false",
			 nodeToString(vtrans->expr));
		index++;
	}
#endif
}

/*
 * fix_gpuhashjoin_expr
 *
 * It mutate expression node to reference pseudo scan relation, instead of
 * the raw relation.
 */
typedef struct
{
	PlannerInfo	   *root;
	List		   *pscan_vartrans;
	int				rtoffset;
} fix_gpuhashjoin_expr_context;

static Var *
search_vartrans_for_var(Var *varnode, List *pscan_vartrans, int rtoffset)
{
	ListCell	   *cell;

	foreach (cell, pscan_vartrans)
	{
		vartrans_info  *vtrans = lfirst(cell);
		Var		   *srcvar = (Var *) vtrans->expr;

		if (IsA(srcvar, Var) &&
			srcvar->varno == varnode->varno &&
			srcvar->varattno == varnode->varattno)
		{
			Var	   *newnode = copyObject(varnode);

			newnode->varno = INDEX_VAR;
			newnode->varattno = vtrans->resno;
			if (newnode->varnoold > 0)
				newnode->varnoold += rtoffset;
			return newnode;
		}
	}
	return NULL;	/* not found */
}

static Var *
search_vartrans_for_non_var(Node *node, List *pscan_vartrans, int rtoffset)
{
	ListCell	   *cell;

	foreach (cell, pscan_vartrans)
	{
		vartrans_info  *vtrans = lfirst(cell);

		if (equal(vtrans->expr, node))
		{
			Var	   *newnode = makeVar(INDEX_VAR,
									  vtrans->resno,
									  vtrans->vartype,
									  vtrans->vartypmod,
									  vtrans->varcollid,
									  0);
			return newnode;
		}
	}
	return NULL;
}

static Node *
fix_gpuhashjoin_expr_mutator(Node *node, fix_gpuhashjoin_expr_context *context)
{
	Var	   *newnode;

	if (!node)
		return NULL;
	if (IsA(node, Var))
	{
		newnode = search_vartrans_for_var((Var *)node,
										  context->pscan_vartrans,
										  context->rtoffset);
		if (newnode)
			return (Node *) newnode;
		/* No referent found for Var */
        elog(ERROR, "variable not found in the pseudo scan target lists");
	}
	else if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		newnode = search_vartrans_for_non_var(node,
											  context->pscan_vartrans,
											  context->rtoffset);
		if (newnode)
			return (Node *) newnode;
		/* If not supplied by input plans, evaluate the contained expr */
		return fix_gpuhashjoin_expr_mutator((Node *)phv->phexpr, context);
	}
	else if (IsA(node, Param))
	{
		/* XXX - logic copied from fix_param_node */
		Param	   *p = (Param *) node;

		if (p->paramkind == PARAM_MULTIEXPR)
		{
			PlannerInfo *root = context->root;
			int		subqueryid = p->paramid >> 16;
			int		colno = p->paramid & 0xFFFF;
			List   *params;

			if (subqueryid <= 0 ||
				subqueryid > list_length(root->multiexpr_params))
				elog(ERROR, "unexpected PARAM_MULTIEXPR ID: %d", p->paramid);
			params = (List *) list_nth(root->multiexpr_params, subqueryid - 1);
			if (colno <= 0 || colno > list_length(params))
				elog(ERROR, "unexpected PARAM_MULTIEXPR ID: %d", p->paramid);
			return copyObject(list_nth(params, colno - 1));
		}
		return copyObject(p);
	}
	else
	{
		/* Try matching more complex expressions too */
		newnode = search_vartrans_for_non_var(node,
											  context->pscan_vartrans,
											  context->rtoffset);
		if (newnode)
			return (Node *) newnode;
	}
	fix_expr_common(context->root, node);
	return expression_tree_mutator(node,
								   fix_gpuhashjoin_expr_mutator,
								   (void *) context);
}

static List *
fix_gpuhashjoin_expr(PlannerInfo *root,
					 Node *node,
					 List *pscan_vartrans,
					 int rtoffset)
{
	fix_gpuhashjoin_expr_context context;

	memset(&context, 0, sizeof(fix_gpuhashjoin_expr_context));
	context.root           = root;
	context.pscan_vartrans = pscan_vartrans;
	context.rtoffset       = rtoffset;

	return (List *) fix_gpuhashjoin_expr_mutator(node, &context);
}

/*
 * clause_in_depth
 *
 * It checks whether var-nodes in the supplied expression reference
 * the relation in a particular depth or not.
 */
typedef struct
{
	int		depth;
	List   *pscan_vartrans;
} clause_in_depth_context;

static bool
clause_in_depth_walker(Node *node, clause_in_depth_context *context)
{
	if (!node)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		ListCell   *cell;

		Assert(var->varno == INDEX_VAR &&
			   var->varattno > 0 &&
			   var->varattno <= list_length(context->pscan_vartrans));
		foreach (cell, context->pscan_vartrans)
		{
			vartrans_info *vtrans = lfirst(cell);

			if (vtrans->resno == var->varattno)
			{
				if (vtrans->srcdepth == context->depth)
					return false;
				return true;
			}
		}
		elog(ERROR, "Bug? pseudo scan tlist (resno=%u) not found",
			 var->varattno);
	}
	/* Should not find an unplanned subquery */
	Assert(!IsA(node, Query));
	return expression_tree_walker(node, clause_in_depth_walker,
								  (void *) context);
}

static bool
clause_in_depth(Node *node, List *pscan_vartrans, int depth)
{
	clause_in_depth_context context;

	context.depth = depth;
	context.pscan_vartrans = pscan_vartrans;

	if (!clause_in_depth_walker(node, &context))
		return true;
	return false;
}

/*
 * hashkey_setref_scanrel
 *
 * It returns an expression node that references outer relation according
 * to the supplied pscan_vartrans. The supplied expression has to be workable
 * on a particular depth
 */
typedef struct
{
	int		depth;
	List   *pscan_vartrans;
} hashkey_setref_scanrel_context;

static Node *
hashkey_setref_scanrel_mutator(Node *node,
							   hashkey_setref_scanrel_context *context)
{
	if (!node)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *oldvar = (Var *) node;
		Var		   *newvar;
		ListCell   *cell;

		Assert(oldvar->varno == INDEX_VAR &&
			   oldvar->varattno > 0 &&
			   oldvar->varattno <= list_length(context->pscan_vartrans));
		foreach (cell, context->pscan_vartrans)
		{
			vartrans_info *vtrans = lfirst(cell);

			if (vtrans->resno == oldvar->varattno)
			{
				Assert(oldvar->vartype == vtrans->vartype &&
					   oldvar->vartypmod == vtrans->vartypmod &&
					   oldvar->varcollid == vtrans->varcollid);
				newvar = copyObject(oldvar);
				newvar->varno = OUTER_VAR;
				newvar->varattno = vtrans->srcresno;
				return (Node *) newvar;
			}
		}
		elog(ERROR, "Bug? pseudo scan tlist (resno=%u) not found",
			 oldvar->varattno);
	}
	return expression_tree_mutator(node,
								   hashkey_setref_scanrel_mutator,
								   (void *) context);
}

static Node *
hashkey_setref_scanrel(Node *node, List *pscan_vartrans)
{
	hashkey_setref_scanrel_context context;

	context.depth = -1;
	context.pscan_vartrans = pscan_vartrans;
	return hashkey_setref_scanrel_mutator(node, &context);
}

/*
 * gpuhashjoin_set_plan_ref
 *
 * It fixes up varno and varattno according to the data format being
 * visible to targetlist or host_clauses. Unlike built-in join logics,
 * GpuHashJoin looks like a scan on a pseudo relation even though its
 * contents are actually consist of two different input streams.
 * So, note that it looks like all the columns are in outer relation,
 * however, GpuHashJoin manages the mapping which column come from
 * which column of what relation.
 */
static void
gpuhashjoin_set_plan_ref(PlannerInfo *root,
						 CustomPlan *custom_plan,
						 int rtoffset)
{
	GpuHashJoin	   *ghjoin = (GpuHashJoin *) custom_plan;
	MultiHash	   *mhash;
	List		   *pscan_vartrans;
	ListCell	   *lc1, *lc2;
	int				depth;
	codegen_context context;

	/*
	 * build a list of vartrans_info; that tracks which relation is
	 * the source of varnode on the pseudo scan relation
	 */
	pscan_vartrans = build_pseudo_scan_vartrans(ghjoin);
	ghjoin->pscan_vartrans = pscan_vartrans;

	/* fixup expression nodes according to the pscan_vartrans */
	ghjoin->cplan.plan.targetlist
		= fix_gpuhashjoin_expr(root,
							   (Node *)ghjoin->cplan.plan.targetlist,
							   ghjoin->pscan_vartrans,
							   rtoffset);
	ghjoin->cplan.plan.qual
		= fix_gpuhashjoin_expr(root,
							   (Node *)ghjoin->cplan.plan.qual,
							   ghjoin->pscan_vartrans,
							   rtoffset);
	ghjoin->hash_clauses
		= fix_gpuhashjoin_expr(root,
							   (Node *)ghjoin->hash_clauses,
							   ghjoin->pscan_vartrans,
							   rtoffset);
	ghjoin->qual_clauses
		= fix_gpuhashjoin_expr(root,
							   (Node *)ghjoin->qual_clauses,
							   ghjoin->pscan_vartrans,
							   rtoffset);
	ghjoin->host_clauses
		= fix_gpuhashjoin_expr(root,
							   (Node *)ghjoin->host_clauses,
							   ghjoin->pscan_vartrans,
							   rtoffset);
	/* picks up hash clauses */
	mhash = (MultiHash *) ghjoin;
	depth = 1;
	foreach (lc1, ghjoin->hash_clauses)
	{
		List   *hash_clause = lfirst(lc1);
		List   *hash_inner_keys = NIL;
		List   *hash_outer_keys = NIL;

		mhash = (MultiHash *) innerPlan(mhash);
		foreach (lc2, hash_clause)
		{
			OpExpr *oper = lfirst(lc2);
			Node   *i_expr;
			Node   *o_expr;

			if (!IsA(oper, OpExpr) || list_length(oper->args) != 2)
				elog(ERROR, "Binary OpExpr is expected in hash_clause: %s",
					 nodeToString(oper));
			if (clause_in_depth(linitial(oper->args),
								pscan_vartrans, depth))
			{
				i_expr = linitial(oper->args);
				o_expr = lsecond(oper->args);
			}
			else if (clause_in_depth(lsecond(oper->args),
									 pscan_vartrans, depth))
			{
				i_expr = lsecond(oper->args);
				o_expr = linitial(oper->args);
			}
			else
				elog(ERROR, "Unexpected OpExpr arguments: %s",
					 nodeToString(oper));
			/* See the comment in MultiHash declaration. 'i_expr' is used
			 * to calculate hash-value on construction of hentry, so it
			 * has to reference OUTER_VAR; that means relation being scanned.
			 */
			i_expr = hashkey_setref_scanrel(i_expr, pscan_vartrans);
			hash_inner_keys = lappend(hash_inner_keys, i_expr);
			hash_outer_keys = lappend(hash_outer_keys, o_expr);
		}
		mhash->hash_inner_keys = hash_inner_keys;
		mhash->hash_outer_keys = hash_outer_keys;
		depth++;
	}

	/* OK, let's generate kernel source code */
	ghjoin->kernel_source = gpuhashjoin_codegen(root, ghjoin, &context);
	ghjoin->extra_flags = context.extra_flags |
		(!devprog_enable_optimize ? DEVKERNEL_DISABLE_OPTIMIZE : 0);
	ghjoin->used_params = context.used_params;
	ghjoin->outer_attrefs = NULL;
	foreach (lc1, context.used_vars)
	{
		Var	   *var = lfirst(lc1);

		Assert(IsA(var, Var) && var->varno == INDEX_VAR);

		foreach (lc2, pscan_vartrans)
		{
			vartrans_info  *vtrans = lfirst(lc2);

			if (var->varattno == vtrans->resno && vtrans->srcdepth == 0)
			{
				ghjoin->outer_attrefs =
					bms_add_member(ghjoin->outer_attrefs,
								   vtrans->srcresno -
								   FirstLowInvalidHeapAttributeNumber);
				break;
			}
		}
	}
}

static void
gpuhashjoin_finalize_plan(PlannerInfo *root,
						  CustomPlan *custom_plan,
						  Bitmapset **paramids,
						  Bitmapset **valid_params,
						  Bitmapset **scan_params)
{
	GpuHashJoin	   *ghj = (GpuHashJoin *)custom_plan;

	finalize_primnode(root, (Node *)ghj->hash_clauses, *paramids);
	finalize_primnode(root, (Node *)ghj->qual_clauses, *paramids);
}


/*
 * gpuhashjoin_support_multi_exec (obsolete)
 *
 * It gives a hint whether the supplied plan-state support bulk-exec mode,
 * or not. If it is GpuHashJooin provided by PG-Strom, it does not allow
 * bulk- exec mode right now.
 */
bool
gpuhashjoin_support_multi_exec(const CustomPlanState *cps)
{
	return false;	/* not supported yet */
	/* we can issue bulk-exec mode if no projection */
	if (cps->ps.ps_ProjInfo == NULL)
		return true;
	return false;
}

/*
 * pgstrom_plan_is_gpuhashjoin
 *
 * It returns true, if supplied plan node is gpuhashjoin.
 */
bool
pgstrom_plan_is_gpuhashjoin(const Plan *plan)
{
	CustomPlan     *cplan = (CustomPlan *) plan;

	if (IsA(cplan, CustomPlan) &&
		cplan->methods == &gpuhashjoin_plan_methods)
		return true;
	return false;
}

/*
 * pgstrom_gpuhashjoin_setup_bulkslot
 *
 * 
 *
 */
void
pgstrom_gpuhashjoin_setup_bulkslot(PlanState *outer_ps,
								   ProjectionInfo **p_bulk_proj,
								   TupleTableSlot **p_bulk_slot)
{
	GpuHashJoinState   *ghjs = (GpuHashJoinState *) outer_ps;

	if (!IsA(ghjs, CustomPlanState) ||
		ghjs->cps.methods != &gpuhashjoin_plan_methods)
		elog(ERROR, "Bug? PlanState node is not GpuHashJoin");

	/* XXX - is it wider proj/slot? which is correct? */
	*p_bulk_proj = ghjs->pscan_projection;
	*p_bulk_slot = ghjs->pscan_slot;
}

/*
 * multihash_dump_tables
 *
 * For debugging, it dumps contents of multihash-tables
 */
static inline void
multihash_dump_tables(pgstrom_multihash_tables *mhtables)
{
	StringInfoData	str;
	int		i, j;

	initStringInfo(&str);
	for (i=1; i <= mhtables->kern.ntables; i++)
	{
		kern_hashtable *khash = KERN_HASHTABLE(&mhtables->kern, i);

		elog(INFO, "----hashtable[%d] {nslots=%u ncols=%u} ------------",
			 i, khash->nslots, khash->ncols);
		for (j=0; j < khash->ncols; j++)
		{
			elog(INFO, "colmeta {attbyval=%d attalign=%d attlen=%d "
				 "attnum=%d attcacheoff=%d}",
				 khash->colmeta[j].attbyval,
				 khash->colmeta[j].attalign,
				 khash->colmeta[j].attlen,
				 khash->colmeta[j].attnum,
				 khash->colmeta[j].attcacheoff);
		}

		for (j=0; j < khash->nslots; j++)
		{
			kern_hashentry *kentry;

			for (kentry = KERN_HASH_FIRST_ENTRY(khash, j);
				 kentry;
				 kentry = KERN_HASH_NEXT_ENTRY(khash, kentry))
			{
				elog(INFO, "entry[%d] hash=%08x rowid=%u t_len=%u",
					 j, kentry->hash, kentry->rowid, kentry->t_len);
			}
		}
	}
}

static void
setup_pseudo_scan_slot(GpuHashJoinState *ghjs, bool is_fallback)
{
	EState		   *estate = ghjs->cps.ps.state;
	TupleDesc		tupdesc;
	TupleTableSlot *slot;
	ProjectionInfo *projection;
	int				i, nattrs = 0;
	bool			has_oid;

	for (i=0; i < ghjs->pscan_nattrs; i++)
	{
		vartrans_info  *vtrans = &ghjs->pscan_vartrans[i];

		if (!is_fallback && !vtrans->ref_host)
			continue;
		if (nattrs < ghjs->pscan_vartrans[i].resno)
			nattrs = ghjs->pscan_vartrans[i].resno;
	}

	/* construct a pseudo scan slot for this */
	if (!ExecContextForcesOids(&ghjs->cps.ps, &has_oid))
		has_oid = false;
	tupdesc = CreateTemplateTupleDesc(nattrs, has_oid);

	/* dummy */
	for (i=1; i <= nattrs; i++)
		TupleDescInitEntry(tupdesc, i, NULL, INT4OID, -1, 0);

	for (i=0; i < ghjs->pscan_nattrs; i++)
	{
		vartrans_info   *vtrans = &ghjs->pscan_vartrans[i];

		if (!is_fallback && !vtrans->ref_host)
			continue;
		TupleDescInitEntry(tupdesc,
						   vtrans->resno,
						   vtrans->resname,
						   vtrans->vartype,
						   vtrans->vartypmod,
						   0);
		TupleDescInitEntryCollation(tupdesc,
									vtrans->resno,
									vtrans->varcollid);
	}
	slot = ExecAllocTableSlot(&estate->es_tupleTable);
	ExecSetSlotDescriptor(slot, tupdesc);

	/* make a projection if needed */
	if (tlist_matches_tupdesc(&ghjs->cps.ps,
							  ghjs->cps.ps.plan->targetlist,
							  INDEX_VAR,
							  tupdesc))
		projection = NULL;
	else
		projection = ExecBuildProjectionInfo(ghjs->cps.ps.targetlist,
											 ghjs->cps.ps.ps_ExprContext,
											 ghjs->cps.ps.ps_ResultTupleSlot,
											 tupdesc);
	if (!is_fallback)
	{
		ghjs->pscan_slot = slot;
		ghjs->pscan_projection = projection;
	}
	else
	{
		ghjs->pscan_wider_slot = slot;
		ghjs->pscan_wider_projection = projection;
	}
}

static int
pscan_vartrans_comp(const void *v1, const void *v2)
{
	const vartrans_info	   *vtrans1 = v1;
	const vartrans_info	   *vtrans2 = v2;

	if (vtrans1->srcdepth < vtrans2->srcdepth)
		return -1;
	if (vtrans1->srcdepth > vtrans2->srcdepth)
		return  1;
	if (vtrans1->srcresno < vtrans2->srcresno)
		return -1;
	if (vtrans1->srcresno > vtrans2->srcresno)
		return  1;
	return 0;
}

static inline List *
ExecInitExprOnlyValid(List *clauses_list, PlanState *pstate)
{
	List	   *results = NIL;
	ListCell   *lc1;
	ListCell   *lc2;

	foreach (lc1, clauses_list)
	{
		List	   *expr_list = lfirst(lc1);

		Assert(expr_list == NIL || IsA(expr_list, List));
		foreach (lc2, expr_list)
		{
			Expr   *expr = lfirst(lc2);

			if (expr)
				results = lappend(results, ExecInitExpr(expr, pstate));
		}
	}
	return results;
}

static CustomPlanState *
gpuhashjoin_begin(CustomPlan *node, EState *estate, int eflags)
{
	GpuHashJoin		   *ghjoin = (GpuHashJoin *) node;
	GpuHashJoinState   *ghjs;
	ListCell		   *cell;
	int					i, outer_width;

	/*
	 * create a state structure
	 */
	ghjs = palloc0(sizeof(GpuHashJoinState));
	NodeSetTag(ghjs, T_CustomPlanState);
	ghjs->cps.ps.plan = &node->plan;
	ghjs->cps.ps.state = estate;
	ghjs->cps.methods = &gpuhashjoin_plan_methods;
	ghjs->join_types = copyObject(ghjoin->join_types);

	/*
	 * create expression context
	 */
	ExecAssignExprContext(estate, &ghjs->cps.ps);

	/*
	 * initialize child expression
	 */
	ghjs->cps.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist, &ghjs->cps.ps);
	Assert(!node->plan.qual);
	ghjs->hash_clauses = ExecInitExprOnlyValid(ghjoin->hash_clauses,
											   &ghjs->cps.ps);
	ghjs->qual_clauses = ExecInitExprOnlyValid(ghjoin->qual_clauses,
											   &ghjs->cps.ps);
	ghjs->host_clauses = ExecInitExprOnlyValid(ghjoin->host_clauses,
											   &ghjs->cps.ps);
	/*
	 * initialize child nodes
	 */
	outerPlanState(ghjs) = ExecInitNode(outerPlan(ghjoin), estate, eflags);
	innerPlanState(ghjs) = ExecInitNode(innerPlan(ghjoin), estate, eflags);

	/* rough estimation of number of tuples per page on the outer relation */
	outer_width = outerPlanState(ghjs)->plan->plan_width;
	ghjs->ntups_per_page =
		((double)(BLCKSZ - MAXALIGN(SizeOfPageHeaderData))) /
		((double)(sizeof(ItemIdData) +
				  sizeof(HeapTupleHeaderData) + outer_width));

	/*
	 * initialize result tuple type and projection info
	 */
	ExecInitResultTupleSlot(estate, &ghjs->cps.ps);
	ExecAssignResultTypeFromTL(&ghjs->cps.ps);

	/*
	 * initialize "pseudo" scan slot - we use two types of pseudo scan slot;
	 * one contains var-nodes referenced in host expression only, to avoid
	 * unnecessary projection in usual cases. the other one contains all
	 * the var-nodes referenced in both of host and device expression to
	 * handle host retrying.
	 */
	ghjs->pscan_nattrs = list_length(ghjoin->pscan_vartrans);
	ghjs->pscan_vartrans = palloc(sizeof(vartrans_info) * ghjs->pscan_nattrs);
	i = 0;
	foreach (cell, ghjoin->pscan_vartrans)
	{
		vartrans_info *vtrans = lfirst(cell);

		memcpy(&ghjs->pscan_vartrans[i], vtrans, sizeof(vartrans_info));
		i++;
	}
	qsort(ghjs->pscan_vartrans,
		  ghjs->pscan_nattrs,
		  sizeof(vartrans_info),
		  pscan_vartrans_comp);
	setup_pseudo_scan_slot(ghjs, false);
	setup_pseudo_scan_slot(ghjs, true);

	/*
	 * estimate average ratio to populete join results towards the supplied
	 * input records, but we ensure results buffer to keep same number.
	 */
	ghjs->row_population_ratio = (ghjoin->cplan.plan.plan_rows /
								  outerPlan(ghjoin)->plan_rows);
	if (ghjs->row_population_ratio < 1.0)
		ghjs->row_population_ratio = 1.0;
	if (ghjs->row_population_ratio > 5.0)
	{
		elog(NOTICE, "row population ratio (%.2f) too large, rounded to 5.0",
			 ghjs->row_population_ratio);
		ghjs->row_population_ratio = 5.0;
	}

	/*
	 * Is bulk-scan available on the outer node?
	 * If CustomPlan provided by PG-Strom, it may be able to produce bulk
	 * data chunk, instead of row-by-row format.
	 */
	ghjs->outer_bulkload = ghjoin->outer_bulkload;

	/* construction of kernel parameter buffer */
	ghjs->kparams = pgstrom_create_kern_parambuf(ghjoin->used_params,
												 ghjs->cps.ps.ps_ExprContext);

	/*
	 * Setting up a kernel program and message queue
	 */
	Assert(ghjoin->kernel_source != NULL);
	ghjs->dprog_key = pgstrom_get_devprog_key(ghjoin->kernel_source,
											  ghjoin->extra_flags);
	pgstrom_track_object((StromObject *)ghjs->dprog_key, 0);

	ghjs->mqueue = pgstrom_create_queue();
	pgstrom_track_object(&ghjs->mqueue->sobj, 0);

	/* Is perfmon needed? */
	ghjs->pfm.enabled = pgstrom_perfmon_enabled;

	return &ghjs->cps;
}

static void
pgstrom_release_gpuhashjoin(pgstrom_message *message)
{
	pgstrom_gpuhashjoin *gpuhashjoin = (pgstrom_gpuhashjoin *) message;

	/* unlink message queue and device program */
	pgstrom_put_queue(gpuhashjoin->msg.respq);
    pgstrom_put_devprog_key(gpuhashjoin->dprog_key);

	/* unlink hashjoin-table */
	multihash_put_tables(gpuhashjoin->mhtables);

	/* unlink outer data store */
	if (gpuhashjoin->pds)
		pgstrom_put_data_store(gpuhashjoin->pds);

	/* unlink destination data store */
	if (gpuhashjoin->pds_dest)
		pgstrom_put_data_store(gpuhashjoin->pds_dest);

	/* release this message itself */
	pgstrom_shmem_free(gpuhashjoin);
}

static pgstrom_gpuhashjoin *
pgstrom_create_gpuhashjoin(GpuHashJoinState *ghjs,
						   pgstrom_bulkslot *bulk,
						   int result_format)
{
	pgstrom_multihash_tables *mhtables = ghjs->mhtables;
	pgstrom_gpuhashjoin	*gpuhashjoin;
	pgstrom_data_store *pds_dest;
	pgstrom_data_store *pds = bulk->pds;
	kern_data_store	   *kds = pds->kds;
	cl_int				nvalids = bulk->nvalids;
	cl_int				nrels = mhtables->kern.ntables;
	cl_uint				nrooms;
	Size				required;
	TupleDesc			tupdesc;
	kern_hashjoin	   *khashjoin;
	kern_resultbuf	   *kresults;
	kern_parambuf	   *kparams;
	kern_row_map	   *krowmap;

	/*
	 * Allocation of pgstrom_gpuhashjoin message object
	 */
	required = (offsetof(pgstrom_gpuhashjoin, khashjoin) +
				STROMALIGN(ghjs->kparams->length) +
				STROMALIGN(sizeof(kern_resultbuf)) +
				(nvalids < 0 ?
				 STROMALIGN(offsetof(kern_row_map, rindex[0])) :
				 STROMALIGN(offsetof(kern_row_map, rindex[nvalids]))));
	gpuhashjoin = pgstrom_shmem_alloc(required);
	if (!gpuhashjoin)
		elog(ERROR, "out of shared memory");

	/* initialization of the common message field */
	pgstrom_init_message(&gpuhashjoin->msg,
						 StromTag_GpuHashJoin,
						 ghjs->mqueue,
						 clserv_process_gpuhashjoin,
						 pgstrom_release_gpuhashjoin,
						 ghjs->pfm.enabled);
	/* initialization of other fields also */
	gpuhashjoin->dprog_key = pgstrom_retain_devprog_key(ghjs->dprog_key);
	gpuhashjoin->mhtables = multihash_get_tables(mhtables);
	gpuhashjoin->pds = pds;
	gpuhashjoin->pds_dest = NULL;		/* to be set below */
	khashjoin = &gpuhashjoin->khashjoin;

	/* setup kern_parambuf */
	kparams = KERN_HASHJOIN_PARAMBUF(khashjoin);
	memcpy(kparams, ghjs->kparams, ghjs->kparams->length);

	/* setup kern_resultbuf */
	nrooms = (cl_uint)((double)(nvalids < 0 ? kds->nitems : nvalids) *
					   ghjs->row_population_ratio * 1.1);
	kresults = KERN_HASHJOIN_RESULTBUF(khashjoin);
    memset(kresults, 0, sizeof(kern_resultbuf));
	kresults->nrels = nrels + 1;
	kresults->nrooms = nrooms;
	kresults->nitems = 0;
	kresults->errcode = StromError_Success;

	/* setup kern_row_map */
	krowmap = KERN_HASHJOIN_ROWMAP(khashjoin);
	if (nvalids < 0)
		krowmap->nvalids = -1;
	else
	{
		krowmap->nvalids = nvalids;
		memcpy(krowmap->rindex, bulk->rindex, sizeof(cl_int) * nvalids);
	}

	/*
	 * Once a pgstrom_data_store connected to the pgstrom_gpuhashjoin
	 * structure, it becomes pgstrom_release_gpuhashjoin's role to
	 * unlink this data-store. So, we don't need to track individual
	 * data-store no longer.
	 */
	pgstrom_untrack_object(&pds->sobj);
	pgstrom_track_object(&gpuhashjoin->msg.sobj, 0);

	/*
	 * allocation of the destination data-store
	 */
	//tupdesc = ghjs->pscan_wider_slot->tts_tupleDescriptor;
	tupdesc = ghjs->pscan_slot->tts_tupleDescriptor;
	if (result_format == KDS_FORMAT_TUPSLOT)
		pds_dest = pgstrom_create_data_store_tupslot(tupdesc, nrooms);
	else if (result_format == KDS_FORMAT_ROW_FLAT)
	{
		int		plan_width = ghjs->cps.ps.plan->plan_width;
		Size	length;

		length = (STROMALIGN(offsetof(kern_data_store,
									  colmeta[tupdesc->natts])) +
				  STROMALIGN(sizeof(kern_rowitem) * nrooms) +
				  (MAXALIGN(offsetof(HeapTupleHeaderData,
									 t_bits[BITMAPLEN(tupdesc->natts)]) +
							(tupdesc->tdhasoid ? sizeof(Oid) : 0)) +
				   MAXALIGN(plan_width)) * nrooms);
		pds_dest = pgstrom_create_data_store_row_flat(tupdesc, length);
	}
	else
		elog(ERROR, "Bug? unexpected result format: %d", result_format);
	gpuhashjoin->pds_dest = pds_dest;

	return gpuhashjoin;
}

static pgstrom_gpuhashjoin *
gpuhashjoin_load_next_chunk(GpuHashJoinState *ghjs, int result_format)
{
	PlanState		   *subnode = outerPlanState(ghjs);
	TupleDesc			tupdesc = ExecGetResultType(subnode);
	pgstrom_gpuhashjoin *gpuhashjoin = NULL;
	pgstrom_bulkslot	bulkdata;
	pgstrom_bulkslot   *bulk = NULL;
	struct timeval		tv1, tv2, tv3;

	/*
	 * Logic to fetch inner multihash-table looks like nested-loop.
	 * If all the underlying inner scan already scaned its outer relation,
	 * current depth makes advance its scan pointer with reset of underlying
	 * scan pointer, or returns NULL if it is already reached end of scan.
	 */
retry:
	if (ghjs->pfm.enabled)
		gettimeofday(&tv1, NULL);

	if (ghjs->outer_done || !ghjs->mhtables)
	{
		PlanState	   *inner_ps = innerPlanState(ghjs);
		MultiHashNode  *mhnode;

		/* unlink the previous pgstrom_multihash_tables */
		if (ghjs->mhtables)
		{
			pgstrom_multihash_tables *mhtables = ghjs->mhtables;

			Assert(ghjs->outer_done);	/* should not be the first call */
			pgstrom_untrack_object(&mhtables->sobj);
			multihash_put_tables(mhtables);
			ghjs->mhtables = NULL;
		}
		/* load an inner hash-table */
		mhnode = (MultiHashNode *) MultiExecProcNode(inner_ps);
		if (!mhnode)
		{
			if (ghjs->pfm.enabled)
			{
				gettimeofday(&tv2, NULL);
				ghjs->pfm.time_inner_load += timeval_diff(&tv1, &tv2);
			}
			return NULL;	/* end of inner multi-hashtable */
		}
		ghjs->mhtables = mhnode->mhtables;
		pfree(mhnode);

		/* rewind the outer scan for the new inner hash table */
		if (ghjs->outer_done)
		{
			ExecReScan(outerPlanState(ghjs));
			ghjs->outer_done = false;
		}
	}

	if (ghjs->pfm.enabled)
		gettimeofday(&tv2, NULL);

	if (!ghjs->outer_bulkload)
	{
		/* Scan the outer relation using row-by-row mode */
		pgstrom_data_store *pds = NULL;

		while (true)
		{
			TupleTableSlot *slot;

			if (ghjs->outer_overflow)
			{
				slot = ghjs->outer_overflow;
				ghjs->outer_overflow = NULL;
			}
			else
			{
				slot = ExecProcNode(subnode);
				if (TupIsNull(slot))
				{
					ghjs->outer_done = true;
					break;
				}
			}
			/* create a new data-store if not constructed yet */
			if (!pds)
			{
				pds = pgstrom_create_data_store_row(tupdesc,
													pgstrom_chunk_size << 20,
													ghjs->ntups_per_page);
				pgstrom_track_object(&pds->sobj, 0);
			}
			/* insert the tuple on the data-store */
			if (!pgstrom_data_store_insert_tuple(pds, slot))
			{
				ghjs->outer_overflow = slot;
				break;
			}
		}
		if (pds)
		{
			memset(&bulkdata, 0, sizeof(pgstrom_bulkslot));
			bulkdata.pds = pds;
			bulkdata.nvalids = -1;	/* all valid */
			bulk = &bulkdata;
		}
	}
	else
	{
		/*
		 * FIXME: Right now, bulk-loading is supported only when target-list
		 * of the underlyin relation has compatible layout.
		 * It reduces the cases when we can apply bulk loding, however, it
		 * can be revised later.
		 * An idea is to fix-up target list on planner stage to fit bulk-
		 * loading.
		 */

		/* load a bunch of records at once */
		bulk = (pgstrom_bulkslot *) MultiExecProcNode(subnode);
		if (!bulk)
			ghjs->outer_done = true;
	}
	if (ghjs->pfm.enabled)
	{
		gettimeofday(&tv3, NULL);
		ghjs->pfm.time_inner_load += timeval_diff(&tv1, &tv2);
		ghjs->pfm.time_outer_load += timeval_diff(&tv2, &tv3);
	}

	if (bulk)
		gpuhashjoin = pgstrom_create_gpuhashjoin(ghjs, bulk, result_format);
	else
		goto retry;

	return gpuhashjoin;
}

static bool
gpuhashjoin_next_tuple(GpuHashJoinState *ghjs,
					   TupleTableSlot **p_slot,
					   ProjectionInfo **p_projection)
{
	pgstrom_gpuhashjoin	   *gpuhashjoin = ghjs->curr_ghjoin;
	pgstrom_data_store	   *pds_dest = gpuhashjoin->pds_dest;
	kern_data_store		   *kds_dest = pds_dest->kds;
	struct timeval			tv1, tv2;

	/*
	 * TODO: All fallback code here
	 */
	Assert(kds_dest->format == KDS_FORMAT_TUPSLOT);

	if (ghjs->pfm.enabled)
		gettimeofday(&tv1, NULL);

	while (ghjs->curr_index < kds_dest->nitems)
	{
		TupleTableSlot *pslot = ghjs->pscan_slot;
		ProjectionInfo *projection = ghjs->pscan_projection;
		TupleDesc		tupdesc = pslot->tts_tupleDescriptor;
		Datum		   *tts_values;
		cl_char		   *tts_isnull;
		int				index = ghjs->curr_index++;

		/* fetch a result tuple */
		ExecClearTuple(pslot);
		tts_values = KERN_DATA_STORE_VALUES(kds_dest, index);
		tts_isnull = KERN_DATA_STORE_ISNULL(kds_dest, index);
		Assert(tts_values != NULL && tts_isnull != NULL);
		memcpy(pslot->tts_values, tts_values,
			   sizeof(Datum) * tupdesc->natts);
		memcpy(pslot->tts_isnull, tts_isnull,
			   sizeof(bool) * tupdesc->natts);
		ExecStoreVirtualTuple(pslot);

		if (ghjs->host_clauses != NIL)
		{
			ExprContext	   *econtext = ghjs->cps.ps.ps_ExprContext;

			econtext->ecxt_scantuple = pslot;
			if (!ExecQual(ghjs->host_clauses, econtext, false))
				continue;	/* try to fetch next tuple */
		}

		if (ghjs->pfm.enabled)
		{
			gettimeofday(&tv2, NULL);
			ghjs->pfm.time_materialize += timeval_diff(&tv1, &tv2);
		}
		*p_slot = pslot;
		*p_projection = projection;
		return true;
	}

	if (ghjs->pfm.enabled)
	{
		gettimeofday(&tv2, NULL);
		ghjs->pfm.time_materialize += timeval_diff(&tv1, &tv2);
	}
	*p_slot = NULL;
	*p_projection = NULL;
	return false;
}

static pgstrom_gpuhashjoin *
pgstrom_fetch_gpuhashjoin(GpuHashJoinState *ghjs,
						  bool *needs_recheck,
						  int result_format)
{
	pgstrom_message	   *msg;
	pgstrom_gpuhashjoin *ghjoin;
	dlist_node			*dnode;

	/*
	 * Keep number of asynchronous hashjoin request a particular level,
	 * unless it does not exceed pgstrom_max_async_chunks and any new
	 * response is not replied during the loading.
	 */
	while (!ghjs->outer_done &&
		   ghjs->num_running <= pgstrom_max_async_chunks)
	{
		pgstrom_gpuhashjoin *ghjoin
			= gpuhashjoin_load_next_chunk(ghjs, result_format);
		if (!ghjoin)
			break;	/* outer scan reached to end of the relation */

		if (!pgstrom_enqueue_message(&ghjoin->msg))
		{
			pgstrom_put_message(&ghjoin->msg);
			elog(ERROR, "failed to enqueue pgstrom_gpuhashjoin message");
		}
		ghjs->num_running++;

		msg = pgstrom_try_dequeue_message(ghjs->mqueue);
		if (msg)
		{
			ghjs->num_running--;
			dlist_push_tail(&ghjs->ready_pscans, &msg->chain);
			break;
		}
	}

	/*
	 * wait for server's response if no available chunks were replied
	 */
	if (dlist_is_empty(&ghjs->ready_pscans))
	{
		if (ghjs->num_running == 0)
			return NULL;
		msg = pgstrom_dequeue_message(ghjs->mqueue);
		if (!msg)
			elog(ERROR, "message queue wait timeout");
		ghjs->num_running--;
		dlist_push_tail(&ghjs->ready_pscans, &msg->chain);
	}

	/*
	 * picks up next available chunks, if any
	 */
	Assert(!dlist_is_empty(&ghjs->ready_pscans));
	dnode = dlist_pop_head_node(&ghjs->ready_pscans);
	ghjoin = dlist_container(pgstrom_gpuhashjoin, msg.chain, dnode);

	/*
	 * Raise an error, if significan error was reported
	 */
	if (ghjoin->msg.errcode != StromError_Success)
	{
#if 0
		/* FIXME: Go to fallback case if CPUReCheck or OutOfSharedMemory */
		if (ghjoin->msg.errcode == StromError_CpuReCheck ||
			ghjoin->msg.errcode == StromError_OutOfSharedMemory)
			*needs_recheck = true;
		else
#endif
		if (ghjoin->msg.errcode == CL_BUILD_PROGRAM_FAILURE)
		{
			const char *buildlog
				= pgstrom_get_devprog_errmsg(ghjoin->dprog_key);
			const char *kern_source
				= ((GpuHashJoin *)ghjs->cps.ps.plan)->kernel_source;

			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("PG-Strom: OpenCL execution error (%s)\n%s",
							pgstrom_strerror(ghjoin->msg.errcode),
							kern_source),
					 errdetail("%s", buildlog)));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("PG-Strom: OpenCL execution error (%s)",
							pgstrom_strerror(ghjoin->msg.errcode))));
		}
	}
	else
		*needs_recheck = false;
	return ghjoin;
}

static TupleTableSlot *
gpuhashjoin_exec(CustomPlanState *node)
{
	GpuHashJoinState   *ghjs = (GpuHashJoinState *) node;
	TupleTableSlot	   *pscan_slot = NULL;
	ProjectionInfo	   *pscan_proj = NULL;
	pgstrom_gpuhashjoin *ghjoin;

	while (!ghjs->curr_ghjoin ||
		   !gpuhashjoin_next_tuple(ghjs, &pscan_slot, &pscan_proj))
	{
		pgstrom_message	   *msg;

		/*
		 * Release previous hashjoin chunk that
		 * should be already fetched.
		 */
		if (ghjs->curr_ghjoin)
		{
			msg = &ghjs->curr_ghjoin->msg;
			if (msg->pfm.enabled)
				pgstrom_perfmon_add(&ghjs->pfm, &msg->pfm);
			Assert(msg->refcnt == 1);
			pgstrom_untrack_object(&msg->sobj);
			pgstrom_put_message(msg);
			ghjs->curr_ghjoin = NULL;
			ghjs->curr_index = 0;
		}
		/*
		 * Fetch a next hashjoin chunk already processed
		 */
		ghjoin = pgstrom_fetch_gpuhashjoin(ghjs, &ghjs->curr_recheck,
										   KDS_FORMAT_TUPSLOT);
		if (!ghjoin)
			break;
		ghjs->curr_ghjoin = ghjoin;
		ghjs->curr_index = 0;
	}
	/* can valid tuple be fetched? */
	if (TupIsNull(pscan_slot))
		return pscan_slot;

	/* needs to apply projection? */
	if (pscan_proj)
	{
		ExprContext	   *econtext = ghjs->cps.ps.ps_ExprContext;
		ExprDoneCond	is_done;

		econtext->ecxt_scantuple = pscan_slot;
		return ExecProject(pscan_proj, &is_done);
	}
	return pscan_slot;
}

static Node *
gpuhashjoin_exec_multi(CustomPlanState *node)
{
	GpuHashJoinState	   *ghjs = (GpuHashJoinState *) node;
	pgstrom_gpuhashjoin	   *ghjoin;
	pgstrom_data_store	   *pds;
	pgstrom_data_store	   *pds_dest;
	pgstrom_bulkslot	   *bulk = NULL;

	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStartNode(node->ps.instrument);

	while (true)
	{
		bool		needs_rechecks;
		cl_uint		nitems;
		cl_uint		i, j;

		ghjoin = pgstrom_fetch_gpuhashjoin(ghjs, &needs_rechecks,
										   KDS_FORMAT_ROW_FLAT);
		if (!ghjoin)
			break;
		if (needs_rechecks)
		{
			/* fill up kds_dest by CPU */
			elog(ERROR, "CPU Recheck not implemented yet");
		}

		/* source kds performs as ktoast of pds_dest */
		pds = ghjoin->pds;
		pds_dest = ghjoin->pds_dest;
		Assert(pds->kds->format == KDS_FORMAT_ROW ||
			   pds->kds->format == KDS_FORMAT_ROW_FLAT);
		Assert(pds_dest->kds->format == KDS_FORMAT_ROW_FLAT);

		/* update perfmon info */
		if (ghjoin->msg.pfm.enabled)
			pgstrom_perfmon_add(&ghjs->pfm, &ghjoin->msg.pfm);

		/*
		 * Make a bulk-slot according to the result
		 */
		nitems = pds_dest->kds->nitems;
		bulk = palloc0(offsetof(pgstrom_bulkslot, rindex[nitems]));
		bulk->pds = pgstrom_get_data_store(pds_dest);
		bulk->nvalids = -1;
		pgstrom_track_object(&pds_dest->sobj, 0);

		/* No longer gpuhashjoin is referenced any more. Its pds_dest
		 * shall not be actually released because its refcnt is already
		 * incremented above
		 */
		pgstrom_untrack_object(&ghjoin->msg.sobj);
        pgstrom_put_message(&ghjoin->msg);

		/*
		 * Reduce results if host-only qualifiers
		 */
		if (node->ps.qual)
		{
			ExprContext	   *econtext = ghjs->cps.ps.ps_ExprContext;
			//TupleTableSlot *slot = ghjs->pscan_wider_slot;
			TupleTableSlot *slot = ghjs->pscan_slot;
			HeapTupleData	tuple;

			for (i=0, j=0; i < nitems; i++)
			{
				if (!pgstrom_fetch_data_store(slot, bulk->pds, i, &tuple))
					elog(ERROR, "Bug? unable to fetch a result slot");
				econtext->ecxt_scantuple = slot;

				if (!ExecQual(node->ps.qual, econtext, false))
					continue;
				bulk->rindex[j++] = i;
			}
			bulk->nvalids = j;
		}
		break;
	}

	/* must provide our own instrumentation support */
    if (node->ps.instrument)
	{
		if (!bulk)
			InstrStopNode(node->ps.instrument, 0.0);
		else
			InstrStopNode(node->ps.instrument,
						  bulk->nvalids < 0 ?
						  (double) bulk->pds->kds->nitems :
						  (double) bulk->nvalids);
	}
	return (Node *) bulk;
}

static void
gpuhashjoin_end(CustomPlanState *node)
{
	GpuHashJoinState	   *ghjs = (GpuHashJoinState *) node;
	pgstrom_gpuhashjoin	   *ghjoin;

	/* release asynchronous jobs */
	if (ghjs->curr_ghjoin)
	{
		ghjoin = ghjs->curr_ghjoin;
		pgstrom_untrack_object(&ghjoin->msg.sobj);
        pgstrom_put_message(&ghjoin->msg);
	}

	while (ghjs->num_running > 0)
	{
		ghjoin = (pgstrom_gpuhashjoin *)pgstrom_dequeue_message(ghjs->mqueue);
		if (!ghjoin)
			elog(ERROR, "message queue wait timeout");
		pgstrom_untrack_object(&ghjoin->msg.sobj);
        pgstrom_put_message(&ghjoin->msg);
		ghjs->num_running--;
	}

	/*
	 *  Free the exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * clean out multiple hash tables on the portion of shared memory
	 * regison (because private memory stuff shall be released in-auto.
	 */
	if (ghjs->mhtables)
	{
		pgstrom_multihash_tables   *mhtables = ghjs->mhtables;
		pgstrom_untrack_object(&mhtables->sobj);
		multihash_put_tables(mhtables);
	}

	/*
	 * clean out kernel source and message queue
	 */
	Assert(ghjs->dprog_key);
	pgstrom_untrack_object((StromObject *)ghjs->dprog_key);
	pgstrom_put_devprog_key(ghjs->dprog_key);

	Assert(ghjs->mqueue);
	pgstrom_untrack_object(&ghjs->mqueue->sobj);
	pgstrom_close_queue(ghjs->mqueue);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	ExecClearTuple(ghjs->pscan_slot);
	ExecClearTuple(ghjs->pscan_wider_slot);

	/*
	 * clean up subtrees
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
}

static void
gpuhashjoin_rescan(CustomPlanState *node)
{
	GpuHashJoinState	   *ghjs = (GpuHashJoinState *) node;
	pgstrom_gpuhashjoin	   *ghjoin;

	/* release asynchronous jobs, if any */
	if (ghjs->curr_ghjoin)
	{
		ghjoin = ghjs->curr_ghjoin;
		pgstrom_untrack_object(&ghjoin->msg.sobj);
		pgstrom_put_message(&ghjoin->msg);
		ghjs->curr_ghjoin = NULL;
		ghjs->curr_index = 0;
		ghjs->curr_recheck = false;
	}

	while (ghjs->num_running > 0)
	{
		ghjoin = (pgstrom_gpuhashjoin *)pgstrom_dequeue_message(ghjs->mqueue);
		if (!ghjoin)
			elog(ERROR, "message queue wait timeout");
		pgstrom_untrack_object(&ghjoin->msg.sobj);
		pgstrom_put_message(&ghjoin->msg);
		ghjs->num_running--;
	}

	/*
	 * We may be reuse inner hash table, if single-batch join, and there is
	 * no parameter change for the inner subnodes.
	 */
	if (ghjs->mhtables)
	{
		pgstrom_multihash_tables *mhtables = ghjs->mhtables;
		PlanState	   *inner_ps = innerPlanState(ghjs);

		if (mhtables->is_divided || inner_ps->chgParam == NULL)
		{
			/*
             * if chgParam of subnode is not null then plan will be
			 * re-scanned by first ExecProcNode.
             */
			if (inner_ps->chgParam == NULL)
				ExecReScan(inner_ps);

			/*
			 * Release previous multi-hash-table
			 */
			pgstrom_untrack_object(&mhtables->sobj);
			multihash_put_tables(mhtables);
			ghjs->mhtables = NULL;
		}
	}

	/*
	 * if chgParam of subnode is not null then plan will be
	 * re-scanned by first ExecProcNode.
	 */
	ghjs->outer_done = false;
	ghjs->outer_overflow = NULL;
	if (outerPlanState(ghjs)->chgParam == NULL)
		ExecReScan(outerPlanState(ghjs));
}

static void
gpuhashjoin_explain(CustomPlanState *node, List *ancestors, ExplainState *es)
{
	GpuHashJoinState *ghjs = (GpuHashJoinState *) node;
	GpuHashJoin	   *ghjoin = (GpuHashJoin *) node->ps.plan;
	ListCell	   *cell;
	ListCell	   *lc1;
	ListCell	   *lc2;
	ListCell	   *lc3;
	bool			verbose_saved;
	int				depth;

	/* pseudo scan tlist if verbose mode */
	if (es->verbose)
	{
		StringInfoData	str;
		List		   *context;
		vartrans_info  *vt_prev;
		vartrans_info  *vt_curr;
		vartrans_info  *vt_next;
		char		   *temp;

		initStringInfo(&str);
		context = deparse_context_for_planstate((Node *) &node->ps,
												ancestors,
												es->rtable,
												es->rtable_names);
		vt_prev = NULL;
		while (true)
		{
			vt_next = NULL;
			foreach (cell, ghjoin->pscan_vartrans)
			{
				vt_curr = lfirst(cell);
				if ((!vt_prev || vt_prev->resno < vt_curr->resno) &&
					(!vt_next || vt_next->resno > vt_curr->resno))
					vt_next = vt_curr;
			}
			if (!vt_next)
				break;
			if (vt_prev)
				appendStringInfo(&str, ", ");
			temp = deparse_expression((Node *)vt_next->expr,
									  context,
									  true,
									  false);
			if (vt_next->ref_host)
				appendStringInfo(&str, "%s", temp);
			else
				appendStringInfo(&str, "(%s)", temp);

			vt_prev = vt_next;
		}
		ExplainPropertyText("pscan tlist", str.data, es);
	}
	verbose_saved = es->verbose;
	es->verbose = true;
	depth = 1;
	forthree (lc1, ghjoin->hash_clauses,
			  lc2, ghjoin->qual_clauses,
			  lc3, ghjoin->host_clauses)
	{
		char	qlabel[80];

		/* hash clause */
		snprintf(qlabel, sizeof(qlabel), "hash clause %d", depth);
		show_scan_qual(lfirst(lc1), qlabel, &node->ps, ancestors, es);
		/* qual clause */
		snprintf(qlabel, sizeof(qlabel), "qual clause %d", depth);
		show_scan_qual(lfirst(lc2), qlabel, &node->ps, ancestors, es);
		/* host clause */
		snprintf(qlabel, sizeof(qlabel), "host clause %d", depth);
		show_scan_qual(lfirst(lc3), qlabel, &node->ps, ancestors, es);
		depth++;
	}
	es->verbose = verbose_saved;

	ExplainPropertyText("Bulkload", ghjs->outer_bulkload ? "On" : "Off", es);

	show_device_kernel(ghjs->dprog_key, es);

	if (es->analyze && ghjs->pfm.enabled)
		pgstrom_perfmon_explain(&ghjs->pfm, es);
}

static Bitmapset *
gpuhashjoin_get_relids(CustomPlanState *node)
{
	/* nothing to do because core backend walks down inner/outer subtree */
	return NULL;
}

static Node *
gpuhashjoin_get_special_var(CustomPlanState *node,
							Var *varnode,
							PlanState **child_ps)
{
	GpuHashJoinState *ghjs = (GpuHashJoinState *) node;
	GpuHashJoin	   *ghjoin = (GpuHashJoin *) node->ps.plan;
	TargetEntry	   *tle;
	ListCell	   *cell;

	if (varnode->varno == INDEX_VAR)
	{
		foreach (cell, ghjoin->pscan_vartrans)
		{
			vartrans_info  *vtrans = lfirst(cell);
			PlanState  *curr_ps;
			int			depth;
			int			resno;

			if (vtrans->resno != varnode->varattno)
				continue;
			depth = vtrans->srcdepth;
			resno = vtrans->srcresno;

			if (depth == 0)
			{
				curr_ps = outerPlanState(ghjs);
				tle = get_tle_by_resno(curr_ps->plan->targetlist, resno);
				if (!tle)
					goto not_found;
				*child_ps = curr_ps;
				return (Node *) tle->expr;
			}
			for (curr_ps = innerPlanState(ghjs);
				 depth > 1;
				 curr_ps = innerPlanState(curr_ps), depth--)
			{
				if (!curr_ps)
					goto not_found;
			}
			tle = get_tle_by_resno(curr_ps->plan->targetlist, resno);
			if (!tle)
				goto not_found;
			*child_ps = curr_ps;
			return (Node *) tle->expr;
		}
	}
	else if (varnode->varno == OUTER_VAR)
	{
		Plan   *outer_plan = outerPlan(ghjoin);

		if (outer_plan)
		{
			tle = get_tle_by_resno(outer_plan->targetlist, varnode->varattno);
			if (tle)
			{
				*child_ps = outerPlanState(node);
				return (Node *) tle->expr;
			}
		}
	}
	else if (varnode->varno == INNER_VAR)
	{
		Plan   *inner_plan = innerPlan(ghjoin);

		if (inner_plan)
		{
			tle = get_tle_by_resno(inner_plan->targetlist, varnode->varattno);
			if (tle)
			{
				*child_ps = innerPlanState(node);
				return (Node *) tle->expr;
			}
		}
	}
not_found:
	Assert(false);
	elog(ERROR, "variable (varno=%u,varattno=%d) is not relevant tlist",
		 varnode->varno, varnode->varattno);
	return NULL;	/* be compiler quiet */
}


static void
gpuhashjoin_textout_plan(StringInfo str, const CustomPlan *node)
{
	GpuHashJoin	   *plannode = (GpuHashJoin *) node;
	ListCell	   *cell;

	appendStringInfo(str, " :num_rels %d", plannode->num_rels);

	appendStringInfo(str, " :kernel_source ");
	_outToken(str, plannode->kernel_source);

	appendStringInfo(str, " :extra_flags %u", plannode->extra_flags);

	appendStringInfo(str, " :join_types %s",
					 nodeToString(plannode->join_types));
	appendStringInfo(str, " :hash_clauses %s",
					 nodeToString(plannode->hash_clauses));
	appendStringInfo(str, " :qual_clauses %s",
					 nodeToString(plannode->qual_clauses));
	appendStringInfo(str, " :host_clauses %s",
					 nodeToString(plannode->host_clauses));
	appendStringInfo(str, " :used_params %s",
					 nodeToString(plannode->used_params));
	appendStringInfo(str, " :outer_attrefs ");
	_outBitmapset(str, plannode->outer_attrefs);

	foreach (cell, plannode->pscan_vartrans)
	{
		vartrans_info *vtrans = lfirst(cell);

		appendStringInfo(str,
						 "{"
						 ":srcdepth %d "
						 ":srcresno %d "
						 ":resno %d "
						 ":resname %s "
						 ":vartype %u "
						 ":vartypmod %d "
						 ":varcollid %u "
						 ":ref_host %s "
						 ":ref_device %s "
						 ":expr %s"
						 "}",
						 (int)vtrans->srcdepth,
						 (int)vtrans->srcresno,
						 (int)vtrans->resno,
						 vtrans->resname,
						 vtrans->vartype,
						 vtrans->vartypmod,
						 vtrans->varcollid,
						 vtrans->ref_host ? "true" : "false",
						 vtrans->ref_device ? "true" : "false",
						 nodeToString(vtrans->expr));
	}
}

static CustomPlan *
gpuhashjoin_copy_plan(const CustomPlan *from)
{
	GpuHashJoin	   *oldnode = (GpuHashJoin *) from;
	GpuHashJoin	   *newnode = palloc0(sizeof(GpuHashJoin));
	ListCell	   *cell;

	CopyCustomPlanCommon((Node *)from, (Node *)newnode);
	newnode->num_rels      = oldnode->num_rels;
	if (oldnode->kernel_source)
		newnode->kernel_source = pstrdup(oldnode->kernel_source);
	newnode->extra_flags   = oldnode->extra_flags;
	newnode->join_types    = list_copy(oldnode->join_types);
	newnode->hash_clauses  = copyObject(oldnode->hash_clauses);
	newnode->qual_clauses  = copyObject(oldnode->qual_clauses);
	newnode->host_clauses  = copyObject(oldnode->host_clauses);
	newnode->used_params   = copyObject(oldnode->used_params);
	newnode->outer_attrefs = bms_copy(oldnode->outer_attrefs);
	newnode->pscan_vartrans = NIL;
	foreach (cell, oldnode->pscan_vartrans)
	{
		vartrans_info *vtrans_old = lfirst(cell);
		vartrans_info *vtrans_new = palloc(sizeof(vartrans_info));

		memcpy(vtrans_new, vtrans_old, sizeof(vartrans_info));
		vtrans_new->resname = (vtrans_old->resname ?
							   pstrdup(vtrans_old->resname) : NULL);
		vtrans_new->expr    = copyObject(vtrans_old->expr);

		newnode->pscan_vartrans = lappend(newnode->pscan_vartrans,
										  vtrans_new);
	}
	return &newnode->cplan;
}

/* ----------------------------------------------------------------
 *
 * Callback routines for MultiHash node
 *
 * ---------------------------------------------------------------- */
static void
multihash_set_plan_ref(PlannerInfo *root,
					   CustomPlan *custom_plan,
					   int rtoffset)
{
	MultiHash  *mhash = (MultiHash *) custom_plan;
	List	   *tlist = NIL;
	ListCell   *cell;

	/* logic is copied from set_dummy_tlist_reference */
	foreach (cell, mhash->cplan.plan.targetlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(cell);
		Var	   *oldvar = (Var *) tle->expr;
		Var	   *newvar;

		newvar = makeVar(OUTER_VAR,
						 tle->resno,
						 exprType((Node *) oldvar),
						 exprTypmod((Node *) oldvar),
                         exprCollation((Node *) oldvar),
                         0);
        if (IsA(oldvar, Var))
		{
			newvar->varnoold = oldvar->varno + rtoffset;
            newvar->varoattno = oldvar->varattno;
		}
		else
		{
			newvar->varnoold = 0;		/* wasn't ever a plain Var */
            newvar->varoattno = 0;
		}
		tle = flatCopyTargetEntry(tle);
		tle->expr = (Expr *) newvar;
		tlist = lappend(tlist, tle);
	}
	mhash->cplan.plan.targetlist = tlist;
	Assert(mhash->cplan.plan.qual == NIL);
}

pgstrom_multihash_tables *
multihash_get_tables(pgstrom_multihash_tables *mhtables)
{
	SpinLockAcquire(&mhtables->lock);
	Assert(mhtables->refcnt > 0);
	mhtables->refcnt++;
	SpinLockRelease(&mhtables->lock);

	return mhtables;
}

void
multihash_put_tables(pgstrom_multihash_tables *mhtables)
{
	bool	do_release = false;

	SpinLockAcquire(&mhtables->lock);
	Assert(mhtables->refcnt > 0);
	if (--mhtables->refcnt == 0)
	{
		Assert(mhtables->n_kernel == 0 && mhtables->m_hash == NULL);
		do_release = true;
	}
	SpinLockRelease(&mhtables->lock);
	if (do_release)
		pgstrom_shmem_free(mhtables);
}

static CustomPlanState *
multihash_begin(CustomPlan *node,
				struct EState *estate,
				int eflags)
{
	MultiHash	   *mhash = (MultiHash *) node;
	MultiHashState *mhs = palloc0(sizeof(MultiHashState));
	List		   *hash_keylen = NIL;
	List		   *hash_keybyval = NIL;
	ListCell	   *cell;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));
	/* ensure the plan is MultiHash */
	Assert(plan_is_multihash((Plan *) mhash));

	NodeSetTag(mhs, T_CustomPlanState);
	mhs->cps.methods = &multihash_plan_methods;
	mhs->cps.ps.plan = (Plan *) mhash;
	mhs->cps.ps.state = estate;
	mhs->depth = mhash->depth;
	mhs->nslots = mhash->nslots;
	mhs->threshold_ratio = mhash->threshold_ratio;
	//mhs->chunk_size = mhash->chunk_size;
	mhs->hashtable_size = mhash->hashtable_size;
	mhs->outer_overflow = NULL;
	mhs->outer_done = false;
	mhs->curr_chunk = NULL;

	/*
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &mhs->cps.ps);

	/*
	 * initialize our result slot
	 */
	ExecInitResultTupleSlot(estate, &mhs->cps.ps);

	/*
	 * initialize child expressions
	 */
	mhs->cps.ps.targetlist = (List *)
		ExecInitExpr((Expr *) mhash->cplan.plan.targetlist, &mhs->cps.ps);
	mhs->cps.ps.qual = (List *)
		ExecInitExpr((Expr *) mhash->cplan.plan.qual, &mhs->cps.ps);
	mhs->hash_keys = (List *)
		ExecInitExpr((Expr *) mhash->hash_inner_keys, &mhs->cps.ps);

	foreach (cell, mhash->hash_inner_keys)
	{
		int16	typlen;
		bool	typbyval;

		get_typlenbyval(exprType(lfirst(cell)), &typlen, &typbyval);

		hash_keylen = lappend_int(hash_keylen, typlen);
		hash_keybyval = lappend_int(hash_keybyval, typbyval);
	}
	mhs->hash_keylen = hash_keylen;
	mhs->hash_keybyval = hash_keybyval;

	/*
	 * initialize child nodes
	 */
	outerPlanState(mhs) = ExecInitNode(outerPlan(mhash), estate, eflags);
	innerPlanState(mhs) = ExecInitNode(innerPlan(mhash), estate, eflags);

	/*
	 * initialize tuple type, but no need to initialize projection info
	 * because this node never have projection
	 */
	ExecAssignResultTypeFromTL(&mhs->cps.ps);
	mhs->cps.ps.ps_ProjInfo = NULL;

	return &mhs->cps;
}

static TupleTableSlot *
multihash_exec(CustomPlanState *node)
{
	elog(ERROR, "MultiHash does not support ExecProcNode call convention");
	return NULL;
}

static bool
expand_multihash_tables(MultiHashState *mhs,
						pgstrom_multihash_tables **p_mhtables, Size consumed)
{

	pgstrom_multihash_tables *mhtables_old = *p_mhtables;
	pgstrom_multihash_tables *mhtables_new;
	Size	length_old = mhtables_old->length;
	Size	allocated;

	mhtables_new = pgstrom_shmem_alloc_alap(2 * length_old, &allocated);
	if (!mhtables_new)
		return false;	/* out of shmem, or too large to allocate */
	memcpy(mhtables_new, mhtables_old,
		   offsetof(pgstrom_multihash_tables, kern) +
		   mhtables_old->usage + consumed);

	mhtables_new->length =
		allocated - offsetof(pgstrom_multihash_tables, kern);
	mhtables_new->kern.hostptr = (hostptr_t)&mhtables_new->kern.hostptr;
	Assert(mhtables_new->length > mhtables_old->length);
	elog(INFO, "pgstrom_multihash_tables was expanded %zu (%p) => %zu (%p)",
		 mhtables_old->length, mhtables_old,
		 mhtables_new->length, mhtables_new);
	pgstrom_track_object(&mhtables_new->sobj, 0);

	pgstrom_untrack_object(&mhtables_old->sobj);
	multihash_put_tables(mhtables_old);

	/* update hashtable_size of MultiHashState */
	do {
		Assert(IsA(mhs, CustomPlanState) &&
			   mhs->cps.methods == &multihash_plan_methods);
		mhs->hashtable_size = allocated;
		mhs = (MultiHashState *) innerPlanState(mhs);
	} while (mhs);

	*p_mhtables = mhtables_new;

	return true;
}

static void
multihash_preload_khashtable(MultiHashState *mhs,
							 pgstrom_multihash_tables **p_mhtables,
							 bool scan_forward)
{
	TupleDesc		tupdesc = ExecGetResultType(outerPlanState(mhs));
	ExprContext	   *econtext = mhs->cps.ps.ps_ExprContext;
	int				depth = mhs->depth;
	pgstrom_multihash_tables *mhtables = *p_mhtables;
	kern_hashtable *khtable;
	kern_hashentry *hentry;
	Size			required;
	Size			consumed;
	cl_uint		   *hash_slots;
	cl_uint			ntuples = 0;
	int				attcacheoff;
	int				attalign;
	int				i;

	/* preload should be done under the MultiExec context */
	Assert(CurrentMemoryContext == mhs->cps.ps.state->es_query_cxt);

	/*
	 * First of all, construct a kern_hashtable on the tail of current
	 * usage pointer of mhtables.
	 */
	Assert(StromTagIs(mhtables, HashJoinTable));
	Assert(mhtables->kern.htable_offset[depth] == 0);
	Assert(mhtables->usage == LONGALIGN(mhtables->usage));
	mhtables->kern.htable_offset[depth] = mhtables->usage;

	if (!scan_forward)
	{
		Assert(mhs->curr_chunk);
		required = mhtables->usage + mhs->curr_chunk->length;
		while (required > mhs->threshold_ratio * mhtables->length)
		{
			if (!expand_multihash_tables(mhs, p_mhtables, 0))
				elog(ERROR, "No multi-hashtables expandable any more");
			mhtables = *p_mhtables;
		}
		memcpy((char *)&mhtables->kern + mhtables->usage,
			   mhs->curr_chunk,
			   mhs->curr_chunk->length);
		mhtables->usage += mhs->curr_chunk->length;
		Assert(mhtables->usage < mhtables->length);
		if (!mhs->outer_done)
			mhtables->is_divided = true;
		return;
	}

	/*
	 * Below is the case when we need to make the scan pointer advanced
	 */
	required = (mhtables->usage +
				LONGALIGN(offsetof(kern_hashtable,
								   colmeta[tupdesc->natts])) +
				LONGALIGN(sizeof(cl_uint) * mhs->nslots));
	while (required > mhs->threshold_ratio * mhtables->length)
	{
		if (!expand_multihash_tables(mhs, &mhtables, 0))
			elog(ERROR, "No multi-hashtables expandable any more");
		mhtables = *p_mhtables;
	}
	khtable = (kern_hashtable *)((char *)&mhtables->kern + mhtables->usage);
	khtable->ncols = tupdesc->natts;
	khtable->nslots = mhs->nslots;
	khtable->is_outer = false;	/* Only INNER is supported right now */

	attcacheoff = offsetof(HeapTupleHeaderData, t_bits);
	if (tupdesc->tdhasoid)
		attcacheoff += sizeof(Oid);
	attcacheoff = MAXALIGN(attcacheoff);

	for (i=0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = tupdesc->attrs[i];

		attalign = typealign_get_width(attr->attalign);
        if (attcacheoff > 0)
		{
			if (attr->attlen > 0)
				attcacheoff = TYPEALIGN(attalign, attcacheoff);
			else
				attcacheoff = -1;	/* no more shortcut any more */
		}
		khtable->colmeta[i].attbyval = attr->attbyval;
		khtable->colmeta[i].attalign = attalign;
		khtable->colmeta[i].attlen = attr->attlen;
		khtable->colmeta[i].attnum = attr->attnum;
		khtable->colmeta[i].attcacheoff = attcacheoff;
		if (attcacheoff >= 0)
			attcacheoff += attr->attlen;
	}
	hash_slots = KERN_HASHTABLE_SLOT(khtable);
	memset(hash_slots, 0, sizeof(cl_uint) * khtable->nslots);
	consumed = LONGALIGN((uintptr_t)&hash_slots[khtable->nslots] -
						 (uintptr_t)khtable);

	/*
	 * Nest, fill up tuples fetched from the outer relation into
	 * the hash-table in this level
	 */
	while (true)
	{
		TupleTableSlot *scan_slot;
		HeapTuple		scan_tuple;
		Size			entry_size;
		pg_crc32		hash;
		ListCell	   *lc1;
		ListCell	   *lc2;
		ListCell	   *lc3;

		if (!mhs->outer_overflow)
			scan_slot = ExecProcNode(outerPlanState(mhs));
		else
		{
			scan_slot = mhs->outer_overflow;
			mhs->outer_overflow = NULL;
		}
		if (TupIsNull(scan_slot))
		{
			mhs->outer_done = true;
			break;
		}
		scan_tuple = ExecFetchSlotTuple(scan_slot);

		/* acquire the space on buffer */
		entry_size = LONGALIGN(offsetof(kern_hashentry, htup) +
							   scan_tuple->t_len);
		required = mhtables->usage + consumed + entry_size;
		while (required > mhs->threshold_ratio * mhtables->length)
		{
			if (!expand_multihash_tables(mhs, p_mhtables, consumed))
			{
				mhs->outer_overflow = scan_slot;
				goto out;
			}
			mhtables = *p_mhtables;
			khtable = (kern_hashtable *)
				((char *)&mhtables->kern + mhtables->usage);
			hash_slots = KERN_HASHTABLE_SLOT(khtable);
		}

		/* calculation of a hash value of this entry */
		INIT_CRC32(hash);
		econtext->ecxt_outertuple = scan_slot;
		forthree (lc1, mhs->hash_keys,
				  lc2, mhs->hash_keylen,
				  lc3, mhs->hash_keybyval)
		{
			ExprState  *clause = lfirst(lc1);
			int			keylen = lfirst_int(lc2);
			bool		keybyval = lfirst_int(lc3);
			Datum		value;
			bool		isnull;

			value = ExecEvalExpr(clause, econtext, &isnull, NULL);
			if (isnull)
				continue;
			if (keylen > 0)
			{
				if (keybyval)
					COMP_CRC32(hash, &value, keylen);
				else
					COMP_CRC32(hash, DatumGetPointer(value), keylen);
			}
			else
			{
				COMP_CRC32(hash,
						   VARDATA_ANY(value),
						   VARSIZE_ANY_EXHDR(value));
			}
		}
		FIN_CRC32(hash);

		/* allocation of hash entry and insert it */
		hentry = (kern_hashentry *)((char *)khtable + consumed);
		hentry->hash = hash;
		hentry->rowid = ntuples;	/* actually not used... */
		hentry->t_len = scan_tuple->t_len;
		memcpy(&hentry->htup, scan_tuple->t_data, scan_tuple->t_len);

		i = hash % khtable->nslots;
		hentry->next = hash_slots[i];
		hash_slots[i] = (cl_uint)consumed;

		/* increment buffer consumption */
		consumed += entry_size;
		/* increment number of tuples read */
		ntuples++;
	}
out:
	mhtables->ntuples += (double) ntuples;
	mhtables->usage += consumed;
	Assert(mhtables->usage < mhtables->length);
	khtable->length = consumed;
	if (mhs->curr_chunk || !mhs->outer_done)
		mhtables->is_divided = true;
	if (mhs->curr_chunk)
		pfree(mhs->curr_chunk);
	mhs->curr_chunk = pmemcpy(khtable, khtable->length);
}

static Node *
multihash_exec_multi(CustomPlanState *node)
{
	MultiHashState *mhs = (MultiHashState *) node;
	MultiHashNode  *mhnode = NULL;
	PlanState	   *inner_ps;	/* underlying MultiHash, if any */
	bool			scan_forward = false;
	int				depth = mhs->depth;

	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStartNode(node->ps.instrument);

	inner_ps = innerPlanState(mhs);
	if (inner_ps)
	{
		mhnode = (MultiHashNode *) MultiExecProcNode(inner_ps);
		if (!mhnode)
		{
			if (mhs->outer_done)
				goto out;
			ExecReScan(inner_ps);
			mhnode = (MultiHashNode *) MultiExecProcNode(inner_ps);
			if (!mhnode)
				goto out;
			scan_forward = true;
		}
		else if (!mhs->curr_chunk)
			scan_forward = true;
		Assert(mhnode);
	}
	else
	{
		/* no more deep hash-table, so create a MultiHashNode */
		pgstrom_multihash_tables *mhtables;
		int			nrels = depth;
		Size		usage;
		Size		allocated;

		if (mhs->outer_done)
			goto out;
		scan_forward = true;

		mhnode = palloc0(sizeof(MultiHashNode));
		NodeSetTag(mhnode, T_Invalid);
		mhnode->nrels = nrels;

		/* allocation of multihash_tables on shared memory */
		mhtables = pgstrom_shmem_alloc_alap(mhs->hashtable_size, &allocated);
		if (!mhtables)
			elog(ERROR, "out of shared memory");

		/* initialize multihash_tables */
		usage = STROMALIGN(offsetof(kern_multihash,
									htable_offset[nrels + 1]));
		memset(mhtables, 0, usage);

		mhtables->sobj.stag = StromTag_HashJoinTable;
		mhtables->length =
			(allocated - offsetof(pgstrom_multihash_tables, kern));
		mhtables->usage = usage;
		mhtables->ntuples = 0.0;
		SpinLockInit(&mhtables->lock);
		mhtables->refcnt = 1;
		mhtables->dindex = -1;		/* set by opencl-server */
		mhtables->n_kernel = 0;		/* set by opencl-server */
		mhtables->m_hash = NULL;	/* set by opencl-server */
		mhtables->ev_hash = NULL;	/* set by opencl-server */

		memcpy(mhtables->kern.pg_crc32_table,
			   pg_crc32_table,
			   sizeof(uint32) * 256);
		mhtables->kern.hostptr = (hostptr_t) &mhtables->kern.hostptr;
		mhtables->kern.ntables = nrels;
		memset(mhtables->kern.htable_offset, 0, sizeof(cl_uint) * (nrels + 1));
		pgstrom_track_object(&mhtables->sobj, 0);

		mhnode->mhtables = mhtables;
	}
	/*
	 * construct a kernel hash-table that stores all the inner-keys
	 * in this level, being loaded from the outer relation
	 */
	multihash_preload_khashtable(mhs, &mhnode->mhtables, scan_forward);
out:
	/* must provide our own instrumentation support */
	if (node->ps.instrument)
		InstrStopNode(node->ps.instrument,
					  !mhnode ? 0.0 : mhnode->mhtables->ntuples);
	return (Node *) mhnode;
}

static void
multihash_end(CustomPlanState *node)
{
	/*
	 * free exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * shut down the subplan
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
}

static void
multihash_rescan(CustomPlanState *node)
{
	MultiHashState *mhs = (MultiHashState *) node;

	if (innerPlanState(node))
		ExecReScan(innerPlanState(node));
	ExecReScan(outerPlanState(node));

	if (mhs->curr_chunk)
		pfree(mhs->curr_chunk);
	mhs->curr_chunk = NULL;
	mhs->outer_done = false;
	mhs->outer_overflow = NULL;
}

static void
multihash_explain(CustomPlanState *node, List *ancestors, ExplainState *es)
{
	MultiHash  *mhash = (MultiHash *) node->ps.plan;
	StringInfoData str;
	List	   *context;
	ListCell   *cell;

	/* set up deparsing context */
	context = deparse_context_for_planstate((Node *) node,
                                            ancestors,
                                            es->rtable,
                                            es->rtable_names);
	/* shows hash keys */
	initStringInfo(&str);
	foreach (cell, mhash->hash_inner_keys)
	{
		char   *exprstr;

		if (cell != list_head(mhash->hash_inner_keys))
			appendStringInfo(&str, ", ");

		exprstr = deparse_expression(lfirst(cell),
									 context,
									 es->verbose,
									 false);
		appendStringInfo(&str, "%s", exprstr);
		pfree(exprstr);
	}
    ExplainPropertyText("hash keys", str.data, es);

	/* shows hash parameters */
	if (es->format != EXPLAIN_FORMAT_TEXT)
	{
		resetStringInfo(&str);
		ExplainPropertyInteger("Buckets", mhash->nslots, es);
		ExplainPropertyInteger("Batches", mhash->nloops, es);
		appendStringInfo(&str, "%.2f%%", 100.0 * mhash->threshold_ratio);
		ExplainPropertyText("Memory Usage", str.data, es);
	}
	else
	{
		appendStringInfoSpaces(es->str, es->indent * 2);
		appendStringInfo(es->str,
						 "Buckets: %u  Batches: %u  Memory Usage: %.2f%%\n",
						 mhash->nslots, mhash->nloops,
						 100.0 * mhash->threshold_ratio);
	}
}

static Bitmapset *
multihash_get_relids(CustomPlanState *node)
{
	/* nothing to do because core backend walks down inner/outer subtree */
	return NULL;
}

static Node *
multihash_get_special_var(CustomPlanState *node,
						  Var *varnode,
						  PlanState **child_ps)
{
	PlanState	   *outer_ps = outerPlanState(node);
	TargetEntry	   *tle;

	Assert(varnode->varno == OUTER_VAR);
	tle = list_nth(outer_ps->plan->targetlist, varnode->varattno - 1);

	*child_ps = outer_ps;
	return (Node *)tle->expr;
}

static void
multihash_textout_plan(StringInfo str, const CustomPlan *node)
{
	MultiHash  *plannode = (MultiHash *) node;

	appendStringInfo(str, " :depth %d", plannode->depth);
	appendStringInfo(str, " :nslots %u", plannode->nslots);
	appendStringInfo(str, " :nloops %u", plannode->nloops);
	appendStringInfo(str, " :threshold_ratio %f", plannode->threshold_ratio);
	//appendStringInfo(str, " :chunk_size %zu", plannode->chunk_size);
	appendStringInfo(str, " :hashtable_size %zu", plannode->hashtable_size);
	appendStringInfo(str, " :hash_inner_keys %s",
					 nodeToString(plannode->hash_inner_keys));
	appendStringInfo(str, " :hash_outer_keys %s",
					 nodeToString(plannode->hash_outer_keys));
}

static CustomPlan *
multihash_copy_plan(const CustomPlan *from)
{
	MultiHash  *oldnode = (MultiHash *) from;
	MultiHash  *newnode = palloc0(sizeof(MultiHash));

	CopyCustomPlanCommon((Node *)oldnode, (Node *)newnode);
	newnode->depth           = oldnode->depth;
	newnode->nslots          = oldnode->nslots;
	newnode->nloops          = oldnode->nslots;
	newnode->threshold_ratio = oldnode->threshold_ratio;
	//newnode->chunk_size      = oldnode->chunk_size;
	newnode->hashtable_size  = oldnode->hashtable_size;
	newnode->hash_inner_keys = copyObject(oldnode->hash_inner_keys);
	newnode->hash_outer_keys = copyObject(oldnode->hash_outer_keys);

	return &newnode->cplan;
}

/*
 * pgstrom_init_gpuhashjoin
 *
 * a startup routine to initialize gpuhashjoin.c
 */
void
pgstrom_init_gpuhashjoin(void)
{
	/* enable_gpuhashjoin parameter */
	DefineCustomBoolVariable("enable_gpuhashjoin",
							 "Enables the use of GPU accelerated hash-join",
							 NULL,
							 &enable_gpuhashjoin,
							 true,
							 PGC_USERSET,
							 GUC_NOT_IN_SAMPLE,
							 NULL, NULL, NULL);

	/* setup path methods */
	gpuhashjoin_path_methods.CustomName = "GpuHashJoin";
	gpuhashjoin_path_methods.CreateCustomPlan	= gpuhashjoin_create_plan;
	gpuhashjoin_path_methods.TextOutCustomPath	= gpuhashjoin_textout_path;

	/* setup plan methods */
	gpuhashjoin_plan_methods.CustomName = "GpuHashJoin";
	gpuhashjoin_plan_methods.SetCustomPlanRef	= gpuhashjoin_set_plan_ref;
	gpuhashjoin_plan_methods.SupportBackwardScan= NULL;
	gpuhashjoin_plan_methods.FinalizeCustomPlan	= gpuhashjoin_finalize_plan;
	gpuhashjoin_plan_methods.BeginCustomPlan	= gpuhashjoin_begin;
	gpuhashjoin_plan_methods.ExecCustomPlan		= gpuhashjoin_exec;
	gpuhashjoin_plan_methods.MultiExecCustomPlan= gpuhashjoin_exec_multi;
	gpuhashjoin_plan_methods.EndCustomPlan		= gpuhashjoin_end;
	gpuhashjoin_plan_methods.ReScanCustomPlan	= gpuhashjoin_rescan;
	gpuhashjoin_plan_methods.ExplainCustomPlan	= gpuhashjoin_explain;
	gpuhashjoin_plan_methods.GetRelidsCustomPlan= gpuhashjoin_get_relids;
	gpuhashjoin_plan_methods.GetSpecialCustomVar= gpuhashjoin_get_special_var;
	gpuhashjoin_plan_methods.TextOutCustomPlan	= gpuhashjoin_textout_plan;
	gpuhashjoin_plan_methods.CopyCustomPlan		= gpuhashjoin_copy_plan;

	/* setup plan methods of MultiHash */
	multihash_plan_methods.CustomName          = "MultiHash";
	multihash_plan_methods.SetCustomPlanRef    = multihash_set_plan_ref;
	multihash_plan_methods.BeginCustomPlan     = multihash_begin;
	multihash_plan_methods.ExecCustomPlan      = multihash_exec;
	multihash_plan_methods.MultiExecCustomPlan = multihash_exec_multi;
	multihash_plan_methods.EndCustomPlan       = multihash_end;
	multihash_plan_methods.ReScanCustomPlan    = multihash_rescan;
	multihash_plan_methods.ExplainCustomPlan   = multihash_explain;
	multihash_plan_methods.GetRelidsCustomPlan = multihash_get_relids;
	multihash_plan_methods.GetSpecialCustomVar = multihash_get_special_var;
	multihash_plan_methods.TextOutCustomPlan   = multihash_textout_plan;
	multihash_plan_methods.CopyCustomPlan      = multihash_copy_plan;

	/* hook registration */
	add_hashjoin_path_next = add_hashjoin_path_hook;
	add_hashjoin_path_hook = gpuhashjoin_add_path;
}

/* ----------------------------------------------------------------
 *
 * NOTE: below is the code being run on OpenCL server context
 *
 * ---------------------------------------------------------------- */

typedef struct
{
	pgstrom_gpuhashjoin *gpuhashjoin;
	cl_command_queue kcmdq;
	cl_program		program;
	cl_kernel		kern_main;
	cl_kernel		kern_proj;
	cl_mem			m_join;
	cl_mem			m_hash;
	cl_mem			m_dstore;
	cl_mem			m_ktoast;
	cl_mem			m_rowmap;
	cl_mem			m_kresult;
	cl_int			dindex;
	bool			hash_loader;/* true, if this context loads hash table */
	cl_uint			ev_kern_main;	/* event index of kern_main */
	cl_uint			ev_kern_proj;	/* event index of kern_proj */
	cl_uint			ev_index;
	cl_event		events[30];
} clstate_gpuhashjoin;

static void
clserv_respond_hashjoin(cl_event event, cl_int ev_status, void *private)
{
	clstate_gpuhashjoin	*clghj = (clstate_gpuhashjoin *) private;
	pgstrom_gpuhashjoin *gpuhashjoin = clghj->gpuhashjoin;
	pgstrom_multihash_tables *mhtables = gpuhashjoin->mhtables;
	kern_resultbuf		*kresults
		= KERN_HASHJOIN_RESULTBUF(&gpuhashjoin->khashjoin);

	if (ev_status == CL_COMPLETE)
		gpuhashjoin->msg.errcode = kresults->errcode;
	else
	{
		clserv_log("unexpected CL_EVENT_COMMAND_EXECUTION_STATUS: %d",
				   ev_status);
		gpuhashjoin->msg.errcode = StromError_OpenCLInternal;
	}

	/* collect performance statistics */
	if (gpuhashjoin->msg.pfm.enabled)
	{
		pgstrom_perfmon *pfm = &gpuhashjoin->msg.pfm;
		cl_ulong	tv_start;
		cl_ulong	tv_end;
		cl_ulong	temp;
		cl_int		i, rc;

		/* Time to load hash-tables should be counted on the context that
		 * actually kicked DMA send request only.
		 */
		if (clghj->hash_loader)
		{
			rc = clGetEventProfilingInfo(clghj->events[0],
										 CL_PROFILING_COMMAND_START,
										 sizeof(cl_ulong),
										 &tv_start,
										 NULL);
			if (rc != CL_SUCCESS)
				goto skip_perfmon;
			rc = clGetEventProfilingInfo(clghj->events[0],
										 CL_PROFILING_COMMAND_END,
										 sizeof(cl_ulong),
										 &tv_end,
										 NULL);
			if (rc != CL_SUCCESS)
				goto skip_perfmon;
			pfm->time_dma_send += (tv_end - tv_start) / 1000;
		}

		/*
		 * DMA send time of hashjoin headers and row-/column-store
		 */
		tv_start = ~0;
		tv_end = 0;
		for (i=1; i < clghj->ev_kern_main; i++)
		{
			rc = clGetEventProfilingInfo(clghj->events[i],
                                         CL_PROFILING_COMMAND_START,
                                         sizeof(cl_ulong),
                                         &temp,
                                         NULL);
			if (rc != CL_SUCCESS)
				goto skip_perfmon;
			tv_start = Min(tv_start, temp);

			rc = clGetEventProfilingInfo(clghj->events[i],
										 CL_PROFILING_COMMAND_END,
										 sizeof(cl_ulong),
										 &temp,
										 NULL);
			if (rc != CL_SUCCESS)
				goto skip_perfmon;
			tv_end = Max(tv_end, temp);
		}
		pfm->time_dma_send += (tv_end - tv_start) / 1000;

		/*
		 * Main kernel execution time
		 */
		rc = clGetEventProfilingInfo(clghj->events[clghj->ev_kern_main],
									 CL_PROFILING_COMMAND_START,
									 sizeof(cl_ulong),
									 &tv_start,
									 NULL);
		if (rc != CL_SUCCESS)
			goto skip_perfmon;

		rc = clGetEventProfilingInfo(clghj->events[clghj->ev_kern_main],
									 CL_PROFILING_COMMAND_END,
									 sizeof(cl_ulong),
									 &tv_end,
									 NULL);
		if (rc != CL_SUCCESS)
			goto skip_perfmon;
		pfm->time_kern_exec += (tv_end - tv_start) / 1000;

		/*
		 * Projection kernel execution time
		 */
		rc = clGetEventProfilingInfo(clghj->events[clghj->ev_kern_proj],
									 CL_PROFILING_COMMAND_START,
									 sizeof(cl_ulong),
									 &tv_start,
									 NULL);
		if (rc != CL_SUCCESS)
			goto skip_perfmon;

		rc = clGetEventProfilingInfo(clghj->events[clghj->ev_kern_proj],
									 CL_PROFILING_COMMAND_END,
									 sizeof(cl_ulong),
									 &tv_end,
									 NULL);
		if (rc != CL_SUCCESS)
			goto skip_perfmon;
		pfm->time_kern_proj += (tv_end - tv_start) / 1000;

		/*
		 * DMA recv time
		 */
		tv_start = ~0;
		tv_end = 0;
		for (i=clghj->ev_kern_proj + 1; i < clghj->ev_index; i++)
		{
			rc = clGetEventProfilingInfo(clghj->events[i],
										 CL_PROFILING_COMMAND_START,
										 sizeof(cl_ulong),
										 &temp,
										 NULL);
			if (rc != CL_SUCCESS)
				goto skip_perfmon;
			tv_start = Min(tv_start, temp);

			rc = clGetEventProfilingInfo(clghj->events[i],
										 CL_PROFILING_COMMAND_END,
										 sizeof(cl_ulong),
										 &temp,
										 NULL);
			if (rc != CL_SUCCESS)
				goto skip_perfmon;
			tv_end = Max(tv_end, temp);
		}
		pfm->time_dma_recv += (tv_end - tv_start) / 1000;

	skip_perfmon:
		if (rc != CL_SUCCESS)
		{
			clserv_log("failed on clGetEventProfilingInfo (%s)",
					   opencl_strerror(rc));
			pfm->enabled = false;	/* turn off profiling */
		}
	}

	/*
	 * release opencl resources
	 *
	 * NOTE: The first event object (a.k.a hjtable->ev_hash) and memory
	 * object of hash table (a.k.a hjtable->m_hash) has to be released
	 * under the hjtable->lock
	 */
	while (clghj->ev_index > 1)
		clReleaseEvent(clghj->events[--clghj->ev_index]);
	if (clghj->m_kresult)
		clReleaseMemObject(clghj->m_kresult);
	if (clghj->m_rowmap)
		clReleaseMemObject(clghj->m_rowmap);
	if (clghj->m_ktoast)
		clReleaseMemObject(clghj->m_ktoast);
	if (clghj->m_dstore)
		clReleaseMemObject(clghj->m_dstore);
	if (clghj->m_join)
		clReleaseMemObject(clghj->m_join);
	if (clghj->kern_main)
		clReleaseKernel(clghj->kern_main);
	if (clghj->kern_proj)
		clReleaseKernel(clghj->kern_proj);
	if (clghj->program)
		clReleaseProgram(clghj->program);

	/* Unload hashjoin-table, if no longer referenced */
	SpinLockAcquire(&mhtables->lock);
	Assert(mhtables->n_kernel > 0);
	clReleaseMemObject(mhtables->m_hash);
	clReleaseEvent(mhtables->ev_hash);
	if (--mhtables->n_kernel == 0)
	{
		mhtables->m_hash = NULL;
		mhtables->ev_hash = NULL;
	}
	SpinLockRelease(&mhtables->lock);	
	free(clghj);

	/*
	 * A hash-join operation may produce unpredicated number of rows;
	 * larger than capability of kern_resultbuf being allocated in-
	 * advance. In this case, kernel code returns the error code of
	 * StromError_DataStoreNoSpace, so we try again with larger result-
	 * buffer.
	 */
	if (gpuhashjoin->msg.errcode == StromError_DataStoreNoSpace)
	{
		/*
		 * Expand the result buffer then retry, if rough estimation didn't
		 * give enough space to store the result
		 */
		pgstrom_data_store *old_pds = gpuhashjoin->pds_dest;
		pgstrom_data_store *new_pds;
		kern_data_store	   *old_kds = old_pds->kds;
		kern_data_store	   *new_kds;
		kern_resultbuf	   *kresults;
		cl_uint				ncols = old_kds->ncols;
		cl_uint				nitems = old_kds->nitems;
		Size				head_len;
		Size				required;

		/* adjust kern_resultbuf */
		kresults = KERN_HASHJOIN_RESULTBUF(&gpuhashjoin->khashjoin);
		clserv_log("GHJ input kresults (%u=>%u)", kresults->nrooms, nitems);
		kresults->nrooms = nitems;
		kresults->nitems = 0;
		kresults->errcode = StromError_Success;

		head_len = STROMALIGN(offsetof(kern_data_store, colmeta[ncols]));
		if (old_kds->format == KDS_FORMAT_TUPSLOT)
		{
			clserv_log("GHJ input again (nrooms: %u => %u)",
					   old_kds->nrooms, nitems);
			required = STROMALIGN(head_len +
								  (LONGALIGN(sizeof(Datum) * ncols) +
								   LONGALIGN(sizeof(bool) * ncols)) * nitems);
			new_kds = pgstrom_shmem_alloc(required);
			if (!new_kds)
			{
				gpuhashjoin->msg.errcode = StromError_OutOfSharedMemory;
				pgstrom_reply_message(&gpuhashjoin->msg);
				return;
			}
			memcpy(new_kds, old_kds, head_len);
			new_kds->hostptr = (hostptr_t) &new_kds->hostptr;
			new_kds->length = required;
			new_kds->usage = 0;
			new_kds->nrooms = nitems;
			new_kds->nitems = 0;
		}
		else if (old_kds->format == KDS_FORMAT_ROW_FLAT)
		{
			clserv_log("GHJ input again (length: %u => %u)",
					   old_kds->length, old_kds->usage);
			required = head_len +
				STROMALIGN(sizeof(kern_blkitem) * old_kds->maxblocks) +
				STROMALIGN(sizeof(kern_rowitem) * nitems) +
				STROMALIGN(old_kds->usage);
			new_kds = pgstrom_shmem_alloc(required);
			if (!new_kds)
			{
				gpuhashjoin->msg.errcode = StromError_OutOfSharedMemory;
				pgstrom_reply_message(&gpuhashjoin->msg);
				return;
			}
			memcpy(new_kds, old_kds, head_len);
			new_kds->hostptr = (hostptr_t) &new_kds->hostptr;
			new_kds->length = required;
			new_kds->usage = 0;
			new_kds->nrooms = (required - head_len) / sizeof(kern_rowitem);
			new_kds->nitems = 0;
		}
		else
		{
			gpuhashjoin->msg.errcode = StromError_DataStoreCorruption;
			pgstrom_reply_message(&gpuhashjoin->msg);
			return;
		}
		/* allocate a new pgstrom_data_store */
		new_pds = pgstrom_shmem_alloc(sizeof(pgstrom_data_store));
		if (!new_pds)
		{
			pgstrom_shmem_free(new_kds);
			gpuhashjoin->msg.errcode = StromError_OutOfSharedMemory;
            pgstrom_reply_message(&gpuhashjoin->msg);
            return;
		}
		memset(new_pds, 0, sizeof(pgstrom_data_store));
		new_pds->sobj.stag = StromTag_DataStore;
		SpinLockInit(&new_pds->lock);
		new_pds->refcnt = 1;
		new_pds->kds = new_kds;

		/* replace an old pds by new pds */
		gpuhashjoin->pds_dest = new_pds;
		pgstrom_put_data_store(old_pds);

		/* retry gpuhashjoin with larger result buffer */
		pgstrom_enqueue_message(&gpuhashjoin->msg);
		return;
	}
	/* otherwise, hash-join is successfully done */
	pgstrom_reply_message(&gpuhashjoin->msg);
}

static void
clserv_process_gpuhashjoin(pgstrom_message *message)
{
	pgstrom_gpuhashjoin *gpuhashjoin = (pgstrom_gpuhashjoin *) message;
	pgstrom_multihash_tables *mhtables = gpuhashjoin->mhtables;
	pgstrom_data_store *pds = gpuhashjoin->pds;
	pgstrom_data_store *pds_dest = gpuhashjoin->pds_dest;
	kern_data_store	   *kds = pds->kds;
	kern_data_store	   *kds_dest = pds_dest->kds;
	clstate_gpuhashjoin	*clghj = NULL;
	kern_row_map	   *krowmap;
	kern_resultbuf	   *kresults;
	size_t				nitems;
	size_t				gwork_sz;
	size_t				lwork_sz;
	Size				offset;
	Size				length;
	void			   *dmaptr;
	cl_int				rc;

	Assert(StromTagIs(gpuhashjoin, GpuHashJoin));
	Assert(StromTagIs(gpuhashjoin->mhtables, HashJoinTable));
	Assert(StromTagIs(gpuhashjoin->pds, DataStore));
	krowmap = KERN_HASHJOIN_ROWMAP(&gpuhashjoin->khashjoin);
	kresults = KERN_HASHJOIN_RESULTBUF(&gpuhashjoin->khashjoin);

	/* state object of gpuhashjoin */
	clghj = calloc(1, (sizeof(clstate_gpuhashjoin) +
					   sizeof(cl_event) * kds->nblocks));
	if (!clghj)
	{
		rc = CL_OUT_OF_HOST_MEMORY;
		goto error;
	}
	clghj->gpuhashjoin = gpuhashjoin;

	/*
	 * First of all, it looks up a program object to be run on
	 * the supplied row-store. We may have three cases.
	 * 1) NULL; it means the required program is under asynchronous
	 *    build, and the message is kept on its internal structure
	 *    to be enqueued again. In this case, we have nothing to do
	 *    any more on the invocation.
	 * 2) BAD_OPENCL_PROGRAM; it means previous compile was failed
	 *    and unavailable to run this program anyway. So, we need
	 *    to reply StromError_ProgramCompile error to inform the
	 *    backend this program.
	 * 3) valid cl_program object; it is an ideal result. pre-compiled
	 *    program object was on the program cache, and cl_program
	 *    object is ready to use.
	 */
	clghj->program = clserv_lookup_device_program(gpuhashjoin->dprog_key,
                                                  &gpuhashjoin->msg);
    if (!clghj->program)
    {
        free(clghj);
		return;	/* message is in waitq, being retried later */
    }
    if (clghj->program == BAD_OPENCL_PROGRAM)
    {
        rc = CL_BUILD_PROGRAM_FAILURE;
        goto error;
    }

	/*
     * Allocation of kernel memory for hash table. If someone already
     * allocated it, we can reuse it.
     */
	SpinLockAcquire(&mhtables->lock);
	if (mhtables->n_kernel == 0)
	{
		int		dindex;

		Assert(!mhtables->m_hash && !mhtables->ev_hash);

		dindex = pgstrom_opencl_device_schedule(&gpuhashjoin->msg);
		mhtables->dindex = dindex;
		clghj->dindex = dindex;
		clghj->kcmdq = opencl_cmdq[dindex];
		clghj->m_hash = clCreateBuffer(opencl_context,
                                       CL_MEM_READ_WRITE,
									   mhtables->length,
									   NULL,
									   &rc);
		if (rc != CL_SUCCESS)
		{
			SpinLockRelease(&mhtables->lock);
			goto error;
		}

		rc = clEnqueueWriteBuffer(clghj->kcmdq,
								  clghj->m_hash,
								  CL_FALSE,
								  0,
                                  mhtables->length,
								  &mhtables->kern,
								  0,
								  NULL,
								  &clghj->events[clghj->ev_index]);
		if (rc != CL_SUCCESS)
		{
			clReleaseMemObject(clghj->m_hash);
			clghj->m_hash = NULL;
			SpinLockRelease(&mhtables->lock);
			goto error;
        }
		mhtables->m_hash = clghj->m_hash;
		mhtables->ev_hash = clghj->events[clghj->ev_index];
		clghj->ev_index++;
		clghj->hash_loader = true;
		gpuhashjoin->msg.pfm.bytes_dma_send += mhtables->length;
		gpuhashjoin->msg.pfm.num_dma_send++;
	}
	else
	{
		Assert(mhtables->m_hash && mhtables->ev_hash);
		rc = clRetainMemObject(mhtables->m_hash);
		Assert(rc == CL_SUCCESS);
		rc = clRetainEvent(mhtables->ev_hash);
		Assert(rc == CL_SUCCESS);

		clghj->dindex = mhtables->dindex;
		clghj->kcmdq = opencl_cmdq[clghj->dindex];
		clghj->m_hash = mhtables->m_hash;
		clghj->events[clghj->ev_index++] = mhtables->ev_hash;
	}
	mhtables->n_kernel++;
	SpinLockRelease(&mhtables->lock);

	/*
	 * __kernel void
	 * kern_gpuhashjoin_main(__global kern_hashjoin *khashjoin,
	 *                        __global kern_multihash *kmhash,
	 *                        __global kern_data_store *kds,
	 *                        __global kern_data_store *ktoast,
	 *                        KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
	 */
	clghj->kern_main = clCreateKernel(clghj->program,
									  "kern_gpuhashjoin_main",
									  &rc);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clCreateKernel: %s", opencl_strerror(rc));
		goto error;
	}

	/*
	 * __kernel void
	 * kern_gpuhashjoin_projection(__global kern_hashjoin *khashjoin,
	 *                             __global kern_multihash *kmhash,
	 *                             __global kern_data_store *kds,
	 *                             __global kern_data_store *ktoast,
	 *                             __global kern_data_store *kds_dest,
	 *                             KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
	 */
	if (pds_dest->kds->format == KDS_FORMAT_TUPSLOT)
		clghj->kern_proj = clCreateKernel(clghj->program,
										  "kern_gpuhashjoin_projection_slot",
										  &rc);
	else if (pds_dest->kds->format == KDS_FORMAT_ROW_FLAT)
		clghj->kern_proj = clCreateKernel(clghj->program,
										  "kern_gpuhashjoin_projection_row",
										  &rc);
	else
	{
		clserv_log("pds_dest has unexpected format");
		goto error;
	}
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clCreateKernel: %s", opencl_strerror(rc));
		goto error;
	}

	/* buffer object of __global kern_hashjoin *khashjoin */
	length = (KERN_HASHJOIN_PARAMBUF_LENGTH(&gpuhashjoin->khashjoin) +
			  KERN_HASHJOIN_RESULTBUF_LENGTH(&gpuhashjoin->khashjoin) +
			  sizeof(cl_int) * kresults->nrels * kresults->nrooms);
	clghj->m_join = clCreateBuffer(opencl_context,
								   CL_MEM_READ_WRITE,
								   length,
								   NULL,
								   &rc);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clCreateBuffer: %s", opencl_strerror(rc));
        goto error;
	}

	/* buffer object of __global kern_data_store *kds */
	clghj->m_dstore = clCreateBuffer(opencl_context,
									 CL_MEM_READ_WRITE,
									 KERN_DATA_STORE_LENGTH(kds),
									 NULL,
									 &rc);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clCreateBuffer: %s", opencl_strerror(rc));
		goto error;
	}

	/* buffer object of __global kern_data_store *ktoast, if needed */
	if (!pds->ktoast)
		clghj->m_ktoast = NULL;
	else
	{
		pgstrom_data_store *ktoast = pds->ktoast;

		clghj->m_ktoast = clCreateBuffer(opencl_context,
										 CL_MEM_READ_WRITE,
										 KERN_DATA_STORE_LENGTH(ktoast->kds),
										 NULL,
										 &rc);
		if (rc != CL_SUCCESS)
		{
			clserv_log("failed on clCreateBuffer: %s", opencl_strerror(rc));
			goto error;
		}
	}

	/* buffer object of __global kern_row_map *krowmap */
	if (krowmap->nvalids < 0)
		clghj->m_rowmap = NULL;
	else
	{
		length = STROMALIGN(offsetof(kern_row_map,
									 rindex[krowmap->nvalids]));
		clghj->m_rowmap = clCreateBuffer(opencl_context,
                                         CL_MEM_READ_WRITE,
										 length,
										 NULL,
										 &rc);
		if (rc != CL_SUCCESS)
		{
			clserv_log("failed on clCreateBuffer: %s", opencl_strerror(rc));
			goto error;
		}
	}

	/* buffer object of __global kern_data_store *kds_dest */
	clghj->m_kresult = clCreateBuffer(opencl_context,
									  CL_MEM_READ_WRITE,
									  STROMALIGN(kds_dest->length),
									  NULL,
									  &rc);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clCreateBuffer: %s", opencl_strerror(rc));
		goto error;
	}

	/*
	 * OK, all the device memory and kernel objects are successfully
	 * constructed. Let's enqueue DMA send/recv and kernel invocations.
	 */

	/* Enqueue DMA send of kern_hashjoin */
	dmaptr = KERN_HASHJOIN_DMA_SENDPTR(&gpuhashjoin->khashjoin);
	offset = KERN_HASHJOIN_DMA_SENDOFS(&gpuhashjoin->khashjoin);
	length = KERN_HASHJOIN_DMA_SENDLEN(&gpuhashjoin->khashjoin);
	rc = clEnqueueWriteBuffer(clghj->kcmdq,
							  clghj->m_join,
							  CL_FALSE,
							  offset,
							  length,
							  dmaptr,
							  0,
							  NULL,
							  &clghj->events[clghj->ev_index]);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clEnqueueWriteBuffer: %s", opencl_strerror(rc));
        goto error;
	}
	clghj->ev_index++;
    gpuhashjoin->msg.pfm.bytes_dma_send += length;
    gpuhashjoin->msg.pfm.num_dma_send++;

	/*
	 * Enqueue DMA send of kern_rowmap, if any
	 */
	if (clghj->m_rowmap)
	{
		length = STROMALIGN(offsetof(kern_row_map,
									 rindex[krowmap->nvalids]));
		rc = clEnqueueWriteBuffer(clghj->kcmdq,
								  clghj->m_rowmap,
								  CL_FALSE,
								  0,
								  length,
								  krowmap,
								  0,
								  NULL,
								  &clghj->events[clghj->ev_index]);
		if (rc != CL_SUCCESS)
		{
			clserv_log("failed on clCreateBuffer: %s", opencl_strerror(rc));
			goto error;
		}
		clghj->ev_index++;
		gpuhashjoin->msg.pfm.bytes_dma_send += length;
		gpuhashjoin->msg.pfm.num_dma_send++;
	}

	/*
	 * Enqueue DMA send of kern_data_store
	 * according to the type of data store
	 */
	rc = clserv_dmasend_data_store(pds,
								   clghj->kcmdq,
								   clghj->m_dstore,
								   clghj->m_ktoast,
								   0,
								   NULL,
								   &clghj->ev_index,
								   clghj->events,
								   &gpuhashjoin->msg.pfm);
	if (rc != CL_SUCCESS)
		goto error;

	/*
	 * Enqueue DMA send of destination kern_data_store
	 */
	length = STROMALIGN(offsetof(kern_data_store, colmeta[kds_dest->ncols]));
	rc = clEnqueueWriteBuffer(clghj->kcmdq,
                              clghj->m_kresult,
							  CL_FALSE,
							  0,
							  length,
							  kds_dest,
							  0,
							  NULL,
							  &clghj->events[clghj->ev_index]);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clEnqueueWriteBuffer: %s", opencl_strerror(rc));
		goto error;
	}
	clghj->ev_index++;
	gpuhashjoin->msg.pfm.bytes_dma_send += length;
	gpuhashjoin->msg.pfm.num_dma_send++;

	/*
	 * __kernel void
	 * kern_gpuhashjoin_main(__global kern_hashjoin *khashjoin,
	 *                       __global kern_multihash *kmhash,
	 *                       __global kern_data_store *kds,
	 *                       __global kern_data_store *ktoast,
	 *                       __global kern_row_map   *krowmap,
	 *                       KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
	 */

	/* Get an optimal workgroup-size of this kernel */
	nitems = (krowmap->nvalids < 0 ? kds->nitems : krowmap->nvalids);
	if (!clserv_compute_workgroup_size(&gwork_sz, &lwork_sz,
									   clghj->kern_main,
									   clghj->dindex,
									   true,	/* larger is better? */
									   nitems,
									   sizeof(cl_uint)))
		goto error;

	rc = clSetKernelArg(clghj->kern_main,
						0,	/* __global kern_hashjoin *khashjoin */
						sizeof(cl_mem),
						&clghj->m_join);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clghj->kern_main,
						1,	/* __global kern_multihash *kmhash */
						sizeof(cl_mem),
						&clghj->m_hash);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clghj->kern_main,
						2,	/* __global kern_data_store *kds */
						sizeof(cl_mem),
						&clghj->m_dstore);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clghj->kern_main,
						3,	/*  __global kern_data_store *ktoast */
						sizeof(cl_mem),
						&clghj->m_ktoast);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clghj->kern_main,
						4,	/*  __global kern_row_map *krowmap */
						sizeof(cl_mem),
						&clghj->m_rowmap);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clghj->kern_main,
						5,	/* KERN_DYNAMIC_LOCAL_WORKMEM_ARG */
						sizeof(cl_uint) * lwork_sz,
						NULL);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clEnqueueNDRangeKernel(clghj->kcmdq,
								clghj->kern_main,
								1,
								NULL,
								&gwork_sz,
								&lwork_sz,
								clghj->ev_index,
								&clghj->events[0],
								&clghj->events[clghj->ev_index]);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clEnqueueNDRangeKernel: %s",
				   opencl_strerror(rc));
		clserv_log("gwork_sz=%zu lwork_sz=%zu", gwork_sz, lwork_sz);
		goto error;
	}
	clghj->ev_kern_main = clghj->ev_index;
	clghj->ev_index++;
	gpuhashjoin->msg.pfm.num_kern_exec++;

	/*
	 * __kernel void
	 * kern_gpuhashjoin_projection(__global kern_hashjoin *khashjoin,
	 *                             __global kern_multihash *kmhash,
	 *                             __global kern_data_store *kds,
	 *                             __global kern_data_store *ktoast,
	 *                             __global kern_data_store *kds_dest,
	 *                             KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
	 */

	/* Get an optimal workgroup-size of this kernel */
	if (!clserv_compute_workgroup_size(&gwork_sz, &lwork_sz,
									   clghj->kern_proj,
									   clghj->dindex,
									   false,   /* smaller is better */
									   kds_dest->nrooms,
									   sizeof(cl_uint)))
		goto error;

	rc = clSetKernelArg(clghj->kern_proj,
						0,	/* __global kern_hashjoin *khashjoin */
						sizeof(cl_mem),
						&clghj->m_join);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clghj->kern_proj,
						1,	/* __global kern_multihash *kmhash */
						sizeof(cl_mem),
						&clghj->m_hash);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clghj->kern_proj,
						2,	/* __global kern_data_store *kds */
						sizeof(cl_mem),
						&clghj->m_dstore);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clghj->kern_proj,
						3,	/*  __global kern_data_store *ktoast */
						sizeof(cl_mem),
						&clghj->m_ktoast);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clghj->kern_proj,
						4,	/* __global kern_data_store *kds_dest */
						sizeof(cl_mem),
						&clghj->m_kresult);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clSetKernelArg(clghj->kern_proj,
						5,	/* __local void *local_workbuf */
						sizeof(cl_uint) * lwork_sz,
						NULL);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetKernelArg: %s", opencl_strerror(rc));
		goto error;
	}

	rc = clEnqueueNDRangeKernel(clghj->kcmdq,
								clghj->kern_proj,
								1,
								NULL,
								&gwork_sz,
								&lwork_sz,
								1,
								&clghj->events[clghj->ev_index - 1],
								&clghj->events[clghj->ev_index]);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clEnqueueNDRangeKernel: %s",
				   opencl_strerror(rc));
		goto error;
	}
	clghj->ev_kern_proj = clghj->ev_index;
	clghj->ev_index++;
	gpuhashjoin->msg.pfm.num_kern_proj++;

	/*
	 * Write back result status
	 */
	dmaptr = KERN_HASHJOIN_DMA_RECVPTR(&gpuhashjoin->khashjoin);
	offset = KERN_HASHJOIN_DMA_RECVOFS(&gpuhashjoin->khashjoin);
	length = KERN_HASHJOIN_DMA_RECVLEN(&gpuhashjoin->khashjoin);
	rc = clEnqueueReadBuffer(clghj->kcmdq,
							 clghj->m_join,
							 CL_FALSE,
							 offset,
							 length,
							 dmaptr,
							 1,
							 &clghj->events[clghj->ev_index - 1],
							 &clghj->events[clghj->ev_index]);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clEnqueueReadBuffer: %s",
				   opencl_strerror(rc));
		goto error;
	}
	clghj->ev_index++;
    gpuhashjoin->msg.pfm.bytes_dma_recv += length;
    gpuhashjoin->msg.pfm.num_dma_recv++;

	/*
	 * Write back projection data-store
	 */
	rc = clEnqueueReadBuffer(clghj->kcmdq,
							 clghj->m_kresult,
							 CL_FALSE,
							 0,
							 kds_dest->length,
							 kds_dest,
							 1,
							 &clghj->events[clghj->ev_index - 1],
							 &clghj->events[clghj->ev_index]);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clEnqueueReadBuffer: %s",
				   opencl_strerror(rc));
		goto error;
	}
	clghj->ev_index++;
	gpuhashjoin->msg.pfm.bytes_dma_recv += kds_dest->length;
	gpuhashjoin->msg.pfm.num_dma_recv++;

	/*
	 * Last, registers a callback to handle post join process; that generate
	 * a pseudo scan relation
	 */
	rc = clSetEventCallback(clghj->events[clghj->ev_index - 1],
							CL_COMPLETE,
							clserv_respond_hashjoin,
							clghj);
	if (rc != CL_SUCCESS)
	{
		clserv_log("failed on clSetEventCallback: %s", opencl_strerror(rc));
		goto error;
	}
	return;

error:
	if (clghj)
	{
		if (clghj->ev_index > 0)
		{
			clWaitForEvents(clghj->ev_index, clghj->events);
			/* NOTE: first event has to be released under mhtables->lock */
			while (clghj->ev_index > 1)
				clReleaseEvent(clghj->events[--clghj->ev_index]);
		}
		if (clghj->m_kresult)
			clReleaseMemObject(clghj->m_kresult);
		if (clghj->m_ktoast)
			clReleaseMemObject(clghj->m_ktoast);
		if (clghj->m_dstore)
			clReleaseMemObject(clghj->m_dstore);
		if (clghj->m_join)
			clReleaseMemObject(clghj->m_join);
		if (clghj->m_hash)
		{
			SpinLockAcquire(&mhtables->lock);
			Assert(mhtables->n_kernel > 0);
			clReleaseMemObject(mhtables->m_hash);
			clReleaseEvent(mhtables->ev_hash);
			if (--mhtables->n_kernel == 0)
			{
				mhtables->m_hash = NULL;
				mhtables->ev_hash = NULL;
			}
			SpinLockRelease(&mhtables->lock);
		}
		if (clghj->kern_main)
			clReleaseKernel(clghj->kern_main);
		if (clghj->kern_proj)
			clReleaseKernel(clghj->kern_proj);
		if (clghj->program && clghj->program != BAD_OPENCL_PROGRAM)
			clReleaseProgram(clghj->program);
		free(clghj);
	}
	gpuhashjoin->msg.errcode = rc;
	pgstrom_reply_message(&gpuhashjoin->msg);
}
