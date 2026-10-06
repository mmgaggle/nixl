/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

// The retry wrappers and the fence, with a fake token provider and a fake
// control plane: no AWS endpoint and no RDMA transport.

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <string>
#include <vector>

#include "object/rdma/rdmaRetry.h"
#include "object/rdma/rdma_protocol.h"

namespace {

using namespace nixl_obj_rdma;
using namespace std::chrono_literals;

class fakeTokens : public iRdmaTokenProvider {
public:
    int mints = 0;
    int releases = 0;
    int failMints = 0; // mints that fail before one succeeds
    std::vector<rdma_op> ops;

    [[nodiscard]] std::string_view
    name() const override {
        return "fake";
    }

    [[nodiscard]] bool
    isConnected() const override {
        return true;
    }

    [[nodiscard]] bool
    supportsMem(nixl_mem_t) const override {
        return true;
    }

    [[nodiscard]] bool
    supportsPut() const override {
        return true;
    }

    [[nodiscard]] nixl_status_t
    registerMemory(void *, size_t, nixl_mem_t, uint64_t) override {
        return NIXL_SUCCESS;
    }

    nixl_status_t
    deregisterMemory(void *) override {
        return NIXL_SUCCESS;
    }

    [[nodiscard]] std::optional<rdmaToken>
    makeToken(void *, size_t, rdma_op op) override {
        ops.push_back(op);
        if (failMints > 0) {
            --failMints;
            return std::nullopt;
        }
        ++mints;
        return rdmaToken("token-" + std::to_string(mints), [this] { ++releases; });
    }
};

class fakeControlPlane : public iS3RdmaControlPlane {
public:
    explicit fakeControlPlane(const fakeTokens &tokens) : tokens_(tokens) {}

    std::deque<ssize_t> results; // one per request; rdma_error when empty
    std::vector<std::string> tokensSent;
    std::vector<void *> bodyDsts;
    int releasedDuringRequest = 0; // a token released before its response came

    [[nodiscard]] ssize_t
    rdmaPut(S3RdmaClientCtx &ctx, const char *token, uint64_t) override {
        return answer(ctx, token);
    }

    [[nodiscard]] ssize_t
    rdmaGet(S3RdmaClientCtx &ctx, const char *token, uint64_t, uint64_t, void *body_dst) override {
        bodyDsts.push_back(body_dst);
        return answer(ctx, token);
    }

private:
    ssize_t
    answer(S3RdmaClientCtx &ctx, const char *token) {
        tokensSent.emplace_back(token);
        if (tokens_.releases != tokens_.mints - 1) {
            ++releasedDuringRequest;
        }
        ctx.answered = true;
        if (results.empty()) {
            return rdma_error;
        }
        const ssize_t r = results.front();
        results.pop_front();
        return r;
    }

    const fakeTokens &tokens_;
};

TEST(RdmaRetry, GetSucceedsOnceAndReleasesItsToken) {
    fakeTokens tokens;
    fakeControlPlane cp(tokens);
    cp.results = {4096};
    S3RdmaClientCtx ctx;
    char buf[4096];
    EXPECT_EQ(rdmaGetWithRetry(tokens, cp, ctx, buf, sizeof buf, 0), 4096);
    EXPECT_EQ(tokens.mints, 1);
    EXPECT_EQ(tokens.releases, 1);
    EXPECT_EQ(cp.releasedDuringRequest, 0);
    EXPECT_EQ(tokens.ops, std::vector<rdma_op>{rdma_op::GET});
    EXPECT_NE(ctx.lastSend, std::chrono::steady_clock::time_point{});
}

TEST(RdmaRetry, GetRetriesAFailedMintOnce) {
    fakeTokens tokens;
    tokens.failMints = 1;
    fakeControlPlane cp(tokens);
    cp.results = {100};
    S3RdmaClientCtx ctx;
    char buf[100];
    EXPECT_EQ(rdmaGetWithRetry(tokens, cp, ctx, buf, sizeof buf, 0), 100);
    EXPECT_EQ(tokens.ops.size(), 2u);
    EXPECT_EQ(cp.tokensSent, std::vector<std::string>{"token-1"});
    EXPECT_EQ(tokens.releases, 1);
}

TEST(RdmaRetry, GetRetriesAnErrorOnceWithAFreshToken) {
    fakeTokens tokens;
    fakeControlPlane cp(tokens);
    cp.results = {rdma_error, 100};
    S3RdmaClientCtx ctx;
    char buf[100];
    EXPECT_EQ(rdmaGetWithRetry(tokens, cp, ctx, buf, sizeof buf, 7), 100);
    EXPECT_EQ(cp.tokensSent, (std::vector<std::string>{"token-1", "token-2"}));
    EXPECT_EQ(tokens.releases, 2);
    EXPECT_EQ(cp.releasedDuringRequest, 0);
}

TEST(RdmaRetry, GetGivesUpAfterTwoErrors) {
    fakeTokens tokens;
    fakeControlPlane cp(tokens);
    cp.results = {rdma_error, rdma_error, 100};
    S3RdmaClientCtx ctx;
    char buf[100];
    EXPECT_EQ(rdmaGetWithRetry(tokens, cp, ctx, buf, sizeof buf, 0), rdma_error);
    EXPECT_EQ(cp.tokensSent.size(), 2u);
    EXPECT_EQ(tokens.releases, 2);
}

TEST(RdmaRetry, GetDoesNotRetryADecline) {
    fakeTokens tokens;
    fakeControlPlane cp(tokens);
    cp.results = {rdma_not_supported, 100};
    S3RdmaClientCtx ctx;
    char buf[100];
    EXPECT_EQ(rdmaGetWithRetry(tokens, cp, ctx, buf, sizeof buf, 0), rdma_not_supported);
    EXPECT_EQ(cp.tokensSent.size(), 1u);
    EXPECT_EQ(tokens.releases, 1);
}

TEST(RdmaRetry, GetPassesTheBodyDestination) {
    fakeTokens tokens;
    fakeControlPlane cp(tokens);
    cp.results = {100};
    S3RdmaClientCtx ctx;
    char buf[100];
    EXPECT_EQ(rdmaGetWithRetry(tokens, cp, ctx, buf, sizeof buf, 0, buf), 100);
    EXPECT_EQ(cp.bodyDsts, std::vector<void *>{buf});
}

TEST(RdmaRetry, GetRejectsAnEmptyRangeWithoutAToken) {
    fakeTokens tokens;
    fakeControlPlane cp(tokens);
    S3RdmaClientCtx ctx;
    char buf[1];
    EXPECT_EQ(rdmaGetWithRetry(tokens, cp, ctx, buf, 0, 0), rdma_error);
    EXPECT_TRUE(tokens.ops.empty());
}

TEST(RdmaRetry, PutMintsAPutTokenAndRetriesOnce) {
    fakeTokens tokens;
    fakeControlPlane cp(tokens);
    cp.results = {rdma_error, 64};
    S3RdmaClientCtx ctx;
    char buf[64];
    EXPECT_EQ(rdmaPutWithRetry(tokens, cp, ctx, buf, sizeof buf), 64);
    EXPECT_EQ(tokens.ops, (std::vector<rdma_op>{rdma_op::PUT, rdma_op::PUT}));
    EXPECT_EQ(tokens.releases, 2);
    EXPECT_EQ(cp.releasedDuringRequest, 0);
}

TEST(RdmaFence, WaitsOnlyForAGetWithoutAnAnswer) {
    S3RdmaClientCtx ctx;
    // never sent: nothing to wait for
    EXPECT_FALSE(fenceFailedGet(ctx, 200ms));

    ctx.lastSend = std::chrono::steady_clock::now();
    ctx.answered = true; // the server drained its writes before it answered
    EXPECT_FALSE(fenceFailedGet(ctx, 200ms));

    ctx.answered = false;
    EXPECT_FALSE(fenceFailedGet(ctx, 0ms));

    // no answer: wait until the fence after the send, not after now
    ctx.lastSend = std::chrono::steady_clock::now() - 50ms;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(fenceFailedGet(ctx, 200ms));
    const auto waited = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(waited, 140ms);
    EXPECT_LT(waited, 1000ms);

    // the fence already passed
    ctx.lastSend = std::chrono::steady_clock::now() - 300ms;
    EXPECT_FALSE(fenceFailedGet(ctx, 200ms));
}

} // namespace
