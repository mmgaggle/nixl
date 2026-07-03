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
        std::string s;
        s.reserve(ns_kind_str.size());
        for (char c : ns_kind_str)
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
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
    if (shim_) {
        spdk_kv_shim_close(shim_);
        shim_ = nullptr;
    }
}

nixl_mem_list_t
nixlSpdkKvEngine::getSupportedMems() const {
    // Local host DRAM source; remote is either an OBJ-style key-addressed KV
    // blob (OBJ_SEG) or an NVMe block LBA range (BLK_SEG). The op-set is chosen
    // per transfer by the remote memory type.
    return {DRAM_SEG, OBJ_SEG, BLK_SEG};
}

nixl_status_t
nixlSpdkKvEngine::registerMem(const nixlBlobDesc &mem,
                              const nixl_mem_t &nixl_mem,
                              nixlBackendMD *&out) {
    if (nixl_mem != DRAM_SEG && nixl_mem != OBJ_SEG && nixl_mem != BLK_SEG)
        return NIXL_ERR_NOT_SUPPORTED;

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
    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << "SPDK_KV: local/remote descriptor count mismatch";
        return NIXL_ERR_INVALID_PARAM;
    }

    if (remote_type == BLK_SEG) {
        // Validate every LBA range (alignment/capacity/single-range) up front so
        // a malformed block transfer is rejected at prep, before postXfer stages
        // any DMA (mirrors gusli, which builds the block IO in prepXfer).
        if (!shim_) {
            NIXL_ERROR << "SPDK: shim not initialized";
            return NIXL_ERR_BACKEND;
        }
        for (int i = 0; i < local.descCount(); ++i) {
            uint64_t lba = 0;
            uint32_t nlba = 0;
            nixl_status_t vs = computeBlockRange(local[i], remote[i], lba, nlba);
            if (vs != NIXL_SUCCESS) return vs;
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
    if (!shim_) {
        NIXL_ERROR << "SPDK_KV: shim not initialized";
        return NIXL_ERR_BACKEND;
    }
    // LBA/KV knob: a BLK_SEG remote drives the NVMe block path; OBJ_SEG falls
    // through to the unchanged KV (Store/Retrieve) path below.
    if (remote.getType() == BLK_SEG) {
        return postXferBlock(operation, local, remote, handle);
    }
    auto *req_h = static_cast<nixlSpdkKvBackendReqH *>(handle);
    // One true-length slot per descriptor for value auto-sizing (all 0 until a
    // READ reports a too-small host buffer).
    req_h->true_lens.assign(local.descCount(), 0);

    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];
        const auto &remote_desc = remote[i];

        // The op uses the local length for the DMA buffer (staged or direct) and
        // the KV value size; a local/remote length mismatch means the caller's
        // view of the value size disagrees, so fail rather than store/retrieve a
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

        // NO striping: a value larger than the single-op region-bounded SGL
        // bound (~64 MiB) is out of scope and REJECTED here, before staging any
        // DMA, rather than being split across ops. The KV-cache use case sizes
        // its blocks (tokens_per_block) so one value = one op.
        const uint32_t max_op = spdk_kv_shim_max_value_len_op(shim_);
        if (data_len > max_op) {
            NIXL_ERROR << "SPDK_KV: descriptor " << i << " length " << data_len
                       << " exceeds the single-op bound " << max_op
                       << " bytes; rejecting (no striping)";
            req_h->status = NIXL_ERR_INVALID_PARAM;
            return NIXL_ERR_INVALID_PARAM;
        }

        // A zero-length descriptor carries no value bytes. Treat it as a
        // successful no-op rather than routing spdk_dma_zmalloc(0) (which may
        // return NULL) through the alloc-failure path and misreporting it as a
        // backend error. (Skeleton: zero-length KV value semantics are out of
        // scope here.)
        if (data_len == 0) {
            continue;
        }

        // Zero-copy: when the local DRAM was DMA-registered in registerMem, hand
        // the caller's buffer straight to the shim (no alloc, no memcpy). When it
        // was not (unaligned / unregisterable), stage through an SPDK-DMA buffer
        // and copy -- always correct, just a copy. The region-bounded SGL walks
        // whichever buffer we pass. dynamic_cast so an unexpected MD type simply
        // yields nullptr and takes the (safe) staged path rather than misreading
        // the direct-path flag.
        auto *dram = dynamic_cast<nixlSpdkKvDramMD *>(local_desc.metadataP);
        const bool direct = dram != nullptr && dram->dma_registered;
        void *io_buf = data_ptr;
        if (!direct) {
            io_buf = spdk_kv_shim_dma_alloc(data_len);
            if (!io_buf) {
                NIXL_ERROR << "SPDK_KV: DMA buffer alloc failed (" << data_len << " bytes)";
                req_h->status = NIXL_ERR_BACKEND;
                return NIXL_ERR_BACKEND;
            }
        }

        int rc = 0;
        if (operation == NIXL_WRITE) {
            if (!direct) std::memcpy(io_buf, data_ptr, data_len);
            rc = spdk_kv_shim_store(shim_,
                                    key.data(),
                                    static_cast<uint8_t>(key.size()),
                                    io_buf,
                                    static_cast<uint32_t>(data_len));
            if (rc != 0) {
                NIXL_ERROR << "SPDK_KV: spdk_kv_shim_store failed: rc=" << rc;
            }
        } else { // NIXL_READ
            uint32_t value_len_out = 0;
            rc = spdk_kv_shim_retrieve(shim_,
                                       key.data(),
                                       static_cast<uint8_t>(key.size()),
                                       io_buf,
                                       static_cast<uint32_t>(data_len),
                                       &value_len_out);
            if (rc == 0) {
                // The whole value fit: value_len_out is the device's TRUE value
                // length and is <= data_len. In direct mode the device already
                // DMA'd exactly value_len_out bytes into the caller's buffer; in
                // staged mode copy exactly those bytes back. Either way a value
                // shorter than the buffer leaves the caller's tail untouched (we
                // never over-read/over-copy).
                if (!direct) std::memcpy(data_ptr, io_buf, value_len_out);
            } else if (rc == SPDK_KV_SHIM_SC_BUFFER_TOO_SMALL) {
                // Value auto-sizing: the stored value is larger than the host
                // buffer, so value_len_out is the TRUE length. Do NOT surface a
                // truncated value: record the true length so the caller can
                // resize its buffer/descriptor and re-Retrieve, and report a
                // distinct MISMATCH status (not a generic backend error).
                // Retrieved via getReqTrueLen(handle, i). NOTE (zero-copy): in
                // direct mode the device may have written up to data_len partial
                // bytes into the caller's buffer; per the shim contract those
                // contents are unusable and the resize+retry overwrites them.
                req_h->true_lens[i] = value_len_out;
                NIXL_WARN << "SPDK_KV: value (" << value_len_out
                          << " B) exceeds host buffer (" << data_len
                          << " B) for descriptor " << i
                          << "; reporting true length for resize+retry";
                if (!direct) spdk_kv_shim_dma_free(io_buf);
                req_h->status = NIXL_ERR_MISMATCH;
                return NIXL_ERR_MISMATCH;
            } else {
                NIXL_ERROR << "SPDK_KV: spdk_kv_shim_retrieve failed: rc=" << rc;
            }
        }

        if (!direct) spdk_kv_shim_dma_free(io_buf);

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
    // Block has no value auto-sizing; leave true_lens empty (getReqTrueLen -> 0).
    req_h->true_lens.clear();

    for (int i = 0; i < local.descCount(); ++i) {
        const auto &local_desc = local[i];

        // LBA + sector count, with alignment/capacity/single-range validation
        // (re-checked here so postXfer is self-contained; prepXfer validated too).
        uint64_t lba = 0;
        uint32_t nlba = 0;
        nixl_status_t vs = computeBlockRange(local_desc, remote[i], lba, nlba);
        if (vs != NIXL_SUCCESS) {
            req_h->status = vs;
            return vs;
        }
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
        // len bytes). dynamic_cast so an unexpected MD type takes the safe staged
        // path (nullptr) rather than misreading the direct-path flag.
        auto *dram = dynamic_cast<nixlSpdkKvDramMD *>(local_desc.metadataP);
        const bool direct = dram != nullptr && dram->dma_registered;
        void *io_buf = data_ptr;
        if (!direct) {
            io_buf = spdk_kv_shim_dma_alloc_aligned(data_len, SPDK_KV_SHIM_DMA_REGION);
            if (!io_buf) {
                NIXL_ERROR << "SPDK: block DMA buffer alloc failed (" << data_len << " bytes)";
                req_h->status = NIXL_ERR_BACKEND;
                return NIXL_ERR_BACKEND;
            }
        }

        int rc = 0;
        if (operation == NIXL_WRITE) {
            if (!direct) std::memcpy(io_buf, data_ptr, data_len);
            rc = spdk_kv_shim_write(shim_, io_buf, lba, nlba);
            if (rc != 0) {
                NIXL_ERROR << "SPDK: spdk_kv_shim_write failed: rc=" << rc;
            }
        } else { // NIXL_READ
            rc = spdk_kv_shim_read(shim_, io_buf, lba, nlba);
            if (rc == 0) {
                if (!direct) std::memcpy(data_ptr, io_buf, data_len);
            } else {
                NIXL_ERROR << "SPDK: spdk_kv_shim_read failed: rc=" << rc;
            }
        }

        if (!direct) spdk_kv_shim_dma_free(io_buf);

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

    // QUERY maps to KV Exist and is OBJ_SEG-only. Block (BLK_SEG) has no per-LBA
    // "existence" notion, so queryMem(BLK_SEG) is NIXL_ERR_NOT_SUPPORTED.
    if (descs.getType() != OBJ_SEG) {
        NIXL_ERROR << "SPDK: queryMem is only supported for OBJ_SEG (KV Exist), got "
                   << descs.getType();
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

uint32_t
nixlSpdkKvEngine::blockSectorSize() const {
    return shim_ ? spdk_kv_shim_sector_size(shim_) : 0;
}

bool
nixlSpdkKvEngine::dramIsDmaRegistered(const nixlBackendMD *md) const {
    const auto *dram = dynamic_cast<const nixlSpdkKvDramMD *>(md);
    return dram != nullptr && dram->dma_registered;
}
