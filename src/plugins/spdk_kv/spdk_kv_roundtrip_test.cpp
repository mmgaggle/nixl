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
 * Instantiates nixlSpdkKvEngine directly (no nixlAgent), registers a DRAM
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
 * Usage: spdk_kv_roundtrip_test <transport-id-or-vfio-user-dir>
 *   e.g. spdk_kv_roundtrip_test "trtype:VFIOUSER traddr:/tmp/.../muser0/0"
 *   or   spdk_kv_roundtrip_test /tmp/.../muser0/0   (wrapped into VFIOUSER)
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

// Store then Retrieve a `size`-byte value under `key`, verifying the retrieved
// bytes match byte-for-byte. Fills the source with a size-dependent pattern so a
// mis-scattered region would corrupt the comparison. Used to exercise the
// region-bounded SGL across sizes that span several 2 MiB DMA regions.
bool
storeRetrieveVerify(nixlSpdkKvEngine &eng, const std::string &agent,
                    const std::string &key, size_t size) {
    std::vector<uint8_t> src(size), dst(size, 0);
    for (size_t i = 0; i < size; ++i) {
        src[i] = static_cast<uint8_t>((i * 1103515245u + 12345u) >> 16);
    }
    const uint64_t dram_dev = 0, key_dev = 1;

    nixlBlobDesc src_desc(reinterpret_cast<uintptr_t>(src.data()), size, dram_dev, "");
    nixlBackendMD *src_md = nullptr;
    nixlBlobDesc dst_desc(reinterpret_cast<uintptr_t>(dst.data()), size, dram_dev, "");
    nixlBackendMD *dst_md = nullptr;
    nixlBlobDesc key_desc(0, size, key_dev, key);
    nixlBackendMD *key_md = nullptr;
    if (eng.registerMem(src_desc, DRAM_SEG, src_md) != NIXL_SUCCESS ||
        eng.registerMem(dst_desc, DRAM_SEG, dst_md) != NIXL_SUCCESS ||
        eng.registerMem(key_desc, OBJ_SEG, key_md) != NIXL_SUCCESS) {
        std::cerr << "FAIL: registerMem failed for size " << size << "\n";
        return false;
    }

    nixl_meta_dlist_t local(DRAM_SEG);
    local.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(src.data()), size, dram_dev, src_md));
    nixl_meta_dlist_t remote(OBJ_SEG);
    remote.addDesc(nixlMetaDesc(0, size, key_dev, key_md));
    nixl_meta_dlist_t local_dst(DRAM_SEG);
    local_dst.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(dst.data()), size, dram_dev, dst_md));

    bool ok = false;
    do {
        // WRITE => KV Store
        nixlBackendReqH *h = nullptr;
        if (eng.prepXfer(NIXL_WRITE, local, remote, agent, h) != NIXL_SUCCESS) break;
        nixl_status_t ps = eng.postXfer(NIXL_WRITE, local, remote, agent, h);
        if (ps != NIXL_SUCCESS && ps != NIXL_IN_PROG) { eng.releaseReqH(h); break; }
        if (!checkComplete(eng, h)) { eng.releaseReqH(h); break; }
        eng.releaseReqH(h);

        // READ => KV Retrieve
        h = nullptr;
        if (eng.prepXfer(NIXL_READ, local_dst, remote, agent, h) != NIXL_SUCCESS) break;
        ps = eng.postXfer(NIXL_READ, local_dst, remote, agent, h);
        if (ps != NIXL_SUCCESS && ps != NIXL_IN_PROG) { eng.releaseReqH(h); break; }
        if (!checkComplete(eng, h)) { eng.releaseReqH(h); break; }
        eng.releaseReqH(h);

        ok = (src == dst);
    } while (0);

    // DRAM registrations return a null backend MD; deregisterMem(nullptr) is safe.
    eng.deregisterMem(key_md);
    eng.deregisterMem(dst_md);
    eng.deregisterMem(src_md);
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
    // Standalone test: no host owns the SPDK env, so this engine brings it up.
    params["init_env"] = "true";

    nixlBackendInitParams init{};
    init.localAgent = "spdk_kv_test_agent";
    init.type = "SPDK";
    init.customParams = &params;
    init.enableProgTh = false;
    init.pthrDelay = 0;
    init.enableTelemetry_ = false;

    nixlSpdkKvEngine eng(&init);
    if (eng.getInitErr()) {
        std::cerr << "FAIL: engine init error (shim open failed)\n";
        return 1;
    }
    std::cout << "engine initialized against '" << arg << "'\n";

    // --- Source / destination DRAM buffers ---
    const std::string payload = "SPDK_KV-NIXL-roundtrip-small-value-0123456789";
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
    {
        nixlBackendReqH *h = nullptr;
        if (eng.prepXfer(NIXL_WRITE, local, remote, init.localAgent, h) != NIXL_SUCCESS) {
            std::cerr << "FAIL: prepXfer(WRITE) failed\n";
            return 1;
        }
        nixl_status_t ps = eng.postXfer(NIXL_WRITE, local, remote, init.localAgent, h);
        if (ps != NIXL_SUCCESS && ps != NIXL_IN_PROG) {
            std::cerr << "FAIL: postXfer(WRITE) status=" << ps << "\n";
            return 1;
        }
        if (!checkComplete(eng, h)) {
            std::cerr << "FAIL: WRITE did not complete\n";
            return 1;
        }
        eng.releaseReqH(h);
        std::cout << "WRITE (KV Store) of " << src.size() << " bytes complete\n";
    }

    // --- READ => KV Retrieve ---
    {
        nixlBackendReqH *h = nullptr;
        if (eng.prepXfer(NIXL_READ, local_dst, remote, init.localAgent, h) != NIXL_SUCCESS) {
            std::cerr << "FAIL: prepXfer(READ) failed\n";
            return 1;
        }
        nixl_status_t ps = eng.postXfer(NIXL_READ, local_dst, remote, init.localAgent, h);
        if (ps != NIXL_SUCCESS && ps != NIXL_IN_PROG) {
            std::cerr << "FAIL: postXfer(READ) status=" << ps << "\n";
            return 1;
        }
        if (!checkComplete(eng, h)) {
            std::cerr << "FAIL: READ did not complete\n";
            return 1;
        }
        eng.releaseReqH(h);
        std::cout << "READ (KV Retrieve) of " << dst.size() << " bytes complete\n";
    }

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
        if (eng.prepXfer(NIXL_READ, s_local, s_remote, init.localAgent, h) != NIXL_SUCCESS) {
            std::cerr << "FAIL: prepXfer(short READ) failed\n";
            return 1;
        }
        nixl_status_t ps = eng.postXfer(NIXL_READ, s_local, s_remote, init.localAgent, h);
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

        nixlBackendReqH *rh = nullptr;
        if (eng.prepXfer(NIXL_READ, r_local, r_remote, init.localAgent, rh) != NIXL_SUCCESS) {
            std::cerr << "FAIL: prepXfer(resized READ) failed\n";
            return 1;
        }
        nixl_status_t rps = eng.postXfer(NIXL_READ, r_local, r_remote, init.localAgent, rh);
        if (rps != NIXL_SUCCESS && rps != NIXL_IN_PROG) {
            std::cerr << "FAIL: postXfer(resized READ) status=" << rps << "\n";
            eng.releaseReqH(rh);
            return 1;
        }
        if (!checkComplete(eng, rh)) {
            std::cerr << "FAIL: resized READ did not complete\n";
            eng.releaseReqH(rh);
            return 1;
        }
        eng.releaseReqH(rh);
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
            if (!storeRetrieveVerify(eng, init.localAgent, c.key, c.size)) {
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
            nixlBackendReqH *h = nullptr;
            if (eng.prepXfer(NIXL_WRITE, l, r, init.localAgent, h) != NIXL_SUCCESS ||
                (eng.postXfer(NIXL_WRITE, l, r, init.localAgent, h), !checkComplete(eng, h))) {
                std::cerr << "FAIL: large Store for auto-sizing case failed\n";
                eng.releaseReqH(h);
                return 1;
            }
            eng.releaseReqH(h);
        }

        // Retrieve into a 1 MiB buffer: must report MISMATCH + true length 8 MiB.
        {
            nixl_meta_dlist_t l(DRAM_SEG);
            l.addDesc(nixlMetaDesc(reinterpret_cast<uintptr_t>(small_dst.data()), small_len, dram_dev, nullptr));
            nixl_meta_dlist_t r(OBJ_SEG);
            r.addDesc(nixlMetaDesc(0, small_len, key_dev, k_md));
            nixlBackendReqH *h = nullptr;
            if (eng.prepXfer(NIXL_READ, l, r, init.localAgent, h) != NIXL_SUCCESS) {
                std::cerr << "FAIL: prepXfer(large too-small READ) failed\n";
                return 1;
            }
            nixl_status_t ps = eng.postXfer(NIXL_READ, l, r, init.localAgent, h);
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
            nixlBackendReqH *h = nullptr;
            if (eng.prepXfer(NIXL_READ, l, r, init.localAgent, h) != NIXL_SUCCESS ||
                (eng.postXfer(NIXL_READ, l, r, init.localAgent, h), !checkComplete(eng, h))) {
                std::cerr << "FAIL: resized large READ failed\n";
                eng.releaseReqH(h);
                eng.deregisterMem(k_md);
                eng.deregisterMem(src_md);
                return 1;
            }
            eng.releaseReqH(h);
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
            static_cast<size_t>(SPDK_KV_SHIM_MAX_VALUE_LEN) + 4 * 1024 * 1024;
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
        nixl_status_t pp = eng.prepXfer(NIXL_WRITE, local, remote, init.localAgent, h);
        nixl_status_t ps = NIXL_SUCCESS, cs = NIXL_SUCCESS;
        if (pp == NIXL_SUCCESS) {
            ps = eng.postXfer(NIXL_WRITE, local, remote, init.localAgent, h);
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
                  << (SPDK_KV_SHIM_MAX_VALUE_LEN / (1024 * 1024))
                  << " MiB single-op bound) correctly rejected (not split)\n";
    }

    std::cout << "spdk_kv_roundtrip_test: PASS\n";
    return 0;
}
