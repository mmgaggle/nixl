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
 * Scope (walking skeleton): Store + Retrieve of small values in host DRAM
 * only. It carries NO backend-specific behavior, NO KV Exec, and NO long-key
 * handling. Exist/Delete/List, value auto-sizing, large-value SGL, and
 * VRAM/P2PDMA are deliberately left for later.
 *
 * This header is a plain C ABI (wrapped in extern "C") so the C++ NIXL backend
 * TU can link it while keeping the C-only SPDK headers isolated in the C TU.
 *
 * Return convention for the op functions (store/retrieve):
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

/** Opaque shim handle. */
struct spdk_kv_shim;

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
	 * The supported form is
	 *   "trtype:VFIOUSER traddr:<socket-directory>".
	 * The generic form ("trtype:PCIE traddr:<BDF>") is accepted by the
	 * parser but only VFIOUSER is currently exercised.
	 */
	const char	*transport_id;
	/** Namespace id to bind; 0 selects the first CSI==KV namespace. */
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

/** Free a buffer returned by spdk_kv_shim_dma_alloc(). Safe with NULL. */
void spdk_kv_shim_dma_free(void *buf);

/** Maximum value length (kvvml) advertised by the bound KV namespace. */
uint32_t spdk_kv_shim_max_value_len(const struct spdk_kv_shim *sh);

/** Maximum key length (kvkml) advertised by the bound KV namespace. */
uint32_t spdk_kv_shim_max_key_len(const struct spdk_kv_shim *sh);

/**
 * KV Store \c value (\c value_len bytes) under \c key. \c value must be a
 * DMA-capable buffer (from spdk_kv_shim_dma_alloc()).
 *
 * \return per the return convention documented at the top of this header.
 */
int spdk_kv_shim_store(struct spdk_kv_shim *sh, const void *key, uint8_t key_len,
		       const void *value, uint32_t value_len);

/**
 * KV Retrieve the value for \c key into \c value (\c buf_len bytes). \c value
 * must be a DMA-capable buffer. On SUCCESS \c *value_len_out (when non-NULL) is
 * set to the device's TRUE value length (completion cdw0), which may exceed
 * \c buf_len if the buffer was too small. \c *value_len_out is written ONLY on
 * success (return 0).
 *
 * \return per the return convention documented at the top of this header.
 */
int spdk_kv_shim_retrieve(struct spdk_kv_shim *sh, const void *key, uint8_t key_len,
			  void *value, uint32_t buf_len, uint32_t *value_len_out);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_KV_SHIM_H */
