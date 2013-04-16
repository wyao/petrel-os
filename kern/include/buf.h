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

#ifndef _BUF_H_
#define _BUF_H_

struct fs;  /* fs.h */


/*
 * The buffer cache is physically indexed; that is, the index or "key"
 * is a filesystem and block number. (The alternative would be
 * virtually indexed, where the key is a vnode and block offset within
 * the vnode.)
 *
 * In the real world you'd want buffers to be able to be different
 * sizes (since not every FS in existence uses the same block size,
 * and some can use different block sizes as a formatting option) but
 * to reduce complexity here we'll require all buffers to be the
 * ONE_TRUE_BUFFER_SIZE, which is defined in buf.c.
 *
 * In any event each FS should use buffers of only one size, or at
 * least of a consistent size for any particular disk offset, because
 * handling partial or overlapping buffers would be extremely
 * problematic.
 */

struct buf; /* Opaque. */

/*
 * Get-a-buffer operations.
 *
 * buffer_get looks for an existing buffer for the given block, and
 * returns it if found; if not, it creates a new buffer and doesn't
 * put any data in it.
 *
 * buffer_read is the same but reads from disk if necessary, so that
 * the contents are always valid on return.
 *
 * Both mark the buffer busy before returning it.
 *
 * buffer_drop looks for an existing buffer and invalidates it
 * immediately without returning it.
 */

int buffer_get(struct fs *fs, daddr_t block, size_t size, struct buf **ret);
int buffer_read(struct fs *fs, daddr_t block, size_t size, struct buf **ret);
void buffer_drop(struct fs *fs, daddr_t block, size_t size);

/*
 * Release-a-buffer operations.
 *
 * buffer_release lets go of the buffer and marks it no longer busy.
 * The cache will make sure it gets written out at some point if it's
 * marked dirty.
 *
 * buffer_release_and_invalidate does the same but marks the contents
 * no longer valid.
 */
void buffer_release(struct buf *buf);
void buffer_release_and_invalidate(struct buf *buf);

/*
 * Other operations on buffers.
 *
 * buffer_map returns the data pointer, which is valid until the
 * (reference to) the buffer is released.
 *
 * buffer_mark_dirty marks the buffer dirty.
 * buffer_mark_valid marks the buffer valid (i.e., contains real data).
 *
 * buffer_writeout flushes the buffer to disk if it's currently dirty,
 * and marks it clean.
 *
 * The buffer must already be marked busy.
 */
void *buffer_map(struct buf *buf);
void buffer_mark_dirty(struct buf *buf);
void buffer_mark_valid(struct buf *buf);
int buffer_writeout(struct buf *buf);

/*
 * Sync.
 */
int sync_fs_buffers(struct fs *fs);

/*
 * Starvation/deadlock avoidance logic.
 *
 * Upon entry to any FS operation that will use buffers, the process
 * involved should reserve the maximum number of buffers it will need;
 * upon completion it should release this reservation. reserve_buffers
 * may wait until enough buffers are available.
 *
 * This prevents pathological cases where e.g. every buffer is in use
 * by a process that's halfway through a truncate and waiting for
 * another buffer to become available.
 */
void reserve_buffers(unsigned count, size_t size);
void unreserve_buffers(unsigned count, size_t size);

/*
 * Bootup.
 */
void buffer_bootstrap(void);

/* Set the SFS doom (DOOOOOM!!!) counter! */
void set_doom(int);

#endif /* _BUF_H_ */
