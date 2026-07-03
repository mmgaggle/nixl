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
 * Direct-engine round-trip test for the generic SPDK backend's BLOCK (BLK_SEG)
 * datapath -- NVMe LBA read/write alongside the KV path in the same plugin.
 *
 * Instantiates nixlSpdkKvEngine directly (no nixlAgent) with the block namespace
 * knob (csi=block), registers a DRAM source/destination buffer and a BLK_SEG
 * remote descriptor whose addr is the starting LBA and devId is the namespace,
 * then:
 *   1. WRITE (DRAM -> remote LBA) => NVMe block write
 *   2. READ  (remote LBA -> DRAM) => NVMe block read
 * and verifies the read-back bytes match byte-for-byte across a sweep of sizes
 * from 4 KiB up through the region-bounded SGL range -- 2 MiB (1 region), 8 MiB
 * (4 regions), and ~60 MiB (30 regions) -- and at a non-zero LBA. It then:
 *   3. distinct-LBA addressing: writes DIFFERENT patterns to two disjoint LBA
 *      ranges and asserts each reads back ITS OWN pattern (so a datapath that
 *      ignored remote.addr and always hit one LBA would fail);
 *   4. independent per-region LBA placement (intra-transfer scatter guard):
 *      writes a LARGE multi-region buffer (8 MiB = 4 x 2 MiB regions) at LBA 0
 *      in ONE WRITE where each 2 MiB region carries a DISTINCT region-keyed
 *      marker, then reads each region back through a SEPARATE single-region op
 *      at that region's OWN LBA (region k -> LBA k*(2 MiB / sector)) and asserts
 *      each returns region k's marker. Because the large WRITE and the per-region
 *      READs use INDEPENDENT addressing, a large-WRITE that scattered a 2 MiB
 *      region to the wrong LBA is caught here -- unlike the same-SGL round-trip
 *      above, which cannot see a symmetric (write==read) region permutation;
 * and exercises the block validation guards, each of which must be REJECTED with
 * the SPECIFIC expected status (not merely "some non-success"), never a partial
 * transfer:
 *   5. a length that is not a multiple of the sector size => INVALID_PARAM;
 *   6. a length past the region-bounded single-op bound (~64 MiB; a 68 MiB
 *      transfer) => INVALID_PARAM, NOT split;
 *   7. an LBA range past the namespace capacity => INVALID_PARAM;
 *   8. QUERY on BLK_SEG => NIXL_ERR_NOT_SUPPORTED (no per-LBA existence).
 *
 * Sizes are multiples of 4096 bytes so the happy path is valid for either a
 * 512- or 4096-byte-sector namespace; the misalignment case (a length that is
 * not a multiple of 512) is rejected on either.
 *
 * Usage: spdk_kv_block_roundtrip_test <transport-id-or-vfio-user-dir>
 *   e.g. spdk_kv_block_roundtrip_test "trtype:VFIOUSER traddr:/tmp/.../muser0/0"
 *   or   spdk_kv_block_roundtrip_test /tmp/.../muser0/0   (wrapped into VFIOUSER)
 */

#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "nixl_descriptors.h"
#include "backend/backend_aux.h"
#include "spdk_kv_backend.h"

extern "C" {
#include "spdk_kv_shim.h" // SPDK_KV_SHIM_MAX_VALUE_LEN (the single-op bound)
}

namespace {

bool
checkComplete(const nixlSpdkKvEngine &eng, nixlBackendReqH *h) {
    // Shim ops are synchronous; checkXfer should report SUCCESS immediately.
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

// Write a `size`-byte pattern to the block namespace at `lba`, read it back into
// a separate DRAM buffer, and verify the bytes match byte-for-byte. The pattern
// depends on both offset and lba so a mis-addressed or mis-staged transfer would
// corrupt the comparison.
bool
writeReadVerify(nixlSpdkKvEngine &eng, const std::string &agent, uint64_t lba, size_t size) {
    std::vector<uint8_t> src(size), dst(size, 0);
    for (size_t i = 0; i < size; ++i) {
        src[i] = static_cast<uint8_t>((i * 2654435761u + static_cast<uint32_t>(lba) * 40503u) >> 13);
    }
    const uint64_t dram_dev = 0, blk_dev = 1;

    nixlBlobDesc src_desc(reinterpret_cast<uintptr_t>(src.data()), size, dram_dev, "");
    nixlBackendMD *src_md = nullptr;
    nixlBlobDesc dst_desc(reinterpret_cast<uintptr_t>(dst.data()), size, dram_dev, "");
    nixlBackendMD *dst_md = nullptr;
    nixlBlobDesc blk_desc(lba, size, blk_dev, ""); // addr == LBA, no key
    nixlBackendMD *blk_md = nullptr;
    if (eng.registerMem(src_desc, DRAM_SEG, src_md) != NIXL_SUCCESS ||
        eng.registerMem(dst_desc, DRAM_SEG, dst_md) != NIXL_SUCCESS ||
        eng.registerMem(blk_desc, BLK_SEG, blk_md) != NIXL_SUCCESS) {
        std::cerr << "FAIL: registerMem failed for size " << size << "\n";
        return false;
    }

    nixl_meta_dlist_t local(DRAM_SEG);
    local.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(src.data()), size, dram_dev, src_md));
    nixl_meta_dlist_t remote(BLK_SEG);
    remote.addDesc(nixlMetaDesc(lba, size, blk_dev, blk_md));
    nixl_meta_dlist_t local_dst(DRAM_SEG);
    local_dst.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(dst.data()), size, dram_dev, dst_md));

    bool ok = false;
    do {
        // WRITE => block write
        nixlBackendReqH *h = nullptr;
        if (eng.prepXfer(NIXL_WRITE, local, remote, agent, h) != NIXL_SUCCESS) break;
        nixl_status_t ps = eng.postXfer(NIXL_WRITE, local, remote, agent, h);
        if (ps != NIXL_SUCCESS && ps != NIXL_IN_PROG) {
            eng.releaseReqH(h);
            break;
        }
        if (!checkComplete(eng, h)) {
            eng.releaseReqH(h);
            break;
        }
        eng.releaseReqH(h);

        // READ => block read
        h = nullptr;
        if (eng.prepXfer(NIXL_READ, local_dst, remote, agent, h) != NIXL_SUCCESS) break;
        ps = eng.postXfer(NIXL_READ, local_dst, remote, agent, h);
        if (ps != NIXL_SUCCESS && ps != NIXL_IN_PROG) {
            eng.releaseReqH(h);
            break;
        }
        if (!checkComplete(eng, h)) {
            eng.releaseReqH(h);
            break;
        }
        eng.releaseReqH(h);

        ok = (src == dst);
    } while (0);

    eng.deregisterMem(blk_md);
    eng.deregisterMem(dst_md);
    eng.deregisterMem(src_md);
    return ok;
}

// Zero-copy variant of writeReadVerify: source/destination come from SPDK-DMA
// memory (spdk_kv_shim_dma_alloc -> fd-backed, reachable by the vfio-user
// target), so registerMem takes the DIRECT block datapath -- the device DMAs
// straight to/from the caller's own buffer, no staging copy. Asserts the engine
// reports zero-copy, then write+read `size` bytes at `lba` byte-exact.
bool
writeReadVerifyDirect(nixlSpdkKvEngine &eng, const std::string &agent, uint64_t lba, size_t size) {
    void *src = spdk_kv_shim_dma_alloc(size);
    void *dst = spdk_kv_shim_dma_alloc(size);
    if (src == nullptr || dst == nullptr) {
        std::cerr << "FAIL: dma_alloc(" << size << ") for zero-copy block test\n";
        spdk_kv_shim_dma_free(src);
        spdk_kv_shim_dma_free(dst);
        return false;
    }
    auto *src_b = static_cast<uint8_t *>(src);
    for (size_t i = 0; i < size; ++i) {
        src_b[i] = static_cast<uint8_t>((i * 2654435761u + static_cast<uint32_t>(lba) * 40503u) >> 13);
    }
    std::memset(dst, 0, size);
    const uint64_t dram_dev = 0, blk_dev = 1;

    nixlBlobDesc src_desc(reinterpret_cast<uintptr_t>(src), size, dram_dev, "");
    nixlBackendMD *src_md = nullptr;
    nixlBlobDesc dst_desc(reinterpret_cast<uintptr_t>(dst), size, dram_dev, "");
    nixlBackendMD *dst_md = nullptr;
    nixlBlobDesc blk_desc(lba, size, blk_dev, ""); // addr == LBA, no key
    nixlBackendMD *blk_md = nullptr;

    bool ok = false;
    do {
        if (eng.registerMem(src_desc, DRAM_SEG, src_md) != NIXL_SUCCESS ||
            eng.registerMem(dst_desc, DRAM_SEG, dst_md) != NIXL_SUCCESS ||
            eng.registerMem(blk_desc, BLK_SEG, blk_md) != NIXL_SUCCESS) {
            std::cerr << "FAIL: registerMem (zero-copy block) failed for size " << size << "\n";
            break;
        }
        if (!eng.dramIsDmaRegistered(src_md) || !eng.dramIsDmaRegistered(dst_md)) {
            std::cerr << "FAIL: expected zero-copy DMA path for SPDK-DMA block buffers (size "
                      << size << ")\n";
            break;
        }

        nixl_meta_dlist_t local(DRAM_SEG);
        local.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(src), size, dram_dev, src_md));
        nixl_meta_dlist_t remote(BLK_SEG);
        remote.addDesc(nixlMetaDesc(lba, size, blk_dev, blk_md));
        nixl_meta_dlist_t local_dst(DRAM_SEG);
        local_dst.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(dst), size, dram_dev, dst_md));

        nixlBackendReqH *h = nullptr;
        if (eng.prepXfer(NIXL_WRITE, local, remote, agent, h) != NIXL_SUCCESS) break;
        nixl_status_t ps = eng.postXfer(NIXL_WRITE, local, remote, agent, h);
        if (ps != NIXL_SUCCESS && ps != NIXL_IN_PROG) { eng.releaseReqH(h); break; }
        if (!checkComplete(eng, h)) { eng.releaseReqH(h); break; }
        eng.releaseReqH(h);

        h = nullptr;
        if (eng.prepXfer(NIXL_READ, local_dst, remote, agent, h) != NIXL_SUCCESS) break;
        ps = eng.postXfer(NIXL_READ, local_dst, remote, agent, h);
        if (ps != NIXL_SUCCESS && ps != NIXL_IN_PROG) { eng.releaseReqH(h); break; }
        if (!checkComplete(eng, h)) { eng.releaseReqH(h); break; }
        eng.releaseReqH(h);

        ok = (std::memcmp(src, dst, size) == 0);
    } while (0);

    eng.deregisterMem(blk_md);
    eng.deregisterMem(dst_md);
    eng.deregisterMem(src_md);
    spdk_kv_shim_dma_free(src);
    spdk_kv_shim_dma_free(dst);
    return ok;
}

// Attempt a WRITE of `size` bytes at `lba` and assert the engine REJECTS it with
// the SPECIFIC status `expected` -- not merely "some non-success". Checks that
// the FIRST non-success across prep -> post -> check equals `expected` exactly,
// so a guard that fired for the wrong reason (or a stray error) still fails the
// test. A tiny backing buffer is enough: every guarded case is rejected before
// any DMA is staged, which also proves the reject is not a partial/striped
// transfer. Returns true iff the reject matched `expected`.
bool
expectReject(nixlSpdkKvEngine &eng, const std::string &agent, uint64_t lba, size_t size,
             nixl_status_t expected) {
    std::vector<uint8_t> tiny(4096, 0);
    const uint64_t dram_dev = 0, blk_dev = 1;

    nixlBlobDesc blk_desc(lba, size, blk_dev, "");
    nixlBackendMD *blk_md = nullptr;
    if (eng.registerMem(blk_desc, BLK_SEG, blk_md) != NIXL_SUCCESS) {
        std::cerr << "FAIL: registerMem(BLK_SEG) failed unexpectedly\n";
        return false;
    }

    nixl_meta_dlist_t local(DRAM_SEG);
    local.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(tiny.data()), size, dram_dev, nullptr));
    nixl_meta_dlist_t remote(BLK_SEG);
    remote.addDesc(nixlMetaDesc(lba, size, blk_dev, blk_md));

    nixlBackendReqH *h = nullptr;
    nixl_status_t pp = eng.prepXfer(NIXL_WRITE, local, remote, agent, h);
    nixl_status_t ps = NIXL_SUCCESS, cs = NIXL_SUCCESS;
    if (pp == NIXL_SUCCESS) {
        ps = eng.postXfer(NIXL_WRITE, local, remote, agent, h);
        cs = eng.checkXfer(h);
        eng.releaseReqH(h);
    }
    eng.deregisterMem(blk_md);

    // The FIRST stage that did not succeed carries the reject reason.
    nixl_status_t first_fail;
    if (pp != NIXL_SUCCESS) {
        first_fail = pp;
    } else if (ps != NIXL_SUCCESS) {
        first_fail = ps;
    } else if (cs != NIXL_SUCCESS) {
        first_fail = cs;
    } else {
        std::cerr << "FAIL: expected reject " << expected
                  << " but prep/post/check all succeeded (lba=" << lba << " size=" << size
                  << ")\n";
        return false;
    }
    if (first_fail != expected) {
        std::cerr << "FAIL: expected reject status " << expected << " but got " << first_fail
                  << " (prep=" << pp << " post=" << ps << " check=" << cs << ", lba=" << lba
                  << " size=" << size << ")\n";
        return false;
    }
    return true;
}

// Perform ONE block op: WRITE copies `data` (bytes) to the namespace at `lba`;
// READ fills `data` from the namespace at `lba`. `data.size()` must be a valid
// (sector-aligned, in-range, single-range) length. Returns true on success.
// Drives writes/reads independently so a caller can address distinct LBAs.
bool
blockOp(nixlSpdkKvEngine &eng, const std::string &agent, nixl_xfer_op_t op, uint64_t lba,
        std::vector<uint8_t> &data) {
    const size_t size = data.size();
    const uint64_t dram_dev = 0, blk_dev = 1;

    nixlBlobDesc dram_desc(reinterpret_cast<uintptr_t>(data.data()), size, dram_dev, "");
    nixlBackendMD *dram_md = nullptr;
    nixlBlobDesc blk_desc(lba, size, blk_dev, "");
    nixlBackendMD *blk_md = nullptr;
    if (eng.registerMem(dram_desc, DRAM_SEG, dram_md) != NIXL_SUCCESS ||
        eng.registerMem(blk_desc, BLK_SEG, blk_md) != NIXL_SUCCESS) {
        std::cerr << "FAIL: registerMem failed (blockOp)\n";
        return false;
    }

    nixl_meta_dlist_t local(DRAM_SEG);
    local.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(data.data()), size, dram_dev, dram_md));
    nixl_meta_dlist_t remote(BLK_SEG);
    remote.addDesc(nixlMetaDesc(lba, size, blk_dev, blk_md));

    bool ok = false;
    nixlBackendReqH *h = nullptr;
    if (eng.prepXfer(op, local, remote, agent, h) == NIXL_SUCCESS) {
        nixl_status_t ps = eng.postXfer(op, local, remote, agent, h);
        if ((ps == NIXL_SUCCESS || ps == NIXL_IN_PROG) && checkComplete(eng, h)) {
            ok = true;
        }
        eng.releaseReqH(h);
    }
    eng.deregisterMem(blk_md);
    eng.deregisterMem(dram_md);
    return ok;
}

} // namespace

int
main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <transport-id-or-vfio-user-dir>\n";
        return 2;
    }
    // Accept either a full SPDK transport ID (contains "trtype:") or a bare
    // vfio-user socket directory, which the engine wraps into a VFIOUSER trid.
    const std::string arg = argv[1];

    nixl_b_params_t params;
    if (arg.find("trtype:") != std::string::npos) {
        params["transport_id"] = arg;
    } else {
        params["vfu_addr"] = arg;
    }
    // Bind a CSI==NVM block namespace (the LBA/KV knob) and bring up our own env.
    params["csi"] = "block";
    params["init_env"] = "true";

    nixlBackendInitParams init{};
    init.localAgent = "spdk_block_test_agent";
    init.type = "SPDK";
    init.customParams = &params;
    init.enableProgTh = false;
    init.pthrDelay = 0;
    init.enableTelemetry_ = false;

    nixlSpdkKvEngine eng(&init);
    if (eng.getInitErr()) {
        std::cerr << "FAIL: engine init error (block shim open failed)\n";
        return 1;
    }
    std::cout << "engine initialized (block mode) against '" << arg << "'\n";

    // --- Block round-trip across sizes (4 KiB .. ~60 MiB) ---
    // All multiples of 4096, so valid for a 512- or 4096-byte-sector namespace.
    // The larger sizes span multiple 2 MiB DMA regions and exercise the
    // region-bounded SGL: 2 MiB (1 region), 8 MiB (4 regions), 60 MiB (30
    // regions, near the ~64 MiB single-op bound). A mis-scattered region would
    // corrupt the byte-for-byte compare.
    {
        const size_t KiB = 1024, MiB = 1024 * 1024;
        const size_t sizes[] = {4 * KiB, 8 * KiB, 64 * KiB, 256 * KiB, 1 * MiB,
                                2 * MiB, 8 * MiB, 60 * MiB};
        for (size_t sz : sizes) {
            if (!writeReadVerify(eng, init.localAgent, /*lba=*/0, sz)) {
                std::cerr << "FAIL: block round-trip failed at LBA 0 for " << sz << " bytes\n";
                return 1;
            }
            const size_t regions = (sz + (2 * MiB - 1)) / (2 * MiB);
            std::cout << "block round-trip OK: " << sz << " bytes at LBA 0 ("
                      << regions << " x 2 MiB region(s))\n";
        }
    }

    // --- Non-zero LBA addressing ---
    {
        const size_t sz = 4096;
        const uint64_t lba = 8;
        if (!writeReadVerify(eng, init.localAgent, lba, sz)) {
            std::cerr << "FAIL: block round-trip failed at LBA " << lba << "\n";
            return 1;
        }
        std::cout << "block round-trip OK: " << sz << " bytes at LBA " << lba << "\n";
    }

    // --- Zero-copy block datapath (direct, no staging copy) ---
    // SPDK-DMA (fd-backed) buffers must take the DIRECT path: the device DMAs
    // straight to/from the caller's buffer. Verify a single-region and a
    // multi-region (zero-copy over the region-bounded SGL) transfer byte-exact.
    {
        const size_t MiB = 1024 * 1024;
        const struct {
            uint64_t lba;
            size_t size;
        } zc[] = {
            {0, 4096},
            {0, 8 * MiB}, // 4 x 2 MiB regions, zero-copy large block transfer
        };
        for (const auto &c : zc) {
            if (!writeReadVerifyDirect(eng, init.localAgent, c.lba, c.size)) {
                std::cerr << "FAIL: zero-copy block round-trip failed (" << c.size
                          << " bytes at LBA " << c.lba << ")\n";
                return 1;
            }
            std::cout << "zero-copy block round-trip OK: " << c.size
                      << " bytes at LBA " << c.lba << " (device DMA'd caller buffer, no staging)\n";
        }
    }

    // --- Distinct-LBA addressing: write DIFFERENT patterns to two DISJOINT LBA
    // ranges, then read each back and assert each returns ITS OWN pattern. A
    // datapath that ignores remote.addr (e.g. always addresses LBA 0) collapses
    // both writes/reads onto one range and fails this. ---
    {
        const size_t sz = 4096;                 // 8 sectors (512) or 1 sector (4096)
        const uint64_t lba_a = 0, lba_b = 16;   // disjoint on either sector size
        std::vector<uint8_t> pat_a(sz), pat_b(sz);
        for (size_t i = 0; i < sz; ++i) {
            pat_a[i] = static_cast<uint8_t>(0xA0 ^ (i * 31u));
            pat_b[i] = static_cast<uint8_t>(0x5B ^ (i * 17u));
        }
        if (pat_a == pat_b) {
            std::cerr << "FAIL: distinct-LBA test patterns are not distinct\n";
            return 1;
        }

        // Write A to lba_a, then B to lba_b (disjoint, so B must not clobber A).
        std::vector<uint8_t> wa = pat_a, wb = pat_b;
        if (!blockOp(eng, init.localAgent, NIXL_WRITE, lba_a, wa) ||
            !blockOp(eng, init.localAgent, NIXL_WRITE, lba_b, wb)) {
            std::cerr << "FAIL: distinct-LBA writes failed\n";
            return 1;
        }
        // Read each range back; each must return its OWN pattern.
        std::vector<uint8_t> ra(sz, 0), rb(sz, 0);
        if (!blockOp(eng, init.localAgent, NIXL_READ, lba_a, ra) ||
            !blockOp(eng, init.localAgent, NIXL_READ, lba_b, rb)) {
            std::cerr << "FAIL: distinct-LBA reads failed\n";
            return 1;
        }
        if (ra != pat_a || rb != pat_b) {
            std::cerr << "FAIL: distinct-LBA addressing mismatch: LBA " << lba_a << " and LBA "
                      << lba_b << " did not each return their own pattern "
                      << "(a datapath ignoring remote.addr would hit this)\n";
            return 1;
        }
        std::cout << "distinct-LBA addressing OK: LBA " << lba_a << " and LBA " << lba_b
                  << " each returned their own distinct pattern\n";
    }

    // --- Independent per-region LBA placement (intra-transfer scatter guard) ---
    // Write a LARGE multi-region buffer (8 MiB = 4 x 2 MiB regions) at LBA 0 in
    // ONE WRITE op, where EACH 2 MiB region carries a DISTINCT, region-keyed
    // marker; then read each region back through a SEPARATE single-region (2 MiB)
    // op at that region's OWN LBA (region k -> LBA k*(2 MiB / sector_size)) and
    // assert it returns region k's marker. The large WRITE and the per-region
    // READs use INDEPENDENT addressing (single-region per-op addressing is proven
    // by the distinct-LBA test above), so a large WRITE that scattered a 2 MiB
    // region to the wrong LBA makes some independent read return the wrong
    // region's marker. This is strictly stronger than the same-SGL round-trip,
    // which cannot see a symmetric (write==read) intra-transfer region permutation.
    {
        const size_t MiB = 1024 * 1024;
        const size_t region = 2 * MiB;    // one region-bounded SGL segment
        const size_t regions = 4;         // 8 MiB total = 4 regions
        const size_t total = regions * region;

        // Map a region's byte offset to its LBA via the namespace sector size.
        const uint32_t sector = eng.blockSectorSize();
        if (sector == 0 || (region % sector) != 0) {
            std::cerr << "FAIL: bad block sector size " << sector
                      << " for region-scatter test\n";
            return 1;
        }
        const uint64_t lba_per_region = region / sector;

        // Fill each 2 MiB region with a DISTINCT, region-identifying marker so a
        // mis-placed region is detectable by content alone.
        auto fillRegion = [](std::vector<uint8_t> &buf, size_t off, size_t len, size_t k) {
            for (size_t i = 0; i < len; ++i) {
                buf[off + i] = static_cast<uint8_t>(
                    (0x11u * (k + 1)) ^ ((i * 2246822519u + k * 2654435761u) >> 15));
            }
        };
        std::vector<uint8_t> big(total, 0);
        for (size_t k = 0; k < regions; ++k) {
            fillRegion(big, k * region, region, k);
        }
        // Extract the per-region markers and assert they are PAIRWISE DISTINCT --
        // else a scattered region could alias another and slip past the compare.
        std::vector<std::vector<uint8_t>> markers(regions);
        for (size_t k = 0; k < regions; ++k) {
            markers[k].assign(big.begin() + k * region, big.begin() + (k + 1) * region);
        }
        for (size_t a = 0; a < regions; ++a) {
            for (size_t b = a + 1; b < regions; ++b) {
                if (markers[a] == markers[b]) {
                    std::cerr << "FAIL: region markers " << a << " and " << b
                              << " are not distinct\n";
                    return 1;
                }
            }
        }

        // ONE large WRITE of all 4 regions at LBA 0.
        std::vector<uint8_t> wbig = big;
        if (!blockOp(eng, init.localAgent, NIXL_WRITE, /*lba=*/0, wbig)) {
            std::cerr << "FAIL: large multi-region WRITE at LBA 0 failed\n";
            return 1;
        }

        // Read EACH region back INDEPENDENTLY via its own single-region op at that
        // region's own LBA and assert it returns region k's marker. A large WRITE
        // that scattered a region to the wrong LBA is caught here.
        for (size_t k = 0; k < regions; ++k) {
            std::vector<uint8_t> rk(region, 0);
            const uint64_t lba = static_cast<uint64_t>(k) * lba_per_region;
            if (!blockOp(eng, init.localAgent, NIXL_READ, lba, rk)) {
                std::cerr << "FAIL: independent region READ failed (region " << k
                          << ", LBA " << lba << ")\n";
                return 1;
            }
            if (rk != markers[k]) {
                std::cerr << "FAIL: region-scatter: independent read at LBA " << lba
                          << " did not return region " << k << "'s marker "
                          << "(a large WRITE that scattered a 2 MiB region to the wrong "
                          << "LBA would hit this)\n";
                return 1;
            }
        }
        std::cout << "independent region-scatter OK: 8 MiB (4 x 2 MiB) WRITE at LBA 0, "
                  << "each region read back independently at its own LBA (sector="
                  << sector << ", " << lba_per_region
                  << " LBAs/region) returned its own distinct marker\n";
    }

    // --- Validation guards (each must be REJECTED with the SPECIFIC status
    // NIXL_ERR_INVALID_PARAM at prepXfer, not merely "some non-success") ---
    {
        // Length not a multiple of the sector size (odd offset from a 4 KiB base;
        // not a multiple of 512, so misaligned on any real sector size).
        if (!expectReject(eng, init.localAgent, /*lba=*/0, 4096 + 7, NIXL_ERR_INVALID_PARAM)) {
            std::cerr << "FAIL: sector-misaligned length reject\n";
            return 1;
        }
        std::cout << "sector-misaligned length correctly rejected (NIXL_ERR_INVALID_PARAM)\n";

        // Length past the region-bounded single-op bound (~64 MiB): a 68 MiB
        // transfer must be REJECTED, NOT split. A multiple of 4096 so it clears
        // alignment and is caught by the single-op bound guard, before any DMA is
        // staged (proving the reject is not a partial/striped transfer).
        const size_t oversize =
            static_cast<size_t>(SPDK_KV_SHIM_MAX_VALUE_LEN) + 4 * 1024 * 1024;
        if (!expectReject(eng, init.localAgent, /*lba=*/0, oversize, NIXL_ERR_INVALID_PARAM)) {
            std::cerr << "FAIL: over-single-op-bound length (" << oversize << " B) reject\n";
            return 1;
        }
        std::cout << "oversize length (" << (oversize / (1024 * 1024)) << " MiB > "
                  << (SPDK_KV_SHIM_MAX_VALUE_LEN / (1024 * 1024))
                  << " MiB single-op bound) correctly rejected (NIXL_ERR_INVALID_PARAM, not split)\n";

        // LBA range past the namespace capacity (well beyond any malloc bdev here).
        const uint64_t far_lba = 1ull << 40;
        if (!expectReject(eng, init.localAgent, far_lba, 4096, NIXL_ERR_INVALID_PARAM)) {
            std::cerr << "FAIL: out-of-capacity LBA " << far_lba << " reject\n";
            return 1;
        }
        std::cout << "out-of-capacity LBA correctly rejected (NIXL_ERR_INVALID_PARAM)\n";
    }

    // --- QUERY on BLK_SEG is not supported (no per-LBA existence) ---
    {
        nixl_reg_dlist_t q(BLK_SEG);
        q.addDesc(nixlBlobDesc(0, 4096, /*devId=*/1, ""));
        std::vector<nixl_query_resp_t> resp;
        nixl_status_t qs = eng.queryMem(q, resp);
        if (qs != NIXL_ERR_NOT_SUPPORTED) {
            std::cerr << "FAIL: queryMem(BLK_SEG) returned " << qs
                      << ", expected NIXL_ERR_NOT_SUPPORTED\n";
            return 1;
        }
        std::cout << "queryMem(BLK_SEG) correctly reports NOT_SUPPORTED\n";
    }

    // --- Cross-mode guard: OBJ_SEG (KV) is refused on a block-bound engine ---
    // This engine's namespace is CSI==NVM. Because the KV opcodes alias the NVM
    // WRITE/READ opcodes (KV_STORE == WRITE, KV_RETRIEVE == READ), accepting a KV
    // (OBJ_SEG) transfer here would execute a KV Store as an NVM WRITE at an SLBA
    // decoded from the KV key fields -- a silent write to a wild LBA. Assert
    // every entry point refuses OBJ_SEG cleanly, before any device op.
    {
        // getSupportedMems() must advertise the block op-set only (no OBJ_SEG).
        const nixl_mem_list_t mems = eng.getSupportedMems();
        bool has_obj = false, has_blk = false, has_dram = false;
        for (nixl_mem_t m : mems) {
            if (m == OBJ_SEG) has_obj = true;
            if (m == BLK_SEG) has_blk = true;
            if (m == DRAM_SEG) has_dram = true;
        }
        if (has_obj || !has_blk || !has_dram) {
            std::cerr << "FAIL: block-mode getSupportedMems must be {DRAM_SEG, BLK_SEG} "
                         "(no OBJ_SEG)\n";
            return 1;
        }

        // registerMem(OBJ_SEG) must be refused before building any KV-key metadata.
        nixlBlobDesc obj_desc(0, 4096, /*devId=*/1, "nixl-crossmode01");
        nixlBackendMD *obj_md = nullptr;
        if (eng.registerMem(obj_desc, OBJ_SEG, obj_md) != NIXL_ERR_NOT_SUPPORTED) {
            std::cerr << "FAIL: registerMem(OBJ_SEG) on a block engine was not refused "
                         "with NIXL_ERR_NOT_SUPPORTED\n";
            eng.deregisterMem(obj_md);
            return 1;
        }

        // queryMem(OBJ_SEG) (KV Exist) must be refused before any device op.
        nixl_reg_dlist_t q(OBJ_SEG);
        q.addDesc(nixlBlobDesc(0, 0, /*devId=*/1, "nixl-crossmode01"));
        std::vector<nixl_query_resp_t> resp;
        if (eng.queryMem(q, resp) != NIXL_ERR_NOT_SUPPORTED) {
            std::cerr << "FAIL: queryMem(OBJ_SEG) on a block engine was not refused "
                         "with NIXL_ERR_NOT_SUPPORTED\n";
            return 1;
        }

        // postXfer of an OBJ_SEG remote must be refused BEFORE any device op. The
        // remote carries a nullptr KV-key metadata, so had the mode guard NOT
        // fired first the KV path would instead fail with INVALID_PARAM (missing
        // metadata); asserting exactly NIXL_ERR_NOT_SUPPORTED proves the guard
        // refused it up front, before touching the device.
        std::vector<uint8_t> buf(4096, 0);
        nixlBlobDesc dram_desc(reinterpret_cast<uintptr_t>(buf.data()), buf.size(), 0, "");
        nixlBackendMD *dram_md = nullptr;
        if (eng.registerMem(dram_desc, DRAM_SEG, dram_md) != NIXL_SUCCESS) {
            std::cerr << "FAIL: registerMem(DRAM) for cross-mode postXfer test failed\n";
            return 1;
        }
        nixl_meta_dlist_t local(DRAM_SEG);
        local.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(buf.data()), buf.size(), 0, dram_md));
        nixl_meta_dlist_t remote(OBJ_SEG);
        remote.addDesc(nixlMetaDesc(0, buf.size(), 1, nullptr));

        nixlBackendReqH *h = nullptr;
        nixl_status_t pp = eng.prepXfer(NIXL_WRITE, local, remote, init.localAgent, h);
        nixl_status_t ps = NIXL_SUCCESS, cs = NIXL_SUCCESS;
        if (pp == NIXL_SUCCESS) {
            ps = eng.postXfer(NIXL_WRITE, local, remote, init.localAgent, h);
            cs = eng.checkXfer(h);
            eng.releaseReqH(h);
        }
        eng.deregisterMem(dram_md);
        if (ps != NIXL_ERR_NOT_SUPPORTED) {
            std::cerr << "FAIL: postXfer(OBJ_SEG) on a block engine was not refused with "
                         "NIXL_ERR_NOT_SUPPORTED (prep=" << pp << " post=" << ps
                      << " check=" << cs << ")\n";
            return 1;
        }
        std::cout << "cross-mode guard OK: block-mode engine refuses OBJ_SEG at "
                     "getSupportedMems/registerMem/queryMem/postXfer (no wild-LBA KV op)\n";
    }

    std::cout << "spdk_kv_block_roundtrip_test: PASS\n";
    return 0;
}
