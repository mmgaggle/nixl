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
 * Direct-engine round-trip test for the generic SPDK_KV backend.
 *
 * Instantiates nixlSpdkKvEngine directly (no nixlAgent), registers a DRAM
 * source buffer and an OBJ_SEG remote descriptor carrying a 16-byte inline key
 * (verbatim, opaque) in metaInfo, then:
 *   1. WRITE (DRAM -> remote)  => KV Store
 *   2. READ  (remote -> DRAM)  => KV Retrieve
 * and verifies the retrieved bytes match the stored bytes. Also exercises the
 * opaque key-mapping guards (empty rejected, over-16-byte rejected, verbatim).
 *
 * Usage: spdk_kv_roundtrip_test <transport-id-or-vfio-user-dir>
 *   e.g. spdk_kv_roundtrip_test "trtype:VFIOUSER traddr:/tmp/.../muser0/0"
 *   or   spdk_kv_roundtrip_test /tmp/.../muser0/0   (wrapped into VFIOUSER)
 */

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "nixl_descriptors.h"
#include "backend/backend_aux.h"
#include "spdk_kv_backend.h"

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
    init.type = "SPDK_KV";
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

    // --- Short-read regression: a READ whose transfer length does not match
    // the stored value length must error, not silently truncate. ---
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
        eng.releaseReqH(h);
        if (ps == NIXL_SUCCESS || cs == NIXL_SUCCESS) {
            std::cerr << "FAIL: short READ (" << short_len << " bytes) of a "
                      << payload.size() << "-byte value did NOT error (post=" << ps
                      << " check=" << cs << ")\n";
            return 1;
        }
        std::cout << "short READ (" << short_len << " bytes) of a " << payload.size()
                  << "-byte value correctly errored (no silent truncation)\n";
    }

    eng.deregisterMem(key_md);
    eng.deregisterMem(dram_md);

    std::cout << "spdk_kv_roundtrip_test: PASS\n";
    return 0;
}
