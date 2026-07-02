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
 * and exercises the block validation guards, each of which must be REJECTED with
 * the SPECIFIC expected status (not merely "some non-success"), never a partial
 * transfer:
 *   4. a length that is not a multiple of the sector size => INVALID_PARAM;
 *   5. a length past the region-bounded single-op bound (~64 MiB; a 68 MiB
 *      transfer) => INVALID_PARAM, NOT split;
 *   6. an LBA range past the namespace capacity => INVALID_PARAM;
 *   7. QUERY on BLK_SEG => NIXL_ERR_NOT_SUPPORTED (no per-LBA existence).
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

    std::cout << "spdk_kv_block_roundtrip_test: PASS\n";
    return 0;
}
