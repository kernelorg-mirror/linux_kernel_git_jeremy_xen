/**************************************************************************
 *
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "ttm/ttm_memory.h"
#include "ttm/ttm_module.h"
#include "ttm/ttm_page_alloc.h"
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

#define TTM_MEMORY_RETRIES 4

static struct attribute ttm_mem_sys = {
	.name = "memory",
	.mode = S_IRUGO
};
static struct attribute ttm_mem_emer = {
	.name = "emergency_memory",
	.mode = S_IRUGO | S_IWUSR
};
static struct attribute ttm_mem_max = {
	.name = "available_memory",
	.mode = S_IRUGO | S_IWUSR
};
static struct attribute ttm_mem_swap = {
	.name = "swap_limit",
	.mode = S_IRUGO | S_IWUSR
};
static struct attribute ttm_mem_dma32_swap = {
	.name = "swap_dma32_limit",
	.mode = S_IRUGO | S_IWUSR
};
static struct attribute ttm_mem_used = {
	.name = "used_memory",
	.mode = S_IRUGO
};
static struct attribute ttm_mem_dma32_used = {
	.name = "used_dma32_memory",
	.mode = S_IRUGO
};

static ssize_t ttm_mem_global_show(struct kobject *kobj,
				   struct attribute *attr,
				   char *buffer)
{
	struct ttm_mem_global *glob =
		container_of(kobj, struct ttm_mem_global, kobj);
	unsigned long val = 0;

	spin_lock(&glob->lock);
	if (attr == &ttm_mem_sys)
		val = glob->mem;
	else if (attr == &ttm_mem_emer)
		val = glob->emer_mem;
	else if (attr == &ttm_mem_max)
		val = glob->max_mem;
	else if (attr == &ttm_mem_swap)
		val = glob->swap_limit;
	else if (attr == &ttm_mem_used)
		val = glob->used_mem;
	else if (attr == &ttm_mem_dma32_used)
		val = glob->used_dma32_mem;
	else if (attr == &ttm_mem_dma32_swap)
		val = glob->swap_dma32_limit;
	spin_unlock(&glob->lock);

	return snprintf(buffer, PAGE_SIZE, "%lu\n", val >> 10);
}

static void ttm_check_swapping(struct ttm_mem_global *glob);

static ssize_t ttm_mem_global_store(struct kobject *kobj,
				    struct attribute *attr,
				    const char *buffer,
				    size_t size)
{
	struct ttm_mem_global *glob =
		container_of(kobj, struct ttm_mem_global, kobj);
	unsigned long val;
	int chars;

	chars = sscanf(buffer, "%lu", &val);
	if (chars == 0)
		return size;

	val <<= 10;

	spin_lock(&glob->lock);
	/* limit to maximum memory */
	if (val > glob->mem)
		val = glob->mem;

	if (attr == &ttm_mem_emer) {
		glob->emer_mem = val;
		if (glob->max_mem > val)
			glob->max_mem = val;
	} else if (attr == &ttm_mem_max) {
		glob->max_mem = val;
		if (glob->emer_mem < val)
			glob->emer_mem = val;
	} else if (attr == &ttm_mem_swap) {
		glob->swap_limit = val;
	} else if (attr == &ttm_mem_dma32_swap) {
		glob->swap_dma32_limit = val;
	}
	spin_unlock(&glob->lock);
	ttm_check_swapping(glob);
	return size;
}

static void ttm_mem_global_kobj_release(struct kobject *kobj)
{
	struct ttm_mem_global *glob =
		container_of(kobj, struct ttm_mem_global, kobj);

	kfree(glob);
}

static struct attribute *ttm_mem_global_attrs[] = {
	&ttm_mem_sys,
	&ttm_mem_emer,
	&ttm_mem_max,
	&ttm_mem_swap,
	&ttm_mem_dma32_swap,
	&ttm_mem_used,
	&ttm_mem_dma32_used,
	NULL
};

static const struct sysfs_ops ttm_mem_global_ops = {
	.show = &ttm_mem_global_show,
	.store = &ttm_mem_global_store
};

static struct kobj_type ttm_mem_glob_kobj_type = {
	.release = &ttm_mem_global_kobj_release,
	.sysfs_ops = &ttm_mem_global_ops,
	.default_attrs = ttm_mem_global_attrs,
};

static bool ttm_above_swap_target(struct ttm_mem_global *glob,
				  bool from_wq, uint64_t extra)
{
	unsigned long target;

	if (from_wq) {
		if (glob->used_mem > glob->swap_limit) {
			return true;
		}
		if (glob->used_dma32_mem > glob->swap_dma32_limit) {
			return true;
		}
	} else {
		if (capable(CAP_SYS_ADMIN))
			target = glob->emer_mem;
		else
			target = glob->max_mem;
		if (extra > target) {
			return true;
		}
		if ((glob->used_mem + glob->used_dma32_mem) > target) {
			return true;
		}
	}
	return false;
}

/**
 * At this point we only support a single shrink callback.
 * Extend this if needed, perhaps using a linked list of callbacks.
 * Note that this function is reentrant:
 * many threads may try to swap out at any given time.
 */
static void ttm_shrink(struct ttm_mem_global *glob,
		       bool from_wq,
		       uint64_t extra)
{
	struct ttm_mem_shrink *shrink;
	int ret, nretries = TTM_MEMORY_RETRIES;

	spin_lock(&glob->lock);
	if (glob->shrink == NULL)
		goto out;

	while (ttm_above_swap_target(glob, from_wq, extra)) {
		shrink = glob->shrink;
		spin_unlock(&glob->lock);
		ret = shrink->do_shrink(shrink);
		spin_lock(&glob->lock);
		if (unlikely(ret != 0))
			goto out;
		if (--nretries < 0)
			goto out;
	}
out:
	spin_unlock(&glob->lock);
}

static void ttm_shrink_work(struct work_struct *work)
{
	struct ttm_mem_global *glob =
	    container_of(work, struct ttm_mem_global, work);

	ttm_shrink(glob, true, 0ULL);
}

int ttm_mem_global_init(struct ttm_mem_global *glob)
{
	struct sysinfo si;
	int ret;

	spin_lock_init(&glob->lock);
	glob->swap_queue = create_singlethread_workqueue("ttm_swap");
	INIT_WORK(&glob->work, ttm_shrink_work);
	init_waitqueue_head(&glob->queue);
	ret = kobject_init_and_add(
		&glob->kobj, &ttm_mem_glob_kobj_type, ttm_get_kobj(), "memory_accounting");
	if (unlikely(ret != 0)) {
		kobject_put(&glob->kobj);
		return ret;
	}

	/* compute limit */
	si_meminfo(&si);
	glob->mem = si.totalram;
	glob->mem *= si.mem_unit;
	glob->used_mem = 0;
	glob->used_dma32_mem = 0;
	glob->max_mem = glob->mem >> 1;
	glob->emer_mem = (glob->mem >> 1) + (glob->mem >> 2);
	glob->swap_limit = glob->max_mem - (glob->mem >> 3);
	glob->swap_dma32_limit = ((1ULL << 32) >> 1) - (((1ULL << 32) >> 3));
	ttm_page_alloc_init(glob, glob->max_mem/(2*PAGE_SIZE));
	ttm_dma_page_alloc_init(glob, glob->max_mem/(2*PAGE_SIZE));
	return 0;
}
EXPORT_SYMBOL(ttm_mem_global_init);

void ttm_mem_global_release(struct ttm_mem_global *glob)
{
	/* let the page allocator first stop the shrink work. */
	ttm_page_alloc_fini();
	ttm_dma_page_alloc_fini();

	flush_workqueue(glob->swap_queue);
	destroy_workqueue(glob->swap_queue);
	glob->swap_queue = NULL;
	kobject_del(&glob->kobj);
	kobject_put(&glob->kobj);
}
EXPORT_SYMBOL(ttm_mem_global_release);

static void ttm_check_swapping(struct ttm_mem_global *glob)
{
	bool needs_swapping = false;

	spin_lock(&glob->lock);
	if ((glob->used_mem + glob->used_dma32_mem) > glob->swap_limit) {
		needs_swapping = true;
	}
	if (glob->used_dma32_mem > glob->swap_dma32_limit) {
		needs_swapping = true;
	}
	spin_unlock(&glob->lock);

	if (unlikely(needs_swapping))
		(void)queue_work(glob->swap_queue, &glob->work);

}

void ttm_mem_global_free(struct ttm_mem_global *glob,
			 uint64_t amount)
{
	spin_lock(&glob->lock);
	glob->used_mem -= amount;
	spin_unlock(&glob->lock);
}
EXPORT_SYMBOL(ttm_mem_global_free);

int ttm_mem_global_alloc(struct ttm_mem_global *glob,
			 uint64_t memory,
			 bool no_wait)
{
	unsigned long limit;
	int i;

	for (i = 0; i < TTM_MEMORY_RETRIES; i++) {
		spin_lock(&glob->lock);
		limit = (capable(CAP_SYS_ADMIN)) ? glob->emer_mem : glob->max_mem;
		if ((glob->used_mem + glob->used_dma32_mem + memory) < limit) {
			glob->used_mem += memory;
			spin_unlock(&glob->lock);
			return 0;
		}
		spin_unlock(&glob->lock);
		if (no_wait)
			return -ENOMEM;
		ttm_shrink(glob, false, memory + (memory >> 2) + 16);
	}
	return -ENOMEM;
}
EXPORT_SYMBOL(ttm_mem_global_alloc);

int ttm_mem_global_alloc_pages(struct ttm_mem_global *glob,
			       unsigned npages,
			       bool no_wait)
{
	if (ttm_mem_global_alloc(glob, PAGE_SIZE * npages, no_wait))
		return -ENOMEM;
	ttm_check_swapping(glob);
	return 0;
}

void ttm_mem_global_account_pages(struct ttm_mem_global *glob,
				  struct page **pages,
				  unsigned npages)
{
	unsigned i;

	/* check if page is dma32 */
	spin_lock(&glob->lock);
	for (i = 0; i < npages; i++) {
		if (page_to_pfn(pages[i]) > 0x00100000UL) {
			glob->used_mem -= PAGE_SIZE;
			glob->used_dma32_mem += PAGE_SIZE;
		}
	}
	spin_unlock(&glob->lock);
}

void ttm_mem_global_free_pages(struct ttm_mem_global *glob,
			       struct page **pages, unsigned npages)
{
	unsigned i;

	spin_lock(&glob->lock);
	for (i = 0; i < npages; i++) {
		if (page_to_pfn(pages[i]) > 0x00100000UL) {
			glob->used_dma32_mem -= PAGE_SIZE;
		} else {
			glob->used_mem -= PAGE_SIZE;
		}
	}
	spin_unlock(&glob->lock);
}

size_t ttm_round_pot(size_t size)
{
	if ((size & (size - 1)) == 0)
		return size;
	else if (size > PAGE_SIZE)
		return PAGE_ALIGN(size);
	else {
		size_t tmp_size = 4;

		while (tmp_size < size)
			tmp_size <<= 1;

		return tmp_size;
	}
	return 0;
}
EXPORT_SYMBOL(ttm_round_pot);
