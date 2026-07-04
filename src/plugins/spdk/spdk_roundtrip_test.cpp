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
 * Direct-engine round-trip test for the generic SPDK backend (Store /
 * Retrieve, Exist, value auto-sizing, and large values).
 *
 * Instantiates nixlSpdkEngine directly (no nixlAgent), registers a DRAM
 * source buffer and an OBJ_SEG remote descriptor carrying a 16-byte inline key
 * (verbatim, opaque) in metaInfo, then:
 *   1. WRITE (DRAM -> remote)  => KV Store
 *   2. READ  (remote -> DRAM)  => KV Retrieve
 * and verifies the retrieved bytes match the stored bytes. It then exercises:
 *   3. the opaque key-mapping guards (empty rejected, over-16-byte rejected,
 *      taken verbatim);
 *   4. QUERY (queryMem => KV Exist): a stored key reports a hit and an absent
 *      key reports a miss (no data transfer);
 *   5. value auto-sizing: a short-buffer READ reports the device's TRUE value
 *      length (no silent truncation) and a subsequent correctly-sized READ
 *      returns the value byte-exact;
 *   6. large values via the region-bounded SGL: byte-exact Store/Retrieve at
 *      1/8/60 MiB (spanning up to 30 x 2 MiB regions);
 *   7. large-value auto-sizing (a too-small READ of a multi-region value reports
 *      the true length, then a resized re-Retrieve is byte-exact) and the NO-
 *      striping guard (a value past the ~64 MiB single-op bound is rejected).
 *
 * Usage: spdk_roundtrip_test <transport-id-or-vfio-user-dir>
 *   e.g. spdk_roundtrip_test "trtype:VFIOUSER traddr:/tmp/.../muser0/0"
 *   or   spdk_roundtrip_test /tmp/.../muser0/0   (wrapped into VFIOUSER)
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <sys/mman.h> // mmap/munmap for the partial-reachability construction

#include "spdk_test_common.h"

using namespace spdk_test;

int
main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <transport-id-or-vfio-user-dir>\n";
        return 2;
    }
    const char *kAgent = "spdk_test_agent";
    const std::string agent = kAgent;

    // Standalone test: no host owns the SPDK env, so this engine brings it up.
    auto eng_ptr = makeEngine(argv[1], kAgent, /*block=*/false);
    if (!eng_ptr) {
        std::cerr << "FAIL: engine init error (shim open failed)\n";
        return 1;
    }
    nixlSpdkEngine &eng = *eng_ptr;
    std::cout << "engine initialized against '" << argv[1] << "'\n";

    // --- Source / destination DRAM buffers ---
    const std::string payload = "SPDK-NIXL-roundtrip-small-value-0123456789";
    std::vector<uint8_t> src(payload.begin(), payload.end());
    std::vector<uint8_t> dst(src.size(), 0);

    const uint64_t dram_dev = 0;
    const uint64_t key_dev = 1;
    const std::string kv_key = "nixl-key-0000001"; // 16 bytes, opaque

    // Register the local DRAM region (source).
    nixlBlobDesc dram_desc(reinterpret_cast<uintptr_t>(src.data()), src.size(), dram_dev, "");
    nixlBackendMD *dram_md = nullptr;
    if (eng.registerMem(dram_desc, DRAM_SEG, dram_md) != NIXL_SUCCESS) {
        std::cerr << "FAIL: registerMem(DRAM) failed\n";
        return 1;
    }

    // Register the remote OBJ_SEG descriptor carrying the KV key in metaInfo.
    nixlBlobDesc key_desc(0, src.size(), key_dev, kv_key);
    nixlBackendMD *key_md = nullptr;
    if (eng.registerMem(key_desc, OBJ_SEG, key_md) != NIXL_SUCCESS) {
        std::cerr << "FAIL: registerMem(OBJ key) failed\n";
        return 1;
    }
    std::cout << "registered DRAM source and 16-byte KV key '" << kv_key << "'\n";

    // Build meta dlists for the transfer.
    nixl_meta_dlist_t local(DRAM_SEG);
    local.addDesc(nixlMetaDesc(
        reinterpret_cast<uintptr_t>(src.data()), src.size(), dram_dev, dram_md));

    nixl_meta_dlist_t remote(OBJ_SEG);
    remote.addDesc(nixlMetaDesc(0, src.size(), key_dev, key_md));

    nixl_meta_dlist_t local_dst(DRAM_SEG);
    local_dst.addDesc(nixlMetaDesc(
        reinterpret_cast<uintptr_t>(dst.data()), dst.size(), dram_dev, dram_md));

    // --- WRITE => KV Store ---
    if (!doXfer(eng, NIXL_WRITE, local, remote, agent)) {
        std::cerr << "FAIL: WRITE (KV Store) did not complete\n";
        return 1;
    }
    std::cout << "WRITE (KV Store) of " << src.size() << " bytes complete\n";

    // --- READ => KV Retrieve ---
    if (!doXfer(eng, NIXL_READ, local_dst, remote, agent)) {
        std::cerr << "FAIL: READ (KV Retrieve) did not complete\n";
        return 1;
    }
    std::cout << "READ (KV Retrieve) of " << dst.size() << " bytes complete\n";

    // --- Verify byte-for-byte ---
    if (src != dst) {
        std::cerr << "FAIL: data mismatch after round-trip\n";
        std::cerr << "  wrote: " << payload << "\n";
        std::cerr << "  read : " << std::string(dst.begin(), dst.end()) << "\n";
        return 1;
    }
    std::cout << "verified " << dst.size() << " bytes match: \""
              << std::string(dst.begin(), dst.end()) << "\"\n";

    // --- Opaque key mapping guards (pure checks; no Store) ---
    {
        std::vector<uint8_t> k_ok, k_over, k_empty;
        // Verbatim: a 16-byte id maps to the same 16 bytes.
        if (!spdkKvKeyFromBlobId("nixl-key-0000001", 16, k_ok) || k_ok.size() != 16 ||
            std::string(k_ok.begin(), k_ok.end()) != "nixl-key-0000001") {
            std::cerr << "FAIL: 16-byte key was not taken verbatim\n";
            return 1;
        }
        // Over-length is rejected, not truncated.
        if (spdkKvKeyFromBlobId(std::string(17, 'x'), 16, k_over)) {
            std::cerr << "FAIL: 17-byte key was NOT rejected (truncation risk)\n";
            return 1;
        }
        // Empty is rejected.
        if (spdkKvKeyFromBlobId("", 16, k_empty)) {
            std::cerr << "FAIL: empty key was NOT rejected\n";
            return 1;
        }
        std::cout << "key mapping: 16-byte id verbatim; empty and >16-byte rejected\n";
    }

    // --- QUERY => KV Exist: the stored key reports a hit; an absent key
    // reports a miss. No value data is transferred either way. ---
    {
        // Hit: the key we just Stored.
        nixl_reg_dlist_t q_hit(OBJ_SEG);
        q_hit.addDesc(nixlBlobDesc(0, 0, key_dev, kv_key));
        std::vector<nixl_query_resp_t> resp_hit;
        nixl_status_t qs = eng.queryMem(q_hit, resp_hit);
        if (qs != NIXL_SUCCESS || resp_hit.size() != 1 || !resp_hit[0].has_value()) {
            std::cerr << "FAIL: QUERY on stored key '" << kv_key
                      << "' did not report a hit (status=" << qs
                      << ", n=" << resp_hit.size() << ")\n";
            return 1;
        }

        // Miss: a 16-byte key that was never Stored.
        const std::string absent_key = "nixl-key-ABSENT!"; // 16 bytes, never stored
        nixl_reg_dlist_t q_miss(OBJ_SEG);
        q_miss.addDesc(nixlBlobDesc(0, 0, key_dev, absent_key));
        std::vector<nixl_query_resp_t> resp_miss;
        qs = eng.queryMem(q_miss, resp_miss);
        if (qs != NIXL_SUCCESS || resp_miss.size() != 1 || resp_miss[0].has_value()) {
            std::cerr << "FAIL: QUERY on absent key '" << absent_key
                      << "' did not report a miss (status=" << qs
                      << ", n=" << resp_miss.size() << ")\n";
            return 1;
        }
        std::cout << "QUERY (KV Exist): stored key -> hit, absent key -> miss\n";
    }

    // --- Value auto-sizing: a short-buffer READ reports the device's TRUE
    // value length (no silent truncation), then a correctly-sized re-Retrieve
    // returns the value byte-exact. ---
    {
        const size_t short_len = 10; // < payload.size()
        std::vector<uint8_t> short_dst(short_len, 0);

        nixl_meta_dlist_t s_local(DRAM_SEG);
        s_local.addDesc(nixlMetaDesc(
            reinterpret_cast<uintptr_t>(short_dst.data()), short_len, dram_dev, dram_md));
        nixl_meta_dlist_t s_remote(OBJ_SEG);
        s_remote.addDesc(nixlMetaDesc(0, short_len, key_dev, key_md));

        nixlBackendReqH *h = nullptr;
        if (eng.prepXfer(NIXL_READ, s_local, s_remote, agent, h) != NIXL_SUCCESS) {
            std::cerr << "FAIL: prepXfer(short READ) failed\n";
            return 1;
        }
        nixl_status_t ps = eng.postXfer(NIXL_READ, s_local, s_remote, agent, h);
        nixl_status_t cs = eng.checkXfer(h);
        if (ps != NIXL_ERR_MISMATCH || cs != NIXL_ERR_MISMATCH) {
            std::cerr << "FAIL: short READ (" << short_len << " bytes) of a "
                      << payload.size() << "-byte value did NOT report MISMATCH (post="
                      << ps << " check=" << cs << ")\n";
            eng.releaseReqH(h);
            return 1;
        }
        const size_t true_len = eng.getReqTrueLen(h, 0);
        eng.releaseReqH(h);
        if (true_len != payload.size()) {
            std::cerr << "FAIL: short READ reported true length " << true_len
                      << ", expected " << payload.size() << "\n";
            return 1;
        }
        std::cout << "short READ (" << short_len << " bytes) reported true length "
                  << true_len << " (no silent truncation)\n";

        // Resize to the reported true length and re-Retrieve => byte-exact.
        std::vector<uint8_t> resized_dst(true_len, 0);
        nixl_meta_dlist_t r_local(DRAM_SEG);
        r_local.addDesc(nixlMetaDesc(
            reinterpret_cast<uintptr_t>(resized_dst.data()), true_len, dram_dev, dram_md));
        nixl_meta_dlist_t r_remote(OBJ_SEG);
        r_remote.addDesc(nixlMetaDesc(0, true_len, key_dev, key_md));

        if (!doXfer(eng, NIXL_READ, r_local, r_remote, agent)) {
            std::cerr << "FAIL: resized READ did not complete\n";
            return 1;
        }
        if (resized_dst != src) {
            std::cerr << "FAIL: resized READ data mismatch after auto-sizing\n";
            return 1;
        }
        std::cout << "resized READ (" << true_len
                  << " bytes) after auto-sizing matches byte-exact\n";
    }

    eng.deregisterMem(key_md);
    eng.deregisterMem(dram_md);

    // --- Large values via region-bounded SGL ---
    // Store then Retrieve byte-exact across sizes that span multiple 2 MiB DMA
    // regions: 1 MiB (single region), 8 MiB (4 regions), 60 MiB (30 regions,
    // near the ~64 MiB single-op bound). Requires the target's kvdev to allow
    // values this large (run_roundtrip.sh raises kvdev_mem --max-value-len).
    {
        const size_t MiB = 1024 * 1024;
        const struct {
            const char *key;
            size_t size;
        } cases[] = {
            {"nixl-large-1mib0", 1 * MiB},
            {"nixl-large-8mib0", 8 * MiB},
            {"nixl-large-60mib", 60 * MiB},
        };
        for (const auto &c : cases) {
            if (!storeRetrieveVerify(eng, agent, c.key, c.size)) {
                std::cerr << "FAIL: large-value round-trip failed for " << (c.size / MiB)
                          << " MiB (key '" << c.key << "')\n";
                return 1;
            }
            const size_t regions = (c.size + (2 * MiB - 1)) / (2 * MiB);
            std::cout << "large-value round-trip OK: " << (c.size / MiB) << " MiB ("
                      << regions << " x 2 MiB region(s), key '" << c.key << "')\n";
        }
    }

    // --- Large value + value auto-sizing (integration) ---
    // Store a multi-region large value, Retrieve it into a TOO-SMALL buffer: the
    // read must report the device's TRUE length (no partial/striped copy), then
    // a correctly-sized (multi-region SGL) re-Retrieve must be byte-exact.
    {
        const size_t MiB = 1024 * 1024;
        const size_t big_len = 8 * MiB;   // 4 x 2 MiB regions
        const size_t small_len = 1 * MiB; // too small: forces auto-sizing
        const uint64_t dram_dev = 0, key_dev = 1;
        const std::string key = "nixl-lgautosz-01"; // 16 bytes

        std::vector<uint8_t> big_src(big_len), small_dst(small_len, 0);
        for (size_t i = 0; i < big_len; ++i) {
            big_src[i] = static_cast<uint8_t>((i * 2654435761u) >> 11);
        }

        nixlBlobDesc src_desc(reinterpret_cast<uintptr_t>(big_src.data()), big_len, dram_dev, "");
        nixlBackendMD *src_md = nullptr;
        nixlBlobDesc key_desc(0, big_len, key_dev, key);
        nixlBackendMD *k_md = nullptr;
        if (eng.registerMem(src_desc, DRAM_SEG, src_md) != NIXL_SUCCESS ||
            eng.registerMem(key_desc, OBJ_SEG, k_md) != NIXL_SUCCESS) {
            std::cerr << "FAIL: registerMem (large+autosize) failed\n";
            return 1;
        }

        // Store the 8 MiB value.
        {
            nixl_meta_dlist_t l(DRAM_SEG);
            l.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(big_src.data()), big_len, dram_dev, src_md));
            nixl_meta_dlist_t r(OBJ_SEG);
            r.addDesc(nixlMetaDesc(0, big_len, key_dev, k_md));
            if (!doXfer(eng, NIXL_WRITE, l, r, agent)) {
                std::cerr << "FAIL: large Store for auto-sizing case failed\n";
                eng.deregisterMem(k_md);
                eng.deregisterMem(src_md);
                return 1;
            }
        }

        // Retrieve into a 1 MiB buffer: must report MISMATCH + true length 8 MiB.
        {
            nixl_meta_dlist_t l(DRAM_SEG);
            l.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(small_dst.data()), small_len, dram_dev, nullptr));
            nixl_meta_dlist_t r(OBJ_SEG);
            r.addDesc(nixlMetaDesc(0, small_len, key_dev, k_md));
            nixlBackendReqH *h = nullptr;
            if (eng.prepXfer(NIXL_READ, l, r, agent, h) != NIXL_SUCCESS) {
                std::cerr << "FAIL: prepXfer(large too-small READ) failed\n";
                return 1;
            }
            nixl_status_t ps = eng.postXfer(NIXL_READ, l, r, agent, h);
            nixl_status_t cs = eng.checkXfer(h);
            const size_t tl = eng.getReqTrueLen(h, 0);
            eng.releaseReqH(h);
            if (ps != NIXL_ERR_MISMATCH || cs != NIXL_ERR_MISMATCH || tl != big_len) {
                std::cerr << "FAIL: too-small READ of an " << (big_len / MiB)
                          << " MiB value did not auto-size (post=" << ps << " check=" << cs
                          << " true_len=" << tl << ", expected " << big_len << ")\n";
                eng.deregisterMem(k_md);
                eng.deregisterMem(src_md);
                return 1;
            }
            std::cout << "large too-small READ (" << (small_len / MiB) << " MiB buf) reported true length "
                      << (tl / MiB) << " MiB (no partial/striped copy)\n";
        }

        // Resize to the true length and re-Retrieve => byte-exact.
        {
            std::vector<uint8_t> big_dst(big_len, 0);
            nixl_meta_dlist_t l(DRAM_SEG);
            l.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(big_dst.data()), big_len, dram_dev, nullptr));
            nixl_meta_dlist_t r(OBJ_SEG);
            r.addDesc(nixlMetaDesc(0, big_len, key_dev, k_md));
            if (!doXfer(eng, NIXL_READ, l, r, agent)) {
                std::cerr << "FAIL: resized large READ failed\n";
                eng.deregisterMem(k_md);
                eng.deregisterMem(src_md);
                return 1;
            }
            if (big_dst != big_src) {
                std::cerr << "FAIL: resized large READ data mismatch after auto-sizing\n";
                eng.deregisterMem(k_md);
                eng.deregisterMem(src_md);
                return 1;
            }
            std::cout << "resized large READ (" << (big_len / MiB)
                      << " MiB, multi-region SGL) matches byte-exact after auto-sizing\n";
        }

        eng.deregisterMem(k_md);
        eng.deregisterMem(src_md);
    }

    // --- NO striping: a value beyond the single-op bound is cleanly REJECTED ---
    // The backend rejects on size BEFORE staging/allocating or touching the
    // buffer, so a small backing buffer with an oversize descriptor length is
    // sufficient (and proves the reject is not a partial/striped transfer).
    {
        const size_t oversize =
            static_cast<size_t>(SPDK_SHIM_MAX_VALUE_LEN) + 4 * 1024 * 1024;
        std::vector<uint8_t> tiny(4096, 0);
        const uint64_t dram_dev = 0, key_dev = 1;
        const std::string big_key = "nixl-oversize-01"; // 16 bytes, opaque

        nixlBlobDesc key_desc(0, oversize, key_dev, big_key);
        nixlBackendMD *key_md2 = nullptr;
        if (eng.registerMem(key_desc, OBJ_SEG, key_md2) != NIXL_SUCCESS) {
            std::cerr << "FAIL: registerMem(oversize key) failed\n";
            return 1;
        }
        nixl_meta_dlist_t local(DRAM_SEG);
        local.addDesc(nixlMetaDesc(
            reinterpret_cast<uintptr_t>(tiny.data()), oversize, dram_dev, nullptr));
        nixl_meta_dlist_t remote(OBJ_SEG);
        remote.addDesc(nixlMetaDesc(0, oversize, key_dev, key_md2));

        nixlBackendReqH *h = nullptr;
        nixl_status_t pp = eng.prepXfer(NIXL_WRITE, local, remote, agent, h);
        nixl_status_t ps = NIXL_SUCCESS, cs = NIXL_SUCCESS;
        if (pp == NIXL_SUCCESS) {
            ps = eng.postXfer(NIXL_WRITE, local, remote, agent, h);
            cs = eng.checkXfer(h);
            eng.releaseReqH(h);
        }
        eng.deregisterMem(key_md2);
        if (pp == NIXL_SUCCESS && ps == NIXL_SUCCESS && cs == NIXL_SUCCESS) {
            std::cerr << "FAIL: oversize value (" << (oversize / (1024 * 1024))
                      << " MiB) was NOT rejected (must not be split)\n";
            return 1;
        }
        std::cout << "oversize value (" << (oversize / (1024 * 1024)) << " MiB > "
                  << (SPDK_SHIM_MAX_VALUE_LEN / (1024 * 1024))
                  << " MiB single-op bound) correctly rejected (not split)\n";
    }

    // --- Zero-copy DMA datapath (direct, no staging copy) ---
    // SPDK-DMA (fd-backed) buffers must take the DIRECT path: the device DMAs
    // into the caller's own buffer. Verify byte-exact for a small value and a
    // multi-region large value (zero-copy across the region-bounded SGL).
    {
        const size_t MiB = 1024 * 1024;
        const struct {
            const char *key;
            size_t size;
        } zc[] = {
            {"nixl-zerocopy-01", 4096},
            {"nixl-zerocopy-8m", 8 * MiB}, // 4 x 2 MiB regions, zero-copy large value
        };
        for (const auto &c : zc) {
            if (!storeRetrieveVerify(eng, agent, c.key, c.size, BufKind::Dma, /*expect_direct=*/true)) {
                std::cerr << "FAIL: zero-copy round-trip failed (key '" << c.key
                          << "', size " << c.size << ")\n";
                return 1;
            }
            std::cout << "zero-copy round-trip OK: " << c.size
                      << " bytes, device DMA'd into the caller buffer (no staging, key '"
                      << c.key << "')\n";
        }
    }

    // --- Staging fallback: ordinary DRAM must NOT be marked zero-copy ---
    // A plain 16-byte-aligned heap buffer is neither fd-backed nor 4 KiB-aligned,
    // so registerMem cannot make it directly reachable and it takes the staged
    // path. Assert the engine reports it so -- making the zero-copy assertions
    // above a real discriminator, not vacuously true. (The distinct
    // register-then-rollback path for a page-aligned anonymous buffer is covered
    // separately below.)
    {
        std::vector<uint8_t> plain(4096, 0);
        nixlBlobDesc d(reinterpret_cast<uintptr_t>(plain.data()), plain.size(), 0, "");
        nixlBackendMD *md = nullptr;
        if (eng.registerMem(d, DRAM_SEG, md) != NIXL_SUCCESS) {
            std::cerr << "FAIL: registerMem(plain DRAM) failed\n";
            return 1;
        }
        const bool direct = eng.dramIsDmaRegistered(md);
        eng.deregisterMem(md);
        if (direct) {
            std::cerr << "FAIL: ordinary heap DRAM unexpectedly took the zero-copy path "
                         "(expected staging fallback over vfio-user)\n";
            return 1;
        }
        std::cout << "staging fallback confirmed: ordinary DRAM is not fd-backed, "
                     "stages over vfio-user\n";
    }

    // --- Register-then-rollback path + no corruption (the anti-silent-failure) ---
    // A PAGE-ALIGNED anonymous buffer passes the 4 KiB pre-check, so
    // registerMem calls spdk_mem_register() (which succeeds, yet its vfio-user
    // DMA-map notify silently fails because anonymous memory is not fd-backed).
    // The engine must detect the region is still not reachable, ROLL BACK the
    // registration, and stage -- NOT mark it zero-copy (which would transfer to
    // memory the target cannot see). Assert not-direct AND that a full round-trip
    // through this buffer is byte-exact (proving the rollback left no corruption).
    {
        const size_t sz = 8192; // page-aligned length
        void *buf = nullptr;
        if (posix_memalign(&buf, 4096, sz) != 0 || buf == nullptr) {
            std::cerr << "FAIL: posix_memalign for rollback test\n";
            return 1;
        }
        auto *bb = static_cast<uint8_t *>(buf);
        for (size_t i = 0; i < sz; ++i) bb[i] = static_cast<uint8_t>(i * 31u + 7u);
        std::vector<uint8_t> expect(bb, bb + sz);
        const uint64_t dram_dev = 0, key_dev = 1;
        const std::string key = "nixl-rollback-01"; // 16 bytes

        nixlBlobDesc src_desc(reinterpret_cast<uintptr_t>(buf), sz, dram_dev, "");
        nixlBackendMD *src_md = nullptr;
        nixlBlobDesc key_desc(0, sz, key_dev, key);
        nixlBackendMD *key_md = nullptr;
        int rc = 1;
        if (eng.registerMem(src_desc, DRAM_SEG, src_md) != NIXL_SUCCESS ||
            eng.registerMem(key_desc, OBJ_SEG, key_md) != NIXL_SUCCESS) {
            std::cerr << "FAIL: registerMem (rollback case) failed\n";
        } else if (eng.dramIsDmaRegistered(src_md)) {
            std::cerr << "FAIL: page-aligned anonymous DRAM was marked zero-copy over "
                         "vfio-user (spdk_mem_register's swallowed DMA-map failure not "
                         "caught -> would silently transfer nothing)\n";
        } else {
            nixl_meta_dlist_t local(DRAM_SEG);
            local.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(buf), sz, dram_dev, src_md));
            nixl_meta_dlist_t remote(OBJ_SEG);
            remote.addDesc(nixlMetaDesc(0, sz, key_dev, key_md));
            bool round_ok = doXfer(eng, NIXL_WRITE, local, remote, agent);
            std::memset(buf, 0, sz);
            if (round_ok) round_ok = doXfer(eng, NIXL_READ, local, remote, agent);
            rc = (round_ok && std::memcmp(buf, expect.data(), sz) == 0) ? 0 : 2;
        }
        eng.deregisterMem(key_md);
        eng.deregisterMem(src_md);
        free(buf);
        if (rc != 0) {
            std::cerr << "FAIL: register-then-rollback staged round-trip (rc=" << rc << ")\n";
            return 1;
        }
        std::cout << "register-then-rollback OK: page-aligned anonymous DRAM staged "
                     "safely over vfio-user, byte-exact (no silent no-op)\n";
    }

    // --- Cross-mode guard: BLK_SEG is refused on a KV-bound engine ---
    // Symmetric to the block test's OBJ_SEG-on-block guard: a KV-bound engine
    // has no LBA space, so it must not advertise or accept BLK_SEG. Refusing here
    // (before any device op) keeps the op-set matched to the bound namespace kind.
    {
        // getSupportedMems() must advertise the KV op-set only (no BLK_SEG).
        const nixl_mem_list_t mems = eng.getSupportedMems();
        bool has_obj = false, has_blk = false, has_dram = false;
        for (nixl_mem_t m : mems) {
            if (m == OBJ_SEG) has_obj = true;
            if (m == BLK_SEG) has_blk = true;
            if (m == DRAM_SEG) has_dram = true;
        }
        if (has_blk || !has_obj || !has_dram) {
            std::cerr << "FAIL: KV-mode getSupportedMems must be {DRAM_SEG, OBJ_SEG} "
                         "(no BLK_SEG)\n";
            return 1;
        }

        // registerMem(BLK_SEG) must be refused on a KV engine.
        nixlBlobDesc blk_desc(/*lba=*/0, 4096, /*devId=*/1, "");
        nixlBackendMD *blk_md = nullptr;
        if (eng.registerMem(blk_desc, BLK_SEG, blk_md) != NIXL_ERR_NOT_SUPPORTED) {
            std::cerr << "FAIL: registerMem(BLK_SEG) on a KV engine was not refused "
                         "with NIXL_ERR_NOT_SUPPORTED\n";
            eng.deregisterMem(blk_md);
            return 1;
        }

        // queryMem(BLK_SEG) has no per-LBA existence notion and must be refused.
        nixl_reg_dlist_t q(BLK_SEG);
        q.addDesc(nixlBlobDesc(0, 4096, /*devId=*/1, ""));
        std::vector<nixl_query_resp_t> resp;
        if (eng.queryMem(q, resp) != NIXL_ERR_NOT_SUPPORTED) {
            std::cerr << "FAIL: queryMem(BLK_SEG) on a KV engine was not refused "
                         "with NIXL_ERR_NOT_SUPPORTED\n";
            return 1;
        }
        std::cout << "cross-mode guard OK: KV-mode engine refuses BLK_SEG at "
                     "getSupportedMems/registerMem/queryMem\n";
    }

    // --- Partial DMA-reachability: base fd-backed, tail anonymous (the fix) ---
    // A multi-region buffer whose BASE region is DMA-reachable but whose TAIL
    // region is NOT must be classified UNREACHABLE and staged -- NOT latched
    // zero-copy off the reachable base (which would silently DMA the unreachable
    // tail over vfio-user: wrong data reported SUCCESS). kv_mem_reachable() walks
    // the whole [base, len) span region-by-region, so a reachable base no longer
    // masks an unreachable tail.
    //
    // Construct such a buffer as [fd-backed SPDK-DMA 2 MiB region | anonymous
    // 2 MiB region] laid out contiguously: an anonymous mapping placed at the
    // 2 MiB boundary immediately after an SPDK-DMA base (MAP_FIXED_NOREPLACE, so
    // nothing already mapped is disturbed). If that virtual slot is not free
    // (e.g. it falls inside the DPDK heap reservation), SKIP this check rather
    // than false-fail -- the construction, not the property under test, is what
    // could not be set up.
    {
#ifdef MAP_FIXED_NOREPLACE
        const size_t region = SPDK_SHIM_DMA_REGION; // 2 MiB
        void *base = spdk_shim_dma_alloc_aligned(region, region); // fd-backed
        bool skipped = true;
        if (base != nullptr) {
            void *tail_at = static_cast<void *>(static_cast<uint8_t *>(base) + region);
            void *tail = mmap(tail_at, region, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (tail == tail_at) {
                skipped = false;
                const size_t sz = 2 * region; // fd-backed base + anonymous tail
                auto *bb = static_cast<uint8_t *>(base);
                for (size_t i = 0; i < sz; ++i) bb[i] = static_cast<uint8_t>(i * 131u + 17u);
                std::vector<uint8_t> expect(bb, bb + sz);
                const uint64_t dram_dev = 0, key_dev = 1;
                const std::string key = "nixl-partial-001"; // 16 bytes, opaque

                nixlBlobDesc src_desc(reinterpret_cast<uintptr_t>(base), sz, dram_dev, "");
                nixlBackendMD *src_md = nullptr;
                nixlBlobDesc key_desc(0, sz, key_dev, key);
                nixlBackendMD *key_md = nullptr;
                int rc = 1;
                if (eng.registerMem(src_desc, DRAM_SEG, src_md) != NIXL_SUCCESS ||
                    eng.registerMem(key_desc, OBJ_SEG, key_md) != NIXL_SUCCESS) {
                    std::cerr << "FAIL: registerMem (partial-reachability) failed\n";
                } else if (eng.dramIsDmaRegistered(src_md)) {
                    std::cerr << "FAIL: a base-reachable/tail-unreachable buffer was marked "
                                 "zero-copy -- the span walk did not catch the unreachable "
                                 "tail (would silently transfer wrong data over vfio-user)\n";
                } else {
                    // Staged path: a byte-exact WRITE then READ round-trip proves the
                    // fallback moves the FULL span correctly (no silent corruption).
                    nixl_meta_dlist_t local(DRAM_SEG);
                    local.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(base), sz, dram_dev, src_md));
                    nixl_meta_dlist_t remote(OBJ_SEG);
                    remote.addDesc(nixlMetaDesc(0, sz, key_dev, key_md));
                    bool round_ok = doXfer(eng, NIXL_WRITE, local, remote, agent);
                    std::memset(base, 0, sz);
                    if (round_ok) round_ok = doXfer(eng, NIXL_READ, local, remote, agent);
                    rc = (round_ok && std::memcmp(base, expect.data(), sz) == 0) ? 0 : 2;
                }
                eng.deregisterMem(key_md);
                eng.deregisterMem(src_md);
                munmap(tail, region);
                if (rc != 0) {
                    std::cerr << "FAIL: partial-reachability staged round-trip (rc=" << rc << ")\n";
                    spdk_shim_dma_free(base);
                    return 1;
                }
                std::cout << "partial-reachability OK: fd-backed base + anonymous tail "
                             "classified unreachable, staged byte-exact (no silent corruption)\n";
            } else if (tail != MAP_FAILED) {
                munmap(tail, region);
            }
            spdk_shim_dma_free(base);
        }
        if (skipped) {
            std::cout << "partial-reachability check SKIPPED: no free virtual slot after the "
                         "SPDK-DMA base to place an anonymous tail\n";
        }
#else
        std::cout << "partial-reachability check SKIPPED: MAP_FIXED_NOREPLACE unavailable\n";
#endif
    }

    // --- Mid-list partial guard: a 2-descriptor WRITE with ONE bad descriptor
    // must be rejected at prepXfer and issue ZERO device ops -- neither value is
    // Stored. This is the regression for the mid-list partial mutation: the old
    // code validated-and-issued per iteration, so desc0 was durably Stored and
    // THEN desc1's length mismatch failed the transfer with no rollback (a
    // partially-applied mutation reported as a failed transfer). prepXfer now
    // validates the whole list up front, so a bad desc1 rejects before desc0 is
    // ever Stored. ---
    {
        const uint64_t dram_dev = 0, key_dev = 1;
        const std::string key_good = "nixl-midlist-a00"; // 16 bytes, never stored
        const std::string key_bad = "nixl-midlist-b00";  // 16 bytes, never stored
        const size_t good_len = 64;

        std::vector<uint8_t> buf0(good_len), buf1(good_len);
        for (size_t i = 0; i < good_len; ++i) {
            buf0[i] = static_cast<uint8_t>(i + 1);
            buf1[i] = static_cast<uint8_t>(i + 2);
        }

        nixlBlobDesc s0(reinterpret_cast<uintptr_t>(buf0.data()), good_len, dram_dev, "");
        nixlBackendMD *s0_md = nullptr;
        nixlBlobDesc s1(reinterpret_cast<uintptr_t>(buf1.data()), good_len, dram_dev, "");
        nixlBackendMD *s1_md = nullptr;
        nixlBlobDesc k0(0, good_len, key_dev, key_good);
        nixlBackendMD *k0_md = nullptr;
        // desc1's remote length DIFFERS from its local length -> a malformed
        // (length-mismatch) descriptor that prepXfer must reject for the whole list.
        nixlBlobDesc k1(0, good_len * 2, key_dev, key_bad);
        nixlBackendMD *k1_md = nullptr;
        if (eng.registerMem(s0, DRAM_SEG, s0_md) != NIXL_SUCCESS ||
            eng.registerMem(s1, DRAM_SEG, s1_md) != NIXL_SUCCESS ||
            eng.registerMem(k0, OBJ_SEG, k0_md) != NIXL_SUCCESS ||
            eng.registerMem(k1, OBJ_SEG, k1_md) != NIXL_SUCCESS) {
            std::cerr << "FAIL: registerMem (mid-list guard) failed\n";
            return 1;
        }

        nixl_meta_dlist_t l(DRAM_SEG);
        l.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(buf0.data()), good_len, dram_dev, s0_md));
        l.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(buf1.data()), good_len, dram_dev, s1_md));
        nixl_meta_dlist_t r(OBJ_SEG);
        r.addDesc(nixlMetaDesc(0, good_len, key_dev, k0_md));      // desc0: valid
        r.addDesc(nixlMetaDesc(0, good_len * 2, key_dev, k1_md));  // desc1: len mismatch

        nixlBackendReqH *h = nullptr;
        nixl_status_t pp = eng.prepXfer(NIXL_WRITE, l, r, agent, h);
        if (pp == NIXL_SUCCESS) {
            // A malformed list must not prepare a transfer. If it did, drive post
            // so we don't leak the handle, then fail.
            eng.postXfer(NIXL_WRITE, l, r, agent, h);
            eng.releaseReqH(h);
            eng.deregisterMem(k1_md);
            eng.deregisterMem(k0_md);
            eng.deregisterMem(s1_md);
            eng.deregisterMem(s0_md);
            std::cerr << "FAIL: mid-list malformed WRITE was accepted at prepXfer "
                         "(desc1 length mismatch not caught before any device op)\n";
            return 1;
        }

        // Prove ZERO device ops: neither key -- crucially the VALID desc0 key --
        // may have been Stored. A KV Exist on each must report a MISS.
        nixl_reg_dlist_t q(OBJ_SEG);
        q.addDesc(nixlBlobDesc(0, 0, key_dev, key_good));
        q.addDesc(nixlBlobDesc(0, 0, key_dev, key_bad));
        std::vector<nixl_query_resp_t> resp;
        nixl_status_t qs = eng.queryMem(q, resp);
        eng.deregisterMem(k1_md);
        eng.deregisterMem(k0_md);
        eng.deregisterMem(s1_md);
        eng.deregisterMem(s0_md);
        if (qs != NIXL_SUCCESS || resp.size() != 2) {
            std::cerr << "FAIL: mid-list guard queryMem failed (status=" << qs
                      << ", n=" << resp.size() << ")\n";
            return 1;
        }
        if (resp[0].has_value() || resp[1].has_value()) {
            std::cerr << "FAIL: mid-list partial mutation -- a value was Stored despite "
                         "prepXfer rejecting the list (desc0 present=" << resp[0].has_value()
                      << ", desc1 present=" << resp[1].has_value() << ")\n";
            return 1;
        }
        std::cout << "mid-list partial guard OK: a 2-desc WRITE with one bad descriptor "
                     "is rejected at prepXfer; neither value stored (no partial mutation)\n";
    }

    std::cout << "spdk_roundtrip_test: PASS\n";
    return 0;
}
