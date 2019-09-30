/*-------------------------------------------------------------------------
 *
 * vchain.c
 *    Version chain implementation for each tuple
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/vcluster/vchain.c
 *
 *-------------------------------------------------------------------------
 */
#ifdef HYU_LLT
#include "postgres.h"

#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/vchain_hash.h"
#include "storage/thread_table.h"

#include "storage/vchain.h"

#include <assert.h>

/*
 * VChainShmemSize
 *
 * Compute the size of shared memory for the vchain.
 * Physical chain is already embedded in the segment index so that
 * vchain hash table is what we only need to care about.
 */
Size
VChainShmemSize(void)
{
	Size		size = 0;

	size = add_size(size, VChainHashShmemSize(
			NVChainExpected + NUM_VCHAIN_PARTITIONS));

	return size;
}

/*
 * VChainInit
 *
 * Initialize vchain_hash in shared memory.
 * Physical chain is already embedded in the segment index.
 */
void
VChainInit(void)
{
	VChainHashInit(NVChainExpected + NUM_VCHAIN_PARTITIONS);
}

/*
 * VChainLookupLocator
 *
 * Find a version locator of a tuple which has the given primary key
 * and the visible version with the given snapshot.
 * Returns true if it found, false if not found.
 */
bool
VChainLookupLocator(PrimaryKey primary_key,
					Snapshot snapshot,
					VLocator **ret_locator)
{
	LWLock			*partition_lock;
	uint32			hashcode;
	dsa_pointer		dsap_chain;
	VLocator		*chain;
	VLocator		*locator;

	/* Get hash code for the primary key */
	hashcode = VChainHashCode(&primary_key);
	partition_lock = VChainMappingPartitionLock(hashcode);

	/* Acquire partition lock for the primary key with shared mode */
	LWLockAcquire(partition_lock, LW_SHARED);
	if (!VChainHashLookup(&primary_key, hashcode, &dsap_chain))
	{
		/* There is no hash entry for the primary key in the vchain hash */
		LWLockRelease(partition_lock);
		return false;
	}
	LWLockRelease(partition_lock);

	/* Set epoch */
	SetTimestamp();

	chain = (VLocator *)dsa_get_address(dsa_vcluster, dsap_chain);
	if (chain->dsap_prev == chain->dsap)
	{
		/* There is a hash entry, but the version chain is empty */

		/* Clear epoch */
		ClearTimestamp();

		return false;
	}

	/*
	 * Now we have the hash entry (dummy node) that indicates the
	 * head/tail of the version chain.
	 * Try to find the visible version from the recent version (tail)
	 */
	locator = (VLocator *)dsa_get_address(dsa_vcluster, chain->dsap_prev);
	while (locator->dsap != chain->dsap)
	{
		if (!XidInMVCCSnapshot(locator->xmin, snapshot))
		{
			/* Found the visible version */
			*ret_locator = locator;

			/* Clear epoch */
			ClearTimestamp();

			return true;
		}
		locator = (VLocator *)dsa_get_address(
				dsa_vcluster, locator->dsap_prev);
	}
	/* Fail to find a visible version */

	/* Clear epoch */
	ClearTimestamp();

	return false;
}

/*
 * VChainAppendLocator
 *
 * Append a vlocator into the version chain of the corresponding tuple.
 */
void
VChainAppendLocator(PrimaryKey primary_key, VLocator *locator)
{
	LWLock			*partition_lock;
	uint32			hashcode;
	dsa_pointer		dsap_chain;
	VLocator		*chain;
	dsa_pointer		dsap_tail_prev;
	VLocator		*tail_prev;
	VLocatorFlag	flag;

	/* Get hash code for the primary key */
	hashcode = VChainHashCode(&primary_key);
	partition_lock = VChainMappingPartitionLock(hashcode);

	/* Acquire partition lock for the primary key with shared mode */
	LWLockAcquire(partition_lock, LW_SHARED);
	if (VChainHashLookup(&primary_key, hashcode, &dsap_chain))
	{
		/* Hash entry is already exists */
		LWLockRelease(partition_lock);
	}
	else
	{
		/* Re-acquire the partition lock with exclusive mode */
		LWLockRelease(partition_lock);
		LWLockAcquire(partition_lock, LW_EXCLUSIVE);

		/* Insert a new hash entry for the primary key */
		VChainHashInsert(&primary_key, hashcode, &dsap_chain);
		LWLockRelease(partition_lock);
	}
	
	/*
	 * Now we have the hash entry (dummy node) that indicates the
	 * head/tail of the version chain.
	 * Appending a new version node could compete with the cleaner.
	 */

retry:
	chain = (VLocator *)dsa_get_address(dsa_vcluster, dsap_chain);

	/* Start the consensus protocol for transaction(inserter). */

	/* 1) Get pointer of tail's prev. */
	dsap_tail_prev = chain->dsap_prev;
	tail_prev = (VLocator *)dsa_get_address(dsa_vcluster, dsap_tail_prev);

	/* 2) Let's play fetch-and-add to decide winner or loser. */
	flag = (VLocatorFlag) __sync_lock_test_and_set(&tail_prev->flag, VL_APPEND);
	
	if (flag == VL_DELETE) {
		/* 3-2) I'm loser.. Spin until tail's prev is updated by cutter. */
		while (dsap_tail_prev == __sync_fetch_and_add(&chain->dsap_prev, 0)) {
			/* LLB TODO: Need to sleep a little?? */
			;
		}
		
		/* Retry until i'm winner */
		goto retry;
	}

	/* 3-1) Yes, i'm winner! Insert the node in version chain. */
	assert(flag == VL_WINNER);

	locator->dsap_prev = dsap_tail_prev;
	locator->dsap_next = chain->dsap;
	tail_prev->dsap_next = locator->dsap;
	chain->dsap_prev = locator->dsap;

	/* 4) Ok.. Let's check whether cutter have visited. */
	flag = (VLocatorFlag) __sync_lock_test_and_set(&tail_prev->flag, VL_WINNER);

	if (flag == VL_DELETE) {
		/* 5) Cutter have visited hear. Delete this node logically for cutter. */
		VChainFixUpOne(tail_prev);
	}

	/* End the consensus protocol. */
}

/*
 * VChainFixUpOne
 *
 * Fix-up a vlocator.
 * Link between prev and next of one, and delete logically.
 */
void
VChainFixUpOne(VLocator* mid)
{
	dsa_pointer dsap_prev;
	dsa_pointer dsap_next;
	
	VLocator* prev;
	VLocator* next;

	dsap_prev = mid->dsap_prev;
	dsap_next = mid->dsap_next;

	prev = (VLocator *)dsa_get_address(dsa_vcluster, dsap_prev);
	next = (VLocator *)dsa_get_address(dsa_vcluster, dsap_next);

	/* fix up */
	prev->dsap_next = dsap_next;
	next->dsap_prev = dsap_prev;

	/* memory barrier for timestamp */
	pg_memory_barrier();

	/* Set timestamp */
	mid->timestamp = GetCurrentTimestamp();
}

#endif /* HYU_LLT */
