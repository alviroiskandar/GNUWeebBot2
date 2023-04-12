// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023  Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2023  Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include <gw/lib/curl.h>
#include <gw/common.h>
#include <gw/ring.h>

#include <sys/signal.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

struct gw_bot_ctx {
	volatile bool		should_stop;
	struct gw_ring		ring;
	struct tg_api_ctx	tctx;
	struct tg_updates	*updates;
	uint64_t		max_update_id;
	int			signal;
};

enum {
	EV_TG_GET_UPDATES = (1ull << 48ull)
};

enum {
	GET_EV_MASK	= (0xffffull << 48ull),
	CLEAR_EV_MASK	= ~GET_EV_MASK
};

static struct gw_bot_ctx *g_ctx;

static int init_tg_api_ctx(struct tg_api_ctx *ctx)
{
	ctx->token = "308645660:AAFlEKTBWjuwTDiGvyqAaDMuwBXLoiQPijQ";
	return 0;
}

static void signal_handler(int sig)
{
	if (g_ctx) {
		g_ctx->should_stop = true;
		g_ctx->signal = sig;
		putchar('\n');
	}
}

static int init_signal_handlers(struct gw_bot_ctx *ctx)
{
	struct sigaction sa;
	int ret;

	g_ctx = ctx;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	ret = sigaction(SIGINT, &sa, NULL);
	if (unlikely(ret))
		goto out_err;
	ret = sigaction(SIGTERM, &sa, NULL);
	if (unlikely(ret))
		goto out_err;
	ret = sigaction(SIGHUP, &sa, NULL);
	if (unlikely(ret))
		goto out_err;
	sa.sa_handler = SIG_IGN;
	ret = sigaction(SIGPIPE, &sa, NULL);
	if (unlikely(ret))
		goto out_err;

	return 0;

out_err:
	ret = -errno;
	perror("sigaction");
	return ret;
}

static void prep_tg_get_updates(struct gw_bot_ctx *ctx)
{
	struct gw_ring_sqe *sqe;
	int64_t offset;

	if (ctx->max_update_id == 0)
		offset = 0;
	else
		offset = (int64_t)(ctx->max_update_id + 1);

	sqe = gw_ring_get_sqe_nf(&ctx->ring);
	gw_ring_prep_tg_get_updates(sqe, &ctx->tctx, &ctx->updates, offset);
	gw_ring_sqe_set_data(sqe, NULL);
	sqe->user_data |= EV_TG_GET_UPDATES;
}

static int process_tg_update(struct gw_bot_ctx *ctx, struct tg_update *update)
{
	struct tg_message *msg;

	if (update->update_id > ctx->max_update_id)
		ctx->max_update_id = update->update_id;

	if (update->type != TG_UPDATE_MESSAGE)
		return 0;

	msg = &update->message;
	if (msg->type != TG_MSG_TEXT)
		return 0;

	printf("Got message: %s\n", msg->text);
	return 0;
}

static int process_tg_updates(struct gw_bot_ctx *ctx, int64_t res)
{
	int ret = (int)res;
	uint32_t i;

	// if (unlikely(ret < 0)) {
	// 	pr_err("Failed to get updates: %s\n", strerror((int)-res));
	// 	return (int)ret;
	// }

	if (unlikely(!ctx->updates))
		goto out;

	if (ctx->updates->len)
		printf("Got new %zu update(s)\n", ctx->updates->len);

	for (i = 0; i < ctx->updates->len; i++) {
		struct tg_update *update = &ctx->updates->updates[i];

		ret = process_tg_update(ctx, update);
		if (unlikely(ret))
			break;
	}

	tgapi_free_updates(ctx->updates);
	ctx->updates = NULL;

out:
	if (likely(!ret && !ctx->should_stop))
		prep_tg_get_updates(ctx);

	return 0;
}

static int process_event(struct gw_bot_ctx *ctx, struct gw_ring_cqe *cqe)
{
	__maybe_unused void *ptr;
	uint64_t udata;
	uint64_t ev;
	int ret;

	udata = gw_ring_cqe_get_data64(cqe);
	ptr = (void *)(uintptr_t)(udata & CLEAR_EV_MASK);
	ev = udata & GET_EV_MASK;
	switch (ev) {
	case EV_TG_GET_UPDATES:
		ret = process_tg_updates(ctx, cqe->res);
		break;
	default:
		pr_err("Unknown event: %llx\n", (unsigned long long)ev);
		abort();
	}

	return ret;
}

static int process_events(struct gw_bot_ctx *ctx)
{
	struct gw_ring_cqe *cqe;
	uint32_t head, i;
	int ret;

	i = 0;
	gw_ring_for_each_cqe(&ctx->ring, head, cqe) {
		i++;
		ret = process_event(ctx, cqe);
		if (unlikely(ret))
			break;
	}
	gw_ring_cq_advance(&ctx->ring, i);
	return ret;
}

static int run_event_loop(struct gw_bot_ctx *ctx)
{
	int ret = 0;

	prep_tg_get_updates(ctx);
	while (!ctx->should_stop) {
		gw_ring_submit_and_wait(&ctx->ring, 1u);
		ret = process_events(ctx);
		if (unlikely(ret))
			break;
	}
	return ret;
}

int main(void)
{
	struct gw_bot_ctx ctx;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ret = init_tg_api_ctx(&ctx.tctx);
	if (ret)
		return ret;
	ret = init_signal_handlers(&ctx);
	if (ret)
		return ret;
	ret = gw_curl_global_init(0);
	if (ret)
		return ret;
	ret = gw_print_global_init();
	if (ret)
		goto out_curl;
	ret = gw_ring_queue_init(&ctx.ring, 4096);
	if (ret)
		goto out_print;

	ret = run_event_loop(&ctx);
	gw_ring_queue_destroy(&ctx.ring);
	if (ctx.updates)
		tgapi_free_updates(ctx.updates);

out_print:
	gw_print_global_destroy();
out_curl:
	gw_curl_global_cleanup();
	return ret;
}
