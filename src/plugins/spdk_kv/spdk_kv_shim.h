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

/**
 * \file
 * Minimal, generic in-process NVMe Key-Value host (initiator) shim over the
 * SPDK NVMe driver (lib/nvme).
 *
 * This is the transport-agnostic datapath primitive for the generic SPDK_KV
 * NIXL backend. It connects to an NVMe-KV controller via a standard SPDK
 * transport ID string (e.g. "trtype:VFIOUSER traddr:<socket-dir>"), binds a
 * Key-Value namespace, and exposes synchronous Store / Retrieve primitives
 * backed by SPDK-DMA buffers and a bounded qpair poll loop.
 *
 * Scope: Store + Retrieve + Exist in host DRAM. Retrieve does value auto-sizing
 * (the completion cdw0 true-length is surfaced so a short buffer can be resized
 * and retried), and large values (up to ~64 MiB) are carried by a region-bounded
 * scatter-gather list: one data-block descriptor per 2 MiB DMA region, bounded
 * by the target's NVMF_REQ_MAX_BUFFERS (33). There is NO striping — a value past
 * the single-op bound is rejected, never split (see the bound macros below). It
 * carries NO backend-specific behavior, NO KV Exec, and NO long-key
 * handling. Delete/List and VRAM/P2PDMA are deliberately left for later.
 *
 * This header is a plain C ABI (wrapped in extern "C") so the C++ NIXL backend
 * TU can link it while keeping the C-only SPDK headers isolated in the C TU.
 *
 * Return convention for the op functions (store/retrieve/exist):
 *   -  0 on SUCCESS (NVMe status code 0x00).
 *   -  a POSITIVE NVMe status code (sc) for a device-reported logical status
 *      when the status-code type (sct) is GENERIC (e.g. 0x85 BUFFER_TOO_SMALL,
 *      0x87 KEY_DOES_NOT_EXIST). A non-generic sct is reported as -EIO.
 *   -  a NEGATED errno for submit-/transport-level errors, including -ETIMEDOUT
 *      if the op does not complete within the per-op timeout and the negated
 *      return of the underlying submit call. The call never hangs forever.
 */

#ifndef SPDK_KV_SHIM_H
#define SPDK_KV_SHIM_H

/* Keep this public C ABI SPDK-include-free: pull size_t / fixed-width ints /
 * bool from the standard headers so includers are not forced onto SPDK's
 * include path. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NVMe generic status codes surfaced verbatim through the op return convention,
 * mirrored here so includers do NOT need the SPDK headers to recognize them.
 * (Values from enum spdk_nvme_generic_command_status_code in nvme_spec.h.)
 */
/** Retrieve buffer too small: the stored value is longer than the host buffer.
 *  NVMe generic sc 0x85 (SPDK_NVME_SC_INVALID_VALUE_SIZE), surfaced here as
 *  BUFFER_TOO_SMALL. On this return the true length is reported for resize. */
#define SPDK_KV_SHIM_SC_BUFFER_TOO_SMALL 0x85
/** Key not present (Exist miss, or Retrieve/Delete of an absent key).
 *  NVMe generic sc 0x87 (SPDK_NVME_SC_KV_KEY_DOES_NOT_EXIST). */
#define SPDK_KV_SHIM_SC_KEY_DOES_NOT_EXIST 0x87

/*
 * Region-bounded SGL parameters for large values.
 *
 * A single vfio-user DMA region is one 2 MiB hugepage, and the target maps each
 * region independently — so a single NVMe SGL data-block descriptor must not
 * cross a 2 MiB region boundary. A large value is therefore described by one
 * region-bounded data block per 2 MiB region. The number of regions is bounded by the target's
 * NVMF_REQ_MAX_BUFFERS = SPDK_NVMF_MAX_SGL_ENTRIES*2+1 = 33, capping a single
 * KV op at ~64 MiB.
 *
 * NO STRIPING: a value that would need more than the region budget is out of
 * scope for this generic plugin (the KV-cache use case sizes its blocks to fit
 * one op) and is REJECTED, never split across ops. SPDK_KV_SHIM_MAX_VALUE_LEN is
 * the largest value that always fits the 33-region budget even for a worst-case
 * region-unaligned buffer (a partial first region + 32 full regions) = 64 MiB,
 * which also matches the vfio-user target's default max_io_size.
 */
#define SPDK_KV_SHIM_DMA_REGION      (2ULL * 1024 * 1024)
#define SPDK_KV_SHIM_MAX_SGL_REGIONS 33u
#define SPDK_KV_SHIM_MAX_VALUE_LEN \
	((uint32_t)((SPDK_KV_SHIM_MAX_SGL_REGIONS - 1u) * SPDK_KV_SHIM_DMA_REGION))

/** Opaque shim handle. */
struct spdk_kv_shim;

/**
 * Namespace kind to bind (ratified option (b): ONE namespace per engine,
 * whose command set is chosen by an init param). One controller may present
 * both a KV and a block namespace; an agent that needs both opens two shims.
 */
enum spdk_kv_shim_ns_kind {
	/** Bind a CSI==KV namespace; enables Store/Retrieve/Exist.
	 *  This is 0 so a zero-initialized opts keeps the historical KV behavior. */
	SPDK_KV_SHIM_NS_KIND_KV = 0,
	/** Bind a CSI==NVM (block) namespace; enables LBA read/write. */
	SPDK_KV_SHIM_NS_KIND_BLOCK = 1,
};

/**
 * Size-versioned open options. Callers MUST set \c opts_size to
 * sizeof(struct spdk_kv_shim_opts) before calling spdk_kv_shim_open() so the
 * shim can stay ABI-compatible as fields are added.
 */
struct spdk_kv_shim_opts {
	/** Size of this struct as known to the caller. Must be set first. */
	size_t		opts_size;
	/** SPDK env name (used only when init_env is true). May be NULL. */
	const char	*name;
	/**
	 * SPDK transport ID string, parsed with spdk_nvme_transport_id_parse().
	 * The datapath is transport-agnostic: the same open/probe/attach/bind path
	 * drives either
	 *   "trtype:VFIOUSER traddr:<socket-directory>"  (an SPDK vfio-user target)
	 * or
	 *   "trtype:PCIE traddr:<BDF>"                    (a real NVMe controller
	 *                                                  bound to vfio-pci),
	 * selected purely by this string with no code fork. PCIE device-mode is
	 * exercised by run_block_pcie.sh against a scratch NVMe namespace.
	 */
	const char	*transport_id;
	/**
	 * Namespace id to bind; 0 selects the first namespace matching \c ns_kind.
	 * When nonzero the requested namespace must itself be of that kind.
	 */
	uint32_t	nsid;
	/**
	 * When true, the shim calls spdk_env_init() in open() and
	 * spdk_env_fini() in close(). When false, the caller (host/agent) owns
	 * the SPDK env and must have initialized it already.
	 *
	 * IMPORTANT (single-instance / single-lifetime): DPDK cannot
	 * re-initialize the SPDK env within one process, so an init_env=true
	 * shim initializes the process env exactly ONCE for its whole lifetime.
	 * After spdk_kv_shim_close() releases it (spdk_env_fini()), a second
	 * init_env=true open in the same process fails. The env is brought up
	 * with no_huge=true (IOVA=VA) and a 512 MB heap so an unprivileged
	 * in-process host works without reserved hugepages; this path is for
	 * standalone tests. The init_env=false path (production; host owns the
	 * env) may be opened/closed repeatedly.
	 */
	bool		init_env;
	/**
	 * Which namespace kind to bind (option (b), one namespace per engine).
	 * SPDK_KV_SHIM_NS_KIND_KV (the 0 default) preserves the historical KV
	 * datapath; SPDK_KV_SHIM_NS_KIND_BLOCK binds a CSI==NVM namespace and
	 * enables spdk_kv_shim_read()/spdk_kv_shim_write().
	 */
	enum spdk_kv_shim_ns_kind ns_kind;
};

/**
 * Open a shim: parse the transport ID, probe/attach the controller, bind the
 * KV namespace, allocate an I/O qpair, and cache the KV namespace key/value
 * max lengths.
 *
 * \param opts Size-versioned options (opts_size and transport_id required).
 * \param out  Receives the new shim handle. Always written: set to NULL on
 *             entry so on any failure *out is NULL (never left stale).
 *
 * \return 0 on success, a negated errno on failure.
 */
int spdk_kv_shim_open(const struct spdk_kv_shim_opts *opts, struct spdk_kv_shim **out);

/**
 * Close a shim opened by spdk_kv_shim_open(). Safe to call with NULL. For an
 * init_env=true shim this also calls spdk_env_fini() (see the single-lifetime
 * constraint above).
 */
void spdk_kv_shim_close(struct spdk_kv_shim *sh);

/** Allocate a DMA-capable buffer of \c len bytes (zeroed). NULL on failure. */
void *spdk_kv_shim_dma_alloc(size_t len);

/**
 * Allocate a DMA-capable buffer of \c len bytes (zeroed) aligned to \c align
 * bytes (a power of two). NULL on failure. The block datapath aligns its
 * staging buffer to a 2 MiB DMA region (SPDK_KV_SHIM_DMA_REGION) so each 2 MiB
 * span becomes its own region-bounded data-block descriptor and no descriptor
 * straddles two independently-mapped vfio-user regions.
 */
void *spdk_kv_shim_dma_alloc_aligned(size_t len, size_t align);

/** Free a buffer returned by spdk_kv_shim_dma_alloc[_aligned](). Safe with NULL.
 *  Use this ONLY for buffers not tied to an in-flight op (e.g. cleaning up after
 *  an alloc failure). To release a per-op STAGING buffer after an op returned,
 *  use spdk_kv_shim_release_io_buf() so a timed-out op's still-live DMA tracker
 *  cannot be left pointing at freed memory. */
void spdk_kv_shim_dma_free(void *buf);

/**
 * Release a per-op STAGING buffer (from spdk_kv_shim_dma_alloc[_aligned]()) once
 * the op that used it has returned. Normally this frees \c buf immediately, but
 * if the op timed out or the qpair transport-failed -- leaving its DMA tracker
 * possibly still live -- the shim is POISONED and \c buf is QUARANTINED instead
 * of freed, then released at the fencing teardown in spdk_kv_shim_close(). This
 * prevents a recovered target from DMA-ing into a freed buffer (a use-after-free
 * that would silently corrupt caller memory). Safe with \c buf == NULL. After a
 * poisoning error the shim refuses further ops (returns -ESHUTDOWN) until it is
 * closed.
 */
void spdk_kv_shim_release_io_buf(struct spdk_kv_shim *sh, void *buf);

/**
 * Make a caller-owned host region [\c vaddr, \c vaddr + \c len) usable DIRECTLY
 * as the value buffer for spdk_kv_shim_store/retrieve/read/write -- with NO
 * staging copy (zero-copy datapath) -- registering it with SPDK if needed. The
 * region-bounded SGL then walks the caller's buffer in place.
 *
 * DMA reachability is TRANSPORT-SPECIFIC, so this takes the shim handle:
 *   - A vfio-user target maps client memory BY FILE DESCRIPTOR, so only
 *     fd-backed memory (SPDK-DMA / hugepage / memfd, or a dma-buf) is reachable;
 *     ordinary anonymous DRAM is NOT, no matter that spdk_mem_register() accepts
 *     it (spdk_mem_register swallows the failed vfio-user DMA-map notify and
 *     still returns 0 -- using such a region would silently transfer nothing).
 *   - A PCIE/IOMMU controller can reach any vtophys-translatable (pinned) DRAM.
 * This routine registers as needed and then VERIFIES real reachability for the
 * shim's transport, rolling back a registration that did not take.
 *
 * \return  1 if the region was ALREADY DMA-reachable (e.g. caller-provided
 *            SPDK-DMA memory); it was NOT registered here, so the caller must
 *            NOT spdk_kv_shim_mem_unregister() it.
 * \return  0 if the region was newly registered AND verified reachable; the
 *            caller OWNS the registration and MUST release it with
 *            spdk_kv_shim_mem_unregister() when done.
 * \return <0 (negated errno) if the region could not be made reachable
 *            (-EINVAL: invalid args, or not 4 KiB-aligned; -ENOTSUP: registered
 *            but the transport cannot reach it, e.g. non-fd-backed DRAM over
 *            vfio-user -- the registration was rolled back). The caller MUST
 *            stage-copy through a spdk_kv_shim_dma_alloc() buffer. Registration
 *            is an optimization, never a correctness requirement.
 *
 * Release an OWNED registration with spdk_kv_shim_mem_unregister(); that call is
 * env-global (takes no shim handle), so the caller passes back the same
 * (\c vaddr, \c len).
 */
int spdk_kv_shim_mem_register(struct spdk_kv_shim *sh, void *vaddr, size_t len);

/**
 * Release a registration that spdk_kv_shim_mem_register() reported as OWNED
 * (return 0). Do NOT call for a region it reported as already-reachable
 * (return 1). The caller MUST ensure no DMA to/from the region is in flight.
 * Safe with \c vaddr == NULL or \c len == 0 (no-op). \c vaddr / \c len must
 * match the owned registration. Env-global (no shim handle), matching
 * spdk_kv_shim_dma_free().
 *
 * \return 0 on success, a negated errno on failure.
 */
int spdk_kv_shim_mem_unregister(void *vaddr, size_t len);

/** Maximum value length (kvvml) advertised by the bound KV namespace. */
uint32_t spdk_kv_shim_max_value_len(const struct spdk_kv_shim *sh);

/** Maximum key length (kvkml) advertised by the bound KV namespace. */
uint32_t spdk_kv_shim_max_key_len(const struct spdk_kv_shim *sh);

/**
 * Largest value length transferable in a single op: the region-bounded SGL
 * bound (SPDK_KV_SHIM_MAX_VALUE_LEN, ~64 MiB), further clamped to the
 * namespace-advertised max value length (kvvml) when that is smaller and
 * nonzero. A Store/Retrieve above this is REJECTED (-EFBIG), NOT striped.
 */
uint32_t spdk_kv_shim_max_value_len_op(const struct spdk_kv_shim *sh);

/**
 * KV Store \c value (\c value_len bytes) under \c key. \c value must be a
 * DMA-capable buffer (from spdk_kv_shim_dma_alloc()). Large values are carried
 * by a region-bounded SGL (one data-block per 2 MiB region); a value larger
 * than spdk_kv_shim_max_value_len_op() is rejected with -EFBIG (NOT striped).
 *
 * \return per the return convention documented at the top of this header.
 */
int spdk_kv_shim_store(struct spdk_kv_shim *sh, const void *key, uint8_t key_len,
		       const void *value, uint32_t value_len);

/**
 * KV Retrieve the value for \c key into \c value (\c buf_len bytes). \c value
 * must be a DMA-capable buffer. Large buffers are described by a region-bounded
 * SGL (one data-block per 2 MiB region); a \c buf_len larger than
 * spdk_kv_shim_max_value_len_op() is rejected with -EFBIG (NOT striped).
 *
 * Value auto-sizing: the completion cdw0 reports the device's TRUE stored value
 * length, which \c *value_len_out (when non-NULL) receives on both of the
 * value-bearing returns below so a short buffer can be resized and retried:
 *   - return 0 (SUCCESS): the whole value fit; \c *value_len_out is the true
 *     length and is <= \c buf_len (the first \c *value_len_out bytes are valid).
 *   - return SPDK_KV_SHIM_SC_BUFFER_TOO_SMALL (0x85): the stored value is longer
 *     than \c buf_len; \c *value_len_out is the true length (> \c buf_len) and
 *     the buffer holds at most \c buf_len bytes of a longer value (treat the
 *     contents as unusable). The caller resizes to \c *value_len_out and
 *     re-Retrieves (which builds a larger, still region-bounded SGL). Devices
 *     signal a short buffer two ways -- SUCCESS with cdw0 > buf_len, or 0x85
 *     directly -- and BOTH are normalized to this 0x85 return so the caller has
 *     one contract.
 * On any other return (absent key 0x87, other device sc, or a negated errno)
 * \c *value_len_out is left untouched.
 *
 * \return per the return convention documented at the top of this header.
 */
int spdk_kv_shim_retrieve(struct spdk_kv_shim *sh, const void *key, uint8_t key_len,
			  void *value, uint32_t buf_len, uint32_t *value_len_out);

/**
 * KV Exist: query whether \c key is present. This maps NIXL queryMem / QUERY to
 * the NVMe-KV Exist op; it transfers NO value data (cache hit/miss only).
 *
 * \return 0 if the key exists (hit); SPDK_KV_SHIM_SC_KEY_DOES_NOT_EXIST (0x87)
 * if absent (miss); another positive NVMe sc for a device error; a negated
 * errno for submit-/transport-level errors -- per the return convention above.
 */
int spdk_kv_shim_exist(struct spdk_kv_shim *sh, const void *key, uint8_t key_len);

/* --------------------------------------------------------------------------
 * Block (CSI==NVM) datapath.
 *
 * Available only on a shim opened with ns_kind == SPDK_KV_SHIM_NS_KIND_BLOCK.
 * Addressing is by LBA (sector), not by key: the caller converts a byte length
 * to an LBA count using spdk_kv_shim_sector_size() and bounds the range with
 * spdk_kv_shim_num_sectors(). A range up to ~64 MiB rides a single op via the
 * SAME region-bounded SGL as the KV large-value path (one data-block descriptor
 * per 2 MiB DMA region, bounded by the 33-region budget); a range past
 * spdk_kv_shim_max_block_len_op() is REJECTED (-EFBIG), never striped.
 * ------------------------------------------------------------------------ */

/** Logical block (sector) size in bytes of the bound block namespace, or 0 if
 *  the shim is not block-bound. */
uint32_t spdk_kv_shim_sector_size(const struct spdk_kv_shim *sh);

/** Number of logical blocks (sectors) in the bound block namespace, or 0 if
 *  the shim is not block-bound. LBA + lba_count must stay <= this. */
uint64_t spdk_kv_shim_num_sectors(const struct spdk_kv_shim *sh);

/**
 * Largest block transfer (in bytes) carryable in a single op: the region-bounded
 * SGL budget (SPDK_KV_SHIM_MAX_VALUE_LEN, ~64 MiB), mirroring
 * spdk_kv_shim_max_value_len_op() for the block path. Returns 0 for a non-block
 * shim. A read/write above this is REJECTED (-EFBIG), NOT striped.
 */
uint32_t spdk_kv_shim_max_block_len_op(const struct spdk_kv_shim *sh);

/**
 * Block write: copy \c lba_count sectors from the DMA-capable \c buf to the
 * namespace starting at \c lba (spdk_nvme_ns_cmd_writev with the region-bounded
 * SGL). \c buf must come from spdk_kv_shim_dma_alloc[_aligned](). \c lba_count
 * must be nonzero and [\c lba, \c lba + lba_count) must stay within
 * spdk_kv_shim_num_sectors(). A transfer larger than
 * spdk_kv_shim_max_block_len_op() is rejected with -EFBIG (NOT striped).
 *
 * \return per the return convention documented at the top of this header
 *         (0 on success; -EINVAL on a bad/out-of-range request; -EFBIG when
 *         past the single-op bound; -ENXIO etc.).
 */
int spdk_kv_shim_write(struct spdk_kv_shim *sh, const void *buf, uint64_t lba,
		       uint32_t lba_count);

/**
 * Block read: copy \c lba_count sectors from the namespace starting at \c lba
 * into the DMA-capable \c buf (spdk_nvme_ns_cmd_readv with the region-bounded
 * SGL). Same buffer/bounds/size rules as spdk_kv_shim_write().
 *
 * \return per the return convention documented at the top of this header.
 */
int spdk_kv_shim_read(struct spdk_kv_shim *sh, void *buf, uint64_t lba,
		      uint32_t lba_count);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_KV_SHIM_H */
