/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

// The libfabric token provider, end to end on one host. An ofi-rma endpoint
// plays the OSD: it writes with the token the provider minted, over tcp or
// shm, as an OSD writes a GET's stripes into a client's buffer.

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <sys/uio.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <ofi_rma/ofi_rma.hpp>

#include "object/rdma/ofi_token_provider.h"

using namespace nixl_obj_rdma;

namespace {

constexpr std::chrono::milliseconds write_budget{3000};

struct providerParam {
    const char *provider;
    const char *node;
};

[[nodiscard]] nixl_b_params_t
ofiParams(const providerParam &p, const std::string &hmem) {
    nixl_b_params_t params{{"ofi_provider", p.provider}, {"ofi_hmem", hmem}};
    if (*p.node != '\0') {
        params["ofi_node"] = p.node;
    }
    return params;
}

/** The client's provider, or nullptr when the libfabric provider is not available here. */
[[nodiscard]] std::unique_ptr<ofiTokenProvider>
openProvider(const providerParam &p, const std::string &hmem = "none") {
    const nixl_b_params_t params = ofiParams(p, hmem);
    std::string err;
    const auto cfg = ofiTokenConfig::fromParams(&params, &err);
    if (!cfg) {
        ADD_FAILURE() << err;
        return nullptr;
    }
    auto provider = ofiTokenProvider::create(*cfg, &err);
    if (!provider) {
        std::cerr << p.provider << ": " << err << std::endl;
    }
    return provider;
}

/** The OSD's side: an endpoint that writes with the client's tokens. */
[[nodiscard]] std::unique_ptr<ofi_rma::Endpoint>
openWriter(const providerParam &p) {
    ofi_rma::config_t c;
    c.provider = p.provider;
    c.node = p.node;
    c.stage_size = 4 << 20;
    c.stage_count = 2;
    std::string err;
    return ofi_rma::Endpoint::open(c, &err);
}

/** Write src into the range the token names, from its first byte. */
[[nodiscard]] int
writeAll(ofi_rma::Endpoint &writer, const std::string &token, const std::vector<char> &src) {
    const auto t = ofi_rma::parse_token(token);
    if (!t) {
        return -EINVAL;
    }
    iovec iov{const_cast<char *>(src.data()), src.size()};
    const std::vector<ofi_rma::Endpoint::write_t> writes = {{0, src.size(), 0}};
    return writer.write(*t, &iov, 1, writes, write_budget);
}

[[nodiscard]] std::vector<char>
pattern(size_t n, int seed) {
    std::vector<char> v(n);
    for (size_t i = 0; i < n; i++) {
        v[i] = static_cast<char>(i * 7 + seed);
    }
    return v;
}

/**
 * tcp and shm, which need no hardware, and verbs;ofi_rxm when
 * OFI_RMA_TEST_VERBS_NODE names the address of an RDMA device, a soft-RoCE
 * one say. Soft-RoCE needs FI_UNIVERSE_SIZE=16; libfabric reads it once.
 */
[[nodiscard]] std::vector<providerParam>
testProviders() {
    std::vector<providerParam> v = {{"tcp", "127.0.0.1"}, {"shm", ""}};
    if (const char *node = std::getenv("OFI_RMA_TEST_VERBS_NODE"); node && *node) {
        setenv("FI_UNIVERSE_SIZE", "16", 0);
        setenv("FI_MR_CACHE_MONITOR", "disabled", 0);
        v.push_back({"verbs;ofi_rxm", node});
    }
    return v;
}

/** gtest names allow letters, digits and underscores. */
[[nodiscard]] std::string
providerTestName(const ::testing::TestParamInfo<providerParam> &info) {
    std::string n = info.param.provider;
    for (auto &c : n) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    return n;
}

class ofiTokenProviderTest : public ::testing::TestWithParam<providerParam> {
protected:
    void
    SetUp() override {
        provider_ = openProvider(GetParam());
        writer_ = openWriter(GetParam());
        if (!provider_ || !writer_) {
            GTEST_SKIP() << GetParam().provider << " is not available";
        }
    }

    std::unique_ptr<ofiTokenProvider> provider_;
    std::unique_ptr<ofi_rma::Endpoint> writer_;
};

} // namespace

TEST_P(ofiTokenProviderTest, WriterFillsADramBuffer) {
    EXPECT_TRUE(provider_->isConnected());
    EXPECT_EQ("ofi", provider_->name());
    EXPECT_TRUE(provider_->supportsMem(DRAM_SEG));
    EXPECT_FALSE(provider_->supportsPut());

    const size_t n = 2 << 20;
    std::vector<char> buf(n, 0);
    ASSERT_EQ(NIXL_SUCCESS, provider_->registerMemory(buf.data(), n, DRAM_SEG, 0));
    auto token = provider_->makeToken(buf.data(), n, rdma_op::GET);
    ASSERT_TRUE(token);

    // the size field is the range's length, and the provider is the writer's
    const auto parsed = ofi_rma::parse_token(token->value());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(n, parsed->size);
    EXPECT_EQ(writer_->wire(), parsed->wire);

    const auto src = pattern(n, 3);
    ASSERT_EQ(0, writeAll(*writer_, token->value(), src)) << writer_->last_error();
    token->reset(); // the caller's reads now follow the writes
    EXPECT_EQ(0, memcmp(buf.data(), src.data(), n));

    // a PUT payload goes over HTTP only from host memory, so the client asks
    EXPECT_EQ(DRAM_SEG, provider_->memTypeAt(buf.data(), n));
    EXPECT_EQ(DRAM_SEG, provider_->memTypeAt(buf.data() + 100, n - 100));
    EXPECT_EQ(std::nullopt, provider_->memTypeAt(buf.data() + 100, n));
    EXPECT_EQ(NIXL_SUCCESS, provider_->deregisterMemory(buf.data()));
    EXPECT_EQ(std::nullopt, provider_->memTypeAt(buf.data(), n));
}

TEST_P(ofiTokenProviderTest, TokenForPartOfABuffer) {
    const size_t n = 1 << 20;
    std::vector<char> buf(n, 0);
    ASSERT_EQ(NIXL_SUCCESS, provider_->registerMemory(buf.data(), n, DRAM_SEG, 0));

    // a ranged GET lands at the range's own start
    const size_t ofs = 4096;
    const size_t len = 8192;
    auto token = provider_->makeToken(buf.data() + ofs, len, rdma_op::GET);
    ASSERT_TRUE(token);
    EXPECT_EQ(len, ofi_rma::parse_token(token->value())->size);
    const auto src = pattern(len, 5);
    ASSERT_EQ(0, writeAll(*writer_, token->value(), src)) << writer_->last_error();
    token.reset();
    EXPECT_EQ(0, memcmp(buf.data() + ofs, src.data(), len));
    EXPECT_EQ(std::vector<char>(ofs, 0), std::vector<char>(buf.begin(), buf.begin() + ofs));
    EXPECT_EQ(std::vector<char>(n - ofs - len, 0),
              std::vector<char>(buf.begin() + ofs + len, buf.end()));
}

TEST_P(ofiTokenProviderTest, RefusesWhatNoBufferHolds) {
    const size_t n = 64 << 10;
    std::vector<char> a(n), b(n);
    ASSERT_EQ(NIXL_SUCCESS, provider_->registerMemory(a.data(), n, DRAM_SEG, 0));
    ASSERT_EQ(NIXL_SUCCESS, provider_->registerMemory(b.data(), n, DRAM_SEG, 0));

    std::vector<char> elsewhere(4096);
    EXPECT_FALSE(provider_->makeToken(elsewhere.data(), elsewhere.size(), rdma_op::GET));
    EXPECT_FALSE(provider_->makeToken(a.data() + n - 4096, 8192, rdma_op::GET)); // past the end
    EXPECT_FALSE(provider_->makeToken(a.data(), 0, rdma_op::GET));
    EXPECT_FALSE(provider_->makeToken(nullptr, 4096, rdma_op::GET));
    EXPECT_TRUE(provider_->makeToken(a.data() + n - 4096, 4096, rdma_op::GET)); // the last page

    // Ceph reads a PUT's payload over HTTP for a libfabric token
    EXPECT_FALSE(provider_->makeToken(a.data(), n, rdma_op::PUT));

    // one registration per start address, since deregistration names the start
    EXPECT_EQ(NIXL_ERR_NOT_ALLOWED, provider_->registerMemory(a.data(), n / 2, DRAM_SEG, 0));
    EXPECT_EQ(NIXL_ERR_INVALID_PARAM, provider_->registerMemory(a.data(), 0, DRAM_SEG, 0));
}

TEST_P(ofiTokenProviderTest, OverlappingBuffersEachGiveTokens) {
    // NIXL can register a buffer and, separately, a part of it
    const size_t n = 256 << 10;
    std::vector<char> buf(n, 0);
    ASSERT_EQ(NIXL_SUCCESS, provider_->registerMemory(buf.data(), n, DRAM_SEG, 0));
    ASSERT_EQ(NIXL_SUCCESS, provider_->registerMemory(buf.data() + 4096, 8192, DRAM_SEG, 0));

    // past the inner buffer's end, the outer one holds the range
    auto token = provider_->makeToken(buf.data() + 8192, 16384, rdma_op::GET);
    ASSERT_TRUE(token);
    const auto src = pattern(16384, 9);
    ASSERT_EQ(0, writeAll(*writer_, token->value(), src)) << writer_->last_error();
    token.reset();
    EXPECT_EQ(0, memcmp(buf.data() + 8192, src.data(), src.size()));

    EXPECT_EQ(NIXL_SUCCESS, provider_->deregisterMemory(buf.data() + 4096));
    EXPECT_TRUE(provider_->makeToken(buf.data() + 4096, 8192, rdma_op::GET));
}

TEST_P(ofiTokenProviderTest, DeregisteredBufferHasNoToken) {
    const size_t n = 64 << 10;
    std::vector<char> buf(n);
    ASSERT_EQ(NIXL_SUCCESS, provider_->registerMemory(buf.data(), n, DRAM_SEG, 0));
    ASSERT_EQ(NIXL_SUCCESS, provider_->deregisterMemory(buf.data()));
    EXPECT_FALSE(provider_->makeToken(buf.data(), n, rdma_op::GET));
    EXPECT_EQ(NIXL_ERR_NOT_FOUND, provider_->deregisterMemory(buf.data()));
}

TEST_P(ofiTokenProviderTest, ProvidersWithTheSameSettingsShareAnEndpoint) {
    // some providers, the UET reference provider among them, allow one
    // endpoint per process
    auto other = openProvider(GetParam());
    ASSERT_TRUE(other);
    const size_t n = 64 << 10;
    std::vector<char> a(n, 0), b(n, 0);
    ASSERT_EQ(NIXL_SUCCESS, provider_->registerMemory(a.data(), n, DRAM_SEG, 0));
    ASSERT_EQ(NIXL_SUCCESS, other->registerMemory(b.data(), n, DRAM_SEG, 0));
    auto ta = provider_->makeToken(a.data(), n, rdma_op::GET);
    auto tb = other->makeToken(b.data(), n, rdma_op::GET);
    ASSERT_TRUE(ta && tb);
    EXPECT_EQ(ofi_rma::parse_token(ta->value())->name, ofi_rma::parse_token(tb->value())->name);

    // the endpoint outlives the first provider, and the second one's windows work
    ta.reset();
    provider_.reset();
    const auto src = pattern(n, 11);
    ASSERT_EQ(0, writeAll(*writer_, tb->value(), src)) << writer_->last_error();
    tb.reset();
    EXPECT_EQ(0, memcmp(b.data(), src.data(), n));
}

TEST_P(ofiTokenProviderTest, VramNeedsAGpuMemoryType) {
    // ofi_hmem=none: GPU buffers are not this provider's to register
    EXPECT_FALSE(provider_->supportsMem(VRAM_SEG));
    std::vector<char> host(4096);
    EXPECT_EQ(NIXL_ERR_NOT_SUPPORTED,
              provider_->registerMemory(host.data(), host.size(), VRAM_SEG, 0));
}

INSTANTIATE_TEST_SUITE_P(Providers,
                         ofiTokenProviderTest,
                         ::testing::ValuesIn(testProviders()),
                         providerTestName);

TEST(OfiTokenConfig, ReadsTheBackendParameters) {
    std::string err;
    const nixl_b_params_t none = {{"ofi_node", "127.0.0.1"}};
    EXPECT_FALSE(ofiTokenConfig::fromParams(&none, &err));
    EXPECT_NE(std::string::npos, err.find("ofi_provider"));
    EXPECT_FALSE(ofiTokenConfig::fromParams(nullptr, &err));

    const nixl_b_params_t all = {{"ofi_provider", "verbs;ofi_rxm"},
                                 {"ofi_domain", "rxe0"},
                                 {"ofi_node", "10.0.0.5"},
                                 {"ofi_service", "4793"},
                                 {"ofi_hmem", "rocr"}};
    const auto cfg = ofiTokenConfig::fromParams(&all, &err);
    ASSERT_TRUE(cfg) << err;
    EXPECT_EQ("verbs;ofi_rxm", cfg->provider);
    EXPECT_EQ("rxe0", cfg->domain);
    EXPECT_EQ("10.0.0.5", cfg->node);
    EXPECT_EQ("4793", cfg->service);
    EXPECT_EQ("rocr", cfg->hmem);

    // auto resolves to the GPU memory type of this build
    const nixl_b_params_t automatic = {{"ofi_provider", "tcp"}};
    const auto resolved = ofiTokenConfig::fromParams(&automatic, &err);
    ASSERT_TRUE(resolved) << err;
    EXPECT_TRUE(resolved->hmem == "none" || resolved->hmem == "cuda" || resolved->hmem == "rocr" ||
                resolved->hmem == "ze")
        << resolved->hmem;

    const nixl_b_params_t bogus = {{"ofi_provider", "tcp"}, {"ofi_hmem", "vulkan"}};
    EXPECT_FALSE(ofiTokenConfig::fromParams(&bogus, &err));
    EXPECT_NE(std::string::npos, err.find("ofi_hmem"));
}

namespace {

/**
 * The HIP runtime's host API, loaded at run time, so that the test needs no
 * GPU at build time and skips without one.
 */
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
        memcpyAny =
            reinterpret_cast<int (*)(void *, const void *, size_t, int)>(dlsym(lib, "hipMemcpy"));
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

} // namespace

namespace {

/**
 * A writer fills part of an AMD GPU buffer over provider p. ROCm memory is
 * registered as a dma-buf that ROCr exports, else through libfabric's FI_HMEM.
 */
void
writerFillsAnAmdGpuBuffer(const providerParam &p) {
    hipRuntime hip;
    if (!hip.load()) {
        GTEST_SKIP() << "no HIP runtime or no AMD GPU";
    }
    auto provider = openProvider(p, "rocr");
    auto writer = openWriter(p);
    if (!provider || !writer) {
        GTEST_SKIP() << p.provider << " is not available";
    }
    if (!provider->supportsMem(VRAM_SEG)) {
        GTEST_SKIP() << p.provider << " offers no FI_HMEM: " << provider->describe();
    }

    const size_t n = 1 << 20;
    void *gpu = nullptr;
    ASSERT_EQ(0, hip.mallocDevice(&gpu, n));
    ASSERT_EQ(0, hip.memsetDevice(gpu, 0, n));
    ASSERT_EQ(0, hip.synchronize());
    const nixl_status_t st = provider->registerMemory(gpu, n, VRAM_SEG, 0);
    if (st == NIXL_ERR_NOT_SUPPORTED) {
        (void)hip.freeDevice(gpu);
        GTEST_SKIP() << "this libfabric has no ROCr support";
    }
    ASSERT_EQ(NIXL_SUCCESS, st);
    EXPECT_EQ(VRAM_SEG, provider->memTypeAt(gpu, n));

    auto token = provider->makeToken(static_cast<char *>(gpu) + 4096, n - 4096, rdma_op::GET);
    ASSERT_TRUE(token);
    const auto src = pattern(n - 4096, 13);
    ASSERT_EQ(0, writeAll(*writer, token->value(), src)) << writer->last_error();
    token.reset();

    std::vector<char> got(n);
    ASSERT_EQ(0, hip.memcpyAny(got.data(), gpu, n, hipRuntime::device_to_host));
    EXPECT_EQ(std::vector<char>(4096, 0), std::vector<char>(got.begin(), got.begin() + 4096));
    EXPECT_EQ(0, memcmp(got.data() + 4096, src.data(), src.size()));

    EXPECT_EQ(NIXL_SUCCESS, provider->deregisterMemory(gpu));
    EXPECT_EQ(0, hip.freeDevice(gpu));
}

} // namespace

TEST(OfiTokenProviderGpu, WriterFillsAnAmdGpuBufferOverShm) {
    writerFillsAnAmdGpuBuffer({"shm", ""});
}

// The path a NIC takes into GPU memory: OFI_RMA_TEST_VERBS_NODE names the
// address of an RDMA device whose verbs provider offers FI_HMEM.
TEST(OfiTokenProviderGpu, WriterFillsAnAmdGpuBufferOverVerbs) {
    const char *node = std::getenv("OFI_RMA_TEST_VERBS_NODE");
    if (node == nullptr || *node == '\0') {
        GTEST_SKIP() << "OFI_RMA_TEST_VERBS_NODE is not set";
    }
    setenv("FI_UNIVERSE_SIZE", "16", 0);
    setenv("FI_MR_CACHE_MONITOR", "disabled", 0);
    writerFillsAnAmdGpuBuffer({"verbs;ofi_rxm", node});
}
