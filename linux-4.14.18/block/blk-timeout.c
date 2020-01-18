/*
 * Functions related to generic timeout handling of requests.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/fault-inject.h>

#include "blk.h"
#include "blk-mq.h"

#ifdef CONFIG_FAIL_IO_TIMEOUT

static DECLARE_FAULT_ATTR(fail_io_timeout);

static int __init setup_fail_io_timeout(char *str)
{
	return setup_fault_attr(&fail_io_timeout, str);
}
__setup("fail_io_timeout=", setup_fail_io_timeout);

int blk_should_fake_timeout(struct request_queue *q)
{
	if (!test_bit(QUEUE_FLAG_FAIL_IO, &q->queue_flags))
		return 0;

	return should_fail(&fail_io_timeout, 1);
}

static int __init fail_io_timeout_debugfs(void)
{
	struct dentry *dir = fault_create_debugfs_attr("fail_io_timeout",
						NULL, &fail_io_timeout);

	return PTR_ERR_OR_ZERO(dir);
}

late_initcall(fail_io_timeout_debugfs);

ssize_t part_timeout_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct gendisk *disk = dev_to_disk(dev);
	int set = test_bit(QUEUE_FLAG_FAIL_IO, &disk->queue->queue_flags);

	return sprintf(buf, "%d\n", set != 0);
}

ssize_t part_timeout_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct gendisk *disk = dev_to_disk(dev);
	int val;

	if (count) {
		struct request_queue *q = disk->queue;
		char *p = (char *) buf;

		val = simple_strtoul(p, &p, 10);
		spin_lock_irq(q->queue_lock);
		if (val)
			queue_flag_set(QUEUE_FLAG_FAIL_IO, q);
		else
			queue_flag_clear(QUEUE_FLAG_FAIL_IO, q);
		spin_unlock_irq(q->queue_lock);
	}

	return count;
}

#endif /* CONFIG_FAIL_IO_TIMEOUT */

/*
 * blk_delete_timer - Delete/cancel timer for a given function.
 * @req:	request that we are canceling timer for
 *
 */
void blk_delete_timer(struct request *req)
{
	list_del_init(&req->timeout_list);
}

static void blk_rq_timed_out(struct request *req)
{
	struct request_queue *q = req->q;
	enum blk_eh_timer_return ret = BLK_EH_RESET_TIMER;

	if (q->rq_timed_out_fn)
		ret = q->rq_timed_out_fn(req);
	switch (ret) {
	case BLK_EH_HANDLED: //通知已超时?
		__blk_complete_request(req);
		break;
	case BLK_EH_RESET_TIMER: //再给一次机会?
		blk_add_timer(req);
		blk_clear_rq_complete(req);
		break;
	case BLK_EH_NOT_HANDLED: //不处理
		/*
		 * LLD handles this for now but in the future
		 * we can send a request msg to abort the command
		 * and we can move more of the generic scsi eh code to
		 * the blk layer.
		 */
		break;
	default:
		printk(KERN_ERR "block: bad eh return: %d\n", ret);
		break;
	}
}

static void blk_rq_check_expired(struct request *rq, unsigned long *next_timeout,
			  unsigned int *next_set)
{
	if (time_after_eq(jiffies, rq->deadline)) { //到期的timer 移除
		list_del_init(&rq->timeout_list);

		/*
		 * Check if we raced with end io completion
		 */
		if (!blk_mark_rq_complete(rq)) //到期未完成
			blk_rq_timed_out(rq);
	} else if (!*next_set || time_after(*next_timeout, rq->deadline)) { //获取下一个将要到期的timer
		*next_timeout = rq->deadline;
		*next_set = 1;
	}
}

void blk_timeout_work(struct work_struct *work)
{
	struct request_queue *q =
		container_of(work, struct request_queue, timeout_work);
	unsigned long flags, next = 0;
	struct request *rq, *tmp;
	int next_set = 0;

	spin_lock_irqsave(q->queue_lock, flags);

	/*a1.处理到期的timer与获取下一个最早到期的timer*/
	list_for_each_entry_safe(rq, tmp, &q->timeout_list, timeout_list)
		blk_rq_check_expired(rq, &next, &next_set);

	/*a2.修改下一个要到期的timer*/
	if (next_set)
		mod_timer(&q->timeout, round_jiffies_up(next));

	spin_unlock_irqrestore(q->queue_lock, flags);
}

/**
 * blk_abort_request -- Request request recovery for the specified command
 * @req:	pointer to the request of interest
 *
 * This function requests that the block layer start recovery for the
 * request by deleting the timer and calling the q's timeout function.
 * LLDDs who implement their own error recovery MAY ignore the timeout
 * event if they generated blk_abort_req. Must hold queue lock.
 */
void blk_abort_request(struct request *req)
{
	if (blk_mark_rq_complete(req))
		return;

	if (req->q->mq_ops) {
		blk_mq_rq_timed_out(req, false);
	} else {
		blk_delete_timer(req);
		blk_rq_timed_out(req);
	}
}
EXPORT_SYMBOL_GPL(blk_abort_request);

unsigned long blk_rq_timeout(unsigned long timeout)
{
	unsigned long maxt;

	maxt = round_jiffies_up(jiffies + BLK_MAX_TIMEOUT);
	if (time_after(timeout, maxt))
		timeout = maxt;

	return timeout;
}

/**
 * blk_add_timer - Start timeout timer for a single request
 * @req:	request that is about to start running.
 *
 * Notes:
 *    Each request has its own timer, and as it is added to the queue, we
 *    set up the timer. When the request completes, we cancel the timer.
 */
 /*
 关键点:
 1.多队列自已处理timer
 2.单队列处理注意事项
  1)rq timeout为0,采取queue 的rq_timeout
  2)rq timeout不应超过最大BLK_MAX_TIMEOUT(5*HZ),超过也没有用
  3)要加入的timer比下一个到期的timer早HZ/2才有机会优先处理
 */
void blk_add_timer(struct request *req)
{
	struct request_queue *q = req->q;
	unsigned long expiry;

	if (!q->mq_ops)
		lockdep_assert_held(q->queue_lock);

	/* blk-mq has its own handler, so we don't need ->rq_timed_out_fn */
	if (!q->mq_ops && !q->rq_timed_out_fn)
		return;

	BUG_ON(!list_empty(&req->timeout_list));

	/*
	 * Some LLDs, like scsi, peek at the timeout to prevent a
	 * command from being retried forever.
	 */
	if (!req->timeout) //若无超时时间则以queue rq_timeout的为准
		req->timeout = q->rq_timeout;

	/*a1.计算截止超时时间*/
	req->deadline = jiffies + req->timeout;

	/*
	 * Only the non-mq case needs to add the request to a protected list.
	 * For the mq case we simply scan the tag map.
	 */
	/*a2.单队列将req挂入到queue的timeout_list*/
	if (!q->mq_ops)
		list_add_tail(&req->timeout_list, &req->q->timeout_list);

	/*
	 * If the timer isn't already pending or this timeout is earlier
	 * than an existing one, modify the timer. Round up to next nearest
	 * second.
	 */
	/*
	a3.deadline不应超过最大时延BLK_MAX_TIMEOUT
	即req->timeout 即使大于BLK_MAX_TIMEOUT也没有作用.
	*/
	expiry = blk_rq_timeout(round_jiffies_up(req->deadline));

	/*
	a4.比较下一个要到期的timer与此时加入的timer谁更早
	若此时的timer早于下一个要到期的timer HZ/2则优先处理此时的timer
	*/
	if (!timer_pending(&q->timeout) ||
	    time_before(expiry, q->timeout.expires)) {
		unsigned long diff = q->timeout.expires - expiry;

		/*
		 * Due to added timer slack to group timers, the timer
		 * will often be a little in front of what we asked for.
		 * So apply some tolerance here too, otherwise we keep
		 * modifying the timer because expires for value X
		 * will be X + something.
		 */
		if (!timer_pending(&q->timeout) || (diff >= HZ / 2))
			mod_timer(&q->timeout, expiry);
	}

}


/*
block timer几大问题:
1.需要提供给api有哪些
1)如何add/del timer
	blk_add_timer blk_delete_timer
2)如何检测timer到时
	blk_timeout_work
3)如何主动abort timer
	blk_abort_request
2.timer的几大时机
1)add timer
blk_start_request driver开始处理reqeust的时候 add_timer
2)del timer
blk_finish_request 完成请求时
blk_requeue_request req重新入队列
3)什么情况下需要abort timer

*/
