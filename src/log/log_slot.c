/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_log_slot_activate --
 *	Initialize a slot to become active.
 */
void
__wt_log_slot_activate(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;

	slot->slot_state = 0;
	slot->slot_start_lsn = slot->slot_end_lsn = log->alloc_lsn;
	slot->slot_start_offset = log->alloc_lsn.offset;
	slot->slot_last_offset = log->alloc_lsn.offset;
	slot->slot_fh = log->log_fh;
	slot->slot_error = 0;
	slot->slot_unbuffered = 0;
	return;
}

/*
 * __wt_log_slot_close --
 *	Close out the slot the caller is using.  The slot may already be
 *	closed or freed by another thread.
 */
int
__wt_log_slot_close(WT_SESSION_IMPL *session, WT_LOGSLOT *slot, int *releasep)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	int64_t end_offset, new_state, old_state;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_SLOT));
	conn = S2C(session);
	log = conn->log;
	if (releasep != NULL)
		*releasep = 0;
	if (slot == NULL)
		return (0);
retry:
	old_state = slot->slot_state;
	/*
	 * If someone else is switching out this slot we lost.  Nothing to
	 * do but return.
	 */
	if (WT_LOG_SLOT_CLOSED(old_state))
		return (0);
	/*
	 * If someone completely processed this slot, we're done.
	 */
	if (FLD64_ISSET((uint64_t)slot->slot_state, WT_LOG_SLOT_RESERVED))
		return (0);
	new_state = (old_state | WT_LOG_SLOT_CLOSE);
	/*
	 * Close this slot.  If we lose the race retry.
	 */
	if (!__wt_atomic_casiv64(&slot->slot_state, old_state, new_state))
		goto retry;
	/*
	 * We own the slot now.  No one else can join.
	 * Set the end LSN.
	 */
	WT_STAT_FAST_CONN_INCR(session, log_slot_closes);
	if (WT_LOG_SLOT_DONE(new_state) && releasep != NULL)
		*releasep = 1;
	slot->slot_end_lsn = slot->slot_start_lsn;
	end_offset = WT_LOG_SLOT_JOINED(old_state);
	slot->slot_end_lsn.offset += (wt_off_t)end_offset;
	WT_STAT_FAST_CONN_INCRV(session,
	    log_slot_consolidated, end_offset);
	/*
	 * XXX Would like to change so one piece of code advances the LSN.
	 */
	log->alloc_lsn = slot->slot_end_lsn;
	WT_ASSERT(session, log->alloc_lsn.file >= log->write_lsn.file);
	return (0);
}

/*
 * __wt_log_slot_switch --
 *	Switch out the current slot and set up a new one.
 */
int
__wt_log_slot_switch(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
	WT_LOG *log;
	int dummy;
#ifdef HAVE_DIAGNOSTIC
	int64_t state;
	int32_t j, r;
#endif

	log = S2C(session)->log;
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_SLOT));
	/*
	 * If someone else raced us to closing this specific slot, we're
	 * done here.
	 */
	if (slot != log->active_slot)
		return (0);
	WT_RET(__wt_log_slot_close(session, slot, &dummy));
	/*
	 * Only mainline callers use switch.  Our size should be in join
	 * and we have not yet released, so we should never think release
	 * should be done now.
	 */
	WT_ASSERT(session, dummy == 0);
#ifdef HAVE_DIAGNOSTIC
	state = slot->slot_state;
	j = WT_LOG_SLOT_JOINED(state);
	r = WT_LOG_SLOT_RELEASED(state);
	WT_ASSERT(session, j > r);
#endif
	WT_RET(__wt_log_slot_new(session));
	return (0);
}

/*
 * __wt_log_slot_new --
 *	Find a free slot and switch it as the new active slot.
 *	Must be called holding the slot lock.
 */
int
__wt_log_slot_new(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	WT_LOGSLOT *slot;
	int32_t i;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_SLOT));
	conn = S2C(session);
	log = conn->log;
	if (!F_ISSET(log,  WT_LOG_FORCE_CONSOLIDATE))
		return (0);
	/*
	 * Although this function is single threaded, multiple threads could
	 * be trying to set a new active slot sequentially.  If we find an
	 * active slot that is valid, return.
	 */
	if ((slot = log->active_slot) != NULL &&
	    WT_LOG_SLOT_OPEN(slot->slot_state))
		return (0);

	/*
	 * Keep trying until we can find a free slot.
	 */
	for (;;) {
		/*
		 * For now just restart at 0.  We could use log->pool_index
		 * if that is inefficient.
		 */
		for (i = 0; i < WT_SLOT_POOL; i++) {
			slot = &log->slot_pool[i];
			if (slot->slot_state == WT_LOG_SLOT_FREE) {
				/*
				 * Make sure that the next buffer size can
				 * fit in the file.  Proactively switch if
				 * it cannot.  This reduces, but does not
				 * eliminate, log files that exceed the
				 * maximum file size.
				 *
				 * We want to minimize the risk of an
				 * error due to no space.
				 */
				WT_RET(__wt_log_acquire(session,
				    log->slot_buf_size, slot));
				/*
				 * We have a new, free slot to use.
				 * Set it as the active slot.
				 */
				WT_STAT_FAST_CONN_INCR(session,
				    log_slot_transitions);
				log->active_slot = slot;
				return (0);
			}
		}
		/*
		 * If we didn't find any free slots signal the worker thread.
		 */
		(void)__wt_cond_signal(session, conn->log_wrlsn_cond);
		__wt_yield();
	}
	/* NOTREACHED */
}

/*
 * __wt_log_slot_init --
 *	Initialize the slot array.
 */
int
__wt_log_slot_init(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOGSLOT *slot;
	int32_t i;

	conn = S2C(session);
	log = conn->log;
	WT_CACHE_LINE_ALIGNMENT_VERIFY(session, log->slot_pool);
	for (i = 0; i < WT_SLOT_POOL; i++)
		log->slot_pool[i].slot_state = WT_LOG_SLOT_FREE;

	/*
	 * Allocate memory for buffers now that the arrays are setup. Split
	 * this out to make error handling simpler.
	 */
	/*
	 * Cap the slot buffer to the log file size times two if needed.
	 * That means we try to fill to half the buffer but allow some
	 * extra space.
	 *
	 * !!! If the buffer size is too close to the log file size, we will
	 * switch log files very aggressively.  Scale back the buffer for
	 * small log file sizes.
	 */
	log->slot_buf_size = (uint32_t)WT_MIN(
	    (size_t)conn->log_file_max/10, WT_LOG_SLOT_BUF_SIZE);
	for (i = 0; i < WT_SLOT_POOL; i++) {
		WT_ERR(__wt_buf_init(session,
		    &log->slot_pool[i].slot_buf, log->slot_buf_size));
		F_SET(&log->slot_pool[i], WT_SLOT_INIT_FLAGS);
	}
	WT_STAT_FAST_CONN_INCRV(session,
	    log_buffer_size, log->slot_buf_size * WT_SLOT_POOL);
	F_SET(log, WT_LOG_FORCE_CONSOLIDATE);
	/*
	 * Set up the available slot from the pool the first time.
	 */
	slot = &log->slot_pool[0];
	/*
	 * We cannot initialize the release LSN in the activate function
	 * because that is called after a log file switch.
	 */
	slot->slot_release_lsn = log->alloc_lsn;
	__wt_log_slot_activate(session, slot);
	log->active_slot = slot;

	if (0) {
err:		while (--i >= 0)
			__wt_buf_free(session, &log->slot_pool[i].slot_buf);
	}
	return (ret);
}

/*
 * __wt_log_slot_destroy --
 *	Clean up the slot array on shutdown.
 */
int
__wt_log_slot_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	WT_LOGSLOT *slot;
	size_t write_size;
	int64_t rel;
	int i;

	conn = S2C(session);
	log = conn->log;

	/*
	 * Write out any remaining buffers.  Free the buffer.
	 */
	for (i = 0; i < WT_SLOT_POOL; i++) {
		slot = &log->slot_pool[i];
		if (!FLD64_ISSET(
		    (uint64_t)slot->slot_state, WT_LOG_SLOT_RESERVED)) {
			rel = WT_LOG_SLOT_RELEASED(slot->slot_state);
			write_size = (size_t)(rel - slot->slot_unbuffered);
			if (write_size != 0)
				WT_RET(__wt_write(session, slot->slot_fh,
				    slot->slot_start_offset, write_size,
				    slot->slot_buf.mem));
		}
		__wt_buf_free(session, &log->slot_pool[i].slot_buf);
	}
	return (0);
}

/*
 * __wt_log_slot_join --
 *	Join a consolidated logging slot.  Must be called with
 *	the read lock held.
 */
int
__wt_log_slot_join(WT_SESSION_IMPL *session, uint64_t mysize,
    uint32_t flags, WT_MYSLOT *myslotp)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	WT_LOGSLOT *slot;
	int64_t flag_state, new_state, old_state;
	int32_t join_offset, new_join, released;

	conn = S2C(session);
	log = conn->log;

	/*
	 * Make sure the length cannot overflow.  The caller should not
	 * even call this function if it doesn't fit but use direct
	 * writes.
	 */
	WT_ASSERT(session, mysize < WT_LOG_SLOT_MAXIMUM);
	WT_ASSERT(session, !F_ISSET(session, WT_SESSION_LOCKED_SLOT));

	/*
	 * The worker thread is constantly trying to join and write out
	 * the current buffered slot, even when direct writes are in
	 * use.  If we're doing direct writes, there may not be a slot active.
	 * Verify we're from the worker thread (passed in a size of 0).
	 * There is nothing to do so just return.
	 */
	if (log->active_slot == NULL) {
		WT_ASSERT(session, mysize == 0);
		return (0);
	}
	/*
	 * There should almost always be a slot open.
	 */
	for (;;) {
		WT_BARRIER();
		slot = log->active_slot;
		old_state = slot->slot_state;
		/*
		 * Try to join our size into the existing size and
		 * atomically write it back into the state.
		 */
		flag_state = WT_LOG_SLOT_FLAGS(old_state);
		released = WT_LOG_SLOT_RELEASED(old_state);
		join_offset = WT_LOG_SLOT_JOINED(old_state);
		new_join = join_offset + (int32_t)mysize;
		new_state = (int64_t)WT_LOG_SLOT_JOIN_REL(
		    (int64_t)new_join, (int64_t)released, (int64_t)flag_state);

		/*
		 * Check if the slot is open for joining and we are able to
		 * swap in our size into the state.
		 */
		if (WT_LOG_SLOT_OPEN(old_state) &&
		    __wt_atomic_casiv64(
		    &slot->slot_state, old_state, new_state))
			break;
		else {
			/*
			 * The slot is no longer open or we lost the race to
			 * update it.  Yield and try again.
			 */
			WT_STAT_FAST_CONN_INCR(session, log_slot_races);
			__wt_yield();
		}
	}
	/*
	 * We joined this slot.  Fill in our information to return to
	 * the caller.
	 */
	if (mysize != 0)
		WT_STAT_FAST_CONN_INCR(session, log_slot_joins);
	if (LF_ISSET(WT_LOG_DSYNC | WT_LOG_FSYNC))
		F_SET(slot, WT_SLOT_SYNC_DIR);
	if (LF_ISSET(WT_LOG_FSYNC))
		F_SET(slot, WT_SLOT_SYNC);
	myslotp->slot = slot;
	myslotp->offset = join_offset;
	myslotp->end_offset = (wt_off_t)((uint64_t)join_offset + mysize);
	return (0);
}

/*
 * __wt_log_slot_release --
 *	Each thread in a consolidated group releases its portion to
 *	signal it has completed copying its piece of the log into
 *	the memory buffer.
 */
int64_t
__wt_log_slot_release(WT_MYSLOT *myslot, int64_t size)
{
	WT_LOGSLOT *slot;
	wt_off_t cur_offset, my_start;
	int64_t my_size;

	slot = myslot->slot;
	my_start = slot->slot_start_offset + myslot->offset;
	while ((cur_offset = slot->slot_last_offset) < my_start) {
		/*
		 * Set our offset if we are larger.
		 */
		if (__wt_atomic_casiv64(
		    &slot->slot_last_offset, cur_offset, my_start))
			break;
		/*
		 * If we raced another thread updating this, try again.
		 */
		WT_BARRIER();
	}
	/*
	 * Add my size into the state and return the new size.
	 */
	my_size = (int64_t)WT_LOG_SLOT_JOIN_REL((int64_t)0, size, 0);
	return (__wt_atomic_addiv64(&slot->slot_state, my_size));
}

/*
 * __wt_log_slot_free --
 *	Free a slot back into the pool.
 */
int
__wt_log_slot_free(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{

	/*
	 * Make sure flags don't get retained between uses.
	 * We have to reset them them here because multiple threads may
	 * change the flags when joining the slot.
	 */
	WT_UNUSED(session);
	slot->flags = WT_SLOT_INIT_FLAGS;
	slot->slot_error = 0;
	slot->slot_state = WT_LOG_SLOT_FREE;
	return (0);
}
