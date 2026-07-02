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

#ifndef SPDK_KV_BACKEND_H
#define SPDK_KV_BACKEND_H

#include <cstdint>
#include <string>
#include <vector>

#include "backend/backend_engine.h"
#include "spdk_kv_key.h" // spdkKvKeyFromBlobId (SPDK-free key mapping)

// Forward declaration of the opaque SPDK KV shim handle (C ABI).
struct spdk_kv_shim;

/**
 * nixlSpdkKvEngine: a clean-sheet, transport-agnostic, backend-agnostic NIXL
 * backend that speaks the ratified NVMe Key-Value command set over the SPDK
 * NVMe driver (lib/nvme). It works against ANY SPDK NVMe-KV target (an
 * in-memory kvdev, a librados-backed kvdev, a DPU-presented VF, ...); it
 * carries NO backend-specific behavior, NO KV Exec, and NO long-key path.
 *
 * SCOPE -- walking skeleton:
 *   - Store (NIXL_WRITE) and Retrieve (NIXL_READ) of SMALL values in host DRAM.
 *   - 16-byte inline keys taken verbatim (opaque); no lineage parsing.
 *   - DRAM only. VRAM / P2PDMA is deferred.
 *   - Small values only. Large-value region-bounded SGL + value auto-sizing is
 *     deferred.
 *   - NO Exist/QUERY, NO Delete/List (deferred), NO KV Exec (out of scope
 *     for this generic plugin by design).
 *
 * Memory-type mapping (storage-backend shape, mirrors OBJ):
 *   - local  source/destination : DRAM_SEG (host DRAM)
 *   - remote key-addressed blob  : OBJ_SEG (the NVMe-KV key space)
 *
 * Operation mapping:
 *   - NIXL_WRITE (local DRAM -> remote) becomes a KV Store
 *   - NIXL_READ  (remote -> local DRAM) becomes a KV Retrieve
 *
 * The remote OBJ_SEG descriptor carries the NIXL block identifier in its
 * metaInfo blob. The engine takes those bytes VERBATIM as the NVMe-KV key
 * (spdkKvKeyFromBlobId) -- opaque, 1..16 bytes, no hash, no truncation -- and
 * stores the key in the per-descriptor metadata so transfers read it back from
 * the descriptor. An empty or over-16-byte key is rejected
 * (NIXL_ERR_INVALID_PARAM).
 *
 * Transport: the "transport_id" custom param is a standard SPDK transport ID
 * string (e.g. "trtype:VFIOUSER traddr:<socket-dir>"), which is what makes the
 * datapath transport-agnostic. For ergonomics a bare socket dir may be passed
 * as "vfu_addr"/"socket" and is wrapped into a VFIOUSER transport ID.
 *
 * Staging / zero-copy approach (DOCUMENTED):
 *   SPDK Store/Retrieve require the value buffer to be SPDK-DMA memory. User
 *   DRAM registered through registerMem() is ordinary host memory and is
 *   generally NOT DMA-capable, so this skeleton STAGES through a per-request
 *   shim DMA buffer and copies:
 *     - WRITE: memcpy(user DRAM -> DMA buf) then Store(key, DMA buf)
 *     - READ : Retrieve(key, DMA buf) then memcpy(DMA buf -> user DRAM)
 *   A future revision (VRAM/P2PDMA, large-value SGL) registers the user/GPU
 *   buffer directly and skips the copy.
 */
class nixlSpdkKvEngine : public nixlBackendEngine {
public:
    explicit nixlSpdkKvEngine(const nixlBackendInitParams *init_params);
    ~nixlSpdkKvEngine() override;

    bool
    supportsRemote() const override {
        return false;
    }

    bool
    supportsLocal() const override {
        return true;
    }

    bool
    supportsNotif() const override {
        return false;
    }

    nixl_mem_list_t
    getSupportedMems() const override;

    nixl_status_t
    registerMem(const nixlBlobDesc &mem, const nixl_mem_t &nixl_mem, nixlBackendMD *&out) override;

    nixl_status_t
    deregisterMem(nixlBackendMD *meta) override;

    nixl_status_t
    connect(const std::string &remote_agent) override {
        return NIXL_SUCCESS;
    }

    nixl_status_t
    disconnect(const std::string &remote_agent) override {
        return NIXL_SUCCESS;
    }

    nixl_status_t
    unloadMD(nixlBackendMD *input) override {
        return NIXL_SUCCESS;
    }

    nixl_status_t
    loadLocalMD(nixlBackendMD *input, nixlBackendMD *&output) override {
        output = input;
        return NIXL_SUCCESS;
    }

    nixl_status_t
    prepXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    postXfer(const nixl_xfer_op_t &operation,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &remote_agent,
             nixlBackendReqH *&handle,
             const nixl_opt_b_args_t *opt_args = nullptr) const override;

    nixl_status_t
    checkXfer(nixlBackendReqH *handle) const override;

    nixl_status_t
    releaseReqH(nixlBackendReqH *handle) const override;

private:
    // Ratified maximum NVMe-KV inline key length (bytes).
    static constexpr uint8_t kMaxKeyLen = 16;

    // The SPDK KV shim handle (owns the controller attach + qpair).
    spdk_kv_shim *shim_ = nullptr;

    // Effective key length: min(kMaxKeyLen, kvkml advertised by the namespace).
    uint8_t maxKeyLen_ = kMaxKeyLen;
};

#endif // SPDK_KV_BACKEND_H
