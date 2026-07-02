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
 * Implementation of the minimal generic NVMe-KV host shim over SPDK lib/nvme.
 *
 * Lifecycle: parse a transport ID, spdk_nvme_probe()/attach the controller,
 * find the first CSI==KV namespace (or the requested nsid), alloc an io qpair,
 * cache the KV key/value max lengths, then submit each op and poll the qpair to
 * completion capturing sct/sc/cdw0. This mirrors the idioms of SPDK's KV host
 * test harness but stays generic and backend-agnostic.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/nvme.h"
#include "spdk/nvme_kv.h"

#include "spdk_kv_shim.h"

/*
 * Per-op completion budget. Bounds how long an in-flight KV command may run
 * before poll_to_completion() gives up with -ETIMEDOUT so a dead/wedged target
 * cannot hang the datapath forever. A healthy in-memory or vfio-user KV op is
 * sub-millisecond; 20s is generous headroom for a slow round-trip while still
 * bounding a dead target to seconds.
 */
#define SPDK_KV_SHIM_OP_TIMEOUT_S 20u

struct spdk_kv_shim {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	struct spdk_nvme_qpair	*qpair;
	/* Cached KV namespace capabilities. */
	uint32_t		kvvml;	/* max value length */
	uint32_t		kvkml;	/* max key length */
	/* Whether this shim owns the SPDK env (called spdk_env_init). */
	bool			owns_env;
	/*
	 * Single in-flight op completion state. Ops are strictly synchronous
	 * (submit, then poll to completion before returning), and the shim uses
	 * one qpair from one thread, so at most one op is outstanding at a time.
	 * The completion callback records into these fields and the submitting
	 * op reads them back after the poll.
	 *
	 * NOTE (deferred robustness): if an op ever times out or the qpair
	 * transport-fails, vfio-user does not abort the outstanding hardware
	 * tracker on disconnect/reconnect, so a late "orphan" completion could
	 * fire during a later op's poll and be mis-recorded. The production
	 * datapath needs per-op identity-token + reconnect handling; that
	 * hardening is out of scope for this walking skeleton, which targets a
	 * healthy in-memory/vfio-user target.
	 */
	volatile bool		op_done;
	volatile uint8_t	op_sct;
	volatile uint8_t	op_sc;
	volatile uint32_t	op_cdw0;
};

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_kv_shim *sh = cb_ctx;

	sh->ctrlr = ctrlr;
}

static void
io_complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
	struct spdk_kv_shim *sh = arg;

	sh->op_sct = cpl->status.sct;
	sh->op_sc = cpl->status.sc;
	sh->op_cdw0 = cpl->cdw0;
	sh->op_done = true;
}

/*
 * Poll the qpair until the in-flight command completes, the qpair fails at the
 * transport layer (-ENXIO), or the per-op timeout expires (-ETIMEDOUT).
 * Returns 0 on completion (caller reads status_to_rc()).
 */
static int
poll_to_completion(struct spdk_kv_shim *sh)
{
	uint64_t deadline = spdk_get_ticks() +
			    (uint64_t)SPDK_KV_SHIM_OP_TIMEOUT_S * spdk_get_ticks_hz();

	while (!sh->op_done) {
		int32_t n = spdk_nvme_qpair_process_completions(sh->qpair, 0);

		if (n < 0) {
			return n;
		}
		if (!sh->op_done && spdk_get_ticks() >= deadline) {
			return -ETIMEDOUT;
		}
	}
	return 0;
}

/*
 * Translate the captured completion into the public return convention:
 *   0        -> SUCCESS
 *   positive -> device-reported NVMe status code (sct == GENERIC)
 *   negative -> transport/other error (negated errno)
 */
static int
status_to_rc(struct spdk_kv_shim *sh)
{
	if (sh->op_sct != SPDK_NVME_SCT_GENERIC) {
		return -EIO;
	}
	return (int)sh->op_sc;
}

int
spdk_kv_shim_open(const struct spdk_kv_shim_opts *opts, struct spdk_kv_shim **out)
{
	struct spdk_kv_shim *sh;
	struct spdk_nvme_transport_id trid = {};
	const struct spdk_nvme_kv_ns_data *kv_ns_data;
	uint32_t nsid;
	int rc;

	if (opts == NULL || out == NULL) {
		return -EINVAL;
	}
	/* Define the out-param up front so every failure path leaves the
	 * caller's handle defined rather than stale. */
	*out = NULL;
	if (opts->opts_size < sizeof(struct spdk_kv_shim_opts)) {
		return -EINVAL;
	}
	if (opts->transport_id == NULL) {
		return -EINVAL;
	}

	sh = calloc(1, sizeof(*sh));
	if (sh == NULL) {
		return -ENOMEM;
	}

	if (opts->init_env) {
		struct spdk_env_opts env_opts;

		env_opts.opts_size = sizeof(env_opts);
		spdk_env_opts_init(&env_opts);
		env_opts.name = opts->name ? opts->name : "spdk_kv_shim";
		/* As an in-process host we may run unprivileged without reserved
		 * hugepages. no_huge selects IOVA=VA so DMA works without
		 * root/PA access; mem_size bounds the no-huge heap. */
		env_opts.no_huge = true;
		env_opts.mem_size = 512;
		if (spdk_env_init(&env_opts) < 0) {
			free(sh);
			return -EFAULT;
		}
		sh->owns_env = true;
	}

	/* Parse the standard SPDK transport ID (e.g. "trtype:VFIOUSER
	 * traddr:<dir>"). This is what makes the datapath transport-agnostic. */
	if (spdk_nvme_transport_id_parse(&trid, opts->transport_id) != 0) {
		rc = -EINVAL;
		goto err_env;
	}

	if (spdk_nvme_probe(&trid, sh, probe_cb, attach_cb, NULL) != 0 ||
	    sh->ctrlr == NULL) {
		/* probe may have attached a controller before failing; route
		 * through err_detach (NULL-guarded) so it is not leaked. */
		rc = -ENODEV;
		goto err_detach;
	}

	/* Bind the requested namespace, or the first CSI==KV ns when nsid==0. */
	if (opts->nsid != 0) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(sh->ctrlr, opts->nsid);

		if (ns != NULL && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_KV) {
			sh->ns = ns;
		}
	} else {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(sh->ctrlr); nsid != 0;
		     nsid = spdk_nvme_ctrlr_get_next_active_ns(sh->ctrlr, nsid)) {
			struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(sh->ctrlr, nsid);

			if (ns != NULL && spdk_nvme_ns_get_csi(ns) == SPDK_NVME_CSI_KV) {
				sh->ns = ns;
				break;
			}
		}
	}

	if (sh->ns == NULL) {
		rc = -ENOENT;
		goto err_detach;
	}

	kv_ns_data = spdk_nvme_kv_ns_get_data(sh->ns);
	if (kv_ns_data == NULL) {
		rc = -EPROTO;
		goto err_detach;
	}
	/*
	 * A KV namespace can advertise up to 16 formats; the ACTIVE one is
	 * selected by kvfc.kvfi (4-bit), so read the key/value max lengths from
	 * kvf[kvfc.kvfi], NOT kvf[0]. (Matches SPDK's own canonical reader in
	 * app/spdk_nvme_perf/perf.c.) kvfi is 4 bits and kvf[] has 16 entries,
	 * so the index is always in bounds.
	 */
	{
		uint8_t kvfi = kv_ns_data->kvfc.kvfi;

		sh->kvkml = kv_ns_data->kvf[kvfi].kvkml;
		sh->kvvml = kv_ns_data->kvf[kvfi].kvvml;
	}

	sh->qpair = spdk_nvme_ctrlr_alloc_io_qpair(sh->ctrlr, NULL, 0);
	if (sh->qpair == NULL) {
		rc = -ENOMEM;
		goto err_detach;
	}

	*out = sh;
	return 0;

err_detach:
	if (sh->ctrlr != NULL) {
		spdk_nvme_detach(sh->ctrlr);
	}
err_env:
	if (sh->owns_env) {
		spdk_env_fini();
	}
	free(sh);
	return rc;
}

void
spdk_kv_shim_close(struct spdk_kv_shim *sh)
{
	if (sh == NULL) {
		return;
	}
	if (sh->qpair != NULL) {
		spdk_nvme_ctrlr_free_io_qpair(sh->qpair);
	}
	if (sh->ctrlr != NULL) {
		spdk_nvme_detach(sh->ctrlr);
	}
	if (sh->owns_env) {
		spdk_env_fini();
	}
	free(sh);
}

void *
spdk_kv_shim_dma_alloc(size_t len)
{
	return spdk_dma_zmalloc(len, 0, NULL);
}

void
spdk_kv_shim_dma_free(void *buf)
{
	spdk_dma_free(buf);
}

uint32_t
spdk_kv_shim_max_value_len(const struct spdk_kv_shim *sh)
{
	return sh ? sh->kvvml : 0;
}

uint32_t
spdk_kv_shim_max_key_len(const struct spdk_kv_shim *sh)
{
	return sh ? sh->kvkml : 0;
}

int
spdk_kv_shim_store(struct spdk_kv_shim *sh, const void *key, uint8_t key_len,
		   const void *value, uint32_t value_len)
{
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	sh->op_done = false;
	rc = spdk_nvme_kv_store(sh->ns, sh->qpair, key, key_len, value, value_len,
				io_complete, sh, 0);
	if (rc != 0) {
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	return status_to_rc(sh);
}

int
spdk_kv_shim_retrieve(struct spdk_kv_shim *sh, const void *key, uint8_t key_len,
		      void *value, uint32_t buf_len, uint32_t *value_len_out)
{
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	sh->op_done = false;
	rc = spdk_nvme_kv_retrieve(sh->ns, sh->qpair, key, key_len, value, buf_len,
				   io_complete, sh, 0);
	if (rc != 0) {
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	rc = status_to_rc(sh);
	/* On SUCCESS, cdw0 carries the device's TRUE value length. */
	if (rc == 0 && value_len_out != NULL) {
		*value_len_out = sh->op_cdw0;
	}
	return rc;
}
