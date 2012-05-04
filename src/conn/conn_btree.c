/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_conn_btree_open_lock --
 *	Spin on the current btree handle until either (a) it is open, read
 *	locked; or (b) it is closed, write locked.
 */
static int
__conn_btree_open_lock(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_BTREE *btree;

	btree = session->btree;

	/*
	 * Check that the handle is open.  We've already incremented
	 * the reference count, so once the handle is open it won't be
	 * closed by another thread.
	 *
	 * If we can see the WT_BTREE_OPEN flag set while holding a
	 * lock on the handle, then it's really open and we can start
	 * using it.  Alternatively, if we can get an exclusive lock
	 * and WT_BTREE_OPEN is still not set, we need to do the open.
	 */
	for (;;) {
		if (F_ISSET(btree, WT_BTREE_OPEN) &&
		    !LF_ISSET(WT_BTREE_EXCLUSIVE)) {
			__wt_readlock(session, btree->rwlock);
			if (F_ISSET(btree, WT_BTREE_OPEN))
				break;
			__wt_rwunlock(session, btree->rwlock);
		}

		/*
		 * It isn't open or we want it exclusive: try to get an
		 * exclusive lock.  There is some subtlety here: if we race
		 * with another thread that successfully opens the file, we
		 * don't want to block waiting to get exclusive access.
		 */
		if (__wt_try_writelock(session, btree->rwlock) == 0) {
			/*
			 * If it was opened while we waited, drop the write
			 * lock and get a read lock instead.
			 */
			if (F_ISSET(btree, WT_BTREE_OPEN) &&
			    !LF_ISSET(WT_BTREE_EXCLUSIVE)) {
				__wt_rwunlock(session, btree->rwlock);
				continue;
			}

			/*
			 * We've got the exclusive handle lock and it's
			 * our job to open the file.
			 */
			F_SET(btree, WT_BTREE_EXCLUSIVE);
			break;
		}

		/* Give other threads a chance to make progress. */
		__wt_yield();
	}

	return (0);
}

/*
 * __wt_conn_btree_get --
 *	Find an open btree file handle, otherwise create a new one and link it
 *	into the connection's list.  If successful, it returns with either
 *	(a) an open handle, read locked; or (b) a closed handle, write locked.
 */
static int
__conn_btree_get(WT_SESSION_IMPL *session,
    const char *name, const char *snapshot, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int matched;

	conn = S2C(session);

	/* Increment the reference count if we already have the btree open. */
	matched = 0;
	__wt_spin_lock(session, &conn->spinlock);
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (strcmp(name, btree->name) == 0 &&
		    (snapshot == btree->snapshot ||
		    (snapshot != NULL && btree->snapshot != NULL &&
		    strcmp(snapshot, btree->snapshot) == 0))) {
			++btree->refcnt;
			session->btree = btree;
			matched = 1;
			break;
		}
	}
	if (matched) {
		__wt_spin_unlock(session, &conn->spinlock);
		session->btree = btree;
		return (__conn_btree_open_lock(session, flags));
	}

	/*
	 * Allocate the WT_BTREE structure, its lock, and set the name so we
	 * can put the handle into the list.
	 *
	 * Because this loop checks for existing btree file handles, the
	 * connection layer owns:
	 *	the WT_BTREE structure itself
	 *	the structure lock
	 *	the structure name
	 *	the structure configuration string
	 *	the WT_BTREE_OPEN flag
	 */
	btree = NULL;
	if ((ret = __wt_calloc_def(session, 1, &btree)) == 0 &&
	    (ret = __wt_rwlock_alloc(
		session, "btree handle", &btree->rwlock)) == 0 &&
	    (ret = __wt_strdup(session, name, &btree->name)) == 0 &&
	    (snapshot == NULL ||
	    (ret = __wt_strdup(session, snapshot, &btree->snapshot)) == 0)) {
		/* Lock the handle before it is inserted in the list. */
		__wt_writelock(session, btree->rwlock);
		F_SET(btree, WT_BTREE_EXCLUSIVE);

		/* Add to the connection list. */
		btree->refcnt = 1;
		TAILQ_INSERT_TAIL(&conn->btqh, btree, q);
		++conn->btqcnt;
	}
	__wt_spin_unlock(session, &conn->spinlock);

	if (ret == 0)
		session->btree = btree;
	else if (btree != NULL) {
		if (btree->rwlock != NULL)
			(void)__wt_rwlock_destroy(
			    session, btree->rwlock);
		__wt_free(session, btree->name);
		__wt_free(session, btree->snapshot);
		__wt_free(session, btree);
	}

	return (ret);
}

/*
 * __conn_btree_open --
 *	Open the current btree handle.
 */
static int
__conn_btree_open(WT_SESSION_IMPL *session, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM *addr;

	addr = NULL;
	btree = session->btree;

	do {
		/* Set any special flags on the handle. */
		F_SET(btree, LF_ISSET(WT_BTREE_SPECIAL_FLAGS));

		WT_ERR(__wt_scr_alloc(session, WT_BTREE_MAX_ADDR_COOKIE, &addr));
		WT_ERR(__wt_snapshot_get(
		    session, btree->name, btree->snapshot, addr));
		WT_ERR(__wt_btree_open(session,
		    cfg, addr->data, addr->size, btree->snapshot == NULL ? 0 : 1));
		F_SET(btree, WT_BTREE_OPEN);

		/* Drop back to a readlock if that is all that was needed. */
		if (!LF_ISSET(WT_BTREE_EXCLUSIVE)) {
			F_CLR(btree, WT_BTREE_EXCLUSIVE);
			__wt_rwunlock(session, btree->rwlock);
			WT_RET(__conn_btree_open_lock(session, flags));
		}
	} while (!F_ISSET(btree, WT_BTREE_OPEN));

	if (0) {
err:		(void)__wt_conn_btree_close(session, 1);
	}

	__wt_scr_free(&addr);
	return (ret);
}

/*
 * __wt_conn_btree_open --
 *	Get an open btree file handle, otherwise open a new one.
 */
int
__wt_conn_btree_open(WT_SESSION_IMPL *session,
    const char *name, const char *snapshot, const char *config,
    const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);

	WT_STAT_INCR(conn->stats, file_open);

	if ((ret = __conn_btree_get(session, name, snapshot, flags)) != 0) {
		__wt_free(session, config);
		return (ret);
	}

	btree = session->btree;
	if (F_ISSET(btree, WT_BTREE_OPEN) || LF_ISSET(WT_BTREE_LOCK_ONLY))
		__wt_free(session, config);
	else {
		/* Open the underlying file, free any old config. */
		__wt_free(session, btree->config);
		btree->config = config;
		ret = __conn_btree_open(session, cfg, flags);
	}

	if (ret != 0 || LF_ISSET(WT_BTREE_NO_LOCK))
		__wt_rwunlock(session, btree->rwlock);

	return (ret);
}

/*
 * __wt_conn_btree_close --
 *	Discard a reference to an open btree file handle.
 */
int
__wt_conn_btree_close(WT_SESSION_IMPL *session, int locked)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int inuse;

	btree = session->btree;
	conn = S2C(session);

	if (F_ISSET(btree, WT_BTREE_OPEN))
		WT_STAT_DECR(conn->stats, file_open);

	/*
	 * Decrement the reference count.  If we really are the last reference,
	 * get an exclusive lock on the handle so that we can close it.
	 */
	__wt_spin_lock(session, &conn->spinlock);
	inuse = --btree->refcnt > 0;
	if (!inuse && !locked)
		__wt_writelock(session, btree->rwlock);
	__wt_spin_unlock(session, &conn->spinlock);

	if (!inuse) {
		if (F_ISSET(btree, WT_BTREE_OPEN)) {
			ret = __wt_btree_close(session);
			F_CLR(btree, WT_BTREE_OPEN | WT_BTREE_SPECIAL_FLAGS);
		}
		if (!locked)
			__wt_rwunlock(session, btree->rwlock);
	}

	return (ret);
}

/*
 * __conn_btree_remove --
 *	Discard a single btree file handle structure.
 */
static int
__conn_btree_remove(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
	WT_DECL_RET;

	if (F_ISSET(btree, WT_BTREE_OPEN)) {
		WT_SET_BTREE_IN_SESSION(session, btree);
		WT_TRET(__wt_btree_close(session));
		F_CLR(btree, WT_BTREE_OPEN | WT_BTREE_SPECIAL_FLAGS);
		WT_CLEAR_BTREE_IN_SESSION(session);
	}
	WT_TRET(__wt_rwlock_destroy(session, btree->rwlock));
	__wt_free(session, btree->config);
	__wt_free(session, btree->name);
	__wt_free(session, btree->snapshot);
	__wt_free(session, btree);

	return (ret);
}

/*
 * __wt_conn_btree_remove --
 *	Discard the btree file handle structures.
 */
int
__wt_conn_btree_remove(WT_CONNECTION_IMPL *conn)
{
	WT_BTREE *btree;
	WT_BTREE_SESSION *btree_session;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	/*
	 * We need a session handle because we're potentially reading/writing
	 * pages.
	 */
	WT_RET(__wt_open_session(conn, 1, NULL, NULL, &session));

	/*
	 * Close open btree handles: first, everything but the metadata file
	 * (as closing a normal file may open and write the metadata file),
	 * then the metadata file.  This function isn't called often, and I
	 * don't want to "know" anything about the metadata file's position on
	 * the list, so we do it the hard way.
	 */
restart:
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (strcmp(btree->name, WT_METADATA_URI) == 0)
			continue;

		TAILQ_REMOVE(&conn->btqh, btree, q);
		--conn->btqcnt;
		WT_TRET(__conn_btree_remove(session, btree));
		goto restart;
	}

	/*
	 * Closing the files may have resulted in entries on our session's list
	 * of open btree handles, specifically, we added the metadata file if
	 * any of the files were dirty.  Clean up that list before we shut down
	 * the metadata entry, for good.
	 */
	while ((btree_session = TAILQ_FIRST(&session->btrees)) != NULL)
		WT_TRET(__wt_session_remove_btree(session, btree_session, 0));

	/* Close the metadata file handle. */
	while ((btree = TAILQ_FIRST(&conn->btqh)) != NULL) {
		TAILQ_REMOVE(&conn->btqh, btree, q);
		--conn->btqcnt;
		WT_TRET(__conn_btree_remove(session, btree));
	}

	/* Discard our session. */
	WT_TRET(session->iface.close(&session->iface, NULL));

	return (ret);
}

/*
 * __wt_conn_btree_reopen --
 *	Reset an open btree handle back to its initial state.
 */
int
__wt_conn_btree_reopen(
    WT_SESSION_IMPL *session, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;

	btree = session->btree;

	if (F_ISSET(btree, WT_BTREE_OPEN)) {
		WT_RET(__wt_btree_close(session));
		F_CLR(btree, WT_BTREE_OPEN | WT_BTREE_SPECIAL_FLAGS);
	}

	WT_RET(__conn_btree_open_lock(session, flags));
	WT_RET(__conn_btree_open(session, cfg, flags));

	F_SET(btree, WT_BTREE_OPEN);
	return (0);
}
