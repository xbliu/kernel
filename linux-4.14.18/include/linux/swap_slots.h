/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SWAP_SLOTS_H
#define _LINUX_SWAP_SLOTS_H

#include <linux/swap.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

#define SWAP_SLOTS_CACHE_SIZE			SWAP_BATCH
#define THRESHOLD_ACTIVATE_SWAP_SLOTS_CACHE	(5*SWAP_SLOTS_CACHE_SIZE)
#define THRESHOLD_DEACTIVATE_SWAP_SLOTS_CACHE	(2*SWAP_SLOTS_CACHE_SIZE)

/* 
swap_slots_cache是为加快swap slot分配速度的swap slot cache机制.
 
之所以采用分配和释放两个cache，而不是让刚刚释放的slot可以立刻被使用的一个cache的方案,
目的主要是为了让这些slots有机会回到global pool中,
以便和同一swap cluster中的其他slots聚合,以减少碎片
*/
struct swap_slots_cache {
	bool		lock_initialized;
	struct mutex	alloc_lock; /* protects slots, nr, cur */
	swp_entry_t	*slots; //用于分配
	int		nr;
	int		cur;
	spinlock_t	free_lock;  /* protects slots_ret, n_ret */
	swp_entry_t	*slots_ret; //用于释放
	int		n_ret;
};

void disable_swap_slots_cache_lock(void);
void reenable_swap_slots_cache_unlock(void);
int enable_swap_slots_cache(void);
int free_swap_slot(swp_entry_t entry);

extern bool swap_slot_cache_enabled;

#endif /* _LINUX_SWAP_SLOTS_H */
