/*
 * Copyright (c) 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <array.h>
#include <thread.h>
#include <current.h>
#include <synch.h>
#include <mainbus.h>
#include <vfs.h>
#include <fs.h>
#include <buf.h>

DECLARRAY(buf);
DEFARRAY(buf, /*noinline*/);

/*
 * The required size for all buffers.
 */
#define ONE_TRUE_BUFFER_SIZE		512

/*
 * Illegal array index.
 */
#define INVALID_INDEX ((unsigned)-1)

/*
 * One buffer.
 */
struct buf {
	/* maintenance */
	unsigned b_tableindex;	/* index into {{de,at}tached,busy}_buffers */
	unsigned b_bucketindex;	/* index into buffer_hash bucket */

	/* status flags */
	unsigned b_attached:1;	/* key fields are valid */
	unsigned b_busy:1;	/* currently in use */
	unsigned b_valid:1;	/* contains real data */
	unsigned b_dirty:1;	/* data needs to be written to disk */
	struct thread *b_holder; /* who did buffer_mark_busy() */

	/* key */
	struct fs *b_fs;	/* file system buffer belongs to */
	daddr_t b_physblock;	/* physical block number */

	/* value */
	void *b_data;
	size_t b_size;
};

/*
 * Buffer hash table.
 */
struct bufhash {
	unsigned bh_numbuckets;
	struct bufarray *bh_buckets;
};

/*
 * Global state.
 *
 * Each buffer should be in one of three arrays: detached_buffers,
 * attached_buffers, or busy_buffers.
 *
 * attached_buffers is maintained in LRU order. The other arrays are
 * unordered.
 *
 * Space in all three arrays is preallocated when buffers are created
 * so insert/remove ops won't fail on the fly.
 *
 * To avoid spending a lot of time reshuffling attached_buffers, we
 * preallocate it with extra space and compact it only when the extra
 * space runs out.
 *
 * Attached and busy buffers are also referenced by buffer_hash; this
 * is an index (in the database sense) that allows lookup by fs
 * pointer and disk address.
 *
 * Buffers busy because of file system activity (that is, returned by
 * buffer_get or buffer_read) should be in the busy_buffers array.
 * Buffers busy because they're being written out by the syncer or
 * someone evicting them are *not* moved to the busy_buffers array but
 * are left on the attached_buffers array, to maintain their LRU
 * position.
 */

static struct bufarray detached_buffers;

static struct bufarray attached_buffers;
static unsigned attached_buffers_first;	  /* hint for first empty element */
static unsigned attached_buffers_thresh;  /* size limit before compacting */

static struct bufarray busy_buffers;

static struct bufhash buffer_hash;

/*
 * Counters.
 */
static unsigned num_detached_buffers;
static unsigned num_attached_buffers;
static unsigned num_busy_buffers;
static unsigned num_dirty_buffers;
static unsigned num_reserved_buffers;
static unsigned num_total_buffers;
static unsigned max_total_buffers;

static unsigned num_total_gets;
static unsigned num_valid_gets;
static unsigned num_total_evictions;
static unsigned num_dirty_evictions;

static int doom_counter = -1;
/*
 * Lock
 */

static struct lock *buffer_lock;

/*
 * CVs
 */
static struct cv *buffer_busy_cv;
static struct cv *buffer_reserve_cv;
static struct cv *syncer_cv;

/*
 * Magic numbers (also search the code for "voodoo:")
 *
 * Note that these are put here like this so they're easy to find and
 * tune. There's no particular reason one shouldn't or couldn't
 * completely change the way they're computed. One could, for example,
 * factor buffer reservation calls into some of these decisions somehow.
 */

/* Factor for choosing attached_buffers_thresh. */
#define ATTACHED_THRESH_NUM	3
#define ATTACHED_THRESH_DENOM	2

/* Threshold proportion (of bufs dirty) for starting the syncer */
#define SYNCER_DIRTY_NUM	1
#define SYNCER_DIRTY_DENOM	2

/* Target proportion (of total bufs) for syncer to clean in one run */
#define SYNCER_TARGET_NUM	1
#define SYNCER_TARGET_DENOM	4

/* Limit on the previous proportion, as proportion of dirty buffers */
#define SYNCER_LIMIT_NUM	1
#define SYNCER_LIMIT_DENOM	2

/* Overall limit on fraction of main memory to use for buffers */
#define BUFFER_MAXMEM_NUM	1
#define BUFFER_MAXMEM_DENOM	4

////////////////////////////////////////////////////////////
// state invariants

/*
 * Check consistency of the global state.
 */
static
void
bufcheck(void)
{
	KASSERT(num_detached_buffers == bufarray_num(&detached_buffers));
	KASSERT(num_attached_buffers <= bufarray_num(&attached_buffers));
	KASSERT(num_busy_buffers == bufarray_num(&busy_buffers));

	KASSERT(attached_buffers_first <= bufarray_num(&attached_buffers));
	KASSERT(bufarray_num(&attached_buffers) <= attached_buffers_thresh);

	KASSERT(num_detached_buffers + num_attached_buffers + num_busy_buffers
		== num_total_buffers);
	KASSERT(num_busy_buffers <= num_reserved_buffers);
	KASSERT(num_reserved_buffers <= max_total_buffers);
	KASSERT(num_total_buffers <= max_total_buffers);
}

////////////////////////////////////////////////////////////
// supplemental array ops

/*
 * This knows how the array implementation works, and should probably
 * be moved to array.c.
 */
static
int
bufarray_preallocate(struct bufarray *a, unsigned maxnum)
{
	int result;
	unsigned num;

	num = bufarray_num(a);
	/* grow it temporarily */
	result = bufarray_setsize(a, maxnum);
	if (result) {
		return result;
	}
	/* and set the size back */
	result = bufarray_setsize(a, num);
	KASSERT(result == 0);
	return 0;
}

/*
 * Remove an entry from a bufarray, and (unlike bufarray_remove) don't
 * preserve order.
 *
 * Use fixup() to correct the index stored in the object that gets
 * moved. Sigh.
 */
static
void
bufarray_remove_unordered(struct bufarray *a, unsigned index,
			  void (*fixup)(struct buf *,
					unsigned oldix, unsigned newix))
{
	unsigned num;
	struct buf *b;
	int result;

	num = bufarray_num(a);
	if (index < num-1) {
		b = bufarray_get(a, num-1);
		fixup(b, num-1, index);
		bufarray_set(a, index, b);
	}
	result = bufarray_setsize(a, num-1);
	/* shrinking, should not fail */
	KASSERT(result == 0);
}

/*
 * Routines for that fixup()...
 */
static
void
buf_fixup_bucketindex(struct buf *b, unsigned oldix, unsigned newix)
{
	KASSERT(b->b_bucketindex == oldix);
	b->b_bucketindex = newix;
}

static
void
buf_fixup_tableindex(struct buf *b, unsigned oldix, unsigned newix)
{
	KASSERT(b->b_tableindex == oldix);
	b->b_tableindex = newix;
}

////////////////////////////////////////////////////////////
// bufhash

/*
 * Set up a bufhash.
 */
static
int
bufhash_init(struct bufhash *bh, unsigned numbuckets)
{
	unsigned i;

	bh->bh_buckets = kmalloc(numbuckets*sizeof(*bh->bh_buckets));
	if (bh->bh_buckets == NULL) {
		return ENOMEM;
	}
	for (i=0; i<numbuckets; i++) {
		bufarray_init(&bh->bh_buckets[i]);
	}
	bh->bh_numbuckets = numbuckets;
	return 0;
}

#if 0 /* not used */
/*
 * Destroy a bufhash.
 */
static
int
bufhash_cleanup(struct bufhash *bh)
{
	...
}
#endif /* 0 -- not used */

/*
 * Hash function.
 */
static
unsigned
buffer_hashfunc(struct fs *fs, daddr_t physblock)
{
	unsigned val = 0;

	/* there is nothing particularly special or good about this */
	val = 0xfeeb1e;
	val ^= ((uintptr_t)fs) >> 6;
	val ^= physblock;
	return val;
}

/*
 * Add a buffer to a bufhash.
 */
static
int
bufhash_add(struct bufhash *bh, struct buf *b)
{
	unsigned hash, bn;

	KASSERT(b->b_bucketindex == INVALID_INDEX);

	hash = buffer_hashfunc(b->b_fs, b->b_physblock);
	bn = hash % bh->bh_numbuckets;
	return bufarray_add(&bh->bh_buckets[bn], b, &b->b_bucketindex);
}

/*
 * Remove a buffer from a bufhash.
 */
static
void
bufhash_remove(struct bufhash *bh, struct buf *b)
{
	unsigned hash, bn;

	hash = buffer_hashfunc(b->b_fs, b->b_physblock);
	bn = hash % bh->bh_numbuckets;

	KASSERT(bufarray_get(&bh->bh_buckets[bn], b->b_bucketindex) == b);
	bufarray_set(&bh->bh_buckets[bn], b->b_bucketindex, NULL);
	bufarray_remove_unordered(&bh->bh_buckets[bn], b->b_bucketindex,
				  buf_fixup_bucketindex);
	b->b_bucketindex = INVALID_INDEX;
}

/*
 * Find a buffer in a bufhash.
 */
static
struct buf *
bufhash_get(struct bufhash *bh, struct fs *fs, daddr_t physblock)
{
	unsigned hash, bn;
	unsigned num, i;
	struct buf *b;

	hash = buffer_hashfunc(fs, physblock);
	bn = hash % bh->bh_numbuckets;

	num = bufarray_num(&bh->bh_buckets[bn]);
	for (i=0; i<num; i++) {
		b = bufarray_get(&bh->bh_buckets[bn], i);
		KASSERT(b->b_bucketindex == i);
		if (b->b_fs == fs && b->b_physblock == physblock) {
			/* found */
			return b;
		}
	}
	return NULL;
}

////////////////////////////////////////////////////////////
// buffer tables

/*
 * Preallocate the buffer lists so adding things to them on the fly
 * can't blow up.
 */
static
int
preallocate_buffer_arrays(unsigned newtotal)
{
	int result;
	unsigned newthresh;

	newthresh = (newtotal*ATTACHED_THRESH_NUM)/ATTACHED_THRESH_DENOM;

	result = bufarray_preallocate(&detached_buffers, newtotal);
	if (result) {
		return result;
	}

	result = bufarray_preallocate(&attached_buffers, newthresh);
	if (result) {
		return result;
	}
	attached_buffers_thresh = newthresh;

	result = bufarray_preallocate(&busy_buffers, newtotal);
	if (result) {
		return result;
	}
	
	return 0;
}

/*
 * Go through the attached_buffers array and close up gaps.
 */
static
void
compact_attached_buffers(void)
{
	unsigned num, i, j;
	struct buf *b;
	int result;

	num = bufarray_num(&attached_buffers);
	for (i=j=attached_buffers_first; i<num; i++) {
		b = bufarray_get(&attached_buffers, i);
		if (b != NULL) {
			KASSERT(b->b_tableindex == i);
			if (j < i) {
				b->b_tableindex = j;
				bufarray_set(&attached_buffers, j++, b);
			}
			else {
				j++;
			}
		}
	}
	KASSERT(j <= num);
	result = bufarray_setsize(&attached_buffers, j);
	/* shrinking, shouldn't fail */
	KASSERT(result == 0);
	attached_buffers_first = j;
	KASSERT(num_attached_buffers == j);
}

////////////////////////////////////////////////////////////
// ops on buffers

/*
 * Create a fresh buffer.
 */
static
struct buf *
buffer_create(void)
{
	struct buf *b;
	int result;

	result = preallocate_buffer_arrays(num_total_buffers+1);
	if (result) {
		return NULL;
	}

	b = kmalloc(sizeof(*b));
	if (b == NULL) {
		return NULL;
	}

	b->b_data = kmalloc(ONE_TRUE_BUFFER_SIZE);
	if (b->b_data == NULL) {
		kfree(b);
		return NULL;
	}
	b->b_tableindex = INVALID_INDEX;
	b->b_bucketindex = INVALID_INDEX;
	b->b_attached = 0;
	b->b_busy = 0;
	b->b_valid = 0;
	b->b_dirty = 0;
	b->b_fs = NULL;
	b->b_physblock = 0;
	b->b_size = ONE_TRUE_BUFFER_SIZE;
	num_total_buffers++;
	return b;
}

/*
 * Attach a buffer to a given key (fs and block number)
 */
static
int
buffer_attach(struct buf *b, struct fs *fs, daddr_t block)
{
	int result;

	KASSERT(b->b_attached == 0);
	KASSERT(b->b_valid == 0);
	b->b_attached = 1;
	b->b_fs = fs;
	b->b_physblock = block;
	result = bufhash_add(&buffer_hash, b);
	if (result) {
		b->b_attached = 0;
		b->b_fs = NULL;
		b->b_physblock = 0;
		return result;
	}
	return 0;
}

/*
 * Detach a buffer from a particular key.
 */
static
void
buffer_detach(struct buf *b)
{
	KASSERT(b->b_attached == 1);
	bufhash_remove(&buffer_hash, b);
	b->b_attached = 0;
	b->b_fs = NULL;
	b->b_physblock = 0;
}

/*
 * Mark a buffer busy, waiting if necessary. 
 */
static
void
buffer_mark_busy(struct buf *b)
{
	KASSERT(b->b_holder != curthread);
	while (b->b_busy) {
		cv_wait(buffer_busy_cv, buffer_lock);
	}
	b->b_busy = 1;
	b->b_holder = curthread;
}

/*
 * Unmark a buffer busy, awakening waiters.
 */
static
void
buffer_unmark_busy(struct buf *b)
{
	KASSERT(b->b_busy != 0);
	b->b_busy = 0;
	b->b_holder = NULL;
	cv_broadcast(buffer_busy_cv, buffer_lock);
}

/*
 * I/O: disk to buffer
 */
static
int
buffer_readin(struct buf *b)
{
	int result;

	KASSERT(lock_do_i_hold(buffer_lock));
	KASSERT(b->b_attached);
	KASSERT(b->b_busy);
	KASSERT(b->b_fs != NULL);

	if (b->b_valid) {
		return 0;
	}

	lock_release(buffer_lock);
	result = FSOP_READBLOCK(b->b_fs, b->b_physblock, b->b_data, b->b_size);
	lock_acquire(buffer_lock);
	if (result == 0) {
		b->b_valid = 1;
	}
	return result;
}

void
set_doom(int newval) {
	doom_counter = newval;
}
/*
 * I/O: buffer to disk
 *
 * Note: releases lock to do I/O; busy bit should be set to protect
 */
int
buffer_writeout(struct buf *b)
{
	int result;

	KASSERT(lock_do_i_hold(buffer_lock));
	bufcheck();

	KASSERT(b->b_attached);
	KASSERT(b->b_valid);
	KASSERT(b->b_busy);
	KASSERT(b->b_fs != NULL);

	if (!b->b_dirty) {
		return 0;
	}

	if (doom_counter > 0 && --doom_counter == 0) {
		panic("DOOOOOOOOOOOOOOOOOM!!!!\n");
	}
	lock_release(buffer_lock);
	result = FSOP_WRITEBLOCK(b->b_fs, b->b_physblock, b->b_data,b->b_size);
	lock_acquire(buffer_lock);
	if (result == 0) {
		num_dirty_buffers--;
		b->b_dirty = 0;
	}
	return result;
}

/*
 * Fetch buffer pointer (external op)
 *
 * no lock necessary because of busy bit
 */
void *
buffer_map(struct buf *b)
{
	KASSERT(b->b_busy);
	return b->b_data;
}

/*
 * Mark buffer dirty (external op, for after messing with buffer pointer)
 */
void
buffer_mark_dirty(struct buf *b)
{
	unsigned enough_buffers;

	KASSERT(b->b_busy);
	KASSERT(b->b_valid);

	if (b->b_dirty) {
		/* nothing to do */
		return;
	}

	b->b_dirty = 1;

	lock_acquire(buffer_lock);
	num_dirty_buffers++;

	/* Kick the syncer if enough buffers are dirty */
	enough_buffers =
		(num_total_buffers * SYNCER_DIRTY_NUM) / SYNCER_DIRTY_DENOM;

	if (num_dirty_buffers > enough_buffers) {
		cv_signal(syncer_cv, buffer_lock);
	}
	lock_release(buffer_lock);
}

/*
 * Mark buffer valid (external op, for after messing with buffer pointer)
 */
void
buffer_mark_valid(struct buf *b)
{
	KASSERT(b->b_busy);
	b->b_valid = 1;
}

////////////////////////////////////////////////////////////
// buffer array management

/*
 * Get a buffer from the pool of detached buffers.
 */
static
struct buf *
buffer_get_detached(void)
{
	struct buf *b;
	unsigned num;
	int result;

	num = bufarray_num(&detached_buffers);
	KASSERT(num == num_detached_buffers);
	if (num > 0) {
		b = bufarray_get(&detached_buffers, num-1);
		KASSERT(b->b_tableindex == num-1);
		b->b_tableindex = INVALID_INDEX;

		/* shrink array (should not fail) */
		result = bufarray_setsize(&detached_buffers, num-1);
		KASSERT(result == 0);

		num_detached_buffers--;
		return b;
	}

	return NULL;
}

/*
 * Put a buffer into the pool of detached buffers.
 */
static
void
buffer_put_detached(struct buf *b)
{
	int result;

	KASSERT(b->b_attached == 0);
	KASSERT(b->b_busy == 0);
	KASSERT(b->b_tableindex == INVALID_INDEX);

	result = bufarray_add(&detached_buffers, b, &b->b_tableindex);
	/* arrays are preallocated to avoid failure here */
	KASSERT(result == 0);

	num_detached_buffers++;
}

/*
 * Remove a buffer from the attached (LRU) list.
 */
static
void
buffer_get_attached(struct buf *b, unsigned expected_busy)
{
	unsigned ix;

	KASSERT(b->b_attached == 1);
	KASSERT(b->b_busy == expected_busy);

	ix = b->b_tableindex;

	KASSERT(bufarray_get(&attached_buffers, ix) == b);

	/* Remove from table, leave NULL behind (compact lazily, later) */
	bufarray_set(&attached_buffers, ix, NULL);
	b->b_tableindex = INVALID_INDEX;

	/* cache the first empty slot  */
	if (ix < attached_buffers_first) {
		attached_buffers_first = ix;
	}

	num_attached_buffers--;
}

/*
 * Put a buffer into the attached (LRU) list, always at the end.
 */
static
void
buffer_put_attached(struct buf *b)
{
	unsigned num;
	int result;

	KASSERT(b->b_attached == 1);
	KASSERT(b->b_busy == 0);
	KASSERT(b->b_tableindex == INVALID_INDEX);

	num = bufarray_num(&attached_buffers);
	if (num >= attached_buffers_thresh) {
		compact_attached_buffers();
	}

	result = bufarray_add(&attached_buffers, b, &b->b_tableindex);
	/* arrays are preallocated to avoid failure here */
	KASSERT(result == 0);
	num_attached_buffers++;
}

/*
 * Get a buffer out of the busy list.
 */
static
void
buffer_get_busy(struct buf *b)
{
	unsigned ix;

	KASSERT(b->b_attached == 1);
	KASSERT(b->b_busy == 1);

	ix = b->b_tableindex;

	KASSERT(bufarray_get(&busy_buffers, ix) == b);
	bufarray_remove_unordered(&busy_buffers, ix, buf_fixup_tableindex);
	b->b_tableindex = INVALID_INDEX;
	num_busy_buffers--;
}

/*
 * Put a buffer into the busy list.
 */
static
void
buffer_put_busy(struct buf *b)
{
	int result;

	KASSERT(b->b_attached == 1);
	KASSERT(b->b_busy == 1);
	KASSERT(b->b_tableindex == INVALID_INDEX);

	result = bufarray_add(&busy_buffers, b, &b->b_tableindex);
	/* arrays are preallocated to avoid failure here */
	KASSERT(result == 0);
	num_busy_buffers++;
}

////////////////////////////////////////////////////////////
// buffer get/release

/*
 * Write a buffer (found on attached_buffers[]) out.
 */
static
int
buffer_sync(struct buf *b)
{
	int result;

	KASSERT(b->b_dirty == 1);

	/*
	 * Mark it busy while we do I/O, but do *not* move it to the
	 * busy list; this preserves its LRU ordering.
	 */
	buffer_mark_busy(b);
	curthread->t_busy_buffers++;

	result = buffer_writeout(b);

	buffer_unmark_busy(b);
	curthread->t_busy_buffers--;

	return result;
}

/*
 * Evict a buffer.
 */
static
int
buffer_evict(struct buf **ret)
{
	unsigned num, i;
	struct buf *b, *db;
	int result;

	/*
	 * Find a target buffer.
	 */

	num = bufarray_num(&attached_buffers);
	b = db = NULL;
	for (i=0; i<num; i++) {
		if (i >= num/2 && db != NULL) {
			/*
			 * voodoo: avoid preferring very recent clean
			 * buffers to older dirty buffers.
			 */
			break;
		}
		b = bufarray_get(&attached_buffers, i);
		if (b == NULL) {
			continue;
		}
		if (b->b_busy == 1) {
			b = NULL;
			continue;
		}
		if (b->b_dirty == 1) {
			if (db == NULL) {
				/* remember first dirty buffer we saw */
				db = b;
			}
			b = NULL;
			continue;
		}
		break;
	}
	if (b == NULL && db != NULL) {
		b = db;
	}
	if (b == NULL) {
		/* No buffers at all...? */
		kprintf("buffer_evict: no targets!?\n");
		return EAGAIN;
	}

	/*
	 * Flush the buffer out if necessary.
	 */
	num_total_evictions++;
	if (b->b_dirty) {
		num_dirty_evictions++;
		KASSERT(b->b_busy == 0);
		/* lock may be released here */
		result = buffer_sync(b);
		if (result) {
			/* urgh... */
			kprintf("buffer_evict: warning: %s\n",
				strerror(result));
			/* should we try another buffer? */
			return result;
		}
	}

	KASSERT(b->b_dirty == 0);

	/*
	 * Detach it from its old key, and return it in a state where
	 * it can be reattached properly.
	 */
	buffer_get_attached(b, 0);
	b->b_valid = 0;
	buffer_detach(b);

	*ret = b;
	return 0;
}

static
struct buf *
buffer_find(struct fs *fs, daddr_t physblock)
{
	return bufhash_get(&buffer_hash, fs, physblock);
}

/*
 * Find a buffer for the given block, if one already exists; otherwise
 * attach one but don't bother to read it in.
 */
static
int
buffer_get_internal(struct fs *fs, daddr_t block, size_t size, struct buf **ret)
{
	struct buf *b;
	int result;

	KASSERT(lock_do_i_hold(buffer_lock));
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);

	if (curthread->t_busy_buffers >= curthread->t_reserved_buffers) {
		panic("buffer_get: too many buffers at once\n");
	}

	num_total_gets++;

	b = buffer_find(fs, block);
	if (b != NULL) {
		num_valid_gets++;
		buffer_mark_busy(b);
		buffer_get_attached(b, 1);
	}
	else {
		b = buffer_get_detached();
		if (b == NULL && num_total_buffers < max_total_buffers) {
			/* Can create a new buffer... */
			b = buffer_create();
		}
		if (b == NULL) {
			result = buffer_evict(&b);
			if (result) {
				return result;
			}
			KASSERT(b != NULL);
		}

		KASSERT(b->b_size == ONE_TRUE_BUFFER_SIZE);
		result = buffer_attach(b, fs, block);
		if (result) {
			buffer_put_detached(b);
			return result;
		}
		buffer_mark_busy(b);
	}

	curthread->t_busy_buffers++;
	KASSERT(curthread->t_busy_buffers <= curthread->t_reserved_buffers);

	buffer_put_busy(b);

	*ret = b;
	return 0;
}

/*
 * Find a buffer for the given block, if one already exists; otherwise
 * attach one but don't bother to read it in.
 */
int
buffer_get(struct fs *fs, daddr_t block, size_t size, struct buf **ret)
{
	int result;
	lock_acquire(buffer_lock);
	result = buffer_get_internal(fs, block, size, ret);
	lock_release(buffer_lock);

	return result;
}

/*
 * Same as buffer_get but does a read so the resulting buffer always
 * contains valid data.
 */
int
buffer_read(struct fs *fs, daddr_t block, size_t size, struct buf **ret)
{
	int result;

	lock_acquire(buffer_lock);
	bufcheck();

	result = buffer_get_internal(fs, block, size, ret);
	if (result) {
		lock_release(buffer_lock);
		*ret = NULL;
		return result;
	}

	if (!(*ret)->b_valid) {
		/* may lose (and then re-acquire) lock here */
		result = buffer_readin(*ret);
		if (result) {
			buffer_release(*ret);
			lock_release(buffer_lock);
			*ret = NULL;
			return result;
		}
	}

	lock_release(buffer_lock);
	return 0;
}

/*
 * Shortcut combination of buffer_get and buffer_release_and_invalidate
 * that invalidates any existing buffer and otherwise does nothing.
 */
void
buffer_drop(struct fs *fs, daddr_t block, size_t size)
{
	struct buf *b;

	lock_acquire(buffer_lock);
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);

	b = buffer_find(fs, block);
	if (b != NULL) {
		/* dropping a buffer someone else is using is a big mistake */
		KASSERT(b->b_busy == 0);

		buffer_get_attached(b, 0);
		b->b_valid = 0;
		if (b->b_dirty) {
			b->b_dirty = 0;
			num_dirty_buffers--;
		}
		buffer_detach(b);
		buffer_put_detached(b);
	}
	lock_release(buffer_lock);
}

static
void
buffer_release_internal(struct buf *b)
{
	KASSERT(lock_do_i_hold(buffer_lock));
	bufcheck();

	buffer_get_busy(b);
	buffer_unmark_busy(b);
	curthread->t_busy_buffers--;

	if (!b->b_valid) {
		/* detach it */
		if (b->b_dirty) {
			b->b_dirty = 0;
			num_dirty_buffers--;
		}
		buffer_detach(b);
		buffer_put_detached(b);
	}
	else {
		buffer_put_attached(b);
	}
}

/*
 * Let go of a buffer obtained with buffer_get or buffer_read.
 */
void
buffer_release(struct buf *b)
{
	lock_acquire(buffer_lock);
	buffer_release_internal(b);
	lock_release(buffer_lock);
}

/*
 * Same as buffer_release, but also invalidates the buffer.
 */
void
buffer_release_and_invalidate(struct buf *b)
{
	lock_acquire(buffer_lock);
	bufcheck();

	b->b_valid = 0;
	buffer_release_internal(b);
	lock_release(buffer_lock);
}

////////////////////////////////////////////////////////////
// explicit sync

int
sync_fs_buffers(struct fs *fs)
{
	unsigned i, j;
	struct buf *b;
	int result;

	lock_acquire(buffer_lock);
	bufcheck();

	/* Don't cache the array size; it might change as we work. */
	for (i=0; i<bufarray_num(&attached_buffers); i++) {
		b = bufarray_get(&attached_buffers, i);
		if (b == NULL || b->b_fs != fs) {
			continue;
		}

		if (b->b_dirty) {
			/* lock may be released (and then re-acquired) here */
			result = buffer_sync(b);
			if (result) {
				lock_release(buffer_lock);
				return result;
			}
			j = b->b_tableindex;
			if (i != j) {
				/* compact_attached_buffers ran */
				KASSERT(j<i);
				i = j;
			}
		}
	}

	lock_release(buffer_lock);
	return 0;
}

////////////////////////////////////////////////////////////
// syncer

static
void
sync_some_buffers(void)
{
	unsigned i, targetcount, limit;
	struct buf *b;
	int result;

	KASSERT(lock_do_i_hold(buffer_lock));
	bufcheck();

	targetcount =
		(num_total_buffers * SYNCER_TARGET_NUM) / SYNCER_TARGET_DENOM;
	limit = (num_dirty_buffers * SYNCER_LIMIT_NUM) / SYNCER_LIMIT_DENOM;

	if (targetcount > limit) {
		targetcount = limit;
	}

	/* Don't cache the array size; it might change as we work. */
	for (i=0; i<bufarray_num(&attached_buffers) && targetcount > 0; i++) {
		b = bufarray_get(&attached_buffers, i);
		if (b == NULL || b->b_busy) {
			continue;
		}
		if (b->b_dirty) {
			/* lock may be released (and then re-acquired) here */
			result = buffer_sync(b);
			if (result) {
				kprintf("syncer: warning: %s\n",
					strerror(result));
			}
			targetcount--;
		}
	}
}

static
void
syncer_thread(void *x1, unsigned long x2)
{
	(void)x1;
	(void)x2;

	lock_acquire(buffer_lock);
	while (1) {
		cv_wait(syncer_cv, buffer_lock);
		sync_some_buffers();
	}
	lock_release(buffer_lock);
}

////////////////////////////////////////////////////////////
// reservation

/*
 * Reserve COUNT buffers.
 *
 * This does not allocate buffers or mark buffers busy; it registers
 * the intent, and thereby claims the right, to do so.
 */
void
reserve_buffers(unsigned count, size_t size)
{
	lock_acquire(buffer_lock);
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);

	/* All buffer reservations must be done up front, all at once. */
	KASSERT(curthread->t_reserved_buffers == 0);

	while (num_reserved_buffers + count > max_total_buffers) {
		cv_wait(buffer_reserve_cv, buffer_lock);
	}
	num_reserved_buffers += count;
	curthread->t_reserved_buffers = count;
	lock_release(buffer_lock);
}

/*
 * Release reservation of COUNT buffers.
 */
void
unreserve_buffers(unsigned count, size_t size)
{
	lock_acquire(buffer_lock);
	bufcheck();

	KASSERT(size == ONE_TRUE_BUFFER_SIZE);

	KASSERT(count <= curthread->t_reserved_buffers);
	KASSERT(count <= num_reserved_buffers);

	curthread->t_reserved_buffers -= count;
	num_reserved_buffers -= count;
	cv_broadcast(buffer_reserve_cv, buffer_lock);

	KASSERT(curthread->t_busy_buffers <= curthread->t_reserved_buffers);
	lock_release(buffer_lock);
}

////////////////////////////////////////////////////////////
// bootstrap

void
buffer_bootstrap(void)
{
	size_t max_buffer_mem;
	int result;

	num_detached_buffers = 0;
	num_attached_buffers = 0;
	num_busy_buffers = 0;
	num_dirty_buffers = 0;
	num_reserved_buffers = 0;
	num_total_buffers = 0;

	/* Limit total memory usage for buffers */
	max_buffer_mem =
		(mainbus_ramsize() * BUFFER_MAXMEM_NUM) / BUFFER_MAXMEM_DENOM;
	max_total_buffers = max_buffer_mem / ONE_TRUE_BUFFER_SIZE;

	kprintf("buffers: max count %lu; max size %luk\n",
		(unsigned long) max_total_buffers,
		(unsigned long) max_buffer_mem/1024);

	num_total_gets = 0;
	num_valid_gets = 0;
	num_total_evictions = 0;
	num_dirty_evictions = 0;

	bufarray_init(&detached_buffers);
	bufarray_init(&attached_buffers);
	bufarray_init(&busy_buffers);
	attached_buffers_first = 0;
	attached_buffers_thresh = 0;

	result = bufhash_init(&buffer_hash, max_total_buffers/16);
	if (result) {
		panic("Creating buffer_hash failed\n");
	}

	buffer_lock = lock_create("buffer cache lock");
	if (buffer_lock == NULL) {
		panic("Creating buffer cache lock failed\n");
	}

	buffer_busy_cv = cv_create("bufbusy");
	if (buffer_busy_cv == NULL) {
		panic("Creating buffer_busy_cv failed\n");
	}

	buffer_reserve_cv = cv_create("bufreserve");
	if (buffer_reserve_cv == NULL) {
		panic("Creating buffer_reserve_cv failed\n");
	}

	syncer_cv = cv_create("syncer");
	if (buffer_reserve_cv == NULL) {
		panic("Creating syncer_cv failed\n");
	}

	result = thread_fork("syncer", syncer_thread, NULL, 0, NULL);
	if (result) {
		panic("Starting syncer failed\n");
	}
}
