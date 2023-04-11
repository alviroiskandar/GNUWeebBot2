// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023  Ammar Faizi <ammarfaizi2@gw.org>
 */

#ifndef GNUWEEB__RING_H
#define GNUWEEB__RING_H

#include <gw/common.h>
#include <gw/workqueue.h>
#include <gw/thread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>

#include <gw/lib/tgapi.h>

enum {
	GW_RING_OP_NOP = 0,
	GW_RING_OP_TG_GET_UPDATES = 1,
	GW_RING_OP_TG_API_CALL = 2,

	/* This goes last, don't add anything after this. */
	GW_RING_OP_LAST
};

#define GW_RING_DEFINE_UNION_ARG(arg)		\
	union {					\
		uint64_t	arg##_u64;	\
		int64_t		arg##_s64;	\
		void		*arg##_ptr;	\
	}

struct gw_ring_sqe {
	uint8_t		op;
	uint8_t		__pad[3];
	uint32_t	flags;
	uint64_t	user_data;
	GW_RING_DEFINE_UNION_ARG(arg1);
	GW_RING_DEFINE_UNION_ARG(arg2);
	GW_RING_DEFINE_UNION_ARG(arg3);
	GW_RING_DEFINE_UNION_ARG(arg4);
	GW_RING_DEFINE_UNION_ARG(arg5);
	GW_RING_DEFINE_UNION_ARG(arg6);
};

struct gw_ring_cqe {
	int64_t		res;
	uint64_t	user_data;
};

struct gw_ring {
	mutex_t			sq_lock;
	mutex_t			cq_lock;

	struct gw_ring_sqe	*sqes;
	struct gw_ring_cqe	*cqes;

	uint32_t		sq_head;
	uint32_t		sq_tail;
	uint32_t		sq_mask;

	uint32_t		cq_head;
	uint32_t		cq_tail;
	uint32_t		cq_mask;

	cond_t			post_cqe_cond;
	cond_t			wait_cqe_cond;

	volatile uint16_t	post_cqe_cond_n;
	volatile uint16_t	wait_cqe_cond_n;

	volatile bool		should_stop;
	struct workqueue_struct	*io_wq;
};

#define gw_ring_for_each_cqe(ring, head, cqe) \
	for (head = (ring)->cq_head; \
	     (cqe = head != (ring)->cq_tail ? \
	      &((ring)->cqes[head & (ring)->cq_mask]) : NULL); \
	     head++)

int gw_ring_queue_init(struct gw_ring *ring, uint32_t entries);
void gw_ring_queue_destroy(struct gw_ring *ring);
int gw_ring_submit(struct gw_ring *ring);
struct gw_ring_sqe *gw_ring_get_sqe(struct gw_ring *ring);
struct gw_ring_sqe *gw_ring_get_sqe_nf(struct gw_ring *ring);
int gw_ring_wait_cqe(struct gw_ring *ring, struct gw_ring_cqe **cqe);
int gw_ring_wait_cqe_nr(struct gw_ring *ring, struct gw_ring_cqe **cqe,
			uint32_t nr);
void gw_ring_cq_advance(struct gw_ring *ring, uint32_t n);
int gw_ring_submit_and_wait(struct gw_ring *ring, uint32_t wait_nr);

static inline uint32_t gw_ring_sq_ready(struct gw_ring *ring)
{
	return ring->sq_tail - ring->sq_head;
}

static inline uint32_t gw_ring_sq_left(struct gw_ring *ring)
{
	return ring->sq_mask + 1u - gw_ring_sq_ready(ring);
}

static inline uint32_t gw_ring_cq_ready(struct gw_ring *ring)
{
	return ring->cq_tail - ring->cq_head;
}

static inline uint32_t gw_ring_cq_left(struct gw_ring *ring)
{
	return ring->cq_mask + 1u - gw_ring_cq_ready(ring);
}

static inline void gw_ring_sqe_set_data64(struct gw_ring_sqe *sqe, uint64_t data)
{
	sqe->user_data = data;
}

static inline void gw_ring_sqe_set_data(struct gw_ring_sqe *sqe, void *data)
{
	sqe->user_data = (uint64_t)data;
}

static inline void *gw_ring_cqe_get_data(struct gw_ring_cqe *cqe)
{
	return (void *)cqe->user_data;
}

static inline uint64_t gw_ring_cqe_get_data64(struct gw_ring_cqe *cqe)
{
	return cqe->user_data;
}

static inline void gw_ring_prep_nop(struct gw_ring_sqe *sqe)
{
	sqe->op = GW_RING_OP_NOP;
}

static inline void gw_ring_prep_tg_get_updates(struct gw_ring_sqe *sqe,
					       struct tg_api_ctx *ctx,
					       struct tg_updates **updates_p,
					       int64_t offset)
{
	sqe->op = GW_RING_OP_TG_GET_UPDATES;
	sqe->arg1_ptr = ctx;
	sqe->arg2_ptr = updates_p;
	sqe->arg3_s64 = offset;
}

#endif /* #ifndef GNUWEEB__RING_H */
