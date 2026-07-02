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
#include "spdk/nvme_spec.h"

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
	/* Bound namespace kind (option (b): one kind per shim). */
	bool			is_block;
	/* Cached KV namespace capabilities (KV shim). */
	uint32_t		kvvml;	/* max value length */
	uint32_t		kvkml;	/* max key length */
	/* Cached block namespace capabilities (block shim). */
	uint32_t		sector_size;	/* logical block size (bytes) */
	uint64_t		num_sectors;	/* namespace capacity (sectors) */
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
	/*
	 * Region-bounded SGL iterator state for the in-flight op. lib/nvme drives
	 * kv_reset_sgl()/kv_next_sge() (below) with this shim as the callback arg to
	 * walk the value buffer one 2 MiB-region-bounded segment at a time. Single
	 * in-flight op, single qpair, single thread — so one iterator suffices.
	 */
	const uint8_t		*sgl_base;
	uint32_t		sgl_total;
	uint32_t		sgl_off;
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
	enum spdk_nvme_csi want_csi;
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

	/*
	 * Bind ONE namespace whose command set is chosen by ns_kind (option (b)):
	 * KV -> CSI==KV, BLOCK -> CSI==NVM. The mem-type dispatch in the backend
	 * still selects the op per transfer; ns_kind only scopes which namespace
	 * this shim binds.
	 */
	sh->is_block = (opts->ns_kind == SPDK_KV_SHIM_NS_KIND_BLOCK);
	want_csi = sh->is_block ? SPDK_NVME_CSI_NVM : SPDK_NVME_CSI_KV;

	/* Bind the requested namespace, or the first ns of the wanted CSI when nsid==0. */
	if (opts->nsid != 0) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(sh->ctrlr, opts->nsid);

		if (ns != NULL && spdk_nvme_ns_get_csi(ns) == want_csi) {
			sh->ns = ns;
		}
	} else {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(sh->ctrlr); nsid != 0;
		     nsid = spdk_nvme_ctrlr_get_next_active_ns(sh->ctrlr, nsid)) {
			struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(sh->ctrlr, nsid);

			if (ns != NULL && spdk_nvme_ns_get_csi(ns) == want_csi) {
				sh->ns = ns;
				break;
			}
		}
	}

	if (sh->ns == NULL) {
		rc = -ENOENT;
		goto err_detach;
	}

	if (sh->is_block) {
		/*
		 * Block namespace: cache the sector geometry the datapath needs to
		 * turn a byte length into an LBA count and to bounds-check a range.
		 */
		sh->sector_size = spdk_nvme_ns_get_sector_size(sh->ns);
		sh->num_sectors = spdk_nvme_ns_get_num_sectors(sh->ns);
		if (sh->sector_size == 0) {
			rc = -EPROTO;
			goto err_detach;
		}
	} else {
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
		uint8_t kvfi = kv_ns_data->kvfc.kvfi;

		sh->kvkml = kv_ns_data->kvf[kvfi].kvkml;
		sh->kvvml = kv_ns_data->kvf[kvfi].kvvml;
	}

	{
		/*
		 * Disable the PCIe/vfio-user SGL merge so lib/nvme emits ONE SGL
		 * data-block descriptor per region-bounded segment kv_next_sge()
		 * yields (it would otherwise coalesce physically/IOVA-contiguous
		 * segments into one descriptor). The vfio-user target maps each 2 MiB
		 * DMA region independently, so a single descriptor must not span two
		 * regions — merging IOVA-contiguous-but-separately-registered regions
		 * would produce a region-crossing descriptor the target cannot map.
		 * This yields the region-bounded SGL, matching the raw client.
		 */
		struct spdk_nvme_io_qpair_opts qopts;

		spdk_nvme_ctrlr_get_default_io_qpair_opts(sh->ctrlr, &qopts, sizeof(qopts));
		qopts.disable_pcie_sgl_merge = true;
		sh->qpair = spdk_nvme_ctrlr_alloc_io_qpair(sh->ctrlr, &qopts, sizeof(qopts));
	}
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

void *
spdk_kv_shim_dma_alloc_aligned(size_t len, size_t align)
{
	return spdk_dma_zmalloc(len, align, NULL);
}

void
spdk_kv_shim_dma_free(void *buf)
{
	spdk_dma_free(buf);
}

uint32_t
spdk_kv_shim_sector_size(const struct spdk_kv_shim *sh)
{
	return (sh != NULL && sh->is_block) ? sh->sector_size : 0;
}

uint64_t
spdk_kv_shim_num_sectors(const struct spdk_kv_shim *sh)
{
	return (sh != NULL && sh->is_block) ? sh->num_sectors : 0;
}

uint32_t
spdk_kv_shim_max_block_len_op(const struct spdk_kv_shim *sh)
{
	/*
	 * A block op rides the SAME region-bounded SGL as a KV value, so its
	 * single-op ceiling is the 33-region budget (SPDK_KV_SHIM_MAX_VALUE_LEN,
	 * ~64 MiB). Mirrors spdk_kv_shim_max_value_len_op for the block path; there
	 * is no block-namespace-advertised limit smaller than the budget to clamp
	 * to here (the ~64 MiB budget matches the vfio-user target max_io_size).
	 * Returns 0 for a non-block shim so BLK_SEG is unavailable there.
	 */
	if (sh == NULL || !sh->is_block) {
		return 0;
	}
	return SPDK_KV_SHIM_MAX_VALUE_LEN;
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

uint32_t
spdk_kv_shim_max_value_len_op(const struct spdk_kv_shim *sh)
{
	uint32_t cap = SPDK_KV_SHIM_MAX_VALUE_LEN;

	/* The region-bounded SGL bound (~64 MiB), further clamped to the
	 * namespace-advertised max value length when that is smaller. kvvml==0
	 * means "no advertised limit", so it does not lower the bound. */
	if (sh != NULL && sh->kvvml != 0 && sh->kvvml < cap) {
		cap = sh->kvvml;
	}
	return cap;
}

/*
 * Number of 2 MiB-region-bounded data-block descriptors needed to describe a
 * buffer of \c len bytes starting at \c base — i.e. the count of segments
 * kv_next_sge() will yield. A segment never crosses a 2 MiB boundary, so the
 * first segment runs from \c base to the next region boundary and the rest are
 * full regions (last possibly short). Mirrors nvfu_sgl_set_dptr's nseg formula.
 */
static uint32_t
kv_region_count(const void *base, uint32_t len)
{
	uint64_t addr = (uint64_t)(uintptr_t)base;
	uint64_t first = SPDK_KV_SHIM_DMA_REGION - (addr & (SPDK_KV_SHIM_DMA_REGION - 1));

	if (len == 0) {
		return 0;
	}
	if ((uint64_t)len <= first) {
		return 1;
	}
	return 1u + (uint32_t)(((uint64_t)len - first + SPDK_KV_SHIM_DMA_REGION - 1) /
			       SPDK_KV_SHIM_DMA_REGION);
}

/* SGL iterator: restart the walk at \c offset (lib/nvme may re-drive it). */
static void
kv_reset_sgl(void *cb_arg, uint32_t offset)
{
	struct spdk_kv_shim *sh = cb_arg;

	sh->sgl_off = offset;
}

/*
 * SGL iterator: hand lib/nvme the next value segment, bounded so it never
 * crosses a 2 MiB DMA-region boundary. With the qpair's SGL merge disabled,
 * lib/nvme turns each segment into its own data-block descriptor — one per
 * region — so no descriptor spans two independently-mapped target regions.
 */
static int
kv_next_sge(void *cb_arg, void **address, uint32_t *length)
{
	struct spdk_kv_shim *sh = cb_arg;
	uint64_t addr = (uint64_t)(uintptr_t)sh->sgl_base + sh->sgl_off;
	uint32_t remaining = sh->sgl_total - sh->sgl_off;
	uint32_t to_boundary =
		(uint32_t)(SPDK_KV_SHIM_DMA_REGION - (addr & (SPDK_KV_SHIM_DMA_REGION - 1)));
	uint32_t seg = remaining < to_boundary ? remaining : to_boundary;

	*address = (void *)(uintptr_t)addr;
	*length = seg;
	sh->sgl_off += seg;
	return 0;
}

/*
 * Shared Store/Retrieve datapath. Builds a raw KV command (inline key in the
 * CDW2/3/14/15 slots + CDW11.kl, transfer length in CDW10.vsize) and submits it
 * with the region-bounded SGL iterator above, so a value up to ~64 MiB rides a
 * single op as one data-block descriptor per 2 MiB region. A value that would
 * exceed the region budget is REJECTED here (-EFBIG), never striped.
 *
 * On SUCCESS \c *cdw0_out (when non-NULL) receives the completion cdw0, which
 * for Retrieve is the device's TRUE stored value length.
 */
static int
kv_xfer_sgl(struct spdk_kv_shim *sh, uint8_t opc, const void *key, uint8_t key_len,
	    void *value, uint32_t value_len, uint32_t *cdw0_out)
{
	struct spdk_nvme_cmd cmd;
	int rc;

	if (sh == NULL || key == NULL || value == NULL || value_len == 0) {
		return -EINVAL;
	}
	if (key_len < SPDK_NVME_KV_KEY_MIN_LEN || key_len > SPDK_NVME_KV_KEY_MAX_LEN) {
		return -EINVAL;
	}
	/*
	 * NO striping: reject a value that needs more than the region budget
	 * (NVMF_REQ_MAX_BUFFERS = 33) rather than splitting it across ops. The
	 * budget is also enforced pre-alloc by the backend via
	 * spdk_kv_shim_max_value_len_op(); this is the authoritative guard.
	 */
	if (kv_region_count(value, value_len) > SPDK_KV_SHIM_MAX_SGL_REGIONS) {
		return -EFBIG;
	}

	memset(&cmd, 0, sizeof(cmd));
	cmd.opc = opc;
	cmd.nsid = spdk_nvme_ns_get_id(sh->ns);
	/* CDW10: value size (Store) or host buffer size (Retrieve). */
	cmd.cdw10_bits.kv.vsize = value_len;
	/* CDW11: inline key length (Request Options 'ro' left 0). */
	cmd.cdw11_bits.kv.kl = key_len;
	/* Inline key: first 8 bytes in CDW2/3, remainder in CDW14/15. */
	memcpy((uint8_t *)&cmd.cdw2, key, key_len < 8 ? key_len : 8);
	if (key_len > 8) {
		memcpy((uint8_t *)&cmd.cdw14, (const uint8_t *)key + 8, (size_t)(key_len - 8));
	}

	sh->sgl_base = value;
	sh->sgl_total = value_len;
	sh->sgl_off = 0;

	sh->op_done = false;
	rc = spdk_nvme_ctrlr_cmd_iov_raw_with_md(sh->ctrlr, sh->qpair, &cmd, value_len,
						 NULL, io_complete, sh,
						 kv_reset_sgl, kv_next_sge);
	if (rc != 0) {
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	rc = status_to_rc(sh);
	/*
	 * The op completed; cdw0 carries the device's reported value length (the
	 * TRUE stored length for Retrieve, even when it exceeds the host buffer).
	 * Hand it back unconditionally so the Retrieve wrapper can drive value
	 * auto-sizing off the 0x00-with-cdw0>buf_len and direct-0x85 cases alike.
	 * Store passes cdw0_out == NULL and ignores it.
	 */
	if (cdw0_out != NULL) {
		*cdw0_out = sh->op_cdw0;
	}
	return rc;
}

int
spdk_kv_shim_store(struct spdk_kv_shim *sh, const void *key, uint8_t key_len,
		   const void *value, uint32_t value_len)
{
	return kv_xfer_sgl(sh, SPDK_NVME_OPC_KV_STORE, key, key_len,
			   (void *)(uintptr_t)value, value_len, NULL);
}

int
spdk_kv_shim_retrieve(struct spdk_kv_shim *sh, const void *key, uint8_t key_len,
		      void *value, uint32_t buf_len, uint32_t *value_len_out)
{
	uint32_t cdw0 = 0;
	int rc = kv_xfer_sgl(sh, SPDK_NVME_OPC_KV_RETRIEVE, key, key_len,
			     value, buf_len, &cdw0);

	/*
	 * Value auto-sizing layered over the region-bounded SGL datapath. cdw0 is
	 * the device's TRUE stored value length on the
	 * value-bearing completions. A short host buffer is signalled two ways, and
	 * we NORMALIZE both to a single BUFFER_TOO_SMALL (0x85) return so the caller
	 * has one contract:
	 *   (a) SUCCESS (sc 0x00) with cdw0 > buf_len -- the device transferred the
	 *       leading buf_len bytes and reports the full length; or
	 *   (b) 0x85 INVALID_VALUE_SIZE directly with cdw0 = the true length.
	 * In both cases the buffer holds at most buf_len bytes of a longer value, so
	 * we surface 0x85 and hand back the true length via *value_len_out; the
	 * caller resizes to it and re-Retrieves (which builds a larger, still
	 * region-bounded SGL). On any other return (absent key 0x87, other device
	 * sc, or a negated errno) *value_len_out is left untouched.
	 */
	if (rc == 0) {
		if (value_len_out != NULL) {
			*value_len_out = cdw0;
		}
		if (cdw0 > buf_len) {
			/* (a) SUCCESS but the value did not fit the buffer. */
			return SPDK_NVME_SC_INVALID_VALUE_SIZE;
		}
		return 0;
	}
	if (rc == SPDK_NVME_SC_INVALID_VALUE_SIZE) {
		/* (b) device signalled too-small directly; cdw0 is the true length. */
		if (value_len_out != NULL) {
			*value_len_out = cdw0;
		}
	}
	return rc;
}

int
spdk_kv_shim_exist(struct spdk_kv_shim *sh, const void *key, uint8_t key_len)
{
	int rc;

	if (sh == NULL) {
		return -EINVAL;
	}
	sh->op_done = false;
	rc = spdk_nvme_kv_exist(sh->ns, sh->qpair, key, key_len, io_complete, sh);
	if (rc != 0) {
		return rc < 0 ? rc : -rc;
	}
	rc = poll_to_completion(sh);
	if (rc != 0) {
		return rc;
	}
	/*
	 * status_to_rc maps a GENERIC completion to its NVMe sc: 0x00 -> 0 (the
	 * key exists / hit), 0x87 KEY_DOES_NOT_EXIST -> 0x87 (absent / miss). No
	 * value data is transferred either way.
	 */
	return status_to_rc(sh);
}

/*
 * Shared block read/write datapath. Submits lba_count sectors described by the
 * SAME region-bounded SGL as the KV large-value path: the transfer's byte length
 * (lba_count * sector_size) is walked by the shared kv_reset_sgl()/kv_next_sge()
 * iterator, one 2 MiB-region-bounded segment per data-block descriptor (the
 * qpair's PCIe SGL merge is disabled in open(), so no descriptor spans a region
 * boundary). This carries a range up to ~64 MiB (the 33-region budget) on a
 * single op via spdk_nvme_ns_cmd_readv/writev. NO striping: a range beyond the
 * region budget is REJECTED here (-EFBIG), never split. Reuses the same bounded
 * poll loop and completion capture as the KV ops.
 */
static int
blk_rw(struct spdk_kv_shim *sh, bool is_write, void *buf, uint64_t lba,
       uint32_t lba_count)
{
	uint64_t total_bytes;
	uint32_t byte_len;
	int rc;

	if (sh == NULL || buf == NULL || lba_count == 0) {
		return -EINVAL;
	}
	if (!sh->is_block || sh->sector_size == 0) {
		return -EINVAL;
	}
	/*
	 * Capacity guard: reject an out-of-range LBA range rather than submit it.
	 * lba >= num_sectors is checked first so num_sectors - lba never underflows.
	 */
	if (lba >= sh->num_sectors || (uint64_t)lba_count > sh->num_sectors - lba) {
		return -EINVAL;
	}

	/*
	 * Total transfer length in bytes drives the region-bounded SGL, exactly as
	 * the KV value length does. Compute in 64-bit and reject past the single-op
	 * bound BEFORE truncating to the uint32 the region iterator uses, so a range
	 * that would overflow uint32 (or merely exceed the ~64 MiB budget) is caught
	 * here rather than wrapping. NO striping.
	 */
	total_bytes = (uint64_t)lba_count * sh->sector_size;
	if (total_bytes > SPDK_KV_SHIM_MAX_VALUE_LEN) {
		return -EFBIG;
	}
	byte_len = (uint32_t)total_bytes;
	/*
	 * Authoritative region-budget guard (mirrors kv_xfer_sgl): reject a buffer
	 * that needs more than NVMF_REQ_MAX_BUFFERS (33) region-bounded descriptors.
	 */
	if (kv_region_count(buf, byte_len) > SPDK_KV_SHIM_MAX_SGL_REGIONS) {
		return -EFBIG;
	}

	sh->sgl_base = buf;
	sh->sgl_total = byte_len;
	sh->sgl_off = 0;

	sh->op_done = false;
	if (is_write) {
		rc = spdk_nvme_ns_cmd_writev(sh->ns, sh->qpair, lba, lba_count,
					     io_complete, sh, 0,
					     kv_reset_sgl, kv_next_sge);
	} else {
		rc = spdk_nvme_ns_cmd_readv(sh->ns, sh->qpair, lba, lba_count,
					    io_complete, sh, 0,
					    kv_reset_sgl, kv_next_sge);
	}
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
spdk_kv_shim_write(struct spdk_kv_shim *sh, const void *buf, uint64_t lba,
		   uint32_t lba_count)
{
	return blk_rw(sh, true, (void *)(uintptr_t)buf, lba, lba_count);
}

int
spdk_kv_shim_read(struct spdk_kv_shim *sh, void *buf, uint64_t lba,
		  uint32_t lba_count)
{
	return blk_rw(sh, false, buf, lba, lba_count);
}
