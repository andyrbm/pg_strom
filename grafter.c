/*
 * grafter.c
 *
 * Routines to modify plan tree once constructed.
 * ----
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "postgres.h"
#include "optimizer/planner.h"
#include "pg_strom.h"

static planner_hook_type	planner_hook_next;

static Plan *
grafter_try_replace_recurse(PlannedStmt *pstmt, Plan *plan)
{
	Plan	   *newnode = plan;
	Plan	   *temp;
	List	   *newlist = NIL;
	ListCell   *lc;

	if (!plan)
		return NULL;

	switch (nodeTag(plan))
	{
		case T_Agg:
			/*
			 * Try to inject GpuPreAgg plan if cost of the aggregate plan
			 * is enough expensive to justify preprocess by GPU.
			 */
			pgstrom_try_insert_gpupreagg(pstmt, (Agg *) plan);
			break;

		case T_ModifyTable:
			{
				ModifyTable *mtplan = (ModifyTable *) newnode;

				foreach (lc, mtplan->plans)
				{
					temp = grafter_try_replace_recurse(pstmt, lfirst(lc));
					newlist = lappend(newlist, temp);
				}
				mtplan->plans = newlist;
			}
			break;
		case T_Append:
			{
				Append *aplan = (Append *) newnode;

				foreach (lc, aplan->appendplans)
				{
					temp = grafter_try_replace_recurse(pstmt, lfirst(lc));
					newlist = lappend(newlist, temp);
				}
				aplan->appendplans = newlist;
			}
			break;
		case T_MergeAppend:
			{
				MergeAppend *maplan = (MergeAppend *) newnode;

				foreach (lc, maplan->mergeplans)
				{
					temp = grafter_try_replace_recurse(pstmt, lfirst(lc));
					newlist = lappend(newlist, temp);
				}
				maplan->mergeplans = newlist;
			}
			break;
		case T_BitmapAnd:
			{
				BitmapAnd  *baplan = (BitmapAnd *) newnode;

				foreach (lc, baplan->bitmapplans)
				{
					temp = grafter_try_replace_recurse(pstmt, lfirst(lc));
					newlist = lappend(newlist, temp);
				}
				baplan->bitmapplans = newlist;
			}
			break;
		case T_BitmapOr:
			{
				BitmapOr   *boplan = (BitmapOr *) newnode;

				foreach (lc, boplan->bitmapplans)
				{
					temp = grafter_try_replace_recurse(pstmt, lfirst(lc));
					newlist = lappend(newlist, temp);
				}
				boplan->bitmapplans = newlist;
			}
			break;
		default:
			/* nothing to do, keep existgin one */
			break;
	}

	/* also walk down left and right child plan sub-tree, if any */
	newnode->lefttree
		= grafter_try_replace_recurse(pstmt, newnode->lefttree);
	newnode->righttree
		= grafter_try_replace_recurse(pstmt, newnode->righttree);

	return newnode;
}

static PlannedStmt *
pgstrom_grafter_entrypoint(Query *parse,
						   int cursorOptions,
						   ParamListInfo boundParams)
{
	PlannedStmt	*result;

	if (planner_hook_next)
		result = planner_hook_next(parse, cursorOptions, boundParams);
	else
		result = standard_planner(parse, cursorOptions, boundParams);

	if (pgstrom_enabled)
	{
		List	   *sub_plans = NIL;
		ListCell   *cell;

		result->planTree = grafter_try_replace_recurse(result,
													   result->planTree);
		foreach (cell, result->subplans)
		{
			Plan   *old_plan = lfirst(cell);
			Plan   *new_plan;

			new_plan = grafter_try_replace_recurse(result, old_plan);
			sub_plans = lappend(sub_plans, new_plan);
		}
		result->subplans = sub_plans;
	}
	return result;
}

void
pgstrom_init_grafter(void)
{
	/* hook registration */
	planner_hook_next = planner_hook;
	planner_hook = pgstrom_grafter_entrypoint;
}
