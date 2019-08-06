/*
 *  Deadline i/o scheduler.
 *
 *  Copyright (C) 2002 Jens Axboe <axboe@kernel.dk>
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/rbtree.h>

/*
 * See Documentation/block/deadline-iosched.txt
 */
static const int read_expire = HZ / 2;  /* max time before a read is submitted. */
static const int write_expire = 5 * HZ; /* ditto for writes, these limits are SOFT! */
static const int writes_starved = 2;    /* max times reads can starve a write */
static const int fifo_batch = 16;       /* # of sequential requests treated as one
				     by the above parameters. For throughput. */

struct deadline_data {
	/*
	 * run time data
	 */

	/*
	 * requests (deadline_rq s) are present on both sort_list and fifo_list
	 */
	struct rb_root sort_list[2];	
	struct list_head fifo_list[2];

	/*
	 * next in sort order. read, write or both are NULL
	 */
	struct request *next_rq[2];
	unsigned int batching;		/* number of sequential requests made */
	unsigned int starved;		/* times reads have starved writes */

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	int fifo_expire[2];
	int fifo_batch;
	int writes_starved;
	int front_merges;
};

static void deadline_move_request(struct deadline_data *, struct request *);

static inline struct rb_root *
deadline_rb_root(struct deadline_data *dd, struct request *rq)
{
	return &dd->sort_list[rq_data_dir(rq)];
}

/*
 * get the request after `rq' in sector-sorted order
 */
static inline struct request *
deadline_latter_request(struct request *rq)
{
	struct rb_node *node = rb_next(&rq->rb_node);

	if (node)
		return rb_entry_rq(node);

	return NULL;
}

static void
deadline_add_rq_rb(struct deadline_data *dd, struct request *rq)
{
	struct rb_root *root = deadline_rb_root(dd, rq);

	elv_rb_add(root, rq);
}

static inline void
deadline_del_rq_rb(struct deadline_data *dd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	if (dd->next_rq[data_dir] == rq)
		dd->next_rq[data_dir] = deadline_latter_request(rq);

	elv_rb_del(deadline_rb_root(dd, rq), rq);
}

/*
 * add rq to rbtree and fifo
 */
/*
***request如何加入到此 iosched***
**分别加入到红黑树与fifo列表中，红黑树以扇区排序,fifo以时间先后取出
**
*/
static void
deadline_add_request(struct request_queue *q, struct request *rq)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const int data_dir = rq_data_dir(rq); //a1）获取数据方向(RW/RD)

	deadline_add_rq_rb(dd, rq); //a2)加入到红黑树中

	/*
	 * set expire time and add to fifo list
	 */
	rq->fifo_time = jiffies + dd->fifo_expire[data_dir];
	list_add_tail(&rq->queuelist, &dd->fifo_list[data_dir]); //a3)记录req的超时时间并加入到fifo列表中
}

/*
 * remove rq from rbtree and fifo.
 */
static void deadline_remove_request(struct request_queue *q, struct request *rq)
{
	struct deadline_data *dd = q->elevator->elevator_data;

	rq_fifo_clear(rq);
	deadline_del_rq_rb(dd, rq);
}

/*
以bio的扇区为key查寻req,若有则表示可能可以向前合并
*/
static enum elv_merge
deadline_merge(struct request_queue *q, struct request **req, struct bio *bio)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	struct request *__rq;

	/*
	 * check for front merge
	 */
	if (dd->front_merges) {
		sector_t sector = bio_end_sector(bio);

		__rq = elv_rb_find(&dd->sort_list[bio_data_dir(bio)], sector);
		if (__rq) {
			BUG_ON(sector != blk_rq_pos(__rq));

			if (elv_bio_merge_ok(__rq, bio)) {
				*req = __rq;
				return ELEVATOR_FRONT_MERGE;
			}
		}
	}

	return ELEVATOR_NO_MERGE;
}

/*
*** 3.1)合并完后是否影响iosched即合并后期处理
*若是向前合并,则扇区大小改变,故需重新加入到rb树中  ???
*/
static void deadline_merged_request(struct request_queue *q,
				    struct request *req, enum elv_merge type)
{
	struct deadline_data *dd = q->elevator->elevator_data;

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (type == ELEVATOR_FRONT_MERGE) {
		elv_rb_del(deadline_rb_root(dd, req), req);
		deadline_add_rq_rb(dd, req);
	}
}

/*
***iosched如何合并request {合并分两种情况：向前合并与向后合并,故如何取出request前后临近的request}
*1)若next expires小于req,将next queue list下的bio移动到req queue list,并将next 的expire time分配给req
*2)若next expires大于req 如何处理呢?
*/
static void
deadline_merged_requests(struct request_queue *q, struct request *req,
			 struct request *next)
{
	/*
	 * if next expires before rq, assign its expire time to rq
	 * and move into next position (next will be deleted) in fifo
	 */
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before((unsigned long)next->fifo_time,
				(unsigned long)req->fifo_time)) {
			list_move(&req->queuelist, &next->queuelist);
			req->fifo_time = next->fifo_time;
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	deadline_remove_request(q, next); //从fifo list与rb tree中移除next
}

/*
 * move request from sort list to dispatch queue.
 */
static inline void
deadline_move_to_dispatch(struct deadline_data *dd, struct request *rq)
{
	struct request_queue *q = rq->q;

	deadline_remove_request(q, rq);
	elv_dispatch_add_tail(q, rq);
}

/*
 * move an entry to dispatch queue
 */
static void
deadline_move_request(struct deadline_data *dd, struct request *rq)
{
	const int data_dir = rq_data_dir(rq);

	dd->next_rq[READ] = NULL;
	dd->next_rq[WRITE] = NULL;
	dd->next_rq[data_dir] = deadline_latter_request(rq); //取下一个req放入到next_rq数组中

	/*
	 * take it off the sort and fifo list, move
	 * to dispatch queue
	 */
	deadline_move_to_dispatch(dd, rq);
}

/*
 * deadline_check_fifo returns 0 if there are no expired requests on the fifo,
 * 1 otherwise. Requires !list_empty(&dd->fifo_list[data_dir])
 */
static inline int deadline_check_fifo(struct deadline_data *dd, int ddir)
{
	struct request *rq = rq_entry_fifo(dd->fifo_list[ddir].next);

	/*
	 * rq is expired!
	 */
	if (time_after_eq(jiffies, (unsigned long)rq->fifo_time))
		return 1;

	return 0;
}

/*
 * deadline_dispatch_requests selects the best request according to
 * read/write expire, fifo_batch, etc
 */
/*
***如何从iosched取出request
**1)优先读操作但次数不能超过writes_starved
**2)写操作并starvd重置
**3)批量操作<读写均可>,但次数不能超过fifo_batch
**Q:dd->next_rq哪里赋值
**A:deadline_move_request-->dd->next_rq[data_dir] = deadline_latter_request(rq)
*/
static int deadline_dispatch_requests(struct request_queue *q, int force)
{
	struct deadline_data *dd = q->elevator->elevator_data;
	const int reads = !list_empty(&dd->fifo_list[READ]);
	const int writes = !list_empty(&dd->fifo_list[WRITE]);
	struct request *rq;
	int data_dir;

	/*
	 * batches are currently reads XOR writes
	 */
	if (dd->next_rq[WRITE])
		rq = dd->next_rq[WRITE];
	else
		rq = dd->next_rq[READ];
	/*
	OP3.批量操作<batching小于fifo_batch>
	*/
	if (rq && dd->batching < dd->fifo_batch)
		/* we have a next request are still entitled to batch */
		goto dispatch_request;

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */
	/*
	OP1.读操作
	1)优先读,每读一次starved加一 <优先读但不能饿死写,故设置了读操作次数/写饥饿次数>
	2)当starved>=writes_starved时,进行写
	*/
	if (reads) {
		BUG_ON(RB_EMPTY_ROOT(&dd->sort_list[READ]));

		if (writes && (dd->starved++ >= dd->writes_starved))
			goto dispatch_writes;

		data_dir = READ;

		goto dispatch_find_request;
	}

	/*
	 * there are either no reads or writes have been starved
	 */
	/*
	OP2.写操作,每次写starved重置
	*/
	if (writes) {
dispatch_writes:
		BUG_ON(RB_EMPTY_ROOT(&dd->sort_list[WRITE]));

		dd->starved = 0;

		data_dir = WRITE;

		goto dispatch_find_request;
	}

	return 0;

	/*
	a3.取request规则,批量操作清0
	1)从fifo队列中取 
	   <同方向有req<fifo list第一个>等待超时>
	   <与上一个请求不在同一数据方向>
	2)继续从同数据方向取下一个
	*/
dispatch_find_request:
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	if (deadline_check_fifo(dd, data_dir) || !dd->next_rq[data_dir]) {
		/*
		 * A deadline has expired, the last request was in the other
		 * direction, or we have run out of higher-sectored requests.
		 * Start again from the request with the earliest expiry time.
		 */
		rq = rq_entry_fifo(dd->fifo_list[data_dir].next);
	} else {
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		rq = dd->next_rq[data_dir];
	}

	dd->batching = 0;

	/*
	a4.将req移动到分发queue,并将下一个req放到同方向next_req中
	*/
dispatch_request:
	/*
	 * rq is the selected appropriate request.
	 */
	dd->batching++;
	deadline_move_request(dd, rq);

	return 1;
}

static void deadline_exit_queue(struct elevator_queue *e)
{
	struct deadline_data *dd = e->elevator_data;

	BUG_ON(!list_empty(&dd->fifo_list[READ]));
	BUG_ON(!list_empty(&dd->fifo_list[WRITE]));

	kfree(dd);
}

/*
 * initialize elevator private data (deadline_data).
 */
static int deadline_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct deadline_data *dd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	dd = kzalloc_node(sizeof(*dd), GFP_KERNEL, q->node);
	if (!dd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = dd;

	INIT_LIST_HEAD(&dd->fifo_list[READ]);
	INIT_LIST_HEAD(&dd->fifo_list[WRITE]);
	dd->sort_list[READ] = RB_ROOT;
	dd->sort_list[WRITE] = RB_ROOT;
	dd->fifo_expire[READ] = read_expire;
	dd->fifo_expire[WRITE] = write_expire;
	dd->writes_starved = writes_starved;
	dd->front_merges = 1;
	dd->fifo_batch = fifo_batch;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

/*
 * sysfs parts below
 */

static ssize_t
deadline_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static void
deadline_var_store(int *var, const char *page)
{
	char *p = (char *) page;

	*var = simple_strtol(p, &p, 10);
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data = __VAR;						\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return deadline_var_show(__data, (page));			\
}
SHOW_FUNCTION(deadline_read_expire_show, dd->fifo_expire[READ], 1);
SHOW_FUNCTION(deadline_write_expire_show, dd->fifo_expire[WRITE], 1);
SHOW_FUNCTION(deadline_writes_starved_show, dd->writes_starved, 0);
SHOW_FUNCTION(deadline_front_merges_show, dd->front_merges, 0);
SHOW_FUNCTION(deadline_fifo_batch_show, dd->fifo_batch, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct deadline_data *dd = e->elevator_data;			\
	int __data;							\
	deadline_var_store(&__data, (page));				\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	if (__CONV)							\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else								\
		*(__PTR) = __data;					\
	return count;							\
}
STORE_FUNCTION(deadline_read_expire_store, &dd->fifo_expire[READ], 0, INT_MAX, 1);
STORE_FUNCTION(deadline_write_expire_store, &dd->fifo_expire[WRITE], 0, INT_MAX, 1);
STORE_FUNCTION(deadline_writes_starved_store, &dd->writes_starved, INT_MIN, INT_MAX, 0);
STORE_FUNCTION(deadline_front_merges_store, &dd->front_merges, 0, 1, 0);
STORE_FUNCTION(deadline_fifo_batch_store, &dd->fifo_batch, 0, INT_MAX, 0);
#undef STORE_FUNCTION

#define DD_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, deadline_##name##_show, \
				      deadline_##name##_store)

static struct elv_fs_entry deadline_attrs[] = {
	DD_ATTR(read_expire),
	DD_ATTR(write_expire),
	DD_ATTR(writes_starved),
	DD_ATTR(front_merges),
	DD_ATTR(fifo_batch),
	__ATTR_NULL
};

static struct elevator_type iosched_deadline = {
	.ops.sq = {
		.elevator_merge_fn = 		deadline_merge,
		.elevator_merged_fn =		deadline_merged_request,
		.elevator_merge_req_fn =	deadline_merged_requests,
		.elevator_dispatch_fn =		deadline_dispatch_requests,
		.elevator_add_req_fn =		deadline_add_request,
		.elevator_former_req_fn =	elv_rb_former_request,
		.elevator_latter_req_fn =	elv_rb_latter_request,
		.elevator_init_fn =		deadline_init_queue,
		.elevator_exit_fn =		deadline_exit_queue,
	},

	.elevator_attrs = deadline_attrs,
	.elevator_name = "deadline",
	.elevator_owner = THIS_MODULE,
};

static int __init deadline_init(void)
{
	return elv_register(&iosched_deadline);
}

static void __exit deadline_exit(void)
{
	elv_unregister(&iosched_deadline);
}

module_init(deadline_init);
module_exit(deadline_exit);

MODULE_AUTHOR("Jens Axboe");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("deadline IO scheduler");

/*
*** iosched实现需要从以下几个方面考虑:
*** 1)request如何加入到此 iosched
*** 2)如何从iosched取出request
*** 3)iosched如何合并request {合并分两种情况：向前合并与向后合并,故如何取出request前后临近的request}
*** 3.1)合并完后是否影响iosched即合并后期处理
*** 4)init 与 exit 操作
*/
