// SPDX-License-Identifier: GPL-2.0
/*
 * fs/mpage.c
 *
 * Copyright (C) 2002, Linus Torvalds.
 *
 * Contains functions related to preparing and submitting BIOs which contain
 * multiple pagecache pages.
 *
 * 15May2002	Andrew Morton
 *		Initial version
 * 27Jun2002	axboe@suse.de
 *		use bio_add_page() to build bio's just the right size
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/kdev_t.h>
#include <linux/gfp.h>
#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/prefetch.h>
#include <linux/mpage.h>
#include <linux/mm_inline.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/pagevec.h>
#include <linux/cleancache.h>
#include "internal.h"

/*
 * I/O completion handler for multipage BIOs.
 *
 * The mpage code never puts partial pages into a BIO (except for end-of-file).
 * If a page does not map to a contiguous run of blocks then it simply falls
 * back to block_read_full_page().
 *
 * Why is this?  If a page's completion depends on a number of different BIOs
 * which can complete in any order (or at the same time) then determining the
 * status of that page is hard.  See end_buffer_async_read() for the details.
 * There is no point in duplicating all that complexity.
 */
static void mpage_end_io(struct bio *bio)
{
	struct bio_vec *bv;
	int i;

	bio_for_each_segment_all(bv, bio, i) {
		struct page *page = bv->bv_page;
		page_endio(page, op_is_write(bio_op(bio)),
				blk_status_to_errno(bio->bi_status));
	}

	bio_put(bio);
}

static struct bio *mpage_bio_submit(int op, int op_flags, struct bio *bio)
{
	bio->bi_end_io = mpage_end_io;
	bio_set_op_attrs(bio, op, op_flags);
	guard_bio_eod(op, bio);
	submit_bio(bio);
	return NULL;
}

static struct bio *
mpage_alloc(struct block_device *bdev,
		sector_t first_sector, int nr_vecs,
		gfp_t gfp_flags)
{
	struct bio *bio;

	/* Restrict the given (page cache) mask for slab allocations */
	gfp_flags &= GFP_KERNEL;
	bio = bio_alloc(gfp_flags, nr_vecs);

	if (bio == NULL && (current->flags & PF_MEMALLOC)) {
		while (!bio && (nr_vecs /= 2))
			bio = bio_alloc(gfp_flags, nr_vecs);
	}

	if (bio) {
		bio_set_dev(bio, bdev);
		bio->bi_iter.bi_sector = first_sector;
	}
	return bio;
}

/*
 * support function for mpage_readpages.  The fs supplied get_block might
 * return an up to date buffer.  This is used to map that buffer into
 * the page, which allows readpage to avoid triggering a duplicate call
 * to get_block.
 *
 * The idea is to avoid adding buffers to pages that don't already have
 * them.  So when the buffer is up to date and the page size == block size,
 * this marks the page up to date instead of adding new buffers.
 */
static void 
map_buffer_to_page(struct page *page, struct buffer_head *bh, int page_block) 
{
	struct inode *inode = page->mapping->host;
	struct buffer_head *page_bh, *head;
	int block = 0;

    /*a1.page无buffers则创建*/
	if (!page_has_buffers(page)) {
		/*
		 * don't make any buffers if there is only one buffer on
		 * the page and the page just needs to be set up to date
		 */
        /*
        b1.block size与page size且buffer跟磁盘同步,则无需创建buffers,标记page Uptodate即可 
        这种情况什么时候发生???
        */
		if (inode->i_blkbits == PAGE_SHIFT &&
		    buffer_uptodate(bh)) {
			SetPageUptodate(page);    
			return;
		}
        /*b2.创建buffers*/
		create_empty_buffers(page, i_blocksize(inode), 0);
	}
    /*a2.设置buffer_head*/
	head = page_buffers(page);
	page_bh = head;
	do {
		if (block == page_block) {
			page_bh->b_state = bh->b_state;
			page_bh->b_bdev = bh->b_bdev;
			page_bh->b_blocknr = bh->b_blocknr;
			break;
		}
		page_bh = page_bh->b_this_page;
		block++;
	} while (page_bh != head);
}

/*
 * This is the worker routine which does all the work of mapping the disk
 * blocks and constructs largest possible bios, submits them for IO if the
 * blocks are not contiguous on the disk.
 *
 * We pass a buffer_head back and forth and use its buffer_mapped() flag to
 * represent the validity of its disk mapping and to decide when to do the next
 * get_block() call.
 */
static struct bio *
do_mpage_readpage(struct bio *bio, struct page *page, unsigned nr_pages,
		sector_t *last_block_in_bio, struct buffer_head *map_bh,
		unsigned long *first_logical_block, get_block_t get_block,
		gfp_t gfp)
{
	struct inode *inode = page->mapping->host; //得文件在磁盘中索引节点 <一个文件对应一个inode>
	const unsigned blkbits = inode->i_blkbits; //一个block大小所占位数
	const unsigned blocks_per_page = PAGE_SIZE >> blkbits; //计算一页中有多少个block
	const unsigned blocksize = 1 << blkbits;
	sector_t block_in_file;
	sector_t last_block;
	sector_t last_block_in_file;
	sector_t blocks[MAX_BUF_PER_PAGE];
	unsigned page_block;
	unsigned first_hole = blocks_per_page;
	struct block_device *bdev = NULL;
	int length;
	int fully_mapped = 1;
	unsigned nblocks;
	unsigned relative_block;

	if (page_has_buffers(page)) //page中已有对应有buffer_head,说明已经映射到磁盘空间,无需再执行映射动作
		goto confused;
	
    /*
    块是文件系统的抽象,不是磁盘本身的属性.扇区大小则是磁盘的物理属性,是磁盘设备寻址的最小单元 
    block size是sector size的倍数. 
    一个个block size读比一个个sector size读写消耗更少磁盘IO访问次数,减少访问时间,从而提升IO操作性能. 
    block太大，存放小文件就会造成空间浪费；block太小，又会消耗磁盘IO.
    */
	/*
    将页号转换成文件的块号(毕竟文件系统中以block为单位进行操作,硬盘以扇区为单位进行操作)
	如file size:2M  page size:4K(PAGE_SHIFT 12)  block size:1024byte(blkbits 10)
	则有page总数2M/4k=512 block总数有2M/1024=2048,
    设页序号为5(index),则有block序号5×4K/1024=20 即 index * (1<<PAGE_SHIFT)/(1<<blkbits)=index*1<<(PAGE_SHIFT-blkbits) 
    */ 
	block_in_file = (sector_t)page->index << (PAGE_SHIFT - blkbits);
	last_block = block_in_file + nr_pages * blocks_per_page;  //最后一个block序号
	last_block_in_file = (i_size_read(inode) + blocksize - 1) >> blkbits; //文件中最大block序号
	if (last_block > last_block_in_file)//请求的数据不能超过文件的大小（以block序号来进行判断）
		last_block = last_block_in_file;
	page_block = 0;

	/*
	 * Map blocks using the result from the previous get_blocks call first.
	 */
	nblocks = map_bh->b_size >> blkbits;
	/*
	***若map_bh已经存有查询结果且逻辑块在[*first_logical_block,*first_logical_block+nblocks]之间直接使用上一次的查询结果
	*/
	if (buffer_mapped(map_bh) && block_in_file > *first_logical_block &&
			block_in_file < (*first_logical_block + nblocks)) {
		unsigned map_offset = block_in_file - *first_logical_block;
		unsigned last = nblocks - map_offset;

		for (relative_block = 0; ; relative_block++) {
			if (relative_block == last) {
				clear_buffer_mapped(map_bh);
				break;
			}
			if (page_block == blocks_per_page)
				break;
			blocks[page_block] = map_bh->b_blocknr + map_offset +
						relative_block;
			page_block++;
			block_in_file++;
		}
		bdev = map_bh->b_bdev;
	}

	/*
	 * Then do more get_blocks calls until we are done with this page.
	 */
	map_bh->b_page = page;
	while (page_block < blocks_per_page) {
		map_bh->b_state = 0;
		map_bh->b_size = 0;

		if (block_in_file < last_block) {
			map_bh->b_size = (last_block-block_in_file) << blkbits;
			/*get_block(ext2_get_block):
			***=0 没有找到对应的物理块(文件空洞)
			***=0 [block_in_file,block_in_file+ret-1]为连续块 
			***<0 出错<eg: EIO>
			**** 
			***ext2_get_blocks:
			*return > 0, # of blocks mapped or allocated.
 			* return = 0, if plain lookup failed.
 			* return < 0, error case.
			*/
			if (get_block(inode, block_in_file, map_bh, 0)) //计算文件逻辑块号[block_in_file,last_block]块在硬盘上的位置(即对应的物理块号)是否连续
				goto confused;
			*first_logical_block = block_in_file;
		}
		
		/*文件空洞*/
		if (!buffer_mapped(map_bh)) { 
			fully_mapped = 0;
			if (first_hole == blocks_per_page)
				first_hole = page_block;  /*记录首次查到的文件空洞逻辑号*/
			page_block++;
			block_in_file++;
			continue;
		}

		/* some filesystems will copy data into the page during
		 * the get_block call, in which case we don't want to
		 * read it again.  map_buffer_to_page copies the data
		 * we just collected from get_block into the page's buffers
		 * so readpage doesn't have to repeat the get_block call
		 */
		if (buffer_uptodate(map_bh)) {
			map_buffer_to_page(page, map_bh, page_block);
			goto confused;
		}
	
		if (first_hole != blocks_per_page)
			goto confused;		/* hole -> non-hole */

		/* Contiguous blocks? */
		if (page_block && blocks[page_block-1] != map_bh->b_blocknr-1) /*前后两个查询的物理块号是否连续*/
			goto confused;
		/*
		***将查询到的物理块号存储到blocks数据中(map_bh->b_blocknr,map_bh->b_blocknr+nblocks)
		*/
		nblocks = map_bh->b_size >> blkbits;
		for (relative_block = 0; ; relative_block++) {
			if (relative_block == nblocks) {
				clear_buffer_mapped(map_bh);
				break;
			} else if (page_block == blocks_per_page)
				break;
			blocks[page_block] = map_bh->b_blocknr+relative_block;
			page_block++;
			block_in_file++;
		}
		bdev = map_bh->b_bdev;
	}
	
	/*
	***a.页内块有空洞将空洞内容清0
	***b.页内无空洞，表明最新内容在disk.
	*/
	if (first_hole != blocks_per_page) {
		zero_user_segment(page, first_hole << blkbits, PAGE_SIZE);
		if (first_hole == 0) {
			SetPageUptodate(page);
			unlock_page(page);
			goto out;
		}
	} else if (fully_mapped) {
		SetPageMappedToDisk(page);
	}

	if (fully_mapped && blocks_per_page == 1 && !PageUptodate(page) &&
	    cleancache_get_page(page) == 0) {
		SetPageUptodate(page);
		goto confused;
	}

	/*
	 * This page will go to BIO.  Do we need to send this BIO off first?
	 */
	if (bio && (*last_block_in_bio != blocks[0] - 1))
		bio = mpage_bio_submit(REQ_OP_READ, 0, bio);

alloc_new:
	if (bio == NULL) {
		if (first_hole == blocks_per_page) {
			if (!bdev_read_page(bdev, blocks[0] << (blkbits - 9),
								page))
				goto out;
		}
		bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9),
				min_t(int, nr_pages, BIO_MAX_PAGES), gfp);
		if (bio == NULL)
			goto confused;
	}
	/*
	***将page加入bio
	*/
	length = first_hole << blkbits;
	if (bio_add_page(bio, page, length, 0) < length) {
		bio = mpage_bio_submit(REQ_OP_READ, 0, bio);
		goto alloc_new;
	}

	relative_block = block_in_file - *first_logical_block;
	nblocks = map_bh->b_size >> blkbits;
	/*
	***页内连续块有空洞或有处于边界的物理块提交bio,否则记录最后物理块号尝试与相邻的下一页数据块一起提交bio
	*/
	if ((buffer_boundary(map_bh) && relative_block == nblocks) ||
	    (first_hole != blocks_per_page))
		bio = mpage_bio_submit(REQ_OP_READ, 0, bio);
	else
		*last_block_in_bio = blocks[blocks_per_page - 1];
out:
	return bio;

confused:
	if (bio)
		bio = mpage_bio_submit(REQ_OP_READ, 0, bio);
	if (!PageUptodate(page))
	        block_read_full_page(page, get_block);
	else
		unlock_page(page);
	goto out;
}

/**
 * mpage_readpages - populate an address space with some pages & start reads against them
 * @mapping: the address_space
 * @pages: The address of a list_head which contains the target pages.  These
 *   pages have their ->index populated and are otherwise uninitialised.
 *   The page at @pages->prev has the lowest file offset, and reads should be
 *   issued in @pages->prev to @pages->next order.
 * @nr_pages: The number of pages at *@pages
 * @get_block: The filesystem's block mapper function.
 *
 * This function walks the pages and the blocks within each page, building and
 * emitting large BIOs.
 *
 * If anything unusual happens, such as:
 *
 * - encountering a page which has buffers
 * - encountering a page which has a non-hole after a hole
 * - encountering a page with non-contiguous blocks
 *
 * then this code just gives up and calls the buffer_head-based read function.
 * It does handle a page which has holes at the end - that is a common case:
 * the end-of-file on blocksize < PAGE_SIZE setups.
 *
 * BH_Boundary explanation:
 *
 * There is a problem.  The mpage read code assembles several pages, gets all
 * their disk mappings, and then submits them all.  That's fine, but obtaining
 * the disk mappings may require I/O.  Reads of indirect blocks, for example.
 *
 * So an mpage read of the first 16 blocks of an ext2 file will cause I/O to be
 * submitted in the following order:
 *
 * 	12 0 1 2 3 4 5 6 7 8 9 10 11 13 14 15 16
 *
 * because the indirect block has to be read to get the mappings of blocks
 * 13,14,15,16.  Obviously, this impacts performance.
 *
 * So what we do it to allow the filesystem's get_block() function to set
 * BH_Boundary when it maps block 11.  BH_Boundary says: mapping of the block
 * after this one will require I/O against a block which is probably close to
 * this one.  So you should push what I/O you have currently accumulated.
 *
 * This all causes the disk requests to be issued in the correct order.
 */
int
mpage_readpages(struct address_space *mapping, struct list_head *pages,
				unsigned nr_pages, get_block_t get_block)
{
	struct bio *bio = NULL;
	unsigned page_idx;
	sector_t last_block_in_bio = 0;
	struct buffer_head map_bh;
	unsigned long first_logical_block = 0;
	gfp_t gfp = readahead_gfp_mask(mapping);

	map_bh.b_state = 0;
	map_bh.b_size = 0;
    /*一页一页的处理*/
	for (page_idx = 0; page_idx < nr_pages; page_idx++) {
		struct page *page = lru_to_page(pages);

		prefetchw(&page->flags);
        /*a1.从lru列表中移除*/
		list_del(&page->lru);
        /*a2.page加入到page cache*/
		if (!add_to_page_cache_lru(page, mapping,
					page->index,
					gfp)) {
            /*a3.映射到磁盘空间并构建bio*/
			bio = do_mpage_readpage(bio, page,
					nr_pages - page_idx,
					&last_block_in_bio, &map_bh,
					&first_logical_block,
					get_block, gfp);
		}
		put_page(page);
	}
	BUG_ON(!list_empty(pages));
	if (bio)
		mpage_bio_submit(REQ_OP_READ, 0, bio);
	return 0;
}
EXPORT_SYMBOL(mpage_readpages);

/*
 * This isn't called much at all
 */
int mpage_readpage(struct page *page, get_block_t get_block)
{
	struct bio *bio = NULL;
	sector_t last_block_in_bio = 0;
	struct buffer_head map_bh;
	unsigned long first_logical_block = 0;
	gfp_t gfp = mapping_gfp_constraint(page->mapping, GFP_KERNEL);

	map_bh.b_state = 0;
	map_bh.b_size = 0;
	bio = do_mpage_readpage(bio, page, 1, &last_block_in_bio,
			&map_bh, &first_logical_block, get_block, gfp);
	if (bio)
		mpage_bio_submit(REQ_OP_READ, 0, bio);
	return 0;
}
EXPORT_SYMBOL(mpage_readpage);

/*
 * Writing is not so simple.
 *
 * If the page has buffers then they will be used for obtaining the disk
 * mapping.  We only support pages which are fully mapped-and-dirty, with a
 * special case for pages which are unmapped at the end: end-of-file.
 *
 * If the page has no buffers (preferred) then the page is mapped here.
 *
 * If all blocks are found to be contiguous then the page can go into the
 * BIO.  Otherwise fall back to the mapping's writepage().
 * 
 * FIXME: This code wants an estimate of how many pages are still to be
 * written, so it can intelligently allocate a suitably-sized BIO.  For now,
 * just allocate full-size (16-page) BIOs.
 */

struct mpage_data {
	struct bio *bio;
	sector_t last_block_in_bio;
	get_block_t *get_block;
	unsigned use_writepage;
};

/*
 * We have our BIO, so we can now mark the buffers clean.  Make
 * sure to only clean buffers which we know we'll be writing.
 */
static void clean_buffers(struct page *page, unsigned first_unmapped)
{
	unsigned buffer_counter = 0;
	struct buffer_head *bh, *head;
	if (!page_has_buffers(page))
		return;
	head = page_buffers(page);
	bh = head;

	do {
		if (buffer_counter++ == first_unmapped)
			break;
		clear_buffer_dirty(bh);
		bh = bh->b_this_page;
	} while (bh != head);

	/*
	 * we cannot drop the bh if the page is not uptodate or a concurrent
	 * readpage would fail to serialize with the bh and it would read from
	 * disk before we reach the platter.
	 */
	if (buffer_heads_over_limit && PageUptodate(page))
		try_to_free_buffers(page);
}

/*
 * For situations where we want to clean all buffers attached to a page.
 * We don't need to calculate how many buffers are attached to the page,
 * we just need to specify a number larger than the maximum number of buffers.
 */
void clean_page_buffers(struct page *page)
{
	clean_buffers(page, ~0U);
}
/*
_mpage_writepage函数是写文件的核心接口。代码大致流程如下：                                                                                                                                                                                                                                                                     。
如果page有buffer_head，则完成磁盘映射，代码只支持所有page都被设为脏页的写，除非没有设为脏页的page放到文件的尾部，                                                                                                                                                                                      。
即要求page设置脏页的连续性                                                                                                                                                                                                                                                                                                               。
如果page没有buffer_head，在接口中所有page被设为脏页。如果所有的block都是连续的则直接进入bio请求流程，否则重新回到writepage的映射流程。
用page_has_buffers判断当前page是否有buffer_head(bh)，如果有则用page_buffers将当前page转换为buffer_head的bh指针，                                                                                                                                                                                                            。
之后用bh->b_this_page遍历当前page的所有bh，调用buffer_locked(bh)加锁buffer——head，即使出现一个bh没有被映射都会进入confused流程，                                                                                                                                                                              。
first_unmapped记录了第一个没有映射的bh，除了要保证所有的bh都被映射，还要保证所有的bh都被置为脏页并且完成了uptodate                                                                                                                                                                                       。
如果每个page的block数不为0(通过判断first_unmapped是否非0)，则直接进入当前page已经被映射的流程page_is_mapped，否则进入confused流程。
如果当前page没有buffer_head(bh)，需要将当前page映射到磁盘上，使用buffer_head变量map_bh封装，做buffer_head和bio之间的转换。
page_is_mapped流程中如果有bio资源并且检测到当前的页面和前面一个页面的磁盘块号不连续(代码对应bio && mpd->last_block_in_bio != blocks[0] – 1，blocks[0]表示第一个磁盘块)，                                                                                                                           。
则用mpage_bio_submit来提交一个积累bio请求，将之前的连续block写到设备中。否则进入alloc_new流程。
alloc_new流程中，判断bio为空(表示前面刚刚提交了一个bio)则需要用mpage_alloc重新申请一个bio资源，之后用bio_add_page向bio中添加当前page，                                                                                                                                                                    。
如果bio中的长度不能容纳下这次添加page的整个长度，则先将添加到bio上的数据提交bio请求mpage_bio_submit，剩下的数据重新进入到alloc_new流程做bio的申请操作                                                                                                                                         。
如果一次性将page中的所有数据全部添加到bio上，在page有buffer的情况下要将所有的buffer全部清除脏页位。用set_page_writeback设置该page为写回状态，                                                                                                                                                       。
给page解锁(unlock_page)。当bh的boundary被设置或者当前页面和前面一个页面的磁盘块号不连续，就先提交一个累积连续block的bio                                                                                                                                                                                   。
否则说明当前page中的所有block都是连续的，并且与之前的page中block也是连续的，这种情况下不需要提交bio，                                                                                                                                                                                                       。
只更新前面一个页面的磁盘块号mpd->last_block_in_bio为当前page的最后一个block号，之后退出进行下一个page的连续性检查，直到碰到不连续的再做bio提交。
confused流程中会提交bio操作，但是会设置映射错误。
*/
static int __mpage_writepage(struct page *page, struct writeback_control *wbc,
		      void *data)
{
	struct mpage_data *mpd = data;
	struct bio *bio = mpd->bio;
	struct address_space *mapping = page->mapping;
	struct inode *inode = page->mapping->host;
	const unsigned blkbits = inode->i_blkbits;
	unsigned long end_index;
	const unsigned blocks_per_page = PAGE_SIZE >> blkbits;
	sector_t last_block;
	sector_t block_in_file;
	sector_t blocks[MAX_BUF_PER_PAGE];
	unsigned page_block;
	unsigned first_unmapped = blocks_per_page;
	struct block_device *bdev = NULL;
	int boundary = 0;
	sector_t boundary_block = 0;
	struct block_device *boundary_bdev = NULL;
	int length;
	struct buffer_head map_bh;
	loff_t i_size = i_size_read(inode);
	int ret = 0;
	int op_flags = wbc_to_write_flags(wbc);

    /* 
    a1.page 有buffer_head的情况处理 
    1)page中的脏block连续则一起处理,否则分开处理 
    2)page中脏block不连续的情况有: 
        i: 上一个block unmapped接着的下一个block mapped
        ii: 前面的脏block,当前block的不为脏 
        iii: 上一个block的扇区序号与接着的下一个block扇区序号不连续
    */
	if (page_has_buffers(page)) {
		struct buffer_head *head = page_buffers(page);
		struct buffer_head *bh = head;

		/* If they're all mapped and dirty, do it */
		page_block = 0;
		do {
			BUG_ON(buffer_locked(bh));
			if (!buffer_mapped(bh)) {
				/*
				 * unmapped dirty buffers are created by
				 * __set_page_dirty_buffers -> mmapped data
				 */
				if (buffer_dirty(bh)) //数据未mapped但已修改
					goto confused;
				if (first_unmapped == blocks_per_page) //记录第一个未unmapped的block
					first_unmapped = page_block;
				continue;
			}

			if (first_unmapped != blocks_per_page) //脏页不连续,分开处理
				goto confused;	/* hole -> non-hole */

			if (!buffer_dirty(bh) || !buffer_uptodate(bh)) //page中有block数据未修改或者未对这块进行操作
				goto confused;
			if (page_block) { //前后两个block扇区序号不连续,分开提交处理
				if (bh->b_blocknr != blocks[page_block-1] + 1)
					goto confused;
			}
			blocks[page_block++] = bh->b_blocknr; //序号存入blocks数组
			boundary = buffer_boundary(bh);//ext2 直接寻block<最后一个为boundary值> 一级指针寻block 二级指针寻block
			if (boundary) {
				boundary_block = bh->b_blocknr;
				boundary_bdev = bh->b_bdev;
			}
			bdev = bh->b_bdev;
		} while ((bh = bh->b_this_page) != head);

		if (first_unmapped) //所有的block连续,一并处理
			goto page_is_mapped;

		/*
		 * Page has buffers, but they are all unmapped. The page was
		 * created by pagein or read over a hole which was handled by
		 * block_read_full_page().  If this address_space is also
		 * using mpage_readpages then this can rarely happen.
		 */
		goto confused; //所有的block都unmapped <first_unmapped=0,blocks中存有脏block扇区号时page_block自增>
	}

	/*
	 * The page has no buffers: map it to disk
	 */
    /*
    a2.page未有buffer_head,映射到磁盘 
    不连续的情况(has buffer): 
    i对应mpd->get_block不成功 
    iii一样 
    ii未存在<????> 
    */
	BUG_ON(!PageUptodate(page));
	block_in_file = (sector_t)page->index << (PAGE_SHIFT - blkbits); //在文件中的逻辑序号
	last_block = (i_size - 1) >> blkbits; //文件的最后一个逻辑序号<当前最大的逻辑序号>
	map_bh.b_page = page;
	for (page_block = 0; page_block < blocks_per_page; ) {

		map_bh.b_state = 0;
		map_bh.b_size = 1 << blkbits;
		if (mpd->get_block(inode, block_in_file, &map_bh, 1)) //获取物理扇区号
			goto confused;
		if (buffer_new(&map_bh))
			clean_bdev_bh_alias(&map_bh); //????
		if (buffer_boundary(&map_bh)) { //标记边界
			boundary_block = map_bh.b_blocknr;
			boundary_bdev = map_bh.b_bdev;
		}
		if (page_block) { 
			if (map_bh.b_blocknr != blocks[page_block-1] + 1) //前后两个block(逻辑序号连续)物理扇区序号不一样
				goto confused;
		}
		blocks[page_block++] = map_bh.b_blocknr;
		boundary = buffer_boundary(&map_bh);
		bdev = map_bh.b_bdev;
		if (block_in_file == last_block) //不超过文件的大小
			break;
		block_in_file++;
	}
	BUG_ON(page_block == 0);

	first_unmapped = page_block;

page_is_mapped:
	end_index = i_size >> PAGE_SHIFT;
    /*
    页面跨越i_size.它必须在每个写入页调用时归零，因为它可能被覆盖. 
    "文件以页面大小的倍数映射. 对于不是页面大小的倍数的文件，在映射时将剩余内存归零，并且写入该区域的内存不会写入该文件. 
    (文件对应的最后一页,最后一页的部分block才包含文件的数据,其它的需清0)
    */
	if (page->index >= end_index) {
		/*
		 * The page straddles i_size.  It must be zeroed out on each
		 * and every writepage invocation because it may be mmapped.
		 * "A file is mapped in multiples of the page size.  For a file
		 * that is not a multiple of the page size, the remaining memory
		 * is zeroed when mapped, and writes to that region are not
		 * written out to the file."
		 */
		unsigned offset = i_size & (PAGE_SIZE - 1);

		if (page->index > end_index || !offset)
			goto confused;
		zero_user_segment(page, offset, PAGE_SIZE);
	}

	/*
	 * This page will go to BIO.  Do we need to send this BIO off first?
	 */
    /*a3.提交的脏扇区号与上一个扇区号不连续分开提交*/
	if (bio && mpd->last_block_in_bio != blocks[0] - 1)
		bio = mpage_bio_submit(REQ_OP_WRITE, op_flags, bio);

    /*a4.为这次的连续的脏block分配bio*/
alloc_new:
	if (bio == NULL) {
		if (first_unmapped == blocks_per_page) { //页中所有block都为脏,将其写入块设备
			if (!bdev_write_page(bdev, blocks[0] << (blkbits - 9),
								page, wbc))
				goto out;
		}
		bio = mpage_alloc(bdev, blocks[0] << (blkbits - 9),
				BIO_MAX_PAGES, GFP_NOFS|__GFP_HIGH);
		if (bio == NULL)
			goto confused;

		wbc_init_bio(wbc, bio);
		bio->bi_write_hint = inode->i_write_hint;
	}

	/*
	 * Must try to add the page before marking the buffer clean or
	 * the confused fail path above (OOM) will be very confused when
	 * it finds all bh marked clean (i.e. it will not write anything)
	 */
	wbc_account_io(wbc, page, PAGE_SIZE);
	length = first_unmapped << blkbits;
	if (bio_add_page(bio, page, length, 0) < length) {
		bio = mpage_bio_submit(REQ_OP_WRITE, op_flags, bio);
		goto alloc_new;
	}

	clean_buffers(page, first_unmapped);

	BUG_ON(PageWriteback(page));
	set_page_writeback(page); //设置page回写中
	unlock_page(page);
	if (boundary || (first_unmapped != blocks_per_page)) { //遇到边界值或者文件数据只占一页中部分block
		bio = mpage_bio_submit(REQ_OP_WRITE, op_flags, bio);
		if (boundary_block) {
			write_boundary_block(boundary_bdev,
					boundary_block, 1 << blkbits);
		}
	} else {
		mpd->last_block_in_bio = blocks[blocks_per_page - 1];
	}
	goto out;

confused:
	if (bio) //不连续提交之前的bio
		bio = mpage_bio_submit(REQ_OP_WRITE, op_flags, bio);

	if (mpd->use_writepage) {
		ret = mapping->a_ops->writepage(page, wbc);
	} else {
		ret = -EAGAIN;
		goto out;
	}
	/*
	 * The caller has a ref on the inode, so *mapping is stable
	 */
	mapping_set_error(mapping, ret);
out:
	mpd->bio = bio;
	return ret;
}

/**
 * mpage_writepages - walk the list of dirty pages of the given address space & writepage() all of them
 * @mapping: address space structure to write
 * @wbc: subtract the number of written pages from *@wbc->nr_to_write
 * @get_block: the filesystem's block mapper function.
 *             If this is NULL then use a_ops->writepage.  Otherwise, go
 *             direct-to-BIO.
 *
 * This is a library function, which implements the writepages()
 * address_space_operation.
 *
 * If a page is already under I/O, generic_writepages() skips it, even
 * if it's dirty.  This is desirable behaviour for memory-cleaning writeback,
 * but it is INCORRECT for data-integrity system calls such as fsync().  fsync()
 * and msync() need to guarantee that all the data which was dirty at the time
 * the call was made get new I/O started against them.  If wbc->sync_mode is
 * WB_SYNC_ALL then we were called for data integrity and we must wait for
 * existing IO to complete.
 */
int
mpage_writepages(struct address_space *mapping,
		struct writeback_control *wbc, get_block_t get_block)
{
	struct blk_plug plug;
	int ret;
    /*a1.block 蓄流*/
	blk_start_plug(&plug);

	if (!get_block)
		ret = generic_writepages(mapping, wbc);
	else {
		struct mpage_data mpd = {
			.bio = NULL,
			.last_block_in_bio = 0,
			.get_block = get_block,
			.use_writepage = 1,
		};

        /*a2.写cache pages*/
		ret = write_cache_pages(mapping, wbc, __mpage_writepage, &mpd);
		if (mpd.bio) {
			int op_flags = (wbc->sync_mode == WB_SYNC_ALL ?
				  REQ_SYNC : 0);
            /*a3.提交bio*/
			mpage_bio_submit(REQ_OP_WRITE, op_flags, mpd.bio);
		}
	}
    /*a4.block 泄流*/
	blk_finish_plug(&plug);
	return ret;
}
EXPORT_SYMBOL(mpage_writepages);

int mpage_writepage(struct page *page, get_block_t get_block,
	struct writeback_control *wbc)
{
	struct mpage_data mpd = {
		.bio = NULL,
		.last_block_in_bio = 0,
		.get_block = get_block,
		.use_writepage = 0,
	};
	int ret = __mpage_writepage(page, wbc, &mpd);
	if (mpd.bio) {
		int op_flags = (wbc->sync_mode == WB_SYNC_ALL ?
			  REQ_SYNC : 0);
		mpage_bio_submit(REQ_OP_WRITE, op_flags, mpd.bio);
	}
	return ret;
}
EXPORT_SYMBOL(mpage_writepage);
