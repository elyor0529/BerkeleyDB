/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1996, 1997, 1998, 1999, 2000
 *	Sleepycat Software.  All rights reserved.
 */
/*
 * Copyright (c) 1990, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Margo Seltzer.
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
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: hash_dup.c,v 11.37 2000/04/18 22:19:29 bostic Exp $";
#endif /* not lint */

/*
 * PACKAGE:  hashing
 *
 * DESCRIPTION:
 *      Manipulation of duplicates for the hash package.
 *
 * ROUTINES:
 *
 * External
 *      __add_dup
 * Internal
 */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>

#include <errno.h>
#include <string.h>
#endif

#include "db_int.h"
#include "db_page.h"
#include "hash.h"
#include "btree.h"

static int __ham_check_move __P((DBC *, u_int32_t));
static int __ham_dcursor __P((DBC *, db_pgno_t, u_int32_t));

/*
 * Called from hash_access to add a duplicate key. nval is the new
 * value that we want to add.  The flags correspond to the flag values
 * to cursor_put indicating where to add the new element.
 * There are 4 cases.
 * Case 1: The existing duplicate set already resides on a separate page.
 *	   We return and let the common code handle this.
 * Case 2: The element is small enough to just be added to the existing set.
 * Case 3: The element is large enough to be a big item, so we're going to
 *	   have to push the set onto a new page.
 * Case 4: The element is large enough to push the duplicate set onto a
 *	   separate page.
 *
 * PUBLIC: int __ham_add_dup __P((DBC *, DBT *, u_int32_t, db_pgno_t *));
 */
int
__ham_add_dup(dbc, nval, flags, pgnop)
	DBC *dbc;
	DBT *nval;
	u_int32_t flags;
	db_pgno_t *pgnop;
{
	DB *dbp;
	HASH_CURSOR *hcp;
	DBT pval, tmp_val;
	u_int32_t add_bytes, new_size;
	int cmp, ret;
	u_int8_t *hk;

	dbp = dbc->dbp;
	hcp = (HASH_CURSOR *)dbc->internal;

	DB_ASSERT(flags != DB_CURRENT);

	add_bytes = nval->size +
	    (F_ISSET(nval, DB_DBT_PARTIAL) ? nval->doff : 0);
	add_bytes = DUP_SIZE(add_bytes);

	if ((ret = __ham_check_move(dbc, add_bytes)) != 0)
		return (ret);

	/*
	 * Check if resulting duplicate set is going to need to go
	 * onto a separate duplicate page.  If so, convert the
	 * duplicate set and add the new one.  After conversion,
	 * hcp->dndx is the first free ndx or the index of the
	 * current pointer into the duplicate set.
	 */
	hk = H_PAIRDATA(hcp->page, hcp->indx);
	new_size =
	    LEN_HKEYDATA(hcp->page, dbp->pgsize, H_DATAINDEX(hcp->indx)) +
	    add_bytes;

	/*
	 * We convert to off-page duplicates if the item is a big item,
	 * the addition of the new item will make the set large, or
	 * if there isn't enough room on this page to add the next item.
	 */
	if (HPAGE_PTYPE(hk) != H_OFFDUP &&
	    (HPAGE_PTYPE(hk) == H_OFFPAGE || ISBIG(hcp, new_size) ||
	    add_bytes > P_FREESPACE(hcp->page))) {

		if ((ret = __ham_dup_convert(dbc)) != 0)
			return (ret);
		return(hcp->opd->c_am_put(hcp->opd,
		    NULL, nval, flags, NULL));
	}

	/* There are two separate cases here: on page and off page. */
	if (HPAGE_PTYPE(hk) != H_OFFDUP) {
		if (HPAGE_PTYPE(hk) != H_DUPLICATE) {
			pval.flags = 0;
			pval.data = HKEYDATA_DATA(hk);
			pval.size = LEN_HDATA(hcp->page, dbp->pgsize,
			    hcp->indx);
			if ((ret = __ham_make_dup(dbp->dbenv,
			    &pval, &tmp_val, &dbc->rdata.data,
			    &dbc->rdata.ulen)) != 0 || (ret =
			    __ham_replpair(dbc, &tmp_val, 1)) != 0)
				return (ret);
			hk = H_PAIRDATA(hcp->page, hcp->indx);
			HPAGE_PTYPE(hk) = H_DUPLICATE;

			/*
			 * Update the cursor position since we now are in
			 * duplicates.
			 */
			F_SET(hcp, H_ISDUP);
			hcp->dup_off = 0;
			hcp->dup_len = pval.size;
			hcp->dup_tlen = DUP_SIZE(hcp->dup_len);
		}

		/* Now make the new entry a duplicate. */
		if ((ret = __ham_make_dup(dbp->dbenv, nval,
		    &tmp_val, &dbc->rdata.data, &dbc->rdata.ulen)) != 0)
			return (ret);

		tmp_val.dlen = 0;
		switch (flags) {			/* On page. */
		case DB_KEYFIRST:
		case DB_KEYLAST:
		case DB_NODUPDATA:
			if (dbp->dup_compare != NULL) {
				__ham_dsearch(dbc, nval, &tmp_val.doff, &cmp);

				/* dup dups are not supported w/ sorted dups */
				if (cmp == 0)
					return (__db_duperr(dbp, flags));
			} else {
				hcp->dup_tlen = LEN_HDATA(hcp->page,
				    dbp->pgsize, hcp->indx);
				hcp->dup_len = nval->size;
				F_SET(hcp, H_ISDUP);
				if (flags == DB_KEYFIRST)
					hcp->dup_off = tmp_val.doff = 0;
				else
					hcp->dup_off =
					    tmp_val.doff = hcp->dup_tlen;
			}
			break;
		case DB_BEFORE:
			tmp_val.doff = hcp->dup_off;
			break;
		case DB_AFTER:
			tmp_val.doff = hcp->dup_off + DUP_SIZE(hcp->dup_len);
			break;
		}
		/* Add the duplicate. */
		ret = __ham_replpair(dbc, &tmp_val, 0);
		if (ret == 0)
			ret = __ham_dirty_page(dbp, hcp->page);

		/* Now, update the cursor if necessary. */
		switch (flags) {
		case DB_AFTER:
			hcp->dup_off += DUP_SIZE(hcp->dup_len);
			hcp->dup_len = nval->size;
			hcp->dup_tlen += DUP_SIZE(nval->size);
			break;
		case DB_KEYFIRST:
		case DB_KEYLAST:
		case DB_BEFORE:
			hcp->dup_tlen += DUP_SIZE(nval->size);
			hcp->dup_len = nval->size;
			break;
		}
		__ham_c_update(dbc, hcp->pgno, tmp_val.size, 1, 1);
		return (ret);
	}

	/*
	 * If we get here, then we're on duplicate pages; set pgnop and
	 * return so the common code can handle it.
	 */
	memcpy(pgnop,
	    HOFFDUP_PGNO(H_PAIRDATA(hcp->page, hcp->indx)), sizeof(db_pgno_t));

	return (ret);
}

/*
 * Convert an on-page set of duplicates to an offpage set of duplicates.
 *
 * PUBLIC: int __ham_dup_convert __P((DBC *));
 */
int
__ham_dup_convert(dbc)
	DBC *dbc;
{
	DB *dbp;
	DBC **hcs;
	PAGE *dp;
	HASH_CURSOR *hcp;
	BOVERFLOW bo;
	DBT dbt;
	HOFFPAGE ho;
	db_indx_t i, len, off;
	int c, ret, t_ret;
	u_int8_t *p, *pend;

	dbp = dbc->dbp;
	hcp = (HASH_CURSOR *)dbc->internal;

	/*
	 * Create a new page for the duplicates.
	 */
	if ((ret = __db_new(dbc,
	    dbp->dup_compare == NULL ? P_LRECNO : P_LDUP, &dp)) != 0)
		return (ret);
	P_INIT(dp, dbp->pgsize,
	    dp->pgno, PGNO_INVALID, PGNO_INVALID, LEAFLEVEL, TYPE(dp));

	/*
	 * Get the list of cursors that may need to be updated.
	 */
	if ((ret = __ham_get_clist(dbp,
	    PGNO(hcp->page), (u_int32_t)hcp->indx, &hcs)) != 0)
		return (ret);

	/*
	 * Now put the duplicates onto the new page.
	 */
	dbt.flags = 0;
	switch (HPAGE_PTYPE(H_PAIRDATA(hcp->page, hcp->indx))) {
	case H_KEYDATA:
		/* Simple case, one key on page; move it to dup page. */
		dbt.size = LEN_HDATA(hcp->page, dbp->pgsize, hcp->indx);
		dbt.data = HKEYDATA_DATA(H_PAIRDATA(hcp->page, hcp->indx));
		ret = __db_pitem(dbc,
		    dp, 0, BKEYDATA_SIZE(dbt.size), NULL, &dbt);
		goto finish;
	case H_OFFPAGE:
		/* Simple case, one key on page; move it to dup page. */
		memcpy(&ho,
		    P_ENTRY(hcp->page, H_DATAINDEX(hcp->indx)), HOFFPAGE_SIZE);
		UMRW(bo.unused1);
		B_TSET(bo.type, ho.type, 0);
		UMRW(bo.unused2);
		bo.pgno = ho.pgno;
		bo.tlen = ho.tlen;
		dbt.size = BOVERFLOW_SIZE;
		dbt.data = &bo;

		ret = __db_pitem(dbc, dp, 0, dbt.size, &dbt, NULL);

finish:		if (ret == 0) {
			__ham_dirty_page(dbp, dp);
			/*
			 * Update any other cursors
			 */
			for (c = 0; hcs != NULL && hcs[c] != NULL; c++)
				if ((ret = __ham_dcursor(hcs[c],
				    PGNO(dp), 0)) != 0)
					break;
		}
		break;

	case H_DUPLICATE:
		p = HKEYDATA_DATA(H_PAIRDATA(hcp->page, hcp->indx));
		pend = p +
		    LEN_HDATA(hcp->page, dbp->pgsize, hcp->indx);

		/*
		 * We need to maintain the duplicate cursor position.
		 * Keep track of where we are in the duplicate set via
		 * the offset, and when it matches the one in the cursor,
		 * set the off-page duplicate cursor index to the current
		 * index.
		 */
		for (off = 0, i = 0; p < pend; i++) {
			memcpy(&len, p, sizeof(db_indx_t));
			dbt.size = len;
			p += sizeof(db_indx_t);
			dbt.data = p;
			p += len + sizeof(db_indx_t);
			if ((ret = __db_pitem(dbc, dp,
			    i, BKEYDATA_SIZE(dbt.size), NULL, &dbt)) != 0)
				break;
			/*
			 * Update any other cursors
			 */
			for (c = 0; hcs != NULL && hcs[c] != NULL; c++)
				if (((HASH_CURSOR *)(hcs[c]->internal))->dup_off
				    == off && (ret = __ham_dcursor(hcs[c],
				    PGNO(dp), i)) != 0)
					goto out;
			off += len + 2 * sizeof(db_indx_t);
		}
out:		break;

	default:
		ret = __db_pgfmt(dbp, (u_long)hcp->pgno);
		break;
	}
	if (ret == 0) {
		/*
		 * Now attach this to the source page in place of
		 * the old duplicate item.
		 */
		__ham_move_offpage(dbc, hcp->page,
		    (u_int32_t)H_DATAINDEX(hcp->indx), PGNO(dp));

		ret = __ham_dirty_page(dbp, hcp->page);
		if ((t_ret = __ham_put_page(dbp, dp, 1)) != 0)
			ret = t_ret;
		hcp->dup_tlen = hcp->dup_off = hcp->dup_len = 0;
	} else
		(void)__db_free(dbc, dp);

	if (hcs != NULL)
		__os_free(hcs, 0);

	return (ret);
}

/*
 * __ham_make_dup
 *
 * Take a regular dbt and make it into a duplicate item with all the partial
 * information set appropriately. If the incoming dbt is a partial, assume
 * we are creating a new entry and make sure that we do any initial padding.
 *
 * PUBLIC: int __ham_make_dup __P((DB_ENV *,
 * PUBLIC:     const DBT *, DBT *d, void **, u_int32_t *));
 */
int
__ham_make_dup(dbenv, notdup, duplicate, bufp, sizep)
	DB_ENV *dbenv;
	const DBT *notdup;
	DBT *duplicate;
	void **bufp;
	u_int32_t *sizep;
{
	db_indx_t tsize, item_size;
	int ret;
	u_int8_t *p;

	item_size = (db_indx_t)notdup->size;
	if (F_ISSET(notdup, DB_DBT_PARTIAL))
		item_size += notdup->doff;

	tsize = DUP_SIZE(item_size);
	if ((ret = __ham_init_dbt(dbenv, duplicate, tsize, bufp, sizep)) != 0)
		return (ret);

	duplicate->dlen = 0;
	duplicate->flags = notdup->flags;
	F_SET(duplicate, DB_DBT_PARTIAL);

	p = duplicate->data;
	memcpy(p, &item_size, sizeof(db_indx_t));
	p += sizeof(db_indx_t);
	if (F_ISSET(notdup, DB_DBT_PARTIAL)) {
		memset(p, 0, notdup->doff);
		p += notdup->doff;
	}
	memcpy(p, notdup->data, notdup->size);
	p += notdup->size;
	memcpy(p, &item_size, sizeof(db_indx_t));

	duplicate->doff = 0;
	duplicate->dlen = notdup->size;

	return (0);
}

/*
 * __ham_check_move --
 *
 * Check if we can do whatever we need to on this page.  If not,
 * then we'll have to move the current element to a new page.
 */
static int
__ham_check_move(dbc, add_len)
	DBC *dbc;
	u_int32_t add_len;
{
	DB *dbp;
	HASH_CURSOR *hcp;
	DBT k, d;
	DB_LSN new_lsn;
	PAGE *next_pagep;
	db_pgno_t next_pgno;
	u_int32_t new_datalen, old_len, rectype;
	u_int8_t *hk;
	int ret;

	dbp = dbc->dbp;
	hcp = (HASH_CURSOR *)dbc->internal;

	hk = H_PAIRDATA(hcp->page, hcp->indx);

	/*
	 * If the item is already off page duplicates or an offpage item,
	 * then we know we can do whatever we need to do in-place
	 */
	if (HPAGE_PTYPE(hk) == H_OFFDUP || HPAGE_PTYPE(hk) == H_OFFPAGE)
		return (0);

	old_len = LEN_HITEM(hcp->page, dbp->pgsize, H_DATAINDEX(hcp->indx));
	new_datalen = old_len - HKEYDATA_SIZE(0) + add_len;
	if (HPAGE_PTYPE(hk) != H_DUPLICATE)
		new_datalen += DUP_SIZE(0);

	/*
	 * We need to add a new page under two conditions:
	 * 1. The addition makes the total data length cross the BIG
	 *    threshold and the OFFDUP structure won't fit on this page.
	 * 2. The addition does not make the total data cross the
	 *    threshold, but the new data won't fit on the page.
	 * If neither of these is true, then we can return.
	 */
	if (ISBIG(hcp, new_datalen) && (old_len > HOFFDUP_SIZE ||
	    HOFFDUP_SIZE - old_len <= P_FREESPACE(hcp->page)))
		return (0);

	if (!ISBIG(hcp, new_datalen) && add_len <= P_FREESPACE(hcp->page))
		return (0);

	/*
	 * If we get here, then we need to move the item to a new page.
	 * Check if there are more pages in the chain.
	 */

	new_datalen = ISBIG(hcp, new_datalen) ?
	    HOFFDUP_SIZE : HKEYDATA_SIZE(new_datalen);

	next_pagep = NULL;
	for (next_pgno = NEXT_PGNO(hcp->page); next_pgno != PGNO_INVALID;
	    next_pgno = NEXT_PGNO(next_pagep)) {
		if (next_pagep != NULL &&
		    (ret = __ham_put_page(dbp, next_pagep, 0)) != 0)
			return (ret);

		if ((ret =
		    __ham_get_page(dbp, next_pgno, &next_pagep)) != 0)
			return (ret);

		if (P_FREESPACE(next_pagep) >= new_datalen)
			break;
	}

	/* No more pages, add one. */
	if (next_pagep == NULL && (ret = __ham_add_ovflpage(dbc,
	    hcp->page, 0, &next_pagep)) != 0)
		return (ret);

	/* Add new page at the end of the chain. */
	if (P_FREESPACE(next_pagep) < new_datalen && (ret =
	    __ham_add_ovflpage(dbc, next_pagep, 1, &next_pagep)) != 0) {
		(void)__ham_put_page(dbp, next_pagep, 0);
		return (ret);
	}

	/* Copy the item to the new page. */
	if (DB_LOGGING(dbc)) {
		rectype = PUTPAIR;
		k.flags = 0;
		d.flags = 0;
		if (HPAGE_PTYPE(
		    H_PAIRKEY(hcp->page, hcp->indx)) == H_OFFPAGE) {
			rectype |= PAIR_KEYMASK;
			k.data = H_PAIRKEY(hcp->page, hcp->indx);
			k.size = HOFFPAGE_SIZE;
		} else {
			k.data =
			    HKEYDATA_DATA(H_PAIRKEY(hcp->page, hcp->indx));
			k.size = LEN_HKEY(hcp->page, dbp->pgsize, hcp->indx);
		}

		if (HPAGE_PTYPE(hk) == H_OFFPAGE) {
			rectype |= PAIR_DATAMASK;
			d.data = H_PAIRDATA(hcp->page, hcp->indx);
			d.size = HOFFPAGE_SIZE;
		} else {
			if (HPAGE_PTYPE(H_PAIRDATA(hcp->page, hcp->indx))
			    == H_DUPLICATE)
				rectype |= PAIR_DUPMASK;
			d.data =
			    HKEYDATA_DATA(H_PAIRDATA(hcp->page, hcp->indx));
			d.size = LEN_HDATA(hcp->page, dbp->pgsize, hcp->indx);
		}

		if ((ret = __ham_insdel_log(dbp->dbenv,
		    dbc->txn, &new_lsn, 0, rectype,
		    dbp->log_fileid, PGNO(next_pagep),
		    (u_int32_t)NUM_ENT(next_pagep), &LSN(next_pagep),
		    &k, &d)) != 0)
			return (ret);

		/* Move lsn onto page. */
		LSN(next_pagep) = new_lsn;	/* Structure assignment. */
	}

	__ham_copy_item(dbp->pgsize,
	    hcp->page, H_KEYINDEX(hcp->indx), next_pagep);
	__ham_copy_item(dbp->pgsize,
	    hcp->page, H_DATAINDEX(hcp->indx), next_pagep);

	/* Update all cursors that used to point to this item. */
	__ham_c_chgpg(dbc, PGNO(hcp->page), H_KEYINDEX(hcp->indx),
	    PGNO(next_pagep), NUM_ENT(next_pagep) - 2);

	/* Now delete the pair from the current page. */
	ret = __ham_del_pair(dbc, 0);

	/*
	 * __ham_del_pair decremented nelem.  This is incorrect;  we
	 * manually copied the element elsewhere, so the total number
	 * of elements hasn't changed.  Increment it again.
	 */
	if (!STD_LOCKING(dbc))
		hcp->hdr->nelem++;

	(void)__ham_put_page(dbp, hcp->page, 1);
	hcp->page = next_pagep;
	hcp->pgno = PGNO(hcp->page);
	hcp->indx = NUM_ENT(hcp->page) - 2;
	F_SET(hcp, H_EXPAND);
	return (ret);
}

/*
 * __ham_move_offpage --
 *	Replace an onpage set of duplicates with the OFFDUP structure
 *	that references the duplicate page.
 *
 * XXX
 * This is really just a special case of __onpage_replace; we should
 * probably combine them.
 *
 * PUBLIC: void __ham_move_offpage __P((DBC *, PAGE *, u_int32_t, db_pgno_t));
 */
void
__ham_move_offpage(dbc, pagep, ndx, pgno)
	DBC *dbc;
	PAGE *pagep;
	u_int32_t ndx;
	db_pgno_t pgno;
{
	DB *dbp;
	HASH_CURSOR *hcp;
	DBT new_dbt;
	DBT old_dbt;
	HOFFDUP od;
	db_indx_t i;
	int32_t shrink;
	u_int8_t *src;

	dbp = dbc->dbp;
	hcp = (HASH_CURSOR *)dbc->internal;
	od.type = H_OFFDUP;
	UMRW(od.unused[0]);
	UMRW(od.unused[1]);
	UMRW(od.unused[2]);
	od.pgno = pgno;

	if (DB_LOGGING(dbc)) {
		new_dbt.data = &od;
		new_dbt.size = HOFFDUP_SIZE;
		old_dbt.data = P_ENTRY(pagep, ndx);
		old_dbt.size = LEN_HITEM(pagep, dbp->pgsize, ndx);
		(void)__ham_replace_log(dbp->dbenv,
		    dbc->txn, &LSN(pagep), 0, dbp->log_fileid,
		    PGNO(pagep), (u_int32_t)ndx, &LSN(pagep), -1,
		    &old_dbt, &new_dbt, 0);
	}

	shrink = LEN_HITEM(pagep, dbp->pgsize, ndx) - HOFFDUP_SIZE;

	if (shrink != 0) {
		/* Copy data. */
		src = (u_int8_t *)(pagep) + HOFFSET(pagep);
		memmove(src + shrink, src, pagep->inp[ndx] - HOFFSET(pagep));
		HOFFSET(pagep) += shrink;

		/* Update index table. */
		for (i = ndx; i < NUM_ENT(pagep); i++)
			pagep->inp[i] += shrink;
	}

	/* Now copy the offdup entry onto the page. */
	memcpy(P_ENTRY(pagep, ndx), &od, HOFFDUP_SIZE);
}

/*
 * __ham_dsearch:
 *	Locate a particular duplicate in a duplicate set.  Make sure that
 *	we exit with the cursor set appropriately.
 *
 * PUBLIC: void __ham_dsearch __P((DBC *, DBT *, u_int32_t *, int *));
 */
void
__ham_dsearch(dbc, dbt, offp, cmpp)
	DBC *dbc;
	DBT *dbt;
	u_int32_t *offp;
	int *cmpp;
{
	DB *dbp;
	HASH_CURSOR *hcp;
	DBT cur;
	db_indx_t i, len;
	int (*func) __P((const DBT *, const DBT *));
	u_int8_t *data;

	dbp = dbc->dbp;
	hcp = (HASH_CURSOR *)dbc->internal;
	if (dbp->dup_compare == NULL)
		func = __bam_defcmp;
	else
		func = dbp->dup_compare;

	i = F_ISSET(hcp, H_CONTINUE) ? hcp->dup_off: 0;
	data = HKEYDATA_DATA(H_PAIRDATA(hcp->page, hcp->indx)) + i;
	hcp->dup_tlen = LEN_HDATA(hcp->page, dbp->pgsize, hcp->indx);
	while (i < hcp->dup_tlen) {
		memcpy(&len, data, sizeof(db_indx_t));
		data += sizeof(db_indx_t);
		cur.data = data;
		cur.size = (u_int32_t)len;
		*cmpp = func(dbt, &cur);
		if (*cmpp == 0 || (*cmpp < 0 && dbp->dup_compare != NULL))
			break;
		i += len + 2 * sizeof(db_indx_t);
		data += len + sizeof(db_indx_t);
	}
	*offp = i;
	hcp->dup_off = i;
	hcp->dup_len = len;
	F_SET(hcp, H_ISDUP);
}

#ifdef DEBUG
/*
 * __ham_cprint --
 *	Display the current cursor list.
 *
 * PUBLIC: int __ham_cprint __P((DB *));
 */
int
__ham_cprint(dbp)
	DB *dbp;
{
	HASH_CURSOR *cp;
	DBC *dbc;

	MUTEX_THREAD_LOCK(dbp->mutexp);
	for (dbc = TAILQ_FIRST(&dbp->active_queue);
	    dbc != NULL; dbc = TAILQ_NEXT(dbc, links)) {
		cp = (HASH_CURSOR *)dbc->internal;
		fprintf(stderr, "%#0lx->%#0lx: page: %lu index: %lu",
		    P_TO_ULONG(dbc), P_TO_ULONG(cp), (u_long)cp->pgno,
		    (u_long)cp->indx);
		if (F_ISSET(cp, H_DELETED))
			fprintf(stderr, " (deleted)");
		fprintf(stderr, "\n");
	}
	MUTEX_THREAD_UNLOCK(dbp->mutexp);

	return (0);
}
#endif /* DEBUG */

/*
 * __ham_dcursor --
 *
 *	Create an off page duplicate cursor for this cursor.
 */
static int
__ham_dcursor(dbc, pgno, indx)
	DBC *dbc;
	db_pgno_t pgno;
	u_int32_t indx;
{
	DB *dbp;
	DBC *dbc_nopd;
	HASH_CURSOR *hcp;
	BTREE_CURSOR *dcp;
	int ret;

	dbp = dbc->dbp;

	dbc_nopd = NULL;
	if ((ret = __db_icursor(dbp, dbc->txn,
	    dbp->dup_compare == NULL ? DB_RECNO : DB_BTREE,
	    pgno, 1, &dbc_nopd)) != 0)
		return (ret);

	dcp = (BTREE_CURSOR *)dbc_nopd->internal;
	dcp->pgno = pgno;
	dcp->indx = indx;

	if (dbp->dup_compare == NULL) {
		/*
		 * Converting to off-page Recno trees is tricky.  The
		 * record number for the cursor is the index + 1 (to
		 * convert to 1-based record numbers).
		 */
		dcp->recno = indx + 1;
	}

	/*
	 * Transfer the deleted flag from the top-level cursor to the
	 * created one.
	 */
	hcp = (HASH_CURSOR *)dbc->internal;
	if (F_ISSET(hcp, H_DELETED)) {
		F_SET(dcp, C_DELETED);
		F_CLR(hcp, H_DELETED);
	}

	/* Stack the cursors and reset the initial cursor's index. */
	hcp->opd = dbc_nopd;

	return (0);
}
