/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2008 by Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Geoffery Bantle
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/blenlib/intern/BLI_mempool.c
 *  \ingroup bli
 *
 * Simple, fast memory allocator for allocating many elements of the same size.
 */

#include "BLI_utildefines.h"
#include "BLI_listbase.h"

#include "BLI_mempool.h" /* own include */

#include "DNA_listBase.h"

#include "MEM_guardedalloc.h"

#include <string.h>
#include <stdlib.h>

#ifdef __GNUC__
#  pragma GCC diagnostic error "-Wsign-conversion"
#  if (__GNUC__ * 100 + __GNUC_MINOR__) >= 406  /* gcc4.6+ only */
#    pragma GCC diagnostic error "-Wsign-compare"
#    pragma GCC diagnostic error "-Wconversion"
#  endif
#endif

/* note: copied from BLO_blend_defs.h, don't use here because we're in BLI */
#ifdef __BIG_ENDIAN__
/* Big Endian */
#  define MAKE_ID(a, b, c, d) ( (int)(a) << 24 | (int)(b) << 16 | (c) << 8 | (d) )
#else
/* Little Endian */
#  define MAKE_ID(a, b, c, d) ( (int)(d) << 24 | (int)(c) << 16 | (b) << 8 | (a) )
#endif

#define FREEWORD MAKE_ID('f', 'r', 'e', 'e')

/* currently totalloc isnt used */
// #define USE_TOTALLOC
// #define USE_DATA_PTR

typedef struct BLI_freenode {
	struct BLI_freenode *next;
	int freeword; /* used to identify this as a freed node */
} BLI_freenode;

typedef struct BLI_mempool_chunk {
	struct BLI_mempool_chunk *next, *prev;
#ifdef USE_DATA_PTR
	void *_data;
#endif
} BLI_mempool_chunk;

struct BLI_mempool {
	struct ListBase chunks;
	int esize;         /* element size in bytes */
	int csize;         /* chunk size in bytes */
	int pchunk;        /* number of elements per chunk */
	int flag;
	/* keeps aligned to 16 bits */

	BLI_freenode *free;    /* free element list. Interleaved into chunk datas. */
	int totused;           /* number of elements currently in use */
#ifdef USE_TOTALLOC
	int totalloc;          /* number of elements allocated in total */
#endif
};

#define MEMPOOL_ELEM_SIZE_MIN (sizeof(void *) * 2)

#ifdef USE_DATA_PTR
#  define CHUNK_DATA(chunk) (chunk)->_data
#else
#  define CHUNK_DATA(chunk) (CHECK_TYPE_INLINE(chunk, BLI_mempool_chunk *), (void *)((chunk) + 1))
#endif

static BLI_mempool_chunk *mempool_chunk_alloc(BLI_mempool *pool)
{
	BLI_mempool_chunk *mpchunk;
#ifdef USE_DATA_PTR
	if (pool->flag & BLI_MEMPOOL_SYSMALLOC) {
		mpchunk = malloc(sizeof(BLI_mempool_chunk));
		CHUNK_DATA(mpchunk) = malloc((size_t)pool->csize);
	}
	else {
		mpchunk = MEM_mallocN(sizeof(BLI_mempool_chunk), "BLI_Mempool Chunk");
		CHUNK_DATA(mpchunk) = MEM_mallocN((size_t)pool->csize, "BLI Mempool Chunk Data");
	}
#else
	if (pool->flag & BLI_MEMPOOL_SYSMALLOC) {
		mpchunk = malloc(sizeof(BLI_mempool_chunk) + (size_t)pool->csize);
	}
	else {
		mpchunk = MEM_mallocN(sizeof(BLI_mempool_chunk) + (size_t)pool->csize, "BLI_Mempool Chunk");
	}
#endif

	return mpchunk;
}

/**
 * Initialize a chunk and add into \a pool->chunks
 *
 * \param pool  The pool to add the chunk into.
 * \param mpchunk  The new uninitialized chunk (can be malloc'd)
 * \param lasttail  The last element of the previous chunk
 * (used when building free chunks initially)
 * \return The last chunk,
 */
static BLI_freenode *mempool_chunk_add(BLI_mempool *pool, BLI_mempool_chunk *mpchunk,
                                       BLI_freenode *lasttail)
{
	BLI_freenode *curnode = NULL;
	const int pchunk_last = pool->pchunk - 1;
	char *addr;
	int j;

	mpchunk->next = mpchunk->prev = NULL;
	BLI_addtail(&(pool->chunks), mpchunk);

	if (pool->free == NULL) {
		pool->free = CHUNK_DATA(mpchunk); /* start of the list */
		if (pool->flag & BLI_MEMPOOL_ALLOW_ITER) {
			pool->free->freeword = FREEWORD;
		}
	}

	/* loop through the allocated data, building the pointer structures */
	for (addr = CHUNK_DATA(mpchunk), j = 0; j <= pchunk_last; j++) {
		curnode = ((BLI_freenode *)addr);
		addr += pool->esize;
		curnode->next = (BLI_freenode *)addr;
		if (pool->flag & BLI_MEMPOOL_ALLOW_ITER) {
			if (j != pchunk_last)
				curnode->next->freeword = FREEWORD;
			curnode->freeword = FREEWORD;
		}
	}

	/* terminate the list,
	 * will be overwritten if 'curnode' gets passed in again as 'lasttail' */
	curnode->next = NULL;

#ifdef USE_TOTALLOC
	pool->totalloc += pool->pchunk;
#endif

	/* final pointer in the previously allocated chunk is wrong */
	if (lasttail) {
		lasttail->next = CHUNK_DATA(mpchunk);
		if (pool->flag & BLI_MEMPOOL_ALLOW_ITER) {
			lasttail->freeword = FREEWORD;
		}
	}

	return curnode;
}

static void mempool_chunk_free_all(BLI_mempool *pool)
{
	BLI_mempool_chunk *mpchunk, *mpchunk_next;

	if (pool->flag & BLI_MEMPOOL_SYSMALLOC) {
		for (mpchunk = pool->chunks.first; mpchunk; mpchunk = mpchunk_next) {
			mpchunk_next = mpchunk->next;
#ifdef USE_DATA_PTR
			free(CHUNK_DATA(mpchunk));
#endif
			free(mpchunk);
		}
	}
	else {
		for (mpchunk = pool->chunks.first; mpchunk; mpchunk = mpchunk_next) {
			mpchunk_next = mpchunk->next;
#ifdef USE_DATA_PTR
			MEM_freeN(CHUNK_DATA(mpchunk));
#endif
			MEM_freeN(mpchunk);
		}
	}
	pool->chunks.first = pool->chunks.last = NULL;
}

BLI_mempool *BLI_mempool_create(int esize, int totelem, int pchunk, int flag)
{
	BLI_mempool *pool = NULL;
	BLI_freenode *lasttail = NULL, *curnode = NULL;
	int i, maxchunks;

	/* allocate the pool structure */
	if (flag & BLI_MEMPOOL_SYSMALLOC) {
		pool = malloc(sizeof(BLI_mempool));
	}
	else {
		pool = MEM_mallocN(sizeof(BLI_mempool), "memory pool");
	}

	/* set the elem size */
	if (esize < (int)MEMPOOL_ELEM_SIZE_MIN) {
		esize = (int)MEMPOOL_ELEM_SIZE_MIN;
	}

	if (flag & BLI_MEMPOOL_ALLOW_ITER) {
		pool->esize = MAX2(esize, (int)sizeof(BLI_freenode));
	}
	else {
		pool->esize = esize;
	}

	pool->flag = flag;
	pool->pchunk = pchunk;
	pool->csize = esize * pchunk;
	pool->chunks.first = pool->chunks.last = NULL;
	pool->free = NULL;  /* mempool_chunk_add assigns */
#ifdef USE_TOTALLOC
	pool->totalloc = 0;
#endif
	pool->totused = 0;

	maxchunks = totelem / pchunk + 1;
	if (maxchunks == 0) {
		maxchunks = 1;
	}

	/* allocate the actual chunks */
	for (i = 0; i < maxchunks; i++) {
		BLI_mempool_chunk *mpchunk = mempool_chunk_alloc(pool);

		curnode = mempool_chunk_add(pool, mpchunk, lasttail);

		/* set the end of this chunks memory to the new tail for next iteration */
		lasttail = curnode;
	}

	return pool;
}

void *BLI_mempool_alloc(BLI_mempool *pool)
{
	void *retval = NULL;

	pool->totused++;

	if (!(pool->free)) {
		/* need to allocate a new chunk */
		BLI_mempool_chunk *mpchunk = mempool_chunk_alloc(pool);
		mempool_chunk_add(pool, mpchunk, NULL);
	}

	retval = pool->free;

	if (pool->flag & BLI_MEMPOOL_ALLOW_ITER) {
		pool->free->freeword = 0x7FFFFFFF;
	}

	pool->free = pool->free->next;
	//memset(retval, 0, pool->esize);
	return retval;
}

void *BLI_mempool_calloc(BLI_mempool *pool)
{
	void *retval = BLI_mempool_alloc(pool);
	memset(retval, 0, (size_t)pool->esize);
	return retval;
}

/**
 * Free an element from the mempool.
 *
 * \note doesnt protect against double frees, don't be stupid!
 */
void BLI_mempool_free(BLI_mempool *pool, void *addr)
{
	BLI_freenode *newhead = addr;

#ifndef NDEBUG
	{
		BLI_mempool_chunk *chunk;
		bool found = false;
		for (chunk = pool->chunks.first; chunk; chunk = chunk->next) {
			if (ARRAY_HAS_ITEM((char *)addr, (char *)CHUNK_DATA(chunk), pool->csize)) {
				found = true;
				break;
			}
		}
		if (!found) {
			BLI_assert(!"Attempt to free data which is not in pool.\n");
		}
	}
#endif

	if (pool->flag & BLI_MEMPOOL_ALLOW_ITER) {
#ifndef NDEBUG
		/* this will detect double free's */
		BLI_assert(newhead->freeword != FREEWORD);
#endif
		newhead->freeword = FREEWORD;
	}

	newhead->next = pool->free;
	pool->free = newhead;

	pool->totused--;

	/* nothing is in use; free all the chunks except the first */
	if (pool->totused == 0) {
		BLI_freenode *curnode = NULL;
		char *tmpaddr = NULL;
		int i;
		BLI_mempool_chunk *first = pool->chunks.first;

		BLI_remlink(&pool->chunks, first);
		mempool_chunk_free_all(pool);
		BLI_addtail(&pool->chunks, first);
#ifdef USE_TOTALLOC
		pool->totalloc = pool->pchunk;
#endif

		pool->free = CHUNK_DATA(first); /* start of the list */
		for (tmpaddr = CHUNK_DATA(first), i = 0; i < pool->pchunk; i++) {
			curnode = ((BLI_freenode *)tmpaddr);
			tmpaddr += pool->esize;
			curnode->next = (BLI_freenode *)tmpaddr;
		}
		curnode->next = NULL; /* terminate the list */
	}
}

int BLI_mempool_count(BLI_mempool *pool)
{
	return pool->totused;
}

void *BLI_mempool_findelem(BLI_mempool *pool, int index)
{
	BLI_assert(pool->flag & BLI_MEMPOOL_ALLOW_ITER);

	if ((index >= 0) && (index < pool->totused)) {
		/* we could have some faster mem chunk stepping code inline */
		BLI_mempool_iter iter;
		void *elem;
		BLI_mempool_iternew(pool, &iter);
		for (elem = BLI_mempool_iterstep(&iter); index-- != 0; elem = BLI_mempool_iterstep(&iter)) {
			/* do nothing */
		}
		return elem;
	}

	return NULL;
}

/**
 * Fill in \a data with pointers to each element of the mempool,
 * to create lookup table.
 *
 * \param data array of pointers at least the size of 'pool->totused'
 */
void BLI_mempool_as_table(BLI_mempool *pool, void **data)
{
	BLI_mempool_iter iter;
	void *elem;
	void **p = data;
	BLI_assert(pool->flag & BLI_MEMPOOL_ALLOW_ITER);
	BLI_mempool_iternew(pool, &iter);
	while ((elem = BLI_mempool_iterstep(&iter))) {
		*p++ = elem;
	}
	BLI_assert((p - data) == pool->totused);
}

/**
 * A version of #BLI_mempool_as_table that allocates and returns the data.
 */
void **BLI_mempool_as_tableN(BLI_mempool *pool, const char *allocstr)
{
	void **data = MEM_mallocN((size_t)pool->totused * sizeof(void *), allocstr);
	BLI_mempool_as_table(pool, data);
	return data;
}

/**
 * Fill in \a data with the contents of the mempool.
 */
void BLI_mempool_as_array(BLI_mempool *pool, void *data)
{
	BLI_mempool_iter iter;
	char *elem, *p = data;
	BLI_assert(pool->flag & BLI_MEMPOOL_ALLOW_ITER);
	BLI_mempool_iternew(pool, &iter);
	while ((elem = BLI_mempool_iterstep(&iter))) {
		memcpy(p, elem, (size_t)pool->esize);
		p += pool->esize;
	}
	BLI_assert((p - (char *)data) == pool->totused * pool->esize);
}

/**
 * A version of #BLI_mempool_as_array that allocates and returns the data.
 */
void *BLI_mempool_as_arrayN(BLI_mempool *pool, const char *allocstr)
{
	char *data = MEM_mallocN((size_t)(pool->totused * pool->esize), allocstr);
	BLI_mempool_as_array(pool, data);
	return data;
}

void BLI_mempool_iternew(BLI_mempool *pool, BLI_mempool_iter *iter)
{
	BLI_assert(pool->flag & BLI_MEMPOOL_ALLOW_ITER);

	iter->pool = pool;
	iter->curchunk = pool->chunks.first;
	iter->curindex = 0;
}

#if 0
/* unoptimized, more readable */

static void *bli_mempool_iternext(BLI_mempool_iter *iter)
{
	void *ret = NULL;

	if (!iter->curchunk || !iter->pool->totused) return NULL;

	ret = ((char *)iter->curchunk->data) + iter->pool->esize * iter->curindex;

	iter->curindex++;

	if (iter->curindex >= iter->pool->pchunk) {
		iter->curchunk = iter->curchunk->next;
		iter->curindex = 0;
	}

	return ret;
}

void *BLI_mempool_iterstep(BLI_mempool_iter *iter)
{
	BLI_freenode *ret;

	do {
		ret = bli_mempool_iternext(iter);
	} while (ret && ret->freeword == FREEWORD);

	return ret;
}

#else

/* optimized version of code above */

void *BLI_mempool_iterstep(BLI_mempool_iter *iter)
{
	BLI_freenode *ret;

	if (UNLIKELY(iter->pool->totused == 0)) {
		return NULL;
	}

	do {
		if (LIKELY(iter->curchunk)) {
			ret = (BLI_freenode *)(((char *)CHUNK_DATA(iter->curchunk)) + iter->pool->esize * iter->curindex);
		}
		else {
			return NULL;
		}

		if (UNLIKELY(++iter->curindex >= iter->pool->pchunk)) {
			iter->curindex = 0;
			iter->curchunk = iter->curchunk->next;
		}
	} while (ret->freeword == FREEWORD);

	return ret;
}

#endif

void BLI_mempool_clear(BLI_mempool *pool)
{
	BLI_mempool_chunk *first = pool->chunks.first;

	BLI_remlink(&pool->chunks, first);

	mempool_chunk_free_all(pool);

	/* important for re-initializing */
	pool->totused = 0;
#ifdef USE_TOTALLOC
	pool->totalloc = 0;
#endif
	pool->free = NULL;

	mempool_chunk_add(pool, first, NULL);
}

/**
 * Free the mempool its self (and all elements).
 */
void BLI_mempool_destroy(BLI_mempool *pool)
{
	mempool_chunk_free_all(pool);

	if (pool->flag & BLI_MEMPOOL_SYSMALLOC) {
		free(pool);
	}
	else {
		MEM_freeN(pool);
	}
}
