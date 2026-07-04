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
 *
 * Shared scaffolding for the SPDK direct-engine round-trip tests (KV / OBJ_SEG
 * and block / BLK_SEG). The two test binaries build from this same directory, so
 * this header is resolved via the plugin's own '.' include dir -- no meson.build
 * change is needed. It hoists the pieces the two binaries otherwise copy-paste:
 *
 *   - checkComplete(): poll a synchronous shim op to SUCCESS.
 *   - doXfer(): the prep -> post -> check -> release ceremony for a transfer that
 *     is expected to complete (returns true iff it did).
 *   - makeEngine(): the ~30-line engine bootstrap (arg parse, transport_id/vfu_addr
 *     params, nixlBackendInitParams fill, ctor, getInitErr check). The engine
 *     copies its init params in the base ctor (nixlBackendEngine), so the locals
 *     built here need not outlive the returned engine.
 *   - ValueBuf: a value buffer allocated either from the C++ heap (ordinary DRAM
 *     -> staged path) or from SPDK-DMA memory (fd-backed -> zero-copy path). The
 *     allocator choice is what makes a verify "staged" vs "direct".
 *   - storeRetrieveVerify() / writeReadVerify(): the round-trip verify helpers,
 *     each parameterized on the allocator (BufKind) and an expect_direct flag that
 *     asserts the zero-copy datapath (dramIsDmaRegistered) when set. The former
 *     twins (verify / verifyDirect) collapse into one function each.
 */

#ifndef SPDK_TEST_COMMON_H
#define SPDK_TEST_COMMON_H

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "nixl_descriptors.h"
#include "backend/backend_aux.h"
#include "spdk_backend.h"

extern "C" {
#include "spdk_shim.h" // SPDK_SHIM_MAX_VALUE_LEN, dma_alloc/free, ...
}

namespace spdk_test {

// Poll a synchronous shim op to completion. Shim ops complete inline, so checkXfer
// should report SUCCESS immediately; IN_PROG is tolerated for a bounded spin.
inline bool
checkComplete(const nixlSpdkEngine &eng, nixlBackendReqH *h) {
    for (int i = 0; i < 1000; ++i) {
        nixl_status_t s = eng.checkXfer(h);
        if (s == NIXL_SUCCESS) return true;
        if (s != NIXL_IN_PROG) {
            std::cerr << "checkXfer error status=" << s << "\n";
            return false;
        }
    }
    std::cerr << "checkXfer never completed\n";
    return false;
}

// Run one transfer that is EXPECTED to complete: prep -> post -> check, always
// releasing the handle. Returns true iff prep succeeded, post did not error
// (SUCCESS/IN_PROG), and the op polled to SUCCESS. Not for the reject/mismatch
// cases (which inspect prep/post/check statuses individually).
inline bool
doXfer(const nixlSpdkEngine &eng, nixl_xfer_op_t op, const nixl_meta_dlist_t &local,
       const nixl_meta_dlist_t &remote, const std::string &agent) {
    nixlBackendReqH *h = nullptr;
    if (eng.prepXfer(op, local, remote, agent, h) != NIXL_SUCCESS) return false;
    nixl_status_t ps = eng.postXfer(op, local, remote, agent, h);
    bool ok = (ps == NIXL_SUCCESS || ps == NIXL_IN_PROG) && checkComplete(eng, h);
    eng.releaseReqH(h);
    return ok;
}

// Engine bootstrap. Accepts either a full SPDK transport ID (contains "trtype:")
// or a bare vfio-user socket directory (wrapped into a VFIOUSER trid). block=true
// binds a CSI==NVM block namespace (the LBA/KV knob). init_env=true because the
// standalone test owns no host SPDK env. Returns nullptr on init error (the
// engine ctor has already logged the reason, e.g. the -ENOTSUP metadata refusal);
// the caller prints its own FAIL line and exits non-zero.
inline std::unique_ptr<nixlSpdkEngine>
makeEngine(const std::string &arg, const char *agent, bool block) {
    nixl_b_params_t params;
    if (arg.find("trtype:") != std::string::npos) {
        params["transport_id"] = arg;
    } else {
        params["vfu_addr"] = arg;
    }
    if (block) params["csi"] = "block";
    params["init_env"] = "true";

    nixlBackendInitParams init{};
    init.localAgent = agent;
    init.type = "SPDK";
    init.customParams = &params;
    init.enableProgTh = false;
    init.pthrDelay = 0;
    init.enableTelemetry_ = false;

    auto eng = std::make_unique<nixlSpdkEngine>(&init);
    if (eng->getInitErr()) return nullptr;
    return eng;
}

// A value buffer for a round-trip: Heap (std::vector -> ordinary DRAM, staged
// path) or Dma (spdk_shim_dma_alloc -> fd-backed, zero-copy path). Frees
// through the matching deallocator.
enum class BufKind { Heap, Dma };

class ValueBuf {
public:
    ValueBuf(BufKind kind, size_t len) : kind_(kind), len_(len) {
        if (kind_ == BufKind::Heap) {
            heap_.assign(len_, 0);
            ptr_ = heap_.data();
        } else {
            ptr_ = spdk_shim_dma_alloc(len_);
        }
    }
    ~ValueBuf() {
        if (kind_ == BufKind::Dma) spdk_shim_dma_free(ptr_);
    }
    ValueBuf(const ValueBuf &) = delete;
    ValueBuf &operator=(const ValueBuf &) = delete;

    bool valid() const { return ptr_ != nullptr; }
    uint8_t *data() { return static_cast<uint8_t *>(ptr_); }

private:
    BufKind kind_;
    size_t len_;
    void *ptr_ = nullptr;
    std::vector<uint8_t> heap_; // backing store when kind_==Heap
};

// KV round-trip: Store then Retrieve a `size`-byte value under `key`, verifying
// the retrieved bytes match byte-for-byte. Fills the source with a size-dependent
// pattern so a mis-scattered region corrupts the compare. `kind` selects the
// allocator (Heap -> staged, Dma -> zero-copy); `expect_direct` asserts the
// zero-copy datapath (dramIsDmaRegistered on both DRAM buffers). Exercises the
// region-bounded SGL across sizes that span several 2 MiB DMA regions.
inline bool
storeRetrieveVerify(nixlSpdkEngine &eng, const std::string &agent, const std::string &key,
                    size_t size, BufKind kind = BufKind::Heap, bool expect_direct = false) {
    ValueBuf src(kind, size), dst(kind, size);
    if (!src.valid() || !dst.valid()) {
        std::cerr << "FAIL: value buffer alloc(" << size << ") for round-trip\n";
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        src.data()[i] = static_cast<uint8_t>((i * 1103515245u + 12345u) >> 16);
    }
    std::memset(dst.data(), 0, size);
    const uint64_t dram_dev = 0, key_dev = 1;

    nixlBlobDesc src_desc(reinterpret_cast<uintptr_t>(src.data()), size, dram_dev, "");
    nixlBackendMD *src_md = nullptr;
    nixlBlobDesc dst_desc(reinterpret_cast<uintptr_t>(dst.data()), size, dram_dev, "");
    nixlBackendMD *dst_md = nullptr;
    nixlBlobDesc key_desc(0, size, key_dev, key);
    nixlBackendMD *key_md = nullptr;

    bool ok = false;
    do {
        if (eng.registerMem(src_desc, DRAM_SEG, src_md) != NIXL_SUCCESS ||
            eng.registerMem(dst_desc, DRAM_SEG, dst_md) != NIXL_SUCCESS ||
            eng.registerMem(key_desc, OBJ_SEG, key_md) != NIXL_SUCCESS) {
            std::cerr << "FAIL: registerMem failed for size " << size << "\n";
            break;
        }
        // Zero-copy discriminator: SPDK-DMA buffers MUST take the direct datapath.
        if (expect_direct && (!eng.dramIsDmaRegistered(src_md) || !eng.dramIsDmaRegistered(dst_md))) {
            std::cerr << "FAIL: expected zero-copy DMA path for SPDK-DMA buffers (size " << size
                      << ")\n";
            break;
        }

        nixl_meta_dlist_t local(DRAM_SEG);
        local.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(src.data()), size, dram_dev, src_md));
        nixl_meta_dlist_t remote(OBJ_SEG);
        remote.addDesc(nixlMetaDesc(0, size, key_dev, key_md));
        nixl_meta_dlist_t local_dst(DRAM_SEG);
        local_dst.addDesc(
            nixlMetaDesc(reinterpret_cast<uintptr_t>(dst.data()), size, dram_dev, dst_md));

        if (!doXfer(eng, NIXL_WRITE, local, remote, agent)) break;    // KV Store
        if (!doXfer(eng, NIXL_READ, local_dst, remote, agent)) break; // KV Retrieve
        ok = (std::memcmp(src.data(), dst.data(), size) == 0);
    } while (0);

    eng.deregisterMem(key_md);
    eng.deregisterMem(dst_md);
    eng.deregisterMem(src_md);
    return ok;
}

// Block round-trip: write a `size`-byte pattern to the namespace at `lba`, read
// it back into a separate buffer, verify byte-for-byte. The pattern depends on
// both offset and lba so a mis-addressed/mis-staged transfer corrupts the
// compare. `kind`/`expect_direct` behave as in storeRetrieveVerify (Dma+direct
// asserts the zero-copy block datapath).
inline bool
writeReadVerify(nixlSpdkEngine &eng, const std::string &agent, uint64_t lba, size_t size,
                BufKind kind = BufKind::Heap, bool expect_direct = false) {
    ValueBuf src(kind, size), dst(kind, size);
    if (!src.valid() || !dst.valid()) {
        std::cerr << "FAIL: value buffer alloc(" << size << ") for block round-trip\n";
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        src.data()[i] =
            static_cast<uint8_t>((i * 2654435761u + static_cast<uint32_t>(lba) * 40503u) >> 13);
    }
    std::memset(dst.data(), 0, size);
    const uint64_t dram_dev = 0, blk_dev = 1;

    nixlBlobDesc src_desc(reinterpret_cast<uintptr_t>(src.data()), size, dram_dev, "");
    nixlBackendMD *src_md = nullptr;
    nixlBlobDesc dst_desc(reinterpret_cast<uintptr_t>(dst.data()), size, dram_dev, "");
    nixlBackendMD *dst_md = nullptr;
    nixlBlobDesc blk_desc(lba, size, blk_dev, ""); // addr == LBA, no key
    nixlBackendMD *blk_md = nullptr;

    bool ok = false;
    do {
        if (eng.registerMem(src_desc, DRAM_SEG, src_md) != NIXL_SUCCESS ||
            eng.registerMem(dst_desc, DRAM_SEG, dst_md) != NIXL_SUCCESS ||
            eng.registerMem(blk_desc, BLK_SEG, blk_md) != NIXL_SUCCESS) {
            std::cerr << "FAIL: registerMem failed for size " << size << "\n";
            break;
        }
        if (expect_direct && (!eng.dramIsDmaRegistered(src_md) || !eng.dramIsDmaRegistered(dst_md))) {
            std::cerr << "FAIL: expected zero-copy DMA path for SPDK-DMA block buffers (size "
                      << size << ")\n";
            break;
        }

        nixl_meta_dlist_t local(DRAM_SEG);
        local.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(src.data()), size, dram_dev, src_md));
        nixl_meta_dlist_t remote(BLK_SEG);
        remote.addDesc(nixlMetaDesc(lba, size, blk_dev, blk_md));
        nixl_meta_dlist_t local_dst(DRAM_SEG);
        local_dst.addDesc(
            nixlMetaDesc(reinterpret_cast<uintptr_t>(dst.data()), size, dram_dev, dst_md));

        if (!doXfer(eng, NIXL_WRITE, local, remote, agent)) break; // block write
        if (!doXfer(eng, NIXL_READ, local_dst, remote, agent)) break; // block read
        ok = (std::memcmp(src.data(), dst.data(), size) == 0);
    } while (0);

    eng.deregisterMem(blk_md);
    eng.deregisterMem(dst_md);
    eng.deregisterMem(src_md);
    return ok;
}

} // namespace spdk_test

#endif // SPDK_TEST_COMMON_H
