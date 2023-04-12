// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023  Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */

#include <gw/module.h>
#include <gw/common.h>
#include <gw/ring.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ring/ring_internal.h"

int gw_ring_queue_init(struct gw_ring *ring, uint32_t entries)
{
	const struct workqueue_attr wq_attr = {
		.name = "gw-io-wq",
		.flags = WQ_F_LAZY_THREAD_CREATION,
		.max_threads = 64,
		.min_threads = 1,
		.max_pending_works = 4096,
	};
	uint32_t i;
	int ret;

	memset(ring, 0, sizeof(*ring));

	/*
	 * Round up to the nearest power of 2.
	 */
	i = 2u;
	while (i < entries)
		i *= 2u;

	entries = i;
	ring->sq_mask = entries - 1u;
	ring->cq_mask = entries * 2u - 1u;

	ring->sqes = calloc(ring->sq_mask + 1u, sizeof(*ring->sqes));
	if (!ring->sqes)
		return -ENOMEM;

	ring->cqes = calloc(ring->cq_mask + 1u, sizeof(*ring->cqes));
	if (!ring->cqes) {
		ret = -ENOMEM;
		goto out_free_sqes;
	}

	ret = alloc_workqueue(&ring->io_wq, &wq_attr);
	if (ret)
		goto out_free_cqes;

	ret = mutex_init(&ring->sq_lock);
	if (ret)
		goto out_free_wq;

	ret = mutex_init(&ring->cq_lock);
	if (ret)
		goto out_free_sq_lock;

	ret = cond_init(&ring->post_cqe_cond);
	if (ret)
		goto out_free_cq_lock;

	ret = cond_init(&ring->wait_cqe_cond);
	if (ret)
		goto out_free_post_cqe_cond;

	return 0;

out_free_post_cqe_cond:
	cond_destroy(&ring->post_cqe_cond);
out_free_cq_lock:
	mutex_destroy(&ring->cq_lock);
out_free_sq_lock:
	mutex_destroy(&ring->sq_lock);
out_free_wq:
	destroy_workqueue(ring->io_wq);
out_free_cqes:
	free(ring->cqes);
out_free_sqes:
	free(ring->sqes);
	memset(ring, 0, sizeof(*ring));
	return ret;
}

void gw_ring_queue_destroy(struct gw_ring *ring)
{
	ring->should_stop = true;

	mutex_lock(&ring->cq_lock);

	if (ring->post_cqe_cond_n)
		cond_broadcast_n(&ring->post_cqe_cond, ring->post_cqe_cond_n);

	if (ring->wait_cqe_cond_n)
		cond_broadcast_n(&ring->wait_cqe_cond, ring->wait_cqe_cond_n);

	mutex_unlock(&ring->cq_lock);

	wait_all_work_done(ring->io_wq);
	destroy_workqueue(ring->io_wq);
	free(ring->sqes);
	free(ring->cqes);
	mutex_destroy(&ring->sq_lock);
	mutex_destroy(&ring->cq_lock);
	cond_destroy(&ring->post_cqe_cond);
	cond_destroy(&ring->wait_cqe_cond);
	memset(ring, 0, sizeof(*ring));
}

/*
 * Data structure for posting CQE via workqueue.
 */
struct post_cqe_data {
	int64_t		res;
	uint64_t	user_data;
	struct gw_ring	*ring;
};

static void __gw_post_cqe(struct gw_ring *ring, int64_t res, uint64_t user_data)
	__must_hold(&ring->cq_lock)
{
	struct gw_ring_cqe *cqe;

	cqe = &ring->cqes[ring->cq_tail & ring->cq_mask];
	cqe->res = res;
	cqe->user_data = user_data;
	smp_mb();
	ring->cq_tail++;

	if (ring->wait_cqe_cond_n)
		cond_broadcast_n(&ring->wait_cqe_cond, ring->wait_cqe_cond_n);
}

/*
 * Workqueue callback for posting CQE.
 */
static void gw_iowq_post_cqe(void *data)
{
	struct post_cqe_data *d = data;
	struct gw_ring *ring = d->ring;

	mutex_lock(&ring->cq_lock);
	while (1) {
		if (gw_ring_cq_left(ring) > 0)
			break;

		if (unlikely(ring->should_stop)) {
			mutex_unlock(&ring->cq_lock);
			return;
		}

		ring->post_cqe_cond_n++;
		cond_wait(&ring->post_cqe_cond, &ring->cq_lock);
		ring->post_cqe_cond_n--;
	}
	__gw_post_cqe(ring, d->res, d->user_data);
	mutex_unlock(&ring->cq_lock);
}

/*
 * When the completion queue is full, we need to post the CQE via
 * workqueue.
 */
static bool gw_post_cqe_via_wq(struct gw_ring *ring, int64_t res,
			       uint64_t user_data)
	__must_hold(&ring->cq_lock)
{
	struct post_cqe_data *data;
	int ret;

	data = malloc(sizeof(*data));
	if (!data)
		return false;

	data->res = res;
	data->user_data = user_data;
	data->ring = ring;
	ret = try_queue_work(ring->io_wq, gw_iowq_post_cqe, data, free);
	if (unlikely(ret != 0)) {
		free(data);
		return false;
	}

	return true;
}

bool gw_post_cqe(struct gw_ring *ring, int64_t res, uint64_t user_data)
{
	bool ret;

	mutex_lock(&ring->cq_lock);

	if (unlikely(gw_ring_cq_left(ring) == 0)) {
		ret = gw_post_cqe_via_wq(ring, res, user_data);
	} else {
		__gw_post_cqe(ring, res, user_data);
		ret = true;
	}

	mutex_unlock(&ring->cq_lock);
	return ret;
}

static bool gw_issue_sqe_nop(struct gw_ring *ring, struct gw_ring_sqe *sqe)
	__must_hold(&ring->sq_lock)
{
	return gw_post_cqe(ring, 0, sqe->user_data);
}

static bool gw_issue_sqe(struct gw_ring *ring, struct gw_ring_sqe *sqe)
	__must_hold(&ring->sq_lock)
{
	bool ret;

	switch (sqe->op) {
	case GW_RING_OP_NOP:
		ret = gw_issue_sqe_nop(ring, sqe);
		break;
	case GW_RING_OP_TG_GET_UPDATES:
		ret = gw_issue_sqe_tg_get_updates(ring, sqe);
		break;
	default:
		ret = false;
		break;
	}
	return ret;
}

static int __gw_ring_submit(struct gw_ring *ring)
	__must_hold(&ring->sq_lock)
{
	uint32_t head, tail;
	int ret = 0;

	head = ring->sq_head;
	tail = ring->sq_tail;

	while (head != tail) {
		if (unlikely(ring->should_stop))
			return -EOWNERDEAD;

		if (gw_issue_sqe(ring, &ring->sqes[head++ & ring->sq_mask]))
			ret++;
	}

	ring->sq_head = head;
	return ret;
}

int gw_ring_submit(struct gw_ring *ring)
{
	int ret;

	mutex_lock(&ring->sq_lock);
	ret = __gw_ring_submit(ring);
	mutex_unlock(&ring->sq_lock);
	return ret;
}

struct gw_ring_sqe *gw_ring_get_sqe(struct gw_ring *ring)
{
	struct gw_ring_sqe *sqe = NULL;

	mutex_lock(&ring->sq_lock);

	if (likely(gw_ring_sq_left(ring) > 0))
		sqe = &ring->sqes[ring->sq_tail++ & ring->sq_mask];

	mutex_unlock(&ring->sq_lock);
	return sqe;
}

struct gw_ring_sqe *gw_ring_get_sqe_nf(struct gw_ring *ring)
{
	struct gw_ring_sqe *sqe;

	mutex_lock(&ring->sq_lock);

	if (unlikely(gw_ring_sq_left(ring) == 0))
		__gw_ring_submit(ring);

	sqe = &ring->sqes[ring->sq_tail++ & ring->sq_mask];
	mutex_unlock(&ring->sq_lock);
	return sqe;
}

static int __gw_ring_wait_cqe_nr(struct gw_ring *ring, struct gw_ring_cqe **cqe,
				 uint32_t nr)
	__must_hold(&ring->cq_lock)
{
	while (1) {
		if (unlikely(ring->should_stop))
			return -EOWNERDEAD;

		if (gw_ring_cq_ready(ring) >= nr)
			break;

		ring->wait_cqe_cond_n++;
		cond_wait(&ring->wait_cqe_cond, &ring->cq_lock);
		ring->wait_cqe_cond_n--;
	}

	if (cqe)
		*cqe = &ring->cqes[ring->cq_head & ring->cq_mask];

	return 0;
}

int gw_ring_wait_cqe_nr(struct gw_ring *ring, struct gw_ring_cqe **cqe,
			uint32_t nr)
{
	int ret;

	mutex_lock(&ring->cq_lock);
	ret = __gw_ring_wait_cqe_nr(ring, cqe, nr);
	mutex_unlock(&ring->cq_lock);
	return ret;
}

int gw_ring_wait_cqe(struct gw_ring *ring, struct gw_ring_cqe **cqe)
{
	return gw_ring_wait_cqe_nr(ring, cqe, 1u);
}

int gw_ring_submit_and_wait(struct gw_ring *ring, uint32_t wait_nr)
{
	int ret;

	mutex_lock(&ring->sq_lock);
	ret = __gw_ring_submit(ring);
	mutex_unlock(&ring->sq_lock);

	if (unlikely(ret < 0))
		return ret;

	mutex_lock(&ring->cq_lock);
	ret = __gw_ring_wait_cqe_nr(ring, NULL, wait_nr);
	mutex_unlock(&ring->cq_lock);
	return ret;
}

void gw_ring_cq_advance(struct gw_ring *ring, uint32_t n)
{
	mutex_lock(&ring->cq_lock);

	ring->cq_head += n;
	if (ring->post_cqe_cond_n)
		cond_broadcast_n(&ring->post_cqe_cond, ring->post_cqe_cond_n);

	mutex_unlock(&ring->cq_lock);
}
