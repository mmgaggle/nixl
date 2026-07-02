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

#include "spdk_kv_backend.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>

#include "common/nixl_log.h"

extern "C" {
#include "spdk_kv_shim.h"
}

namespace {

// Per-descriptor metadata for an OBJ_SEG (remote KV-key) registration: the
// verbatim inline NVMe-KV key derived from the descriptor's metaInfo.
class nixlSpdkKvMetadata : public nixlBackendMD {
public:
    explicit nixlSpdkKvMetadata(std::vector<uint8_t> key)
        : nixlBackendMD(true),
          key(std::move(key)) {}
    ~nixlSpdkKvMetadata() override = default;

    std::vector<uint8_t> key;
};

// Synchronous request handle: the shim ops complete inline, so we just stash
// the final status produced by postXfer and report it in checkXfer.
class nixlSpdkKvBackendReqH : public nixlBackendReqH {
public:
    nixlSpdkKvBackendReqH() = default;
    ~nixlSpdkKvBackendReqH() override = default;

    nixl_status_t status = NIXL_IN_PROG;
    // Value auto-sizing: per-descriptor device TRUE value length recorded when a
    // READ's host buffer was too small (status == NIXL_ERR_MISMATCH). Sized to
    // the descriptor count in postXfer; 0 means "no too-small result recorded"
    // (the READ fit, or this descriptor was not reached). Read by getReqTrueLen.
    std::vector<size_t> true_lens;
};

// Parse "true"/"1"/"yes"/"on" (case-insensitive) as boolean true.
bool
parseBool(const std::string &v) {
    std::string s;
    s.reserve(v.size());
    for (char c : v) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s == "true" || s == "1" || s == "yes" || s == "on";
}

} // namespace

// -----------------------------------------------------------------------------
// nixlSpdkKvEngine
// -----------------------------------------------------------------------------

nixlSpdkKvEngine::nixlSpdkKvEngine(const nixlBackendInitParams *init_params)
    : nixlBackendEngine(init_params) {
    // Preferred config is a full SPDK transport ID string,
    // e.g. "trtype:VFIOUSER traddr:<socket-dir>". For ergonomics also accept a
    // bare socket directory and wrap it into a VFIOUSER transport ID.
    std::string transport_id;
    if (getInitParam("transport_id", transport_id) != NIXL_SUCCESS || transport_id.empty()) {
        std::string vfu_addr;
        for (const char *k : {"vfu_addr", "socket", "vfio_user_path"}) {
            if (getInitParam(k, vfu_addr) == NIXL_SUCCESS && !vfu_addr.empty()) break;
            vfu_addr.clear();
        }
        if (!vfu_addr.empty()) {
            transport_id = "trtype:VFIOUSER traddr:" + vfu_addr;
        }
    }

    if (transport_id.empty()) {
        NIXL_ERROR << "SPDK_KV: missing required custom param 'transport_id' "
                      "(a SPDK transport ID, e.g. 'trtype:VFIOUSER traddr:<socket>')";
        initErr = true;
        return;
    }

    std::string nsid_str;
    uint32_t nsid = 0; // 0 selects the first CSI==KV namespace
    if (getInitParam("nsid", nsid_str) == NIXL_SUCCESS && !nsid_str.empty()) {
        try {
            nsid = static_cast<uint32_t>(std::stoul(nsid_str));
        }
        catch (const std::exception &e) {
            NIXL_WARN << "SPDK_KV: bad nsid '" << nsid_str << "', using auto-select";
            nsid = 0;
        }
    }

    // init_env defaults to false: in production the host/agent owns the SPDK
    // env (lets multiple engines coexist in one process). Standalone tests with
    // no host env pass init_env=true so the shim brings up its own (no-hugepage,
    // single-instance) SPDK env.
    bool init_env = false;
    std::string init_env_str;
    if (getInitParam("init_env", init_env_str) == NIXL_SUCCESS && !init_env_str.empty()) {
        init_env = parseBool(init_env_str);
    }

    struct spdk_kv_shim_opts opts = {};
    opts.opts_size = sizeof(opts);
    opts.name = "nixl_spdk_kv";
    opts.transport_id = transport_id.c_str();
    opts.nsid = nsid;
    opts.init_env = init_env;

    int rc = spdk_kv_shim_open(&opts, &shim_);
    if (rc != 0 || shim_ == nullptr) {
        NIXL_ERROR << "SPDK_KV: spdk_kv_shim_open(" << transport_id << ") failed: rc=" << rc;
        shim_ = nullptr;
        initErr = true;
        return;
    }

    // Clamp the effective key length to the namespace-advertised kvkml so a key
    // always fits the device key space. kvkml==0 means the namespace reports no
    // limit; fall back to kMaxKeyLen rather than clamping to 0 (which would
    // reject every key).
    uint32_t shim_kvkml = spdk_kv_shim_max_key_len(shim_);
    if (shim_kvkml == 0) {
        NIXL_WARN << "SPDK_KV: namespace advertised kvkml=0 (no key-length limit); "
                     "using default max key length " << static_cast<unsigned>(kMaxKeyLen);
        shim_kvkml = kMaxKeyLen;
    }
    maxKeyLen_ = static_cast<uint8_t>(std::min<uint32_t>(kMaxKeyLen, shim_kvkml));

    NIXL_INFO << "SPDK_KV: opened SPDK KV shim on '" << transport_id << "'"
              << " (init_env=" << (init_env ? "true" : "false")
              << ", max_key=" << shim_kvkml
              << ", effective_max_key=" << static_cast<unsigned>(maxKeyLen_)
              << ", max_value=" << spdk_kv_shim_max_value_len(shim_) << ")";
}

nixlSpdkKvEngine::~nixlSpdkKvEngine() {
    if (shim_) {
        spdk_kv_shim_close(shim_);
        shim_ = nullptr;
    }
}

nixl_mem_list_t
nixlSpdkKvEngine::getSupportedMems() const {
    // Local host DRAM source, remote OBJ-style key-addressed destination.
    return {DRAM_SEG, OBJ_SEG};
}

nixl_status_t
nixlSpdkKvEngine::registerMem(const nixlBlobDesc &mem,
                              const nixl_mem_t &nixl_mem,
                              nixlBackendMD *&out) {
    if (nixl_mem != DRAM_SEG && nixl_mem != OBJ_SEG) return NIXL_ERR_NOT_SUPPORTED;

    if (nixl_mem == OBJ_SEG) {
        // The remote descriptor carries the NIXL block identifier in metaInfo.
        // Take it VERBATIM as the opaque inline NVMe-KV key. The key lives in
        // the per-desc metadata and is read back at transfer time.
        std::vector<uint8_t> key;
        if (!spdkKvKeyFromBlobId(mem.metaInfo, maxKeyLen_, key)) {
            NIXL_ERROR << "SPDK_KV: invalid KV key in metaInfo (empty or > "
                       << static_cast<unsigned>(maxKeyLen_) << " bytes); rejecting";
            return NIXL_ERR_INVALID_PARAM;
        }
        out = new nixlSpdkKvMetadata(std::move(key));
    } else {
        // Local DRAM registration: nothing to do for the staging skeleton (we
        // stage through a per-request shim DMA buffer in postXfer).
        out = nullptr;
    }
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkKvEngine::deregisterMem(nixlBackendMD *meta) {
    delete static_cast<nixlSpdkKvMetadata *>(meta);
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkKvEngine::prepXfer(const nixl_xfer_op_t &operation,
                           const nixl_meta_dlist_t &local,
                           const nixl_meta_dlist_t &remote,
                           const std::string &remote_agent,
                           nixlBackendReqH *&handle,
                           const nixl_opt_b_args_t *opt_args) const {
    if (operation != NIXL_WRITE && operation != NIXL_READ) {
        NIXL_ERROR << "SPDK_KV: invalid operation " << operation;
        return NIXL_ERR_INVALID_PARAM;
    }
    if (local.getType() != DRAM_SEG) {
        NIXL_ERROR << "SPDK_KV: local memory type must be DRAM_SEG, got " << local.getType();
        return NIXL_ERR_INVALID_PARAM;
    }
    if (remote.getType() != OBJ_SEG) {
        NIXL_ERROR << "SPDK_KV: remote memory type must be OBJ_SEG, got " << remote.getType();
        return NIXL_ERR_INVALID_PARAM;
    }
    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << "SPDK_KV: local/remote descriptor count mismatch";
        return NIXL_ERR_INVALID_PARAM;
    }

    handle = new nixlSpdkKvBackendReqH();
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkKvEngine::postXfer(const nixl_xfer_op_t &operation,
                           const nixl_meta_dlist_t &local,
                           const nixl_meta_dlist_t &remote,
                           const std::string &remote_agent,
                           nixlBackendReqH *&handle,
                           const nixl_opt_b_args_t *opt_args) const {
    if (!handle) {
        NIXL_ERROR << "SPDK_KV: transfer request handle is null";
        return NIXL_ERR_INVALID_PARAM;
    }
    if (!shim_) {
        NIXL_ERROR << "SPDK_KV: shim not initialized";
        return NIXL_ERR_BACKEND;
    }
    auto *req_h = static_cast<nixlSpdkKvBackendReqH *>(handle);
    // One true-length slot per descriptor for value auto-sizing (all 0 until a
    // READ reports a too-small host buffer).
    req_h->true_lens.assign(local.descCount(), 0);

    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];
        const auto &remote_desc = remote[i];

        // The op uses the local length for the DMA staging buffer and the KV
        // value size; a local/remote length mismatch means the caller's view of
        // the value size disagrees, so fail rather than store/retrieve a
        // different number of bytes.
        if (local_desc.len != remote_desc.len) {
            NIXL_ERROR << "SPDK_KV: descriptor " << i << " length mismatch: local="
                       << local_desc.len << " remote=" << remote_desc.len;
            req_h->status = NIXL_ERR_INVALID_PARAM;
            return NIXL_ERR_INVALID_PARAM;
        }

        // The KV key lives in the descriptor's registration metadata (set by
        // registerMem). Read it from there rather than re-deriving.
        auto *md = static_cast<nixlSpdkKvMetadata *>(remote_desc.metadataP);
        if (!md) {
            NIXL_ERROR << "SPDK_KV: remote descriptor " << i
                       << " has no registered KV-key metadata";
            req_h->status = NIXL_ERR_INVALID_PARAM;
            return NIXL_ERR_INVALID_PARAM;
        }
        const std::vector<uint8_t> &key = md->key;

        const auto data_ptr = reinterpret_cast<void *>(local_desc.addr);
        const size_t data_len = local_desc.len;

        // A zero-length descriptor carries no value bytes. Treat it as a
        // successful no-op rather than routing spdk_dma_zmalloc(0) (which may
        // return NULL) through the alloc-failure path and misreporting it as a
        // backend error. (Skeleton: zero-length KV value semantics are out of
        // scope here.)
        if (data_len == 0) {
            continue;
        }

        // Stage through an SPDK-DMA buffer (see header for the rationale).
        void *dma = spdk_kv_shim_dma_alloc(data_len);
        if (!dma) {
            NIXL_ERROR << "SPDK_KV: DMA buffer alloc failed (" << data_len << " bytes)";
            req_h->status = NIXL_ERR_BACKEND;
            return NIXL_ERR_BACKEND;
        }

        int rc = 0;
        if (operation == NIXL_WRITE) {
            std::memcpy(dma, data_ptr, data_len);
            rc = spdk_kv_shim_store(shim_,
                                    key.data(),
                                    static_cast<uint8_t>(key.size()),
                                    dma,
                                    static_cast<uint32_t>(data_len));
            if (rc != 0) {
                NIXL_ERROR << "SPDK_KV: spdk_kv_shim_store failed: rc=" << rc;
            }
        } else { // NIXL_READ
            uint32_t value_len_out = 0;
            rc = spdk_kv_shim_retrieve(shim_,
                                       key.data(),
                                       static_cast<uint8_t>(key.size()),
                                       dma,
                                       static_cast<uint32_t>(data_len),
                                       &value_len_out);
            if (rc == 0) {
                // The whole value fit: value_len_out is the device's TRUE value
                // length and is <= data_len. Copy exactly the value bytes (a
                // value shorter than the buffer leaves the caller's tail as-is;
                // we never over-read the staging buffer).
                std::memcpy(data_ptr, dma, value_len_out);
            } else if (rc == SPDK_KV_SHIM_SC_BUFFER_TOO_SMALL) {
                // Value auto-sizing: the stored value is larger than the host
                // buffer, so value_len_out is the TRUE length. Do NOT copy a
                // truncated value (silent data loss). Record the true length so
                // the caller can resize its buffer/descriptor and re-Retrieve,
                // and report a distinct MISMATCH status (not a generic backend
                // error). Retrieved via getReqTrueLen(handle, i).
                req_h->true_lens[i] = value_len_out;
                NIXL_WARN << "SPDK_KV: value (" << value_len_out
                          << " B) exceeds host buffer (" << data_len
                          << " B) for descriptor " << i
                          << "; reporting true length for resize+retry";
                spdk_kv_shim_dma_free(dma);
                req_h->status = NIXL_ERR_MISMATCH;
                return NIXL_ERR_MISMATCH;
            } else {
                NIXL_ERROR << "SPDK_KV: spdk_kv_shim_retrieve failed: rc=" << rc;
            }
        }

        spdk_kv_shim_dma_free(dma);

        if (rc != 0) {
            req_h->status = NIXL_ERR_BACKEND;
            return NIXL_ERR_BACKEND;
        }
    }

    // The shim ops are synchronous, so the transfer is already complete.
    req_h->status = NIXL_SUCCESS;
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkKvEngine::checkXfer(nixlBackendReqH *handle) const {
    if (!handle) {
        NIXL_ERROR << "SPDK_KV: transfer request handle is null";
        return NIXL_ERR_INVALID_PARAM;
    }
    return static_cast<nixlSpdkKvBackendReqH *>(handle)->status;
}

nixl_status_t
nixlSpdkKvEngine::releaseReqH(nixlBackendReqH *handle) const {
    if (!handle) {
        NIXL_ERROR << "SPDK_KV: transfer request handle is null";
        return NIXL_ERR_INVALID_PARAM;
    }
    delete static_cast<nixlSpdkKvBackendReqH *>(handle);
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkKvEngine::queryMem(const nixl_reg_dlist_t &descs,
                          std::vector<nixl_query_resp_t> &resp) const {
    // Mirror the OBJ backend's queryMem result/absence convention exactly:
    //   - resp is sized to descCount() and defaulted to std::nullopt (absent).
    //   - present => resp[i] = nixl_query_resp_t{nixl_b_params_t{}} (engaged).
    //   - absent  => resp[i] = std::nullopt (left as the default).
    //   - a submit-/transport-level error returns an error status rather than
    //     encoding it as "absent", so a failure is never masked as a cache miss.
    resp.assign(descs.descCount(), std::nullopt);

    if (descs.getType() != OBJ_SEG) {
        NIXL_ERROR << "SPDK_KV: queryMem memory type must be OBJ_SEG, got " << descs.getType();
        return NIXL_ERR_NOT_SUPPORTED;
    }
    if (!shim_) {
        NIXL_ERROR << "SPDK_KV: shim not initialized";
        return NIXL_ERR_BACKEND;
    }

    for (int i = 0; i < descs.descCount(); ++i) {
        // The OBJ_SEG descriptor carries the NIXL block identifier in metaInfo;
        // take it VERBATIM as the opaque inline key (same mapping as registerMem).
        std::vector<uint8_t> key;
        if (!spdkKvKeyFromBlobId(descs[i].metaInfo, maxKeyLen_, key)) {
            NIXL_ERROR << "SPDK_KV: invalid KV key in metaInfo (empty or > "
                       << static_cast<unsigned>(maxKeyLen_)
                       << " bytes) in queryMem descriptor " << i;
            return NIXL_ERR_INVALID_PARAM;
        }

        int rc = spdk_kv_shim_exist(shim_, key.data(), static_cast<uint8_t>(key.size()));
        if (rc == 0) {
            // Hit.
            resp[i] = nixl_query_resp_t{nixl_b_params_t{}};
        } else if (rc == SPDK_KV_SHIM_SC_KEY_DOES_NOT_EXIST) {
            // Miss. Leave resp[i] as std::nullopt.
            resp[i] = std::nullopt;
        } else {
            // Positive device sc other than KEY_DOES_NOT_EXIST, or a negated
            // errno: a real error, NOT a miss.
            NIXL_ERROR << "SPDK_KV: spdk_kv_shim_exist failed for descriptor "
                       << i << ": rc=" << rc;
            return NIXL_ERR_BACKEND;
        }
    }

    return NIXL_SUCCESS;
}

size_t
nixlSpdkKvEngine::getReqTrueLen(nixlBackendReqH *handle, int idx) const {
    if (!handle || idx < 0) {
        return 0;
    }
    const auto *req_h = static_cast<const nixlSpdkKvBackendReqH *>(handle);
    if (static_cast<size_t>(idx) >= req_h->true_lens.size()) {
        return 0;
    }
    return req_h->true_lens[idx];
}
