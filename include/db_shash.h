/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996, 1997, 1998, 1999, 2000
 *	Sleepycat Software.  All rights reserved.
 *
 * $Id: db_shash.h,v 11.4 2000/02/14 02:59:55 bostic Exp $
 */

/* Hash Headers */
typedef	SH_TAILQ_HEAD(__hash_head) DB_HASHTAB;

/*
 * HASHACCESS --
 *
 * Figure out to which bucket an item belongs and lock that bucket,
 * returning the bucket index.
 *
 * synch:  beginning of the array of mutexes that protect the table.
 * elt:	   the item on which we're computing the hash function.
 * nelems: the number of buckets in the hash table.
 * hash:   the hash function that operates on elements of the type of elt
 * ndx:	   the index into the hash/synch array that we're locking.
 * fh:	   the locking file handle.
 */
#define	HASHACCESS(synch, elt, nelems, hash, ndx, fh) do {		\
	ndx = hash(elt) % (nelems);					\
	MUTEX_LOCK(&synch[ndx], fh);					\
} while(0)

/*
 * HASHRELEASE --
 *
 * Release a hash bucket that we have locked.
 *
 * synch: beginning of the array of mutexes that protect the table.
 * ndx:	  the index into the hash/synch array that we're locking.
 * fh:	  the locking file handle.
 */
#define	HASHRELEASE(synch, ndx, fh) do {				\
	MUTEX_UNLOCK(&synch[ndx]);					\
} while(0)

/*
 * HASHLOOKUP --
 *
 * Look up something in a shared memory hash table.  The "elt" argument
 * should be a key, and cmp_func must know how to compare a key to whatever
 * structure it is that appears in the hash table.  The comparison function
 *
 * begin: address of the beginning of the hash table.
 * ndx:	  index into table for this item.
 * type:  the structure type of the elements that are linked in each bucket.
 * field: the name of the field by which the "type" structures are linked.
 * elt:   the item for which we are searching in the hash table.
 * res:   the variable into which we'll store the element if we find it.
 * cmp:   called as: cmp(lookup_elt, table_elt).
 *
 * If the element is not in the hash table, this macro exits with res set
 * to NULL.
 */
#define	HASHLOOKUP(begin, ndx, type, field, elt, res, cmp) do {		\
	DB_HASHTAB *__bucket;						\
									\
	__bucket = &begin[ndx];						\
	for (res = SH_TAILQ_FIRST(__bucket, type);			\
	    res != NULL; res = SH_TAILQ_NEXT(res, field, type))		\
		if (cmp(elt, res))					\
			break;						\
} while(0)

/*
 * HASHINSERT --
 *
 * Insert a new entry into the hash table.  This assumes that you already
 * have the bucket locked and that lookup has failed; don't call it if you
 * haven't already called HASHLOOKUP.  If you do, you could get duplicate
 * entries.
 *
 * begin: the beginning address of the hash table.
 * ndx:   the index for this element.
 * type:  the structure type of the elements that are linked in each bucket.
 * field: the name of the field by which the "type" structures are linked.
 * elt:   the item to be inserted.
 */
#define	HASHINSERT(begin, ndx, type, field, elt) do {			\
	DB_HASHTAB *__bucket;						\
									\
	__bucket = &begin[ndx];						\
	SH_TAILQ_INSERT_HEAD(__bucket, elt, field, type);		\
} while(0)

/*
 * HASHREMOVE_EL --
 *	Given the object "obj" in the table, remove it.
 *
 * begin: address of the beginning of the hash table.
 * ndx:   index into hash table of where this element belongs.
 * type:  the structure type of the elements that are linked in each bucket.
 * field: the name of the field by which the "type" structures are linked.
 * obj:   the object in the table that we with to delete.
 */
#define	HASHREMOVE_EL(begin, ndx, type, field, obj) {			\
	DB_HASHTAB *__bucket;						\
									\
	__bucket = &begin[ndx];						\
	SH_TAILQ_REMOVE(__bucket, obj, field, type);			\
}
