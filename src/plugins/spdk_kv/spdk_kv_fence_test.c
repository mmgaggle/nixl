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
 * Pure-logic unit test for the shim fence (spdk_kv_fence.{c,h}): the stale-
 * orphan discard, the poison latch, and the staging-buffer quarantine. It needs
 * no SPDK, no live NVMe target, and no hugepages -- it drives the exact decision
 * code the shim's io_complete()/poll_to_completion()/close() paths call.
 *
 * The one thing it cannot force here is a REAL hardware timeout (that needs a
 * wedged live target, driven by the run_*.sh harnesses); this asserts the fence
 * logic those paths delegate to.
 */

#include "spdk_kv_fence.h"

#include <errno.h>
#include <stdio.h>

static int g_failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                  \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
				#cond);                                        \
			g_failures++;                                          \
		}                                                              \
	} while (0)

/* Counting fake for the drain() free callback: records how many times each
 * buffer is freed so we can assert "exactly once". */
enum { MAX_FREED = 16 };
static void *g_freed[MAX_FREED];
static int g_free_calls;

static void
counting_free(void *p)
{
	if (g_free_calls < MAX_FREED) {
		g_freed[g_free_calls] = p;
	}
	g_free_calls++;
}

static int
freed_count(const void *p)
{
	int n = 0;
	for (int i = 0; i < g_free_calls && i < MAX_FREED; i++) {
		if (g_freed[i] == p) {
			n++;
		}
	}
	return n;
}

/* A healthy op: begin stamps a fresh generation, the matching completion is
 * recorded, and status reads back. */
static void
test_happy_path(void)
{
	struct spdk_kv_fence f;
	struct spdk_kv_op_tag tag;

	spdk_kv_fence_init(&f);
	CHECK(!spdk_kv_fence_poisoned(&f));
	CHECK(!spdk_kv_fence_done(&f));

	CHECK(spdk_kv_fence_begin(&f, &tag));
	CHECK(!spdk_kv_fence_done(&f));
	CHECK(spdk_kv_fence_complete(&tag, 0 /*GENERIC*/, 0x00, 4096));
	CHECK(spdk_kv_fence_done(&f));
	CHECK(f.op_sct == 0 && f.op_sc == 0x00 && f.op_cdw0 == 4096);
}

/*
 * The core mis-record bug: an earlier op's LATE completion must not be recorded
 * as a later op's status. Two distinct tags share one fence (exactly how a
 * future async datapath's per-op pool tags behave). Op A's orphan fires during
 * op B's wait; B must still report its OWN status/cdw0.
 */
static void
test_stale_orphan_discarded(void)
{
	struct spdk_kv_fence f;
	struct spdk_kv_op_tag tag_a, tag_b;

	spdk_kv_fence_init(&f);

	CHECK(spdk_kv_fence_begin(&f, &tag_a)); /* gen N   */
	CHECK(spdk_kv_fence_begin(&f, &tag_b)); /* gen N+1 (A abandoned) */
	CHECK(tag_a.gen != tag_b.gen);

	/* A's orphan arrives first, carrying a bogus status + length. */
	CHECK(!spdk_kv_fence_complete(&tag_a, 1 /*non-generic*/, 0x85, 12345));
	CHECK(!spdk_kv_fence_done(&f)); /* discarded: B is still outstanding */

	/* B's real completion records B's own status/length. */
	CHECK(spdk_kv_fence_complete(&tag_b, 0, 0x00, 42));
	CHECK(spdk_kv_fence_done(&f));
	CHECK(f.op_sct == 0 && f.op_sc == 0x00 && f.op_cdw0 == 42);

	/* An even-later duplicate orphan for A is still discarded, not recorded. */
	CHECK(!spdk_kv_fence_complete(&tag_a, 1, 0x85, 12345));
	CHECK(f.op_cdw0 == 42); /* B's value intact */
}

/* After a timeout/transport-fail poison, every begin() is refused until drain. */
static void
test_poison_refuses_submits(void)
{
	struct spdk_kv_fence f;
	struct spdk_kv_op_tag tag;
	uint64_t gen_before;

	spdk_kv_fence_init(&f);
	CHECK(spdk_kv_fence_begin(&f, &tag));
	gen_before = f.gen;

	/* poll_to_completion() latches this on -ETIMEDOUT / -ENXIO. */
	spdk_kv_fence_poison(&f);
	CHECK(spdk_kv_fence_poisoned(&f));

	/* Refused, and the generation does not advance (no new op started). */
	CHECK(!spdk_kv_fence_begin(&f, &tag));
	CHECK(f.gen == gen_before);
	/* Idempotent poison. */
	spdk_kv_fence_poison(&f);
	CHECK(!spdk_kv_fence_begin(&f, &tag));

	/* Fencing teardown drains + clears poison; then ops resume. */
	spdk_kv_fence_drain(&f, counting_free);
	CHECK(!spdk_kv_fence_poisoned(&f));
	CHECK(spdk_kv_fence_begin(&f, &tag));
}

/* Quarantined buffers are freed exactly once, only by drain(). */
static void
test_quarantine_freed_once(void)
{
	struct spdk_kv_fence f;
	int buf0, buf1, buf2; /* stand-ins for staging buffers (addresses only) */

	g_free_calls = 0;
	spdk_kv_fence_init(&f);
	spdk_kv_fence_poison(&f);

	CHECK(spdk_kv_fence_quarantine(&f, &buf0) == 0);
	CHECK(spdk_kv_fence_quarantine(&f, &buf1) == 0);
	CHECK(spdk_kv_fence_quarantine(&f, &buf2) == 0);
	/* NULL is a no-op, never counted. */
	CHECK(spdk_kv_fence_quarantine(&f, NULL) == 0);

	/* Nothing is freed before the fencing teardown. */
	CHECK(g_free_calls == 0);

	spdk_kv_fence_drain(&f, counting_free);
	CHECK(g_free_calls == 3);
	CHECK(freed_count(&buf0) == 1);
	CHECK(freed_count(&buf1) == 1);
	CHECK(freed_count(&buf2) == 1);

	/* Quarantine emptied and poison cleared: a second drain frees nothing. */
	g_free_calls = 0;
	spdk_kv_fence_drain(&f, counting_free);
	CHECK(g_free_calls == 0);
	CHECK(!spdk_kv_fence_poisoned(&f));
}

/*
 * End-to-end fenced flow: op A times out (poison + quarantine its buffer), the
 * next op is refused, and only the teardown releases the buffer -- so no op ever
 * runs against a live tracker and no freed buffer is exposed to DMA.
 */
static void
test_fenced_flow(void)
{
	struct spdk_kv_fence f;
	struct spdk_kv_op_tag tag;
	int staging; /* stand-in for A's staging buffer */

	g_free_calls = 0;
	spdk_kv_fence_init(&f);

	CHECK(spdk_kv_fence_begin(&f, &tag)); /* submit op A */
	spdk_kv_fence_poison(&f);             /* poll timed out */
	/* Backend releases the staging buffer: poisoned => quarantined, not freed. */
	CHECK(spdk_kv_fence_quarantine(&f, &staging) == 0);
	CHECK(g_free_calls == 0);

	/* No later op can be submitted onto the fenced qpair. */
	CHECK(!spdk_kv_fence_begin(&f, &tag));

	/* close(): free_io_qpair() proves the tracker dead, then drain releases. */
	spdk_kv_fence_drain(&f, counting_free);
	CHECK(freed_count(&staging) == 1);
	CHECK(!spdk_kv_fence_poisoned(&f));
}

int
main(void)
{
	test_happy_path();
	test_stale_orphan_discarded();
	test_poison_refuses_submits();
	test_quarantine_freed_once();
	test_fenced_flow();

	if (g_failures != 0) {
		fprintf(stderr, "spdk_kv_fence_test: %d check(s) FAILED\n", g_failures);
		return 1;
	}
	printf("spdk_kv_fence_test: all checks passed\n");
	return 0;
}
