/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

// The generic accelerated engine with libfabric ofi1 tokens (rdma_transport=ofi)
// against a live S3-over-RDMA endpoint, such as Ceph RGW with OSD passthrough.
// Skipped unless the environment names the endpoint and the libfabric
// settings:
//
//   NIXL_OBJ_ENDPOINT_OVERRIDE  the S3 endpoint, for example http://10.90.0.2:8100
//   NIXL_OBJ_OFI_PROVIDER       the OSDs' osd_ofi_provider, for example verbs;ofi_rxm
//   NIXL_OBJ_OFI_NODE           the local address the OSDs write to
//   NIXL_OBJ_OFI_DOMAIN         the local RDMA device (optional)
//   AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_DEFAULT_BUCKET
//
// The GPU tests also need an AMD GPU, the HIP runtime, and a provider that
// offers FI_HMEM. A PUT with a libfabric token sends its payload over HTTP, so
// these tests write objects from host memory and read them back over RDMA.

#if defined HAVE_OFI_RMA

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

#include "../common.h"
#include "plugins_common.h"
#include "transfer_handler.h"
#include "obj/obj_backend.h"

namespace gtest::plugins::obj {

namespace {

    [[nodiscard]] std::string
    envOr(const char *name, const std::string &fallback = "") {
        const char *v = std::getenv(name);
        return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
    }

    /** The backend parameters for the endpoint and provider in the environment. */
    [[nodiscard]] nixl_b_params_t
    ofiParams(const std::string &provider, const std::string &hmem = "rocr") {
        const std::string endpoint = envOr("NIXL_OBJ_ENDPOINT_OVERRIDE");
        nixl_b_params_t p = {{"accelerated", "true"},
                             {"rdma_transport", "ofi"},
                             {"ofi_provider", provider},
                             {"ofi_hmem", hmem},
                             {"endpoint_override", endpoint}};
        if (endpoint.rfind("http://", 0) == 0) {
            p["scheme"] = "http";
        }
        if (const std::string node = envOr("NIXL_OBJ_OFI_NODE"); !node.empty()) {
            p["ofi_node"] = node;
        }
        if (const std::string domain = envOr("NIXL_OBJ_OFI_DOMAIN");
            !domain.empty() && provider == envOr("NIXL_OBJ_OFI_PROVIDER")) {
            p["ofi_domain"] = domain;
        }
        return p;
    }

    [[nodiscard]] bool
    haveEndpoint() {
        return !envOr("NIXL_OBJ_ENDPOINT_OVERRIDE").empty() &&
            !envOr("NIXL_OBJ_OFI_PROVIDER").empty();
    }

    [[nodiscard]] std::shared_ptr<nixlObjEngine>
    makeEngine(const std::string &agent, nixl_b_params_t &params) {
        nixlBackendInitParams init = {.localAgent = agent,
                                      .type = "OBJ",
                                      .customParams = &params,
                                      .enableProgTh = false,
                                      .pthrDelay = 0,
                                      .syncMode = nixl_thread_sync_t::NIXL_THREAD_SYNC_RW};
        return std::make_shared<nixlObjEngine>(&init);
    }

    /** Run one transfer to completion; the status checkXfer() ended with. */
    [[nodiscard]] nixl_status_t
    transfer(nixlBackendEngine &engine,
             nixl_xfer_op_t op,
             const nixl_meta_dlist_t &local,
             const nixl_meta_dlist_t &remote,
             const std::string &agent) {
        nixlBackendReqH *handle = nullptr;
        nixl_status_t st = engine.prepXfer(op, local, remote, agent, handle, nullptr);
        if (st != NIXL_SUCCESS) {
            return st;
        }
        st = engine.postXfer(op, local, remote, agent, handle, nullptr);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (st == NIXL_IN_PROG && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            st = engine.checkXfer(handle);
        }
        engine.releaseReqH(handle);
        return st;
    }

    [[nodiscard]] std::vector<char>
    pattern(size_t n, int seed) {
        std::vector<char> v(n);
        for (size_t i = 0; i < n; i++) {
            v[i] = static_cast<char>((i * 131 + i / 4096 + seed) & 0xff);
        }
        return v;
    }

    /** The HIP runtime, loaded at run time; the build needs no GPU toolchain. */
    struct hipRuntime {
        void *lib = nullptr;
        int (*getDeviceCount)(int *) = nullptr;
        int (*mallocDevice)(void **, size_t) = nullptr;
        int (*freeDevice)(void *) = nullptr;
        int (*memsetDevice)(void *, int, size_t) = nullptr;
        int (*memcpyAny)(void *, const void *, size_t, int) = nullptr;
        int (*synchronize)() = nullptr;
        static constexpr int device_to_host = 2; // hipMemcpyDeviceToHost

        bool
        load() {
            lib = dlopen("libamdhip64.so", RTLD_NOW | RTLD_LOCAL);
            if (lib == nullptr) {
                return false;
            }
            getDeviceCount = reinterpret_cast<int (*)(int *)>(dlsym(lib, "hipGetDeviceCount"));
            mallocDevice = reinterpret_cast<int (*)(void **, size_t)>(dlsym(lib, "hipMalloc"));
            freeDevice = reinterpret_cast<int (*)(void *)>(dlsym(lib, "hipFree"));
            memsetDevice = reinterpret_cast<int (*)(void *, int, size_t)>(dlsym(lib, "hipMemset"));
            memcpyAny = reinterpret_cast<int (*)(void *, const void *, size_t, int)>(
                dlsym(lib, "hipMemcpy"));
            synchronize = reinterpret_cast<int (*)()>(dlsym(lib, "hipDeviceSynchronize"));
            int count = 0;
            return getDeviceCount && mallocDevice && freeDevice && memsetDevice && memcpyAny &&
                synchronize && getDeviceCount(&count) == 0 && count > 0;
        }

        ~hipRuntime() {
            if (lib != nullptr) {
                dlclose(lib);
            }
        }
    };

    /**
     * Write an object from host memory over HTTP, then read [ofs, ofs + len)
     * of it into dst, of type mem, with the same engine.
     */
    class objectRoundTrip {
    public:
        objectRoundTrip(nixlBackendEngine &engine, std::string agent, std::string key)
            : engine_(engine),
              agent_(std::move(agent)),
              key_(std::move(key)) {
            nixlBlobDesc obj;
            obj.devId = obj_dev_id;
            obj.metaInfo = key_;
            EXPECT_EQ(NIXL_SUCCESS, engine_.registerMem(obj, OBJ_SEG, objMd_));
        }

        ~objectRoundTrip() {
            if (objMd_ != nullptr) {
                EXPECT_EQ(NIXL_SUCCESS, engine_.deregisterMem(objMd_));
            }
        }

        [[nodiscard]] nixl_status_t
        put(const std::vector<char> &data) {
            return move(NIXL_WRITE, const_cast<char *>(data.data()), DRAM_SEG, data.size(), 0, 1);
        }

        [[nodiscard]] nixl_status_t
        get(void *dst, nixl_mem_t mem, size_t len, size_t ofs, uint64_t dev_id) {
            return move(NIXL_READ, dst, mem, len, ofs, dev_id);
        }

    private:
        static constexpr uint64_t obj_dev_id = 7;

        nixl_status_t
        move(nixl_xfer_op_t op, void *buf, nixl_mem_t mem, size_t len, size_t ofs, uint64_t dev) {
            nixlBlobDesc local;
            local.addr = reinterpret_cast<uintptr_t>(buf);
            local.len = len;
            local.devId = dev;
            nixlBackendMD *md = nullptr;
            nixl_status_t st = engine_.registerMem(local, mem, md);
            if (st != NIXL_SUCCESS) {
                return st;
            }
            nixl_meta_dlist_t local_descs(mem);
            local_descs.addDesc(nixlMetaDesc(local.addr, len, dev));
            nixl_meta_dlist_t remote_descs(OBJ_SEG);
            remote_descs.addDesc(nixlMetaDesc(ofs, len, obj_dev_id));
            st = transfer(engine_, op, local_descs, remote_descs, agent_);
            EXPECT_EQ(NIXL_SUCCESS, engine_.deregisterMem(md));
            return st;
        }

        nixlBackendEngine &engine_;
        std::string agent_;
        std::string key_;
        nixlBackendMD *objMd_ = nullptr;
    };

} // namespace

class objOfiTest : public testing::Test {
protected:
    void
    SetUp() override {
        if (!haveEndpoint()) {
            GTEST_SKIP() << "set NIXL_OBJ_ENDPOINT_OVERRIDE and NIXL_OBJ_OFI_PROVIDER to run";
        }
        params_ = ofiParams(envOr("NIXL_OBJ_OFI_PROVIDER"));
        engine_ = makeEngine(agent_, params_);
        ASSERT_FALSE(engine_->getInitErr()) << "the ofi engine did not initialize";
    }

    const std::string agent_ = "Agent-Obj-Ofi";
    nixl_b_params_t params_;
    std::shared_ptr<nixlObjEngine> engine_;
};

TEST_F(objOfiTest, AdvertisesVramOnlyWithFiHmem) {
    const auto mems = engine_->getSupportedMems();
    EXPECT_NE(std::find(mems.begin(), mems.end(), DRAM_SEG), mems.end());
    EXPECT_NE(std::find(mems.begin(), mems.end(), OBJ_SEG), mems.end());
}

TEST_F(objOfiTest, WriteOverHttpThenReadOverRdma) {
    transferHandler<DRAM_SEG, OBJ_SEG> xfer(engine_, engine_, agent_, agent_, false, 1, 8 << 20);
    xfer.setLocalMem();
    xfer.testTransfer(NIXL_WRITE);
    xfer.resetLocalMem();
    xfer.testTransfer(NIXL_READ);
    xfer.checkLocalMem();
}

TEST_F(objOfiTest, SeveralBuffers) {
    transferHandler<DRAM_SEG, OBJ_SEG> xfer(engine_, engine_, agent_, agent_, false, 3, 4 << 20);
    xfer.setLocalMem();
    xfer.testTransfer(NIXL_WRITE);
    xfer.resetLocalMem();
    xfer.testTransfer(NIXL_READ);
    xfer.checkLocalMem();
}

// Several descriptors into one registered buffer: ranged GETs of different
// parts of the object, each with a token for its own part of the buffer. The
// server must answer each one 206 with x-amz-rdma-reply: 206.
TEST_F(objOfiTest, RangedReadsIntoPartsOfABuffer) {
    const auto data = pattern(8 << 20, 3);
    objectRoundTrip obj(*engine_, agent_, "nixl-ofi-parts");
    ASSERT_EQ(NIXL_SUCCESS, obj.put(data));

    const size_t part = 2 << 20;
    std::vector<char> buf(2 * part, 0);
    nixlBlobDesc local;
    local.addr = reinterpret_cast<uintptr_t>(buf.data());
    local.len = buf.size();
    local.devId = 4;
    nixlBackendMD *md = nullptr;
    ASSERT_EQ(NIXL_SUCCESS, engine_->registerMem(local, DRAM_SEG, md));
    nixl_meta_dlist_t local_descs(DRAM_SEG);
    local_descs.addDesc(nixlMetaDesc(local.addr, part, 4));
    local_descs.addDesc(nixlMetaDesc(local.addr + part, part, 4));
    nixl_meta_dlist_t remote_descs(OBJ_SEG);
    remote_descs.addDesc(nixlMetaDesc(1 << 20, part, 7));
    remote_descs.addDesc(nixlMetaDesc(5 << 20, part, 7));
    EXPECT_EQ(NIXL_SUCCESS, transfer(*engine_, NIXL_READ, local_descs, remote_descs, agent_));
    EXPECT_EQ(NIXL_SUCCESS, engine_->deregisterMem(md));
    EXPECT_EQ(0, std::memcmp(buf.data(), data.data() + (1 << 20), part));
    EXPECT_EQ(0, std::memcmp(buf.data() + part, data.data() + (5 << 20), part));
}

TEST_F(objOfiTest, RangedReadAtAnOffset) {
    const auto data = pattern(6 << 20, 5);
    objectRoundTrip obj(*engine_, agent_, "nixl-ofi-ranged");
    ASSERT_EQ(NIXL_SUCCESS, obj.put(data));
    const size_t ofs = (1 << 20) + 12345;
    const size_t len = (3 << 20) + 7;
    std::vector<char> got(len, 0);
    ASSERT_EQ(NIXL_SUCCESS, obj.get(got.data(), DRAM_SEG, len, ofs, 2));
    EXPECT_EQ(0, std::memcmp(got.data(), data.data() + ofs, len));
}

TEST_F(objOfiTest, ReadIntoAnAmdGpuBuffer) {
    hipRuntime hip;
    if (!hip.load()) {
        GTEST_SKIP() << "no HIP runtime or no AMD GPU";
    }
    const auto mems = engine_->getSupportedMems();
    if (std::find(mems.begin(), mems.end(), VRAM_SEG) == mems.end()) {
        GTEST_SKIP() << "the provider offers no FI_HMEM, so the engine takes no VRAM_SEG";
    }
    const auto data = pattern(16 << 20, 9);
    objectRoundTrip obj(*engine_, agent_, "nixl-ofi-vram");
    ASSERT_EQ(NIXL_SUCCESS, obj.put(data));

    void *gpu = nullptr;
    ASSERT_EQ(0, hip.mallocDevice(&gpu, data.size()));
    ASSERT_EQ(0, hip.memsetDevice(gpu, 0, data.size()));
    ASSERT_EQ(0, hip.synchronize());
    EXPECT_EQ(NIXL_SUCCESS, obj.get(gpu, VRAM_SEG, data.size(), 0, 0));
    std::vector<char> got(data.size());
    ASSERT_EQ(0, hip.memcpyAny(got.data(), gpu, got.size(), hipRuntime::device_to_host));
    EXPECT_EQ(0, std::memcmp(got.data(), data.data(), data.size()));

    // A PUT payload goes over HTTP, which GPU memory cannot feed.
    const LogIgnoreGuard vram_put("sends a PUT payload over HTTP");
    objectRoundTrip from_gpu(*engine_, agent_, "nixl-ofi-vram-put");
    nixlBlobDesc local;
    local.addr = reinterpret_cast<uintptr_t>(gpu);
    local.len = data.size();
    local.devId = 0;
    nixlBackendMD *md = nullptr;
    ASSERT_EQ(NIXL_SUCCESS, engine_->registerMem(local, VRAM_SEG, md));
    nixl_meta_dlist_t local_descs(VRAM_SEG);
    local_descs.addDesc(nixlMetaDesc(local.addr, local.len, 0));
    nixl_meta_dlist_t remote_descs(OBJ_SEG);
    remote_descs.addDesc(nixlMetaDesc(0, local.len, 7));
    const nixl_status_t put = transfer(*engine_, NIXL_WRITE, local_descs, remote_descs, agent_);
    EXPECT_NE(NIXL_SUCCESS, put);
    EXPECT_NE(NIXL_IN_PROG, put);
    EXPECT_EQ(NIXL_SUCCESS, engine_->deregisterMem(md));
    EXPECT_EQ(0, hip.freeDevice(gpu));
}

// A token for a provider the OSDs do not run: the server declines RDMA and
// sends the range in the body. That fails, unless rdma_http_fallback is set
// and the buffer is host memory.
TEST_F(objOfiTest, HttpFallbackIsOptIn) {
    const std::string other = envOr("NIXL_OBJ_OFI_PROVIDER") == "tcp" ? "shm" : "tcp";
    const auto data = pattern(2 << 20, 11);
    objectRoundTrip writer(*engine_, agent_, "nixl-ofi-fallback");
    ASSERT_EQ(NIXL_SUCCESS, writer.put(data));

    const LogIgnoreGuard declined("S3 RDMA get failed \\(-2 of");
    const LogIgnoreGuard came_over_http("the data came over HTTP");
    for (const bool fallback : {false, true}) {
        nixl_b_params_t params = ofiParams(other, "none");
        params["rdma_http_fallback"] = fallback ? "true" : "false";
        const std::string agent = fallback ? "Agent-Obj-Ofi-Fb" : "Agent-Obj-Ofi-NoFb";
        auto engine = makeEngine(agent, params);
        ASSERT_FALSE(engine->getInitErr()) << other << " did not initialize";
        objectRoundTrip obj(*engine, agent, "nixl-ofi-fallback");
        std::vector<char> got(data.size(), 0);
        const nixl_status_t st = obj.get(got.data(), DRAM_SEG, got.size(), 0, 3);
        if (fallback) {
            EXPECT_EQ(NIXL_SUCCESS, st);
            EXPECT_EQ(0, std::memcmp(got.data(), data.data(), data.size()));
        } else {
            EXPECT_NE(NIXL_SUCCESS, st);
        }
    }
}

} // namespace gtest::plugins::obj

#endif // HAVE_OFI_RMA
