// SPDX-License-Identifier: GPL-2.0-only

#undef NDEBUG
#include <gw/ring.h>
#include <assert.h>
#include <stdio.h>

static void test_single_nop(void)
{
	struct gw_ring_sqe *sqe;
	struct gw_ring_cqe *cqe;
	struct gw_ring ring;
	int ret;

	ret = gw_ring_queue_init(&ring, 2);
	assert(!ret);

	sqe = gw_ring_get_sqe(&ring);
	assert(sqe);
	gw_ring_prep_nop(sqe);
	gw_ring_sqe_set_data64(sqe, 0xdeadbeef);

	ret = gw_ring_submit(&ring);
	assert(ret == 1);

	ret = gw_ring_wait_cqe(&ring, &cqe);
	assert(ret == 0);
	assert(cqe->res == 0);
	assert(gw_ring_cqe_get_data64(cqe) == 0xdeadbeef);
	gw_ring_cq_advance(&ring, 1);
	gw_ring_queue_destroy(&ring);
}

static void test_single_multi_nop(void)
{
	struct gw_ring_sqe *sqe;
	struct gw_ring_cqe *cqe;
	struct gw_ring ring;
	uint32_t head, i, j;
	int ret;

	ret = gw_ring_queue_init(&ring, 16);
	assert(!ret);

	for (i = 0; i < 2; i++) {
		for (j = 0; j < 16; j++) {
			sqe = gw_ring_get_sqe(&ring);
			assert(sqe);
			gw_ring_prep_nop(sqe);
			gw_ring_sqe_set_data64(sqe, 0xdeadbeef);
		}

		/*
		* This should fail, as we have no more SQEs available.
		*/
		sqe = gw_ring_get_sqe(&ring);
		assert(!sqe);

		ret = gw_ring_submit(&ring);
		assert(ret == 16);
	}

	ret = gw_ring_wait_cqe_nr(&ring, &cqe, 32);
	i = 0;
	gw_ring_for_each_cqe(&ring, head, cqe) {
		assert(ret == 0);
		assert(cqe->res == 0);
		assert(gw_ring_cqe_get_data64(cqe) == 0xdeadbeef);
		i++;
	}
	assert(i == 32);
	gw_ring_cq_advance(&ring, i);
	gw_ring_queue_destroy(&ring);
}

static void test_cqe_overflow(void)
{
	struct gw_ring_sqe *sqe;
	struct gw_ring_cqe *cqe;
	struct gw_ring ring;
	uint32_t head, i, j;
	int ret;

	ret = gw_ring_queue_init(&ring, 16);
	assert(!ret);

	for (i = 0; i < 3; i++) {
		for (j = 0; j < 16; j++) {
			sqe = gw_ring_get_sqe(&ring);
			assert(sqe);
			gw_ring_prep_nop(sqe);
			gw_ring_sqe_set_data64(sqe, 0xdeadbeef);
		}

		/*
		* This should fail, as we have no more SQEs available.
		*/
		sqe = gw_ring_get_sqe(&ring);
		assert(!sqe);

		ret = gw_ring_submit(&ring);
		assert(ret == 16);
	}

	/*
	 * The CQE ring is only 32 entries, so we should only get 32 CQEs
	 * back. The rest submissions are still pending in the WQ thread.
	 */
	ret = gw_ring_wait_cqe_nr(&ring, &cqe, 32);
	i = 0;
	gw_ring_for_each_cqe(&ring, head, cqe) {
		assert(ret == 0);
		assert(cqe->res == 0);
		assert(gw_ring_cqe_get_data64(cqe) == 0xdeadbeef);
		i++;
	}
	assert(i == 32);
	gw_ring_cq_advance(&ring, i);

	/*
	 * Get the remaining 16 CQEs.
	 */
	ret = gw_ring_wait_cqe_nr(&ring, &cqe, 16);
	i = 0;
	gw_ring_for_each_cqe(&ring, head, cqe) {
		assert(ret == 0);
		assert(cqe->res == 0);
		assert(gw_ring_cqe_get_data64(cqe) == 0xdeadbeef);
		i++;
	}
	assert(i == 16);
	gw_ring_queue_destroy(&ring);
}

int main(void)
{
	test_single_nop();
	test_single_multi_nop();
	test_cqe_overflow();
	return 0;
}
