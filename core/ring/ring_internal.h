// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023  Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2023  Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GNUWEEB__CORE__RING__RING_INTERNAL_H
#define GNUWEEB__CORE__RING__RING_INTERNAL_H

bool gw_post_cqe(struct gw_ring *ring, int64_t res, uint64_t user_data);
bool gw_issue_sqe_tg_get_updates(struct gw_ring *ring, struct gw_ring_sqe *sqe);

#endif /* #ifndef GNUWEEB__CORE__RING__RING_INTERNAL_H */
