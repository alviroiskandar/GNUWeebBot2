
#include <gw/module.h>
#include <gw/common.h>
#include <gw/ring.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ring_internal.h"

struct work_tg_get_updates {
	struct gw_ring		*ring;
	struct tg_api_ctx	*ctx;
	struct tg_updates	**updates_p;
	int64_t			offset;
	uint64_t		user_data;
};

static void iowq_tg_get_updates(void *data)
{
	struct work_tg_get_updates *d = data;
	int64_t res;

	res = tgapi_call_get_updates(d->ctx, d->updates_p, d->offset);
	gw_post_cqe(d->ring, res, d->user_data);
}

bool gw_issue_sqe_tg_get_updates(struct gw_ring *ring, struct gw_ring_sqe *sqe)
	__must_hold(&ring->sq_lock)
{
	struct work_tg_get_updates *data;
	int ret;

	data = malloc(sizeof(*data));
	if (unlikely(!data))
		return false;

	data->ring = ring;
	data->ctx = READ_ONCE(sqe->arg1_ptr);
	data->updates_p = READ_ONCE(sqe->arg2_ptr);
	data->offset = READ_ONCE(sqe->arg3_s64);
	data->user_data = READ_ONCE(sqe->user_data);
	ret = try_queue_work(ring->io_wq, iowq_tg_get_updates, data, free);
	if (unlikely(ret != 0)) {
		free(data);
		return false;
	}
	return true;
}
