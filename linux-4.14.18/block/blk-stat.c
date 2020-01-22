/*
 * Block stat tracking code
 *
 * Copyright (C) 2016 Jens Axboe
 */
#include <linux/kernel.h>
#include <linux/rculist.h>
#include <linux/blk-mq.h>

#include "blk-stat.h"
#include "blk-mq.h"
#include "blk.h"

#define BLK_RQ_STAT_BATCH	64

struct blk_queue_stats {
	struct list_head callbacks;
	spinlock_t lock;
	bool enable_accounting;
};

static void blk_stat_init(struct blk_rq_stat *stat)
{
	stat->min = -1ULL;
	stat->max = stat->nr_samples = stat->mean = 0;
	stat->batch = stat->nr_batch = 0;
}

static void blk_stat_flush_batch(struct blk_rq_stat *stat)
{
	const s32 nr_batch = READ_ONCE(stat->nr_batch);
	const s32 nr_samples = READ_ONCE(stat->nr_samples);

	if (!nr_batch)
		return;
	if (!nr_samples) //首次采样完成
		stat->mean = div64_s64(stat->batch, nr_batch);
	else {
		stat->mean = div64_s64((stat->mean * nr_samples) +
					stat->batch,
					nr_batch + nr_samples);
	}

	stat->nr_samples += nr_batch; //总采样次数增加
	stat->nr_batch = stat->batch = 0; //批量统计清0,进行下一次批量统计
}

static void blk_stat_sum(struct blk_rq_stat *dst, struct blk_rq_stat *src)
{
	blk_stat_flush_batch(src); //刷新这次批量统计

	if (!src->nr_samples)
		return;

	dst->min = min(dst->min, src->min);
	dst->max = max(dst->max, src->max);

	if (!dst->nr_samples) //dst还未完成一次采样
		dst->mean = src->mean;
	else {
		dst->mean = div64_s64((src->mean * src->nr_samples) +
					(dst->mean * dst->nr_samples),
					dst->nr_samples + src->nr_samples);
	}
	dst->nr_samples += src->nr_samples; //采样次数为两者之和
}

static void __blk_stat_add(struct blk_rq_stat *stat, u64 value)
{
	/*a1.更新计量单位的最小与最大值*/
	stat->min = min(stat->min, value);
	stat->max = max(stat->max, value);

	/*
	a2.批量统计的总量数值将要越界或采样次数为BLK_RQ_STAT_BATCH,
	刷新统计值
	*/
	if (stat->batch + value < stat->batch ||
	    stat->nr_batch + 1 == BLK_RQ_STAT_BATCH)
		blk_stat_flush_batch(stat);

	/*a3.更新此次批量统计的采样次数与采样数值*/
	stat->batch += value;
	stat->nr_batch++;
}

void blk_stat_add(struct request *rq)
{
	struct request_queue *q = rq->q;
	struct blk_stat_callback *cb;
	struct blk_rq_stat *stat;
	int bucket;
	s64 now, value;

	now = __blk_stat_time(ktime_to_ns(ktime_get()));
	if (now < blk_stat_time(&rq->issue_stat))
		return;

	value = now - blk_stat_time(&rq->issue_stat); //计算花费的时间

	blk_throtl_stat_add(rq, value);

	rcu_read_lock();
	list_for_each_entry_rcu(cb, &q->stats->callbacks, list) {
		if (!blk_stat_is_active(cb))
			continue;

		bucket = cb->bucket_fn(rq); /*获取本次的stat的类别:像wb分读与写两类*/
		if (bucket < 0)
			continue;

		stat = &get_cpu_ptr(cb->cpu_stat)[bucket]; //获取本地cpu的stat
		__blk_stat_add(stat, value); //更新本地cpu的stat
		put_cpu_ptr(cb->cpu_stat);
	}
	rcu_read_unlock();
}

static void blk_stat_timer_fn(unsigned long data)
{
	struct blk_stat_callback *cb = (void *)data;
	unsigned int bucket;
	int cpu;

	for (bucket = 0; bucket < cb->buckets; bucket++)
		blk_stat_init(&cb->stat[bucket]); //初始化 stat

	for_each_online_cpu(cpu) {
		struct blk_rq_stat *cpu_stat;

		cpu_stat = per_cpu_ptr(cb->cpu_stat, cpu);
		for (bucket = 0; bucket < cb->buckets; bucket++) {
			blk_stat_sum(&cb->stat[bucket], &cpu_stat[bucket]); //更新总stat
			blk_stat_init(&cpu_stat[bucket]); //初始化本地cpu stat
		}
	}

	cb->timer_fn(cb);
}

struct blk_stat_callback *
blk_stat_alloc_callback(void (*timer_fn)(struct blk_stat_callback *),
			int (*bucket_fn)(const struct request *),
			unsigned int buckets, void *data)
{
	struct blk_stat_callback *cb;

	cb = kmalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return NULL;

	cb->stat = kmalloc_array(buckets, sizeof(struct blk_rq_stat),
				 GFP_KERNEL);
	if (!cb->stat) {
		kfree(cb);
		return NULL;
	}
	cb->cpu_stat = __alloc_percpu(buckets * sizeof(struct blk_rq_stat),
				      __alignof__(struct blk_rq_stat));
	if (!cb->cpu_stat) {
		kfree(cb->stat);
		kfree(cb);
		return NULL;
	}

	cb->timer_fn = timer_fn; //定时更新回调
	cb->bucket_fn = bucket_fn; //获取哪一类回调
	cb->data = data;
	cb->buckets = buckets;
	setup_timer(&cb->timer, blk_stat_timer_fn, (unsigned long)cb);

	return cb;
}
EXPORT_SYMBOL_GPL(blk_stat_alloc_callback);

void blk_stat_add_callback(struct request_queue *q,
			   struct blk_stat_callback *cb)
{
	unsigned int bucket;
	int cpu;

	/*blk_stat_callback 的 所有的本地cpu blk_rq_stat初始化*/
	for_each_possible_cpu(cpu) {
		struct blk_rq_stat *cpu_stat;

		cpu_stat = per_cpu_ptr(cb->cpu_stat, cpu);
		for (bucket = 0; bucket < cb->buckets; bucket++)
			blk_stat_init(&cpu_stat[bucket]);
	}

	spin_lock(&q->stats->lock);
	list_add_tail_rcu(&cb->list, &q->stats->callbacks); //增加到request stats callback
	set_bit(QUEUE_FLAG_STATS, &q->queue_flags);
	spin_unlock(&q->stats->lock);
}
EXPORT_SYMBOL_GPL(blk_stat_add_callback);

void blk_stat_remove_callback(struct request_queue *q,
			      struct blk_stat_callback *cb)
{
	spin_lock(&q->stats->lock);
	list_del_rcu(&cb->list);
	if (list_empty(&q->stats->callbacks) && !q->stats->enable_accounting)
		clear_bit(QUEUE_FLAG_STATS, &q->queue_flags);
	spin_unlock(&q->stats->lock);

	del_timer_sync(&cb->timer);
}
EXPORT_SYMBOL_GPL(blk_stat_remove_callback);

static void blk_stat_free_callback_rcu(struct rcu_head *head)
{
	struct blk_stat_callback *cb;

	cb = container_of(head, struct blk_stat_callback, rcu);
	free_percpu(cb->cpu_stat);
	kfree(cb->stat);
	kfree(cb);
}

void blk_stat_free_callback(struct blk_stat_callback *cb)
{
	if (cb)
		call_rcu(&cb->rcu, blk_stat_free_callback_rcu);
}
EXPORT_SYMBOL_GPL(blk_stat_free_callback);

void blk_stat_enable_accounting(struct request_queue *q)
{
	spin_lock(&q->stats->lock);
	q->stats->enable_accounting = true;
	set_bit(QUEUE_FLAG_STATS, &q->queue_flags);
	spin_unlock(&q->stats->lock);
}

struct blk_queue_stats *blk_alloc_queue_stats(void)
{
	struct blk_queue_stats *stats;

	stats = kmalloc(sizeof(*stats), GFP_KERNEL);
	if (!stats)
		return NULL;

	INIT_LIST_HEAD(&stats->callbacks);
	spin_lock_init(&stats->lock);
	stats->enable_accounting = false;

	return stats;
}

void blk_free_queue_stats(struct blk_queue_stats *stats)
{
	if (!stats)
		return;

	WARN_ON(!list_empty(&stats->callbacks));

	kfree(stats);
}

/*
blk_queue_stats可包含多个blk_stat_callback
一个blk_stat_callback包括一个总的blk_rq_stat与多个本地cpu对应的blk_rq_stat
本地cpu的blk_rq_stat负责本cpu的统计,最终都汇总到总的blk_rq_stat
*/

