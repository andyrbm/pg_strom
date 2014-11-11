/*
 * opencl_gpupreagg.h
 *
 * Preprocess of aggregate using GPU acceleration, to reduce number of
 * rows to be processed by CPU; including the Sort reduction.
 * --
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
#ifndef OPENCL_GPUPREAGG_H
#define OPENCL_GPUPREAGG_H

/*
 * Sequential Scan using GPU/MIC acceleration
 *
 * It packs a kern_parambuf and kern_resultbuf structure within a continuous
 * memory ares, to transfer (usually) small chunk by one DMA call.
 *
 * +----------------+  -----
 * | status         |    ^
 * +----------------+    |
 * | rindex_len     |    |
 * +----------------+    |
 * | kern_parambuf  |    |
 * | +--------------+    |
 * | | length   o--------------+
 * | +--------------+    |     | kern_row_map is located just after
 * | | nparams      |    |     | the kern_parambuf (because of DMA
 * | +--------------+    |     | optimization), so head address of
 * | | poffset[0]   |    |     | kern_gpuscan + parambuf.length
 * | | poffset[1]   |    |     | points kern_row_map.
 * | |    :         |    |     |
 * | | poffset[M-1] |    |     |
 * | +--------------+    |     |
 * | | variable     |    |     |
 * | | length field |    |     |
 * | | for Param /  |    |     |
 * | | Const values |    |     |
 * | |     :        |    |     |
 * +-+--------------+ <--------+
 * | kern_row_map   |    |
 * | +--------------+    |
 * | | nvalids (=N) |    |
 * | +--------------+    |
 * | | rindex[0]    |    |
 * | | rindex[1]    |    |
 * | |    :         |    |
 * | | rindex[N]    |    V
 * +-+--------------+  -----
 * | rindex[] for   |    ^
 * |  working of    |    |
 * |  bitonic sort  |  device onlye memory
 * |       :        |    |
 * |       :        |    V
 * +----------------+  -----
 */

typedef struct
{
	cl_int			status;		/* result of kernel execution */
	cl_int			sortbuf_len;/* length of sorting rindex[] that holds
								 * row-index being sorted; must be length
								 * of get_next_log2(nitems)
								 */
	char			__padding[8];	/* align to 128bits */
	kern_parambuf	kparams;
	/*
	 * kern_row_map and rindexp[] for sorting will be here
	 */
} kern_gpupreagg;

/* macro definitions to reference packed values */
#define KERN_GPUPREAGG_PARAMBUF(kgpreagg)				\
	((__global kern_parambuf *)(&(kgpreagg)->kparams))
#define KERN_GPUPREAGG_PARAMBUF_LENGTH(kgpreagg)		\
	(KERN_GPUPREAGG_PARAMBUF(kgpreagg)->length)
#define KERN_GPUPREAGG_KROWMAP(kgpreagg)				\
	((__global kern_row_map *)							\
	 ((__global char *)(kgpreagg) +						\
	  STROMALIGN(offsetof(kern_gpupreagg, kparams) +	\
				 KERN_GPUPREAGG_PARAMBUF_LENGTH(kgpreagg))))
#define KERN_GPUPREAGG_SORT_RINDEX(kgpreagg)			\
	(KERN_GPUPREAGG_KROWMAP(kgpreagg)->rindex +			\
	 (KERN_GPUPREAGG_KROWMAP(kgpreagg)->nvalids < 0		\
	  ? 0 : KERN_GPUPREAGG_KROWMAP(kgpreagg)->nvalids))
#define KERN_GPUPREAGG_BUFFER_SIZE(kgpreagg)			\
	((uintptr_t)(KERN_GPUPREAGG_SORT_RINDEX(kgpreagg) +	\
				 (kgpreagg)->sortbuf_len) -				\
	 (uintptr_t)(kgpreagg))
#define KERN_GPUPREAGG_DMASEND_OFFSET(kgpreagg)			0
#define KERN_GPUPREAGG_DMASEND_LENGTH(kgpreagg)			\
	((uintptr_t)KERN_GPUPREAGG_SORT_RINDEX(kgpreagg) -	\
	 (uintptr_t)(kgpreagg))
#define KERN_GPUPREAGG_DMARECV_OFFSET(kgpreagg)			\
	offsetof(kern_gpupreagg, status)
#define KERN_GPUPREAGG_DMARECV_LENGTH(kgpreagg)			\
	sizeof(cl_uint)

/*
 * NOTE: pagg_datum is a set of information to calculate running total.
 * group_id indicates which group does this work-item belong to, instead
 * of gpupreagg_keycomp().
 * isnull indicates whether the current running total is NULL, or not.
 * XXX_val is a running total itself.
 */
typedef struct
{
	cl_uint			group_id;
	cl_char			isnull;
	cl_char			__padding__[3];
	union {
		cl_short	short_val;
		cl_int		int_val;
		cl_long		long_val;
		cl_float	float_val;
		cl_double	double_val;
	};
} pagg_datum;

/*
 * definition for special system parameter
 *
 * KPARAM_0 - array of the GPUPREAGG_FIELD_IS_* flags as cl_char[] array.
 * Each item informs usage of the related field.
 */
#define GPUPREAGG_FIELD_IS_NULL			0
#define GPUPREAGG_FIELD_IS_GROUPKEY		1
#define GPUPREAGG_FIELD_IS_AGGFUNC		2

#ifdef OPENCL_DEVICE_CODE

/* macro to check overflow on accumlate operation*/
#define CHECK_OVERFLOW_INT(x, y)				\
	((((x) < 0) == ((y) < 0)) && (((x) + (y) < 0) != ((x) < 0)))
	
#define CHECK_OVERFLOW_FLOAT(x, y)				\
	(isinf((x) + (y)) && !isinf(x) && !isinf(y))

/* built-in declarations */
#ifndef PG_INT2_TYPE_DEFINED
#define PG_INT2_TYPE_DEFINED
STROMCL_SIMPLE_TYPE_TEMPLATE(int2,cl_short)
#endif
#ifndef PG_INT4_TYPE_DEFINED
#define PG_INT4_TYPE_DEFINED
STROMCL_SIMPLE_TYPE_TEMPLATE(int4,cl_int)
#endif
#ifndef PG_INT8_TYPE_DEFINED
#define PG_INT8_TYPE_DEFINED
STROMCL_SIMPLE_TYPE_TEMPLATE(int8,cl_long)
#endif

/*
 * comparison function - to be generated by PG-Strom on the fly
 *
 * It compares two records indexed by 'x_index' and 'y_index' on the supplied
 * kern_data_store, then returns -1 if record[X] is less than record[Y],
 * 0 if record[X] is equivalent to record[Y], or 1 if record[X] is greater
 * than record[Y].
 * (auto generated function)
 */
static cl_int
gpupreagg_keycomp(__private cl_int *errcode,
				  __global kern_data_store *kds,
				  __global kern_data_store *ktoast,
				  size_t x_index,
				  size_t y_index);
/*
 * calculation function - to be generated by PG-Strom on the fly
 *
 * It updates the supplied 'accum' value by 'newval' value. Both of data
 * structure is expected to be on the local memory.
 * (auto generated function)
 */
static void
gpupreagg_aggcalc(__private cl_int *errcode,
				  cl_int resno,
				  __local pagg_datum *accum,
				  __local pagg_datum *newval);

/*
 * translate a kern_data_store (input) into an output form
 * (auto generated function)
 */
static void
gpupreagg_projection(__private cl_int *errcode,
					 __global kern_parambuf *kparams,
					 __global kern_data_store *kds_in,
					 __global kern_data_store *kds_src,
					 __global void *ktoast,
					 size_t rowidx_in,
					 size_t rowidx_out);

/*
 * load the data from kern_data_store to pagg_datum structure
 */
static void
gpupreagg_data_load(__local pagg_datum *pdatum,
					__private cl_int *errcode,
					__global kern_data_store *kds,
					__global kern_data_store *ktoast,
					cl_uint colidx, cl_uint rowidx)
{
	kern_colmeta	cmeta;

	if (colidx >= kds->ncols)
	{
		STROM_SET_ERROR(errcode, StromError_DataStoreCorruption);
		return;
	}
	cmeta = kds->colmeta[colidx];
	/*
	 * Right now, expected data length for running total of partial aggregate
	 * are 2, 4, or 8. Elasewhere, it may be a bug.
	 */
	if (cmeta.attlen == sizeof(cl_short))
	{
		__global cl_short  *addr = kern_get_datum(kds,ktoast,colidx,rowidx);
		if (!addr)
			pdatum->isnull = true;
		else
		{
			pdatum->isnull = false;
			pdatum->short_val = *addr;
		}
	}
	else if (cmeta.attlen == sizeof(cl_int))		/* also, cl_float */
	{
		__global cl_int   *addr = kern_get_datum(kds,ktoast,colidx,rowidx);
		if (!addr)
			pdatum->isnull	= true;
		else
		{
			pdatum->isnull	= false;
			pdatum->int_val	= *addr;
		}
	}
	else if (cmeta.attlen == sizeof(cl_long))	/* also, cl_double */
	{
		__global cl_long  *addr = kern_get_datum(kds,ktoast,colidx,rowidx);
		if (!addr)
			pdatum->isnull	= true;
		else
		{
			pdatum->isnull	= false;
			pdatum->long_val= *addr;
		}
	}
	else
	{
		STROM_SET_ERROR(errcode, StromError_DataStoreCorruption);
	}
}

/*
 * store the data from pagg_datum structure to kern_data_store
 */
static void
gpupreagg_data_store(__local pagg_datum *pdatum,
					 __private cl_int *errcode,
					 __global kern_data_store *kds,
					 __global kern_data_store *ktoast,
					 cl_uint colidx, cl_uint rowidx)
{
	kern_colmeta	cmeta;

	if (colidx >= kds->ncols)
	{
		STROM_SET_ERROR(errcode, StromError_DataStoreCorruption);
		return;
	}
	cmeta = kds->colmeta[colidx];
	/*
	 * Right now, expected data length for running total of partial aggregate
	 * are 2, 4, or 8. Elasewhere, it may be a bug.
	 */
	if (cmeta.attlen == sizeof(cl_short))
	{
		pg_int2_t	temp;

		temp.isnull = pdatum->isnull;
		temp.value  = pdatum->short_val;
		pg_int2_vstore(kds, ktoast, errcode, colidx, rowidx, temp);
	}
	else if (cmeta.attlen == sizeof(cl_uint))		/* also, cl_float */
	{
		pg_int4_t	temp;

		temp.isnull	= pdatum->isnull;
		temp.value	= pdatum->int_val;
		pg_int4_vstore(kds, ktoast, errcode, colidx, rowidx, temp);
	}
	else if (cmeta.attlen == sizeof(cl_ulong))	/* also, cl_double */
	{
		pg_int8_t	temp;

		temp.isnull	= pdatum->isnull;
		temp.value	= pdatum->long_val;
		pg_int8_vstore(kds, ktoast, errcode, colidx, rowidx, temp);
	}
	else
	{
		STROM_SET_ERROR(errcode, StromError_DataStoreCorruption);
	}
}

/* gpupreagg_data_move - it moves grouping key from the source kds to
 * the destination kds as is. We assume toast buffer is shared and
 * resource number of varlena key is not changed. So, all we need to
 * do is copying the offset value, not varlena body itself.
 */
static void
gpupreagg_data_move(__private cl_int *errcode,
					__global kern_data_store *kds_src,
					__global kern_data_store *kds_dst,
					__global kern_data_store *ktoast,
					cl_uint colidx,
					cl_uint rowidx_src,
					cl_uint rowidx_dst)
{
	__global Datum	   *src_values;
	__global Datum	   *dst_values;
	__global cl_char   *src_isnull;
	__global cl_char   *dst_isnull;

	if (colidx >= kds_src->ncols || colidx >= kds_dst->ncols)
	{
		STROM_SET_ERROR(errcode, StromError_DataStoreCorruption);
		return;
	}

	src_values = KERN_DATA_STORE_VALUES(kds_src, rowidx_src);
	src_isnull = KERN_DATA_STORE_ISNULL(kds_src, rowidx_src);
	dst_values = KERN_DATA_STORE_VALUES(kds_dst, rowidx_dst);
	dst_isnull = KERN_DATA_STORE_ISNULL(kds_dst, rowidx_dst);

	if (src_isnull[colidx])
	{
		dst_isnull[colidx] = (cl_char) 1;
		dst_values[colidx] = (Datum) 0;
	}
	else
	{
		dst_isnull[colidx] = (cl_char) 0;
		dst_values[colidx] = src_values[colidx];
	}
}

/*
 * gpupreagg_preparation - It translaes an input kern_data_store (that
 * reflects outer relation's tupdesc) into the form of running total
 * and final result of gpupreagg (that reflects target-list of GpuPreAgg).
 *
 * Pay attention on a case when the kern_data_store with row-format is
 * translated. Row-format does not have toast buffer because variable-
 * length fields are in-place. gpupreagg_projection() treats the input
 * kern_data_store as toast buffer of the later stage. So, caller has to
 * give this kern_data_store (never used for data-store in the later
 * stage) as toast buffer if the source kds has row-format.
 */
__kernel void
gpupreagg_preparation(__global kern_gpupreagg *kgpreagg,
					  __global kern_data_store *kds_in,
					  __global kern_data_store *kds_src,
					  KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_parambuf *kparams = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	__global kern_row_map  *krowmap = KERN_GPUPREAGG_KROWMAP(kgpreagg);
	cl_int					errcode = StromError_Success;
	cl_uint					offset;
	cl_uint					nitems;
	size_t					kds_index;
	__local cl_uint			base;


	if (krowmap->nvalids < 0)
		kds_index = get_global_id(0);
	else if (get_global_id(0) < krowmap->nvalids)
		kds_index = (size_t) krowmap->rindex[get_global_id(0)];
	else
		kds_index = kds_in->nitems;	/* ensure this thread is out of range */

	/* calculation of total number of rows to be processed in this work-
	 * group.
	 */
	offset = arithmetic_stairlike_add(kds_index < kds_in->nitems ? 1 : 0,
									  LOCAL_WORKMEM,
									  &nitems);

	/* Allocation of the result slot on the kds_src. */
	if (get_local_id(0) == 0)
	{
		if (nitems > 0)
			base = atomic_add(&kds_src->nitems, nitems);
		else
			base = 0;
	}
	barrier(CLK_LOCAL_MEM_FENCE);

	/* out of range check -- usually, should not happen */
	if (base + nitems > kds_src->nrooms)
	{
		errcode = StromError_DataStoreNoSpace;
		goto out;
	}

	/* do projection */
	if (kds_index < kds_in->nitems)
	{
		gpupreagg_projection(&errcode,
							 kparams,
							 kds_in,			/* input kds */
							 kds_src,			/* source of reduction kds */
							 NULL,				/* never use toast */
							 kds_index,			/* rowidx of kds_in */
							 base + offset);	/* rowidx of kds_src */
	}
out:
	/* write-back execution status into host-side */
	kern_writeback_error_status(&kgpreagg->status, errcode, LOCAL_WORKMEM);
}

/*
 * gpupreagg_reduction - entrypoint of the main logic for GpuPreAgg.
 * The both of kern_data_store have identical form that reflects running 
 * total and final results. rindex will show the sorted order according
 * to the gpupreagg_keycomp() being constructed on the fly.
 * This function makes grouping at first, then run data reduction within
 * the same group. 
 */
#define INVALID_GROUPID	-1

__kernel void
gpupreagg_reduction(__global kern_gpupreagg *kgpreagg,
					__global kern_data_store *kds_src,
					__global kern_data_store *kds_dst,
					__global kern_data_store *ktoast,
					KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_parambuf	*kparams = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	__global cl_int			*rindex  = KERN_GPUPREAGG_SORT_RINDEX(kgpreagg);
	__local pagg_datum *l_data
		= (__local pagg_datum *)STROMALIGN(LOCAL_WORKMEM);
	__global varlena   *kparam_0 = kparam_get_value(kparams, 0);
	__global cl_char   *gpagg_atts = (__global cl_char *) VARDATA(kparam_0);

	cl_int pindex		= 0;

	cl_int ncols		= kds_src->ncols;
	cl_int nrows		= kds_src->nitems;
	cl_int errcode		= StromError_Success;

	cl_int localID		= get_local_id(0);
	cl_int globalID		= get_global_id(0);
    cl_int localSize	= get_local_size(0);

	cl_int prtID		= globalID / localSize;	/* partition ID */
	cl_int prtSize		= localSize;			/* partition Size */
	cl_int prtMask		= prtSize - 1;			/* partition Mask */
	cl_int prtPos		= prtID * prtSize;		/* partition Position */

	cl_int localEntry  = (prtPos+prtSize < nrows) ? prtSize : (nrows-prtPos);

	/* Sanity check of gpagg_atts array */
	if (VARSIZE_EXHDR(kparam_0) != ncols)
	{
		STROM_SET_ERROR(&errcode, StromError_DataStoreCorruption);
		goto out;
	}

	/* Check no data for this work group */
	if(localEntry <= 0) {
		goto out;
	}

	/* Generate group id of local work group. */
	cl_int groupID;
	cl_uint ngroups;
	cl_int isNewID = 0;
	{
		if (localID == 0) 
		{
			isNewID = 1;
		}
		else if (localID < localEntry)
		{
			int rv = gpupreagg_keycomp(&errcode, kds_src, ktoast,
									   rindex[globalID-1], rindex[globalID]);
			isNewID = (rv != 0) ? 1 : 0;
		}
		groupID = (arithmetic_stairlike_add(isNewID, LOCAL_WORKMEM, &ngroups) 
				   + isNewID - 1);
	}

	/* allocation of result buffer */
	__local cl_uint base;
	{
		if (get_local_id(0) == 0)
			base = atomic_add(&kds_dst->nitems, ngroups);
		barrier(CLK_LOCAL_MEM_FENCE);

		if (kds_dst->nrooms < base + ngroups) {
			errcode = StromError_DataStoreNoSpace;
			goto out;
		}
	}

	/* Aggregate for each item. */
	for (cl_int cindex=0; cindex < ncols; cindex++)
	{
		/*
		 * In case when this column is either a grouping-key or not-
		 * referenced one (thus, not a partial aggregation), all we
		 * need to do is copying the data from the source to the
		 * destination; without modification anything.
		 */
		if (gpagg_atts[cindex] != GPUPREAGG_FIELD_IS_AGGFUNC)
		{
			if (isNewID) {
				gpupreagg_data_move(&errcode, kds_src, kds_dst, ktoast,
									cindex,
									rindex[globalID],	/* source rowid */
									base + groupID);	/* destination rowid */
				/* also, fixup varlena datum if needed */
				pg_fixup_tupslot_varlena(&errcode, kds_dst, ktoast,
										 cindex, base + groupID);
			}
			continue;
		}
		pindex++;

		/* Load aggregate item */
		l_data[localID].group_id = INVALID_GROUPID;
		if(localID < localEntry) {
			gpupreagg_data_load(&l_data[localID], &errcode, kds_src, ktoast,
								cindex, rindex[globalID]);
			l_data[localID].group_id = groupID;
		}
		barrier(CLK_LOCAL_MEM_FENCE);

		// Reduction
		for(int unitSize=2; unitSize<=prtSize; unitSize*=2) {
			if(localID % unitSize == unitSize/2  &&  localID < localEntry) {
				cl_int dstID = localID - unitSize/2;
				if(l_data[localID].group_id == l_data[dstID].group_id) {
					// Marge this aggregate data to lower.
					gpupreagg_aggcalc(&errcode, cindex,
									  &l_data[dstID], &l_data[localID]);
					l_data[localID].group_id = INVALID_GROUPID;
				}
			}
			barrier(CLK_LOCAL_MEM_FENCE);

			if(localID % unitSize == unitSize/2  &&  localID < localEntry) {
				if(l_data[localID].group_id != INVALID_GROUPID  &&
				   localID + unitSize/2 < localEntry) {
					cl_int dstID = localID + unitSize/2;
					if(l_data[localID].group_id == l_data[dstID].group_id) {
						// Marge this aggregate data to upper.
						gpupreagg_aggcalc(&errcode, cindex,
										  &l_data[dstID], &l_data[localID]);
						l_data[localID].group_id = INVALID_GROUPID;
					}
				}
			}
			barrier(CLK_LOCAL_MEM_FENCE);
		}

		// write back aggregate data
		if(l_data[localID].group_id != INVALID_GROUPID) {
			gpupreagg_data_store(&l_data[localID], &errcode, kds_dst, ktoast,
								 cindex, base + groupID);
			/*
			 * varlena should never appear here, so we don't need to
			 * put pg_fixup_tupslot_varlena() here
			 */
		}
		barrier(CLK_LOCAL_MEM_FENCE);
	}
out:
	kern_writeback_error_status(&kgpreagg->status, errcode, LOCAL_WORKMEM);
}

/*
 * gpupreagg_set_rindex
 *
 * It makes pseudo rindex value according to the global-id, instead of
 * the sorting by grouping keys. Aggregate functions can be run without
 * grouping keys (that simpliy generate one row consists of the results
 * of aggregate functions only). In this case, we can assume all the
 * records are in a same group, thus rindex[] can be determined without
 * key comparison.
 */
__kernel void
gpupreagg_set_rindex(__global kern_gpupreagg *kgpreagg,
					 __global kern_data_store *kds,
					 KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_parambuf	*kparams  = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	__global cl_int			*rindex	  = KERN_GPUPREAGG_SORT_RINDEX(kgpreagg);

	cl_int nrows		= kds->nitems;
	cl_int errcode		= StromError_Success;
	cl_int globalID		= get_global_id(0);

	if(globalID < nrows)
		rindex[globalID] = globalID;

	kern_writeback_error_status(&kgpreagg->status, errcode, LOCAL_WORKMEM);
}

/*
 * gpupreagg_bitonic_local
 *
 * It tries to apply each steps of bitonic-sorting until its unitsize
 * reaches the workgroup-size (that is expected to power of 2).
 */
__kernel void
gpupreagg_bitonic_local(__global kern_gpupreagg *kgpreagg,
						__global kern_data_store *kds,
						__global kern_data_store *ktoast,
						KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_parambuf	*kparams  = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	__global cl_int			*rindex	  = KERN_GPUPREAGG_SORT_RINDEX(kgpreagg);
	__local  cl_int			*localIdx = LOCAL_WORKMEM;

	cl_int nrows		= kds->nitems;
	cl_int errcode		= StromError_Success;

    cl_int localID		= get_local_id(0);
    cl_int globalID		= get_global_id(0);
    cl_int localSize	= get_local_size(0);

    cl_int prtID		= globalID / localSize; /* partition ID */
    cl_int prtSize		= localSize * 2;		/* partition Size */
    cl_int prtMask		= prtSize - 1;			/* partition Mask */
    cl_int prtPos		= prtID * prtSize;		/* partition Position */

    cl_int localEntry	= ((prtPos + prtSize < nrows)
						   ? prtSize
						   : (nrows - prtPos));

    // create row index and then store to localIdx
    if(localID < localEntry)
		localIdx[localID] = prtPos + localID;

    if(localSize + localID < localEntry)
		localIdx[localSize + localID] = prtPos + localSize + localID;

    barrier(CLK_LOCAL_MEM_FENCE);


	// bitonic sort
	for(int blockSize=2; blockSize<=prtSize; blockSize*=2)
	{
		int blockMask		= blockSize - 1;
		int halfBlockSize	= blockSize / 2;
		int halfBlockMask	= halfBlockSize -1;

		for(int unitSize=blockSize; 2<=unitSize; unitSize/=2)
		{
			int unitMask		= unitSize - 1;
			int halfUnitSize	= unitSize / 2;
			int halfUnitMask	= halfUnitSize - 1;

			bool reversing	= unitSize == blockSize ? true : false;
			int idx0 = ((localID / halfUnitSize) * unitSize
						+ localID % halfUnitSize);
			int idx1 = ((reversing == true)
						? ((idx0 & ~unitMask) | (~idx0 & unitMask))
						: (halfUnitSize + idx0));

			if(idx1 < localEntry) {
				cl_int pos0 = localIdx[idx0];
				cl_int pos1 = localIdx[idx1];
				cl_int rv   = gpupreagg_keycomp(&errcode, kds, ktoast,
												pos0, pos1);

				if(0 < rv) {
					// swap
					localIdx[idx0] = pos1;
					localIdx[idx1] = pos0;
				}
			}
			barrier(CLK_LOCAL_MEM_FENCE);
		}
    }

    if(localID < localEntry)
		rindex[prtPos + localID] = localIdx[localID];

    if(localSize + localID < localEntry)
		rindex[prtPos + localSize + localID] = localIdx[localSize + localID];

	kern_writeback_error_status(&kgpreagg->status, errcode, LOCAL_WORKMEM);
}



/*
 * gpupreagg_bitonic_step
 *
 * It tries to apply individual steps of bitonic-sorting for each step,
 * but does not have restriction of workgroup size. The host code has to
 * control synchronization of each step not to overrun.
 */
__kernel void
gpupreagg_bitonic_step(__global kern_gpupreagg *kgpreagg,
					   cl_int bitonic_unitsz,
					   __global kern_data_store *kds,
					   __global kern_data_store *ktoast,
					   KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_parambuf	*kparams = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	__global cl_int			*rindex	 = KERN_GPUPREAGG_SORT_RINDEX(kgpreagg);

	cl_int	nrows	  = kds->nitems;
	cl_bool reversing = (bitonic_unitsz < 0 ? true : false);
	size_t	unitsz    = (bitonic_unitsz < 0 
						 ? -bitonic_unitsz 
						 : bitonic_unitsz);
	cl_int	errcode	  = StromError_Success;

	cl_int	globalID		= get_global_id(0);
	cl_int	halfUnitSize	= unitsz / 2;
	cl_int	unitMask		= unitsz - 1;

	cl_int	idx0;
	cl_int	idx1;

	idx0 = (globalID / halfUnitSize) * unitsz + globalID % halfUnitSize;
	idx1 = (reversing
			? ((idx0 & ~unitMask) | (~idx0 & unitMask))
			: (idx0 + halfUnitSize));
	if(nrows <= idx1)
		goto out;

	cl_int	pos0	= rindex[idx0];
	cl_int	pos1	= rindex[idx1];
	cl_int	rv;

	rv = gpupreagg_keycomp(&errcode, kds, ktoast, pos0, pos1);
	if(0 < rv) {
		/* Swap */
		rindex[idx0] = pos1;
		rindex[idx1] = pos0;
	}
out:
	kern_writeback_error_status(&kgpreagg->status, errcode, LOCAL_WORKMEM);
}

/*
 * gpupreagg_bitonic_merge
 *
 * It handles the merging step of bitonic-sorting if unitsize becomes less
 * than or equal to the workgroup size.
 */
__kernel void
gpupreagg_bitonic_merge(__global kern_gpupreagg *kgpreagg,
						__global kern_data_store *kds,
						__global kern_data_store *ktoast,
						KERN_DYNAMIC_LOCAL_WORKMEM_ARG)
{
	__global kern_parambuf	*kparams  = KERN_GPUPREAGG_PARAMBUF(kgpreagg);
	__global cl_int			*rindex	  = KERN_GPUPREAGG_SORT_RINDEX(kgpreagg);
	__local	 cl_int			*localIdx = LOCAL_WORKMEM;

	cl_int nrows		= kds->nitems;
	cl_int errcode		= StromError_Success;

    cl_int localID		= get_local_id(0);
    cl_int globalID		= get_global_id(0);
    cl_int localSize	= get_local_size(0);

    cl_int prtID		= globalID / localSize; /* partition ID */
    cl_int prtSize		= localSize * 2;		/* partition Size */
    cl_int prtMask		= prtSize - 1;			/* partition Mask */
    cl_int prtPos		= prtID * prtSize;		/* partition Position */

    cl_int localEntry	= (prtPos+prtSize < nrows) ? prtSize : (nrows-prtPos);


    // load index to localIdx
    if(localID < localEntry)
		localIdx[localID] = rindex[prtPos + localID];

    if(localSize + localID < localEntry)
		localIdx[localSize + localID] = rindex[prtPos + localSize + localID];

    barrier(CLK_LOCAL_MEM_FENCE);


	// marge sorted block
	int blockSize		= prtSize;
	int blockMask		= blockSize - 1;
	int halfBlockSize	= blockSize / 2;
	int halfBlockMask	= halfBlockSize -1;

	for(int unitSize=blockSize; 2<=unitSize; unitSize/=2)
	{
		int unitMask		= unitSize - 1;
		int halfUnitSize	= unitSize / 2;
		int halfUnitMask	= halfUnitSize - 1;

		int idx0 = localID / halfUnitSize * unitSize + localID % halfUnitSize;
		int idx1 = halfUnitSize + idx0;

		if(idx1 < localEntry) {
			cl_int pos0 = localIdx[idx0];
			cl_int pos1 = localIdx[idx1];
			cl_int rv = gpupreagg_keycomp(&errcode, kds, ktoast, pos0, pos1);

			if(0 < rv) {
				// swap
				localIdx[idx0] = pos1;
				localIdx[idx1] = pos0;
			}
		}
		barrier(CLK_LOCAL_MEM_FENCE);
    }

    if(localID < localEntry)
		rindex[prtPos + localID] = localIdx[localID];

    if(localSize + localID < localEntry)
		rindex[prtPos + localSize + localID] = localIdx[localSize + localID];

	kern_writeback_error_status(&kgpreagg->status, errcode, LOCAL_WORKMEM);
}

/*
 * Helper macros for gpupreagg_aggcalc().
 */

#define GPUPREAGG_AGGCALC_PMAX_TEMPLATE(field,accum,newval)			\
	if (!(newval)->isnull)											\
	{																\
		if ((accum)->isnull)										\
			(accum)->##field##_val = (newval)->##field##val;		\
		else														\
			(accum)->##field##_val = max((accum)->##field##_val,	\
										 (newval)->##field##val);	\
		(accum)->isnull = false;									\
	}

#define GPUPREAGG_AGGCALC_PMAX_TEMPLATE(field,accum,newval)			\
	if (!(newval)->isnull)											\
	{																\
		if ((accum)->isnull)										\
			(accum)->##field##_val = (newval)->##field##val;		\
		else														\
			(accum)->##field##_val = min((accum)->##field##_val,	\
										 (newval)->##field##val);	\
		(accum)->isnull = false;									\
	}

#define GPUPREAGG_AGGCALC_PMINMAX_NUMERIC_TEMPLATE(operator,errcode,accum,newval) \
	if (!(newval)->isnull)										\
	{															\
		if ((accum)->isnull)									\
			(accum)->long_val = (newval)->long_val;				\
		else													\
		{														\
			pg_numeric_t	x;									\
			pg_numeric_t	y;									\
																\
			x.isnull = y.isnull = false;						\
			x.value = (accum)->long_val;						\
			y.value = (newval)->long_val;						\
																\
			if (numeric_cmp(errcode,x,y) operator 0)			\
				(accum)->long_val = (newval)->long_val;			\
		}														\
		(accum)->isnull = false;								\
	}

/* In-kernel PMAX() implementation */
#define GPUPREAGG_AGGCALC_PMAX_SHORT(errcode,accum,newval)		\
	GPUPREAGG_AGGCALC_PMAX_TEMPLATE(short,(accum),(newval))
#define GPUPREAGG_AGGCALC_PMAX_INT(errcode,accum,newval)		\
	GPUPREAGG_AGGCALC_PMAX_TEMPLATE(short,(accum),(newval))
#define GPUPREAGG_AGGCALC_PMAX_LONG(errcode,accum,newval)		\
	GPUPREAGG_AGGCALC_PMAX_TEMPLATE(short,(accum),(newval))
#define GPUPREAGG_AGGCALC_PMAX_FLOAT(errcode,accum,newval)		\
	GPUPREAGG_AGGCALC_PMAX_TEMPLATE(short,(accum),(newval))
#define GPUPREAGG_AGGCALC_PMAX_DOUBLE(errcode,accum,newval)		\
	GPUPREAGG_AGGCALC_PMAX_TEMPLATE(short,(accum),(newval))
#define GPUPREAGG_AGGCALC_PMAX_NUMERIC(errcode,accum,newval)	\
	GPUPREAGG_AGGCALC_PMINMAX_NUMERIC_TEMPLATE(<,errcode,accum,newval)

/* In-kernel PMIN() implementation */
#define GPUPREAGG_AGGCALC_PMIN_SHORT(errcode,accum,newval)		\
	GPUPREAGG_AGGCALC_PMIN_TEMPLATE(short,(accum),(newval))
#define GPUPREAGG_AGGCALC_PMIN_INT(errcode,accum,newval)		\
	GPUPREAGG_AGGCALC_PMIN_TEMPLATE(short,(accum),(newval))
#define GPUPREAGG_AGGCALC_PMIN_LONG(errcode,accum,newval)		\
	GPUPREAGG_AGGCALC_PMIN_TEMPLATE(short,(accum),(newval))
#define GPUPREAGG_AGGCALC_PMIN_FLOAT(errcode,accum,newval)		\
	GPUPREAGG_AGGCALC_PMIN_TEMPLATE(short,(accum),(newval))
#define GPUPREAGG_AGGCALC_PMIN_DOUBLE(errcode,accum,newval)		\
	GPUPREAGG_AGGCALC_PMIN_TEMPLATE(short,(accum),(newval))
#define GPUPREAGG_AGGCALC_PMIN_NUMERIC(errcode,accum,newval)	\
	GPUPREAGG_AGGCALC_PMINMAX_NUMERIC_TEMPLATE(>,errcode,accum,newval)

/* In-kernel PSUM() implementation */
#define GPUPREAGG_AGGCALC_PSUM_TEMPLATE(field,check_overflow,errcode,accum,newval) \
	if (!(accum)->isnull)											\
	{																\
		if (!(newval)->isnull)										\
		{															\
			if (check_overflow((accum)->##field##_val,				\
							   (newval)->##field##_val))			\
				STROM_SET_ERROR(errcode, StromError_CpuReCheck);	\
			(accum)->##field##_val += (newval)->##field##_val;		\
		}															\
	}																\
	else if (!(newval)->isnull)										\
	{																\
		(accum)->isnull = (newval)->isnull;							\
		(accum)->##field##_val = (newval)->##field##_val;			\
	}

#define GPUPREAGG_AGGCALC_PSUM_SHORT(errcode,accum,newval)			\
	GPUPREAGG_AGGCALC_PSUM_TEMPLATE(short,CHECK_OVERFLOW_INT,(errcode),(accum),(newval))
#define GPUPREAGG_AGGCALC_PSUM_INT(errcode,accum,newval)			\
	GPUPREAGG_AGGCALC_PSUM_TEMPLATE(int,CHECK_OVERFLOW_INT,(errcode),(accum),(newval))
#define GPUPREAGG_AGGCALC_PSUM_LONG(errcode,accum,newval)			\
	GPUPREAGG_AGGCALC_PSUM_TEMPLATE(long,CHECK_OVERFLOW_INT,(errcode),(accum),(newval))
#define GPUPREAGG_AGGCALC_PSUM_FLOAT(errcode,accum,newval)			\
	GPUPREAGG_AGGCALC_PSUM_TEMPLATE(float,CHECK_OVERFLOW_FLOAT,(errcode),(accum),(newval))
#define GPUPREAGG_AGGCALC_PSUM_DOUBLE(errcode,accum,newval)			\
	GPUPREAGG_AGGCALC_PSUM_TEMPLATE(double,CHECK_OVERFLOW_FLOAT,(errcode),(accum),(newval))
#define GPUPREAGG_AGGCALC_PSUM_NUMERIC(errcode,accum,newval)		\
	if (!(accum)->isnull)											\
	{																\
		if (!(newval)->isnull)										\
		{															\
			pg_numeric_t	x;										\
			pg_numeric_t	y;										\
			pg_numeric_t	r;										\
																	\
			x.isnull = y.isnull = false;							\
			x.value = (accum)->long_value;							\
			y.value = (newval)->long_value;							\
																	\
			r = pgfn_numeric_add(errcode,x,y);						\
																	\
			(accum)->value = r.value;								\
		}															\
		else if (!(newval)->isnull)									\
		{															\
			(accum)->isnull = (newval)->isnull;						\
			(accum)->##field##_val = (newval)->##field##_val;		\
		}															\
	}

#else
/* Host side representation of kern_gpupreagg. It can perform as a message
 * object of PG-Strom, has key of OpenCL device program, a source row/column
 * store and a destination kern_data_store.
 */
typedef struct
{
	pgstrom_message		msg;		/* = StromTag_GpuPreAgg */
	Datum				dprog_key;	/* key of device program */
	bool				needs_grouping;	/* true, if it needs grouping step */
	double				num_groups;	/* estimated number of groups */
	pgstrom_data_store *pds;		/* source data-store */
	pgstrom_data_store *pds_dest;	/* result data-store */
	kern_gpupreagg		kern;		/* kernel portion to be sent */
} pgstrom_gpupreagg;
#endif	/* OPENCL_DEVICE_CODE */
#endif	/* OPENCL_GPUPREAGG_H */
