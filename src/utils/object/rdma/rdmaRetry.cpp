/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rdmaRetry.h"

#include <thread>

#include "rdma_protocol.h"
#include "common/nixl_log.h"

namespace nixl_obj_rdma {

/**
 * Retry wrappers (token lifecycle + one transient retry). A token-mint failure
 * is itself transient (NIC selection / registration hiccup), so it is retried
 * rather than aborting on the first attempt.
 */
ssize_t
rdmaPutWithRetry(iRdmaTokenProvider &tokens,
                 iS3RdmaControlPlane &cp,
                 S3RdmaClientCtx &ctx,
                 void *buf,
                 uint64_t size) {
    ssize_t ret = rdma_error;
    for (int attempt = 0; attempt < rdma_max_attempts; ++attempt) {
        std::optional<rdmaToken> token = tokens.makeToken(buf, size, rdma_op::PUT);
        if (!token) {
            ret = rdma_error;
            continue; // transient mint failure: retry
        }
        ret = cp.rdmaPut(ctx, token->value().c_str(), size);
        token->reset();
        if (ret > 0 || ret == rdma_not_supported) {
            break;
        }
    }
    return ret;
}

ssize_t
rdmaGetWithRetry(iRdmaTokenProvider &tokens,
                 iS3RdmaControlPlane &cp,
                 S3RdmaClientCtx &ctx,
                 void *buf,
                 uint64_t size,
                 uint64_t offset,
                 void *body_dst) {
    // Reject a zero-size GET before minting a token (rdmaGet also guards, but
    // the token is minted here first).
    if (size == 0) {
        NIXL_ERROR << "rdmaGet: zero-size request for key=" << ctx.object;
        return rdma_error;
    }
    ssize_t ret = rdma_error;
    for (int attempt = 0; attempt < rdma_max_attempts; ++attempt) {
        std::optional<rdmaToken> token = tokens.makeToken(buf, size, rdma_op::GET);
        if (!token) {
            ret = rdma_error;
            continue; // transient mint failure: retry
        }
        ctx.lastSend = std::chrono::steady_clock::now();
        ret = cp.rdmaGet(ctx, token->value().c_str(), size, offset, body_dst);
        // The response is in: release the token before the bytes are used.
        token->reset();
        // ret >= 0 is success: 0 is a valid transfer (an empty object/range).
        // Only a negative rdma_error is a transient failure worth retrying; a
        // decline is terminal.
        if (ret >= 0 || ret == rdma_not_supported) {
            break;
        }
    }
    return ret;
}

bool
fenceFailedGet(const S3RdmaClientCtx &ctx, std::chrono::milliseconds fence) {
    if (ctx.answered || fence.count() <= 0 ||
        ctx.lastSend == std::chrono::steady_clock::time_point{}) {
        return false;
    }
    const auto until = ctx.lastSend + fence;
    if (std::chrono::steady_clock::now() >= until) {
        return false;
    }
    std::this_thread::sleep_until(until);
    return true;
}

} // namespace nixl_obj_rdma
