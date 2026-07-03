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
#include <cerrno>
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

// Per-descriptor metadata for a DRAM_SEG (local host buffer) registration. When
// the region is DMA-reachable for the shim's transport -- already (SPDK-DMA
// memory) or after we register it (spdk_kv_shim_mem_register) -- postXfer DMAs
// store/retrieve/read/write straight into the caller's buffer with NO staging
// copy (zero-copy). Otherwise dma_registered is false and postXfer falls back to
// the staged-copy path -- always correct, just a copy. owns_registration is true
// only when WE registered the region (return 0), so deregisterMem releases it;
// it stays false when the region was already reachable (return 1) and thus must
// not be unregistered here. base/len record the registered span.
class nixlSpdkKvDramMD : public nixlBackendMD {
public:
    nixlSpdkKvDramMD(void *base, size_t len, bool dma_registered, bool owns_registration)
        : nixlBackendMD(true),
          base(base),
          len(len),
          dma_registered(dma_registered),
          owns_registration(owns_registration) {}
    ~nixlSpdkKvDramMD() override = default;

    void *base = nullptr;
    size_t len = 0;
    bool dma_registered = false;
    bool owns_registration = false;
};

// Synchronous request handle: the shim ops complete inline, so we just stash
// the final status produced by postXfer and report it in checkXfer.
class nixlSpdkKvBackendReqH : public nixlBackendReqH {
public:
    nixlSpdkKvBackendReqH() = default;
    ~nixlSpdkKvBackendReqH() override = default;

    // A validated block (BLK_SEG) LBA range: one per descriptor, computed and
    // range-checked in prepXfer and consumed verbatim by postXferBlock.
    struct BlockRange {
        uint64_t lba = 0;
        uint32_t nlba = 0;
    };

    nixl_status_t status = NIXL_IN_PROG;
    // Value auto-sizing: the device's TRUE value length recorded when a READ's
    // host buffer was too small (status == NIXL_ERR_MISMATCH). postXfer returns
    // at the FIRST too-small descriptor, so at most one is ever recorded -- a
    // scalar, not a per-descriptor vector. true_len_desc is that descriptor's
    // index; -1 means "no too-small result recorded" (the READ fit, or none was
    // reached). Read by getReqTrueLen, which returns true_len iff idx matches.
    size_t true_len = 0;
    int true_len_desc = -1;
    // Block path: the per-descriptor LBA ranges validated in prepXfer. Filled for
    // a BLK_SEG transfer and consumed by postXferBlock so it issues the pre-
    // validated IO instead of recomputing computeBlockRange per descriptor
    // (mirrors gusli, which builds the block IO at prep). Empty for the KV path.
    std::vector<BlockRange> block_ranges;
};

// ASCII lower-case a string (locale-independent). Shared by the init-param
// parsing below so the boolean and namespace-kind sites normalize identically.
std::string
toLower(const std::string &v) {
    std::string s;
    s.reserve(v.size());
    for (char c : v) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return s;
}

// Parse "true"/"1"/"yes"/"on" (case-insensitive) as boolean true.
bool
parseBool(const std::string &v) {
    const std::string s = toLower(v);
    return s == "true" || s == "1" || s == "yes" || s == "on";
}

} // namespace

// -----------------------------------------------------------------------------
// nixlSpdkKvEngine
// -----------------------------------------------------------------------------

nixlSpdkKvEngine::nixlSpdkKvEngine(const nixlBackendInitParams *init_params)
    : nixlBackendEngine(init_params),
      shim_lock_(init_params->syncMode) {
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

    // Namespace kind (option (b), one kind per engine). Default KV preserves the
    // historical behavior; "csi=block" (aliases: ns_kind/mode = block|blk|nvm)
    // binds a CSI==NVM block namespace and enables BLK_SEG LBA read/write.
    spdk_kv_shim_ns_kind ns_kind = SPDK_KV_SHIM_NS_KIND_KV;
    std::string ns_kind_str;
    for (const char *k : {"csi", "ns_kind", "mode"}) {
        if (getInitParam(k, ns_kind_str) == NIXL_SUCCESS && !ns_kind_str.empty()) break;
        ns_kind_str.clear();
    }
    if (!ns_kind_str.empty()) {
        const std::string s = toLower(ns_kind_str);
        if (s == "block" || s == "blk" || s == "nvm") {
            ns_kind = SPDK_KV_SHIM_NS_KIND_BLOCK;
        } else if (s == "kv") {
            ns_kind = SPDK_KV_SHIM_NS_KIND_KV;
        } else {
            NIXL_WARN << "SPDK: unknown namespace kind '" << ns_kind_str
                      << "', defaulting to kv";
        }
    }
    blockMode_ = (ns_kind == SPDK_KV_SHIM_NS_KIND_BLOCK);

    struct spdk_kv_shim_opts opts = {};
    opts.opts_size = sizeof(opts);
    opts.name = "nixl_spdk_kv";
    opts.transport_id = transport_id.c_str();
    opts.nsid = nsid;
    opts.init_env = init_env;
    opts.ns_kind = ns_kind;

    int rc = spdk_kv_shim_open(&opts, &shim_);
    if (rc != 0 || shim_ == nullptr) {
        if (rc == -ENOTSUP) {
            // The shim refused the bound block namespace because it carries
            // per-LBA metadata (interleaved/extended LBA, or separate DIF/DIX).
            // The block datapath sizes transfers from the data-only sector size,
            // so a metadata-formatted namespace would fault at SGL build; fail
            // cleanly here instead. Full metadata/PI support is out of scope.
            NIXL_ERROR << "SPDK: namespace on '" << transport_id
                       << "' carries per-LBA metadata (extended-LBA/DIF/DIX), which the "
                          "block datapath does not support; refusing (rc=" << rc << ")";
        } else {
            NIXL_ERROR << "SPDK: spdk_kv_shim_open(" << transport_id << ") failed: rc=" << rc;
        }
        shim_ = nullptr;
        initErr = true;
        return;
    }

    if (blockMode_) {
        // Block namespace: no key space; the datapath uses the sector geometry.
        NIXL_INFO << "SPDK: opened block namespace on '" << transport_id << "'"
                  << " (init_env=" << (init_env ? "true" : "false")
                  << ", sector_size=" << spdk_kv_shim_sector_size(shim_)
                  << ", num_sectors=" << spdk_kv_shim_num_sectors(shim_) << ")";
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
    // Free the reusable staging buffer (if any) BEFORE closing the shim. It is
    // only ever cached after a HEALTHY op -- a poisoned op transfers its buffer to
    // the shim's quarantine and NULLs the cache -- so its DMA tracker is dead and
    // a direct free is safe, and it is never also in the quarantine (no
    // double-free with spdk_kv_shim_close's drain). Free it first because for an
    // init_env=true shim spdk_kv_shim_close() tears down the SPDK env, after which
    // spdk_dma_free would be invalid.
    if (stagingBuf_ != nullptr) {
        spdk_kv_shim_dma_free(stagingBuf_);
        stagingBuf_ = nullptr;
        stagingCap_ = 0;
    }
    if (shim_) {
        spdk_kv_shim_close(shim_);
        shim_ = nullptr;
    }
}

void *
nixlSpdkKvEngine::stagingAcquire(size_t len, size_t align) const {
    // Reuse the cached buffer in place when it is big enough. Alignment is
    // invariant per engine (the KV path always passes 0, the block path always
    // SPDK_KV_SHIM_DMA_REGION), so a size check suffices.
    if (stagingBuf_ != nullptr && stagingCap_ >= len) {
        return stagingBuf_;
    }
    // Need a first or bigger buffer. The cache only ever holds a buffer whose last
    // op completed cleanly (a poisoned op routes its buffer to quarantine and
    // NULLs the cache in stagingRelease), so the old buffer's DMA tracker is dead
    // and it is safe to free directly here before growing.
    if (stagingBuf_ != nullptr) {
        spdk_kv_shim_dma_free(stagingBuf_);
        stagingBuf_ = nullptr;
        stagingCap_ = 0;
    }
    void *b = (align != 0) ? spdk_kv_shim_dma_alloc_raw_aligned(len, align)
                           : spdk_kv_shim_dma_alloc_raw(len);
    if (b == nullptr) {
        return nullptr;
    }
    stagingBuf_ = b;
    stagingCap_ = len;
    return b;
}

void
nixlSpdkKvEngine::stagingRelease(void *buf) const {
    // POISON SAFETY (hard invariant from the fence design): if the op timed out or
    // the qpair transport-failed, the shim is poisoned and buf's DMA tracker may
    // still be live. Hand buf to spdk_kv_shim_release_io_buf(), which QUARANTINES
    // it (freed only at the fencing teardown in spdk_kv_shim_close()), and DROP it
    // from the reuse cache so this buffer -- now owned by the quarantine -- is
    // never handed out again. On the healthy path the op's tracker is dead, so
    // keep buf cached for the next descriptor/post (no free, no release) rather
    // than churning an alloc+free per op.
    if (spdk_kv_shim_poisoned(shim_)) {
        spdk_kv_shim_release_io_buf(shim_, buf); // -> quarantine
        stagingBuf_ = nullptr;
        stagingCap_ = 0;
    }
}

nixl_mem_list_t
nixlSpdkKvEngine::getSupportedMems() const {
    // The remote op-set is fixed by the bound namespace kind (option (b)): a
    // KV-bound engine exposes the key-addressed KV blob (OBJ_SEG), a block-bound
    // engine exposes the NVMe LBA range (BLK_SEG). Deriving the list from the
    // mode keeps advertisement and enforcement in lockstep: advertising both
    // regardless of mode would let a caller register (and transfer) a cross-mode
    // remote, which -- because the KV opcodes alias NVM WRITE/READ -- would run a
    // KV op as a wild-LBA block op. Local host DRAM (DRAM_SEG) is the
    // source/sink in either mode.
    if (blockMode_) {
        return {DRAM_SEG, BLK_SEG};
    }
    return {DRAM_SEG, OBJ_SEG};
}

nixl_status_t
nixlSpdkKvEngine::registerMem(const nixlBlobDesc &mem,
                              const nixl_mem_t &nixl_mem,
                              nixlBackendMD *&out) {
    // Serialize the shim's DMA-registration (spdk_kv_shim_mem_register on the
    // DRAM_SEG path, which mutates the shared SPDK memory map and probes the
    // qpair's DMA reachability) against a concurrent postXfer/queryMem.
    NIXL_LOCK_GUARD(shim_lock_);
    if (nixl_mem != DRAM_SEG && nixl_mem != OBJ_SEG && nixl_mem != BLK_SEG)
        return NIXL_ERR_NOT_SUPPORTED;

    // Cross-mode guard: the remote op-set must match the bound namespace kind. A
    // block-bound engine must refuse OBJ_SEG (KV) and a KV-bound engine must
    // refuse BLK_SEG, before any per-descriptor metadata is built -- otherwise a
    // KV op could later be routed to a block namespace (the KV opcodes alias NVM
    // WRITE/READ -> wild-LBA corruption). DRAM_SEG (the local buffer) is valid in
    // either mode.
    if ((nixl_mem == OBJ_SEG && blockMode_) || (nixl_mem == BLK_SEG && !blockMode_)) {
        NIXL_ERROR << "SPDK: memory type " << nixl_mem
                   << " does not match the engine's namespace kind; rejecting";
        return NIXL_ERR_NOT_SUPPORTED;
    }

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
    } else if (nixl_mem == DRAM_SEG) {
        // Make the caller's host buffer DMA-usable so postXfer can transfer
        // directly into it (zero-copy), skipping the per-transfer staging copy.
        // The shim decides per transport whether the region is reachable and
        // registers it if needed: return 1 = already reachable (SPDK-DMA memory),
        // 0 = we registered it (must release in deregisterMem), <0 = not usable
        // directly (unaligned, or non-fd-backed DRAM over vfio-user) so we fall
        // back to a staged copy. A <0 is NOT a registerMem error -- zero-copy is
        // an optimization; the staged path is always correct.
        //
        // Registered regions are expected to be DISJOINT: over a PCIE/IOMMU
        // transport an owned registration covers exactly [base, len), and
        // reachability is probed at the base. Registering OVERLAPPING DRAM
        // regions is unsupported for the direct path -- a sub-range whose base
        // falls inside another region's mapping may be reported reachable, then
        // fail cleanly (NIXL_ERR_BACKEND, never corruption) once that other
        // region is deregistered. NIXL callers register disjoint regions, so this
        // is a documented constraint, not a live hazard.
        //
        // Construct the MD BEFORE registering so an owned registration always has
        // its releasing MD (no leak if the allocation throws).
        void *base = reinterpret_cast<void *>(mem.addr);
        const size_t len = mem.len;
        auto *md = new nixlSpdkKvDramMD(base, len, /*dma_registered=*/false,
                                        /*owns_registration=*/false);
        if (shim_ != nullptr && base != nullptr && len != 0) {
            int rc = spdk_kv_shim_mem_register(shim_, base, len);
            if (rc >= 0) {
                md->dma_registered = true;
                md->owns_registration = (rc == 0);
            } else {
                NIXL_DEBUG << "SPDK: DRAM region [" << base << ", +" << len
                           << ") not DMA-reachable directly (rc=" << rc
                           << "); will stage-copy this buffer";
            }
        }
        out = md;
    } else {
        // BLK_SEG (remote LBA range) needs no per-descriptor metadata: the
        // descriptor's addr carries the starting LBA (read at transfer time, NO
        // key derivation), mirroring gusli's near-no-op block registration.
        out = nullptr;
    }
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkKvEngine::deregisterMem(nixlBackendMD *meta) {
    // Serialize the shim's DMA-deregistration (spdk_kv_shim_mem_unregister
    // mutates the shared SPDK memory map the datapath translates against)
    // against a concurrent postXfer/queryMem/registerMem.
    NIXL_LOCK_GUARD(shim_lock_);
    // A DRAM registration that took the zero-copy path holds an SPDK memory
    // registration; release it before freeing the MD. NIXL deregisters only
    // after all transfers to the region have completed, so no DMA is in flight.
    // (nixlBackendMD has a virtual dtor, so the base-pointer delete below runs
    // the correct derived destructor for either MD type.)
    if (auto *dram = dynamic_cast<nixlSpdkKvDramMD *>(meta); dram != nullptr) {
        if (dram->dma_registered && dram->owns_registration) {
            spdk_kv_shim_mem_unregister(dram->base, dram->len);
        }
    }
    delete meta;
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
    // The remote memory type is the LBA/KV knob: OBJ_SEG -> KV, BLK_SEG -> block.
    const nixl_mem_t remote_type = remote.getType();
    if (remote_type != OBJ_SEG && remote_type != BLK_SEG) {
        NIXL_ERROR << "SPDK: remote memory type must be OBJ_SEG or BLK_SEG, got " << remote_type;
        return NIXL_ERR_INVALID_PARAM;
    }
    // Cross-mode guard at prep: the remote op-set must match the bound namespace
    // kind, refused BEFORE any DMA (defense-in-depth alongside the postXfer and
    // registerMem guards). OBJ_SEG on a block-bound engine would alias a KV Store
    // to an NVM WRITE at a wild LBA; BLK_SEG on a KV-bound engine is equally
    // invalid. Refusing malformed-mode lists here keeps prep and post in lockstep.
    if ((remote_type == OBJ_SEG && blockMode_) || (remote_type == BLK_SEG && !blockMode_)) {
        NIXL_ERROR << "SPDK: remote memory type " << remote_type
                   << " does not match the engine's namespace kind; rejecting";
        return NIXL_ERR_NOT_SUPPORTED;
    }
    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << "SPDK_KV: local/remote descriptor count mismatch";
        return NIXL_ERR_INVALID_PARAM;
    }
    if (!shim_) {
        NIXL_ERROR << "SPDK: shim not initialized";
        return NIXL_ERR_BACKEND;
    }

    // Validate EVERY descriptor up front, before any device op, so a multi-
    // descriptor list with one malformed descriptor is rejected atomically at
    // prep -- postXfer then issues ZERO device ops (no mid-list partial mutation
    // where an earlier descriptor is durably Stored/written and a later one fails
    // with no rollback).
    if (remote_type == BLK_SEG) {
        // Block: derive and range-check (length match, sector alignment, single-op
        // bound, capacity) every LBA range, then STASH it so postXferBlock issues
        // the validated IO rather than recomputing (mirrors gusli's prep-built IO).
        std::vector<nixlSpdkKvBackendReqH::BlockRange> ranges(local.descCount());
        for (int i = 0; i < local.descCount(); ++i) {
            uint64_t lba = 0;
            uint32_t nlba = 0;
            nixl_status_t vs = computeBlockRange(local[i], remote[i], lba, nlba);
            if (vs != NIXL_SUCCESS) return vs;
            ranges[i].lba = lba;
            ranges[i].nlba = nlba;
        }
        auto *req_h = new nixlSpdkKvBackendReqH();
        req_h->block_ranges = std::move(ranges);
        handle = req_h;
        return NIXL_SUCCESS;
    }

    // KV (OBJ_SEG): validate length match, the single-op bound (no striping), and
    // the key-metadata presence/length for every descriptor. These checks used to
    // live inside the postXfer loop, which validated-and-issued per iteration --
    // the source of the mid-list partial Store. Run them all here so a bad
    // descriptor is caught before the first Store/Retrieve.
    const uint32_t max_op = spdk_kv_shim_max_value_len_op(shim_);
    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];
        const auto &remote_desc = remote[i];

        // A local/remote length mismatch means the caller's view of the value
        // size disagrees; fail rather than Store/Retrieve a different byte count.
        if (local_desc.len != remote_desc.len) {
            NIXL_ERROR << "SPDK_KV: descriptor " << i << " length mismatch: local="
                       << local_desc.len << " remote=" << remote_desc.len;
            return NIXL_ERR_INVALID_PARAM;
        }
        // NO striping: a value past the single-op region-bounded SGL bound is
        // rejected here, never split across ops.
        if (local_desc.len > max_op) {
            NIXL_ERROR << "SPDK_KV: descriptor " << i << " length " << local_desc.len
                       << " exceeds the single-op bound " << max_op
                       << " bytes; rejecting (no striping)";
            return NIXL_ERR_INVALID_PARAM;
        }
        // The verbatim inline key lives in the descriptor's registration metadata
        // (built and length-checked in registerMem). Require it to be present and
        // still within [1, maxKeyLen_] so postXfer can take it as-is.
        auto *md = static_cast<nixlSpdkKvMetadata *>(remote_desc.metadataP);
        if (!md) {
            NIXL_ERROR << "SPDK_KV: remote descriptor " << i
                       << " has no registered KV-key metadata";
            return NIXL_ERR_INVALID_PARAM;
        }
        if (md->key.empty() || md->key.size() > maxKeyLen_) {
            NIXL_ERROR << "SPDK_KV: remote descriptor " << i << " has invalid key length "
                       << md->key.size();
            return NIXL_ERR_INVALID_PARAM;
        }
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
    // Serialize the shim's single qpair + shared SGL/completion state for the
    // whole synchronous op (submit+poll). Held here rather than inside
    // postXferBlock so the block delegation below runs UNDER this same lock
    // without re-locking (absl::Mutex is non-recursive -> a second guard would
    // self-deadlock).
    NIXL_LOCK_GUARD(shim_lock_);
    if (!shim_) {
        NIXL_ERROR << "SPDK_KV: shim not initialized";
        return NIXL_ERR_BACKEND;
    }
    // Cross-mode guard: the remote op-set must match the bound namespace kind,
    // enforced before any device op. A KV (OBJ_SEG) transfer on a block-bound
    // engine would alias a KV Store to an NVM WRITE at a wild LBA (silent
    // corruption); a block (BLK_SEG) transfer on a KV-bound engine is equally
    // invalid. Mirrors the shim's is_block guards and registerMem.
    const nixl_mem_t remote_type = remote.getType();
    if ((remote_type == OBJ_SEG && blockMode_) || (remote_type == BLK_SEG && !blockMode_)) {
        NIXL_ERROR << "SPDK: remote memory type " << remote_type
                   << " does not match the engine's namespace kind; rejecting";
        static_cast<nixlSpdkKvBackendReqH *>(handle)->status = NIXL_ERR_NOT_SUPPORTED;
        return NIXL_ERR_NOT_SUPPORTED;
    }
    // LBA/KV knob: a BLK_SEG remote drives the NVMe block path; OBJ_SEG falls
    // through to the unchanged KV (Store/Retrieve) path below.
    if (remote_type == BLK_SEG) {
        return postXferBlock(operation, local, remote, handle);
    }
    auto *req_h = static_cast<nixlSpdkKvBackendReqH *>(handle);
    // Value auto-sizing state: no too-small result yet (getReqTrueLen -> 0). Set
    // to (true_len, i) only if a READ reports the host buffer was too small.
    req_h->true_len = 0;
    req_h->true_len_desc = -1;

    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];
        const auto &remote_desc = remote[i];

        // prepXfer already validated EVERY descriptor (length match, single-op
        // bound, key-metadata presence/length) BEFORE this loop, so a malformed
        // mid-list descriptor was rejected up front and NO Store/Retrieve has run
        // for this list -- there is no mid-list partial mutation. The read below
        // takes the pre-validated key from the descriptor's registration
        // metadata; the nullptr check is a defensive deref guard (prep guarantees
        // it holds for every descriptor), not a mid-list validation reject.
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

        // Zero-copy vs staged, WITH DEMOTION. When the local DRAM was
        // DMA-registered in registerMem, hand the caller's buffer straight to the
        // shim (no alloc, no memcpy). When it was not (unaligned / unregisterable
        // / only partially reachable), stage through an SPDK-DMA buffer and copy
        // -- always correct, just a copy. A direct-path SUBMIT/reachability
        // failure (a negative shim rc, e.g. a region-bounded SGL descriptor whose
        // vaddr is not DMA-translatable) is NOT fatal: demote to the
        // always-correct staged copy for THIS descriptor rather than failing the
        // transfer. A device-reported status (non-negative rc, incl. the
        // BUFFER_TOO_SMALL auto-sizing signal) is a real answer and is never
        // demoted. prepXfer guarantees the local seg is DRAM_SEG and registerMem
        // always builds a nixlSpdkKvDramMD for DRAM_SEG, so the local MD type is
        // statically known -- static_cast; the nullptr guard still covers a
        // descriptor registered with no MD (staged path).
        auto *dram = static_cast<nixlSpdkKvDramMD *>(local_desc.metadataP);
        const bool direct = dram != nullptr && dram->dma_registered;

        // Device TRUE value length for a READ (value auto-sizing); set by run_kv.
        uint32_t value_len_out = 0;
        // One attempt of the KV op over either the caller's buffer (use_direct)
        // or a freshly staged SPDK-DMA copy. Returns the shim rc, or -ENOMEM if
        // the staging allocation fails. On a READ that fits (rc==0) the staged
        // copy is written back to the caller here.
        auto run_kv = [&](bool use_direct) -> int {
            void *io_buf = data_ptr;
            if (!use_direct) {
                // Reuse the engine's cached, non-zeroing staging buffer. The
                // whole span is memcpy'd (WRITE) or device-filled (READ) below, so
                // the skipped zero-fill is never observed.
                io_buf = stagingAcquire(data_len, 0);
                if (!io_buf) {
                    NIXL_ERROR << "SPDK_KV: DMA buffer alloc failed (" << data_len << " bytes)";
                    return -ENOMEM;
                }
            }
            int r;
            if (operation == NIXL_WRITE) {
                if (!use_direct) std::memcpy(io_buf, data_ptr, data_len);
                r = spdk_kv_shim_store(shim_, key.data(),
                                       static_cast<uint8_t>(key.size()),
                                       io_buf, static_cast<uint32_t>(data_len));
            } else { // NIXL_READ
                r = spdk_kv_shim_retrieve(shim_, key.data(),
                                          static_cast<uint8_t>(key.size()),
                                          io_buf, static_cast<uint32_t>(data_len),
                                          &value_len_out);
                // The whole value fit: value_len_out is the device's TRUE value
                // length and is <= data_len. In staged mode copy exactly those
                // bytes back; a value shorter than the buffer leaves the caller's
                // tail untouched (we never over-read/over-copy). Because we copy
                // back only these device-written bytes, the staging buffer's
                // uninitialized (non-zeroed) tail never reaches the caller.
                if (r == 0 && !use_direct) std::memcpy(data_ptr, io_buf, value_len_out);
            }
            // Quarantine-aware release: on the healthy path the buffer stays
            // cached for reuse; if the op timed out / transport-failed the shim is
            // poisoned and this quarantines io_buf (freed later at the fencing
            // teardown) and drops it from the cache, instead of recycling a
            // possibly-live DMA target. Covers the demoted staged retry too.
            if (!use_direct) stagingRelease(io_buf);
            return r;
        };

        int rc = run_kv(direct);
        if (direct && rc < 0) {
            NIXL_WARN << "SPDK_KV: direct DMA failed for descriptor " << i
                      << " (rc=" << rc << "); demoting to a staged copy";
            rc = run_kv(false);
        }

        if (operation == NIXL_READ && rc == SPDK_KV_SHIM_SC_BUFFER_TOO_SMALL) {
            // Value auto-sizing: the stored value is larger than the host buffer,
            // so value_len_out is the TRUE length. Do NOT surface a truncated
            // value: record the true length so the caller can resize its
            // buffer/descriptor and re-Retrieve, and report a distinct MISMATCH
            // status (not a generic backend error). Retrieved via
            // getReqTrueLen(handle, i).
            req_h->true_len = value_len_out;
            req_h->true_len_desc = i;
            NIXL_WARN << "SPDK_KV: value (" << value_len_out
                      << " B) exceeds host buffer (" << data_len
                      << " B) for descriptor " << i
                      << "; reporting true length for resize+retry";
            req_h->status = NIXL_ERR_MISMATCH;
            return NIXL_ERR_MISMATCH;
        }
        if (rc != 0) {
            NIXL_ERROR << "SPDK_KV: descriptor " << i << " transfer failed: rc=" << rc;
            req_h->status = NIXL_ERR_BACKEND;
            return NIXL_ERR_BACKEND;
        }
    }

    // The shim ops are synchronous, so the transfer is already complete.
    req_h->status = NIXL_SUCCESS;
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkKvEngine::computeBlockRange(const nixlMetaDesc &local_desc,
                                   const nixlMetaDesc &remote_desc,
                                   uint64_t &lba_out,
                                   uint32_t &nlba_out) const {
    lba_out = 0;
    nlba_out = 0;

    // local.len == remote.len still holds for block (both are the byte length of
    // the same transfer); reuse the KV check.
    if (local_desc.len != remote_desc.len) {
        NIXL_ERROR << "SPDK: block descriptor length mismatch: local=" << local_desc.len
                   << " remote=" << remote_desc.len;
        return NIXL_ERR_INVALID_PARAM;
    }

    const size_t len = local_desc.len;
    if (len == 0) {
        // Zero-length block transfer: a no-op (0 sectors); the caller skips it.
        return NIXL_SUCCESS;
    }

    const uint32_t sector = spdk_kv_shim_sector_size(shim_);
    if (sector == 0) {
        // Not a block-bound shim (opened csi=kv): BLK_SEG is unavailable here.
        NIXL_ERROR << "SPDK: BLK_SEG transfer requires a block namespace "
                      "(open the engine with csi=block)";
        return NIXL_ERR_BACKEND;
    }

    // Sector alignment: a block op moves whole sectors, so the byte length must
    // be a multiple of the namespace sector size (KV had no alignment rule).
    if ((len % sector) != 0) {
        NIXL_ERROR << "SPDK: block length " << len
                   << " is not a multiple of the sector size " << sector;
        return NIXL_ERR_INVALID_PARAM;
    }

    // NO striping: a single op carries up to the block single-op bound (~64 MiB)
    // as one data-block descriptor per 2 MiB DMA region via the region-bounded
    // SGL (mirrors the KV large-value bound spdk_kv_shim_max_value_len_op). A
    // range past the bound is REJECTED here, before staging any DMA, never split.
    const uint32_t max_op = spdk_kv_shim_max_block_len_op(shim_);
    if (len > max_op) {
        NIXL_ERROR << "SPDK: block length " << len << " exceeds the single-op bound "
                   << max_op << " bytes; rejecting (no striping)";
        return NIXL_ERR_INVALID_PARAM;
    }

    const uint64_t lba = static_cast<uint64_t>(remote_desc.addr);
    const uint64_t nlba = static_cast<uint64_t>(len) / sector; // <= MAX_VALUE_LEN/sector, fits uint32
    const uint64_t capacity = spdk_kv_shim_num_sectors(shim_);

    // Capacity: reject an LBA at/after the end, or a range running past it.
    // (lba >= capacity checked first so capacity - lba never underflows.)
    if (lba >= capacity || nlba > capacity - lba) {
        NIXL_ERROR << "SPDK: block range [" << lba << ", +" << nlba
                   << ") exceeds namespace capacity " << capacity << " sectors";
        return NIXL_ERR_INVALID_PARAM;
    }

    lba_out = lba;
    nlba_out = static_cast<uint32_t>(nlba);
    return NIXL_SUCCESS;
}

nixl_status_t
nixlSpdkKvEngine::postXferBlock(const nixl_xfer_op_t &operation,
                                const nixl_meta_dlist_t &local,
                                const nixl_meta_dlist_t &remote,
                                nixlBackendReqH *handle) const {
    auto *req_h = static_cast<nixlSpdkKvBackendReqH *>(handle);
    // Block has no value auto-sizing (getReqTrueLen -> 0).
    req_h->true_len = 0;
    req_h->true_len_desc = -1;

    // prepXfer validated and stashed every (lba, nlba); consume it here rather
    // than recomputing computeBlockRange per descriptor (mirrors gusli, which
    // builds the block IO at prep). A size mismatch means prep did not run for
    // this handle (or the list changed) -- refuse before any device op.
    if (req_h->block_ranges.size() != static_cast<size_t>(remote.descCount())) {
        NIXL_ERROR << "SPDK: block transfer handle is missing its prep-validated LBA ranges";
        req_h->status = NIXL_ERR_INVALID_PARAM;
        return NIXL_ERR_INVALID_PARAM;
    }

    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];

        // The LBA range was validated (alignment/capacity/single-range) in
        // prepXfer; take it as-is -- no recompute.
        const uint64_t lba = req_h->block_ranges[i].lba;
        const uint32_t nlba = req_h->block_ranges[i].nlba;
        if (nlba == 0) {
            // Zero-length descriptor: nothing to transfer.
            continue;
        }

        const auto data_ptr = reinterpret_cast<void *>(local_desc.addr);
        const size_t data_len = local_desc.len;

        // Zero-copy: DMA straight to/from the caller's buffer when it was
        // DMA-registered in registerMem; otherwise stage through a 2 MiB-aligned
        // SPDK DMA buffer and copy. Aligning the STAGING buffer to a 2 MiB DMA
        // region makes each 2 MiB span its own region-bounded SGL data-block
        // descriptor; the region-bounded SGL likewise bounds a registered (any
        // 4 KiB-aligned) buffer at each 2 MiB boundary, so neither path lets a
        // descriptor straddle two independently-mapped vfio-user regions (up to
        // the ~64 MiB single-op bound). NO value auto-sizing (block moves exactly
        // len bytes). Zero-copy vs staged, WITH DEMOTION (mirrors the KV path): a
        // direct-path submit/reachability failure (negative rc) demotes to the
        // always-correct staged copy for THIS descriptor rather than failing the
        // transfer. prepXfer guarantees the local seg is DRAM_SEG and registerMem
        // always builds a nixlSpdkKvDramMD for DRAM_SEG, so the local MD type is
        // statically known -- static_cast; the nullptr guard still covers a
        // descriptor registered with no MD (staged path).
        auto *dram = static_cast<nixlSpdkKvDramMD *>(local_desc.metadataP);
        const bool direct = dram != nullptr && dram->dma_registered;

        // One attempt of the block op over either the caller's buffer
        // (use_direct) or a 2 MiB-aligned staged SPDK-DMA copy. Returns the shim
        // rc, or -ENOMEM if the staging allocation fails.
        auto run_blk = [&](bool use_direct) -> int {
            void *io_buf = data_ptr;
            if (!use_direct) {
                // Reuse the engine's cached, non-zeroing 2 MiB-aligned staging
                // buffer. A block WRITE memcpys the whole span and a block READ
                // fills every sector, so the skipped zero-fill is never observed.
                io_buf = stagingAcquire(data_len, SPDK_KV_SHIM_DMA_REGION);
                if (!io_buf) {
                    NIXL_ERROR << "SPDK: block DMA buffer alloc failed (" << data_len << " bytes)";
                    return -ENOMEM;
                }
            }
            int r;
            if (operation == NIXL_WRITE) {
                if (!use_direct) std::memcpy(io_buf, data_ptr, data_len);
                r = spdk_kv_shim_write(shim_, io_buf, lba, nlba);
            } else { // NIXL_READ
                // A block read fills every one of the data_len bytes (nlba full
                // sectors), so copying the whole span back never exposes the
                // staging buffer's uninitialized tail (there is none).
                r = spdk_kv_shim_read(shim_, io_buf, lba, nlba);
                if (r == 0 && !use_direct) std::memcpy(data_ptr, io_buf, data_len);
            }
            // Quarantine-aware release: on the healthy path the buffer stays
            // cached for reuse; if the op timed out / transport-failed the shim is
            // poisoned and this quarantines io_buf (freed later at the fencing
            // teardown) and drops it from the cache, instead of recycling a
            // possibly-live DMA target. Covers the demoted staged retry too.
            if (!use_direct) stagingRelease(io_buf);
            return r;
        };

        int rc = run_blk(direct);
        if (direct && rc < 0) {
            NIXL_WARN << "SPDK: direct DMA failed for block descriptor " << i
                      << " (rc=" << rc << "); demoting to a staged copy";
            rc = run_blk(false);
        }

        if (rc != 0) {
            NIXL_ERROR << "SPDK: block descriptor " << i << " transfer failed: rc=" << rc;
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
    // Serialize the shim's single qpair (spdk_kv_shim_exist submits+polls) and
    // shared completion state against a concurrent postXfer/registerMem.
    NIXL_LOCK_GUARD(shim_lock_);
    // Mirror the OBJ backend's queryMem result/absence convention exactly:
    //   - resp is sized to descCount() and defaulted to std::nullopt (absent).
    //   - present => resp[i] = nixl_query_resp_t{nixl_b_params_t{}} (engaged).
    //   - absent  => resp[i] = std::nullopt (left as the default).
    //   - a submit-/transport-level error returns an error status rather than
    //     encoding it as "absent", so a failure is never masked as a cache miss.
    resp.assign(descs.descCount(), std::nullopt);

    // QUERY maps to KV Exist and is OBJ_SEG-only. Block (BLK_SEG) has no per-LBA
    // "existence" notion, so queryMem(BLK_SEG) is NIXL_ERR_NOT_SUPPORTED.
    if (descs.getType() != OBJ_SEG) {
        NIXL_ERROR << "SPDK: queryMem is only supported for OBJ_SEG (KV Exist), got "
                   << descs.getType();
        return NIXL_ERR_NOT_SUPPORTED;
    }
    // Cross-mode guard: KV Exist requires a KV-bound engine. A block-bound
    // engine has no key space, so refuse OBJ_SEG here (before any device op)
    // rather than issuing a KV Exist against a block namespace.
    if (blockMode_) {
        NIXL_ERROR << "SPDK: queryMem (KV Exist) requires a KV namespace; this "
                      "engine is block-bound";
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
    // A too-small READ records exactly one descriptor's true length. Return it
    // only for that descriptor; every other index (and the no-mismatch case,
    // true_len_desc == -1) reports 0, matching the old per-descriptor semantics.
    return (idx == req_h->true_len_desc) ? req_h->true_len : 0;
}

uint32_t
nixlSpdkKvEngine::blockSectorSize() const {
    return shim_ ? spdk_kv_shim_sector_size(shim_) : 0;
}

bool
nixlSpdkKvEngine::dramIsDmaRegistered(const nixlBackendMD *md) const {
    const auto *dram = dynamic_cast<const nixlSpdkKvDramMD *>(md);
    return dram != nullptr && dram->dma_registered;
}
