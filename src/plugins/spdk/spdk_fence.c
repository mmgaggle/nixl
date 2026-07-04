/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Pure, SPDK-free implementation of the shim fence: stale-orphan discard,
 * poison latch, and staging-buffer quarantine. See spdk_fence.h.
 */

#include "spdk_fence.h"

#include <errno.h>
#include <stdlib.h>

void
spdk_fence_init(struct spdk_fence *f)
{
	f->gen = 0;
	f->op_done = false;
	f->op_sct = 0;
	f->op_sc = 0;
	f->op_cdw0 = 0;
	f->poisoned = false;
	f->quarantine = NULL;
}

bool
spdk_fence_begin(struct spdk_fence *f, struct spdk_op_tag *tag)
{
	if (f->poisoned) {
		return false;
	}
	/*
	 * A fresh generation per op is the token io_complete() compares: a late
	 * completion carrying a previous generation is discarded by complete().
	 */
	f->gen++;
	f->op_done = false;
	tag->fence = f;
	tag->gen = f->gen;
	return true;
}

bool
spdk_fence_complete(struct spdk_op_tag *tag, uint8_t sct, uint8_t sc,
		       uint32_t cdw0)
{
	struct spdk_fence *f = tag->fence;

	if (tag->gen != f->gen) {
		/* Stale orphan from a previously abandoned op: leave the current
		 * op's slot untouched so it reports its OWN status, not this one. */
		return false;
	}
	f->op_sct = sct;
	f->op_sc = sc;
	f->op_cdw0 = cdw0;
	f->op_done = true;
	return true;
}

bool
spdk_fence_done(const struct spdk_fence *f)
{
	return f->op_done;
}

void
spdk_fence_poison(struct spdk_fence *f)
{
	f->poisoned = true;
}

bool
spdk_fence_poisoned(const struct spdk_fence *f)
{
	return f->poisoned;
}

int
spdk_fence_quarantine(struct spdk_fence *f, void *buf)
{
	struct spdk_quarantine_node *n;

	if (buf == NULL) {
		return 0;
	}
	n = calloc(1, sizeof(*n));
	if (n == NULL) {
		return -ENOMEM;
	}
	n->buf = buf;
	n->next = f->quarantine;
	f->quarantine = n;
	return 0;
}

void
spdk_fence_drain(struct spdk_fence *f, void (*free_buf)(void *))
{
	struct spdk_quarantine_node *n = f->quarantine;

	while (n != NULL) {
		struct spdk_quarantine_node *next = n->next;

		if (free_buf != NULL) {
			free_buf(n->buf);
		}
		free(n);
		n = next;
	}
	f->quarantine = NULL;
	f->poisoned = false;
}
