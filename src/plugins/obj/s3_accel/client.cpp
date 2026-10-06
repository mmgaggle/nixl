/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "client.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "object/engine_utils.h"
#include "common/backend.h"
#include "common/nixl_log.h"

namespace {

// The token type when `rdma_transport` is not set: cuObject in a build with
// it, so an accelerated backend behaves as before, and libfabric otherwise.
[[nodiscard]] std::string
defaultRdmaTransport() {
#ifdef HAVE_CUOBJ_CLIENT
    return "cuobj";
#else
    return "ofi";
#endif
}

[[nodiscard]] std::shared_ptr<nixl_obj_rdma::iRdmaTokenProvider>
makeTokenProvider(const nixl_b_params_t *params, const std::string &transport, std::string *err) {
    if (transport == "cuobj") {
#ifdef HAVE_CUOBJ_CLIENT
        std::shared_ptr<nixl_obj_rdma::iRdmaTokenProvider> tokens =
            nixl_obj_rdma::cuobjTokenProvider::create();
        if (!tokens) {
            *err = "the cuObject RDMA fabric is not connected";
        }
        return tokens;
#else
        *err = "rdma_transport=cuobj, but NIXL was built without cuObject";
        return nullptr;
#endif
    }
    if (transport == "ofi") {
#ifdef HAVE_OFI_RMA
        const auto cfg = nixl_obj_rdma::ofiTokenConfig::fromParams(params, err);
        if (!cfg) {
            return nullptr;
        }
        return nixl_obj_rdma::ofiTokenProvider::create(*cfg, err);
#else
        *err = "rdma_transport=ofi, but NIXL was built without ofi-rma";
        return nullptr;
#endif
    }
    *err = "rdma_transport='" + transport + "' is neither cuobj nor ofi";
    return nullptr;
}

} // namespace

awsS3AccelClient::awsS3AccelClient(nixl_b_params_t *custom_params,
                                   std::shared_ptr<Aws::Utils::Threading::Executor> executor)
    : awsS3Client(custom_params, executor),
      executor_(std::move(executor)),
      rdma_requested_(isGenericAccelRequested(custom_params)) {
    // Attach the S3-over-RDMA fast path only for the generic accel path. Vendor
    // subclasses (selected by an explicit `type`) leave rdma_requested_ false and
    // manage RDMA on their own, so this stays the plain HTTP base for them.
    std::string why;
    if (rdma_requested_) {
        const std::string transport =
            nixl::getBackendParamDefaulted(custom_params, "rdma_transport", defaultRdmaTransport());
        tokens_ = makeTokenProvider(custom_params, transport, &why);
        if (tokens_ && !tokens_->isConnected()) {
            why = "the " + transport + " transport is not connected";
            tokens_.reset();
        }
        if (tokens_) {
            auto cp = std::make_shared<nixl_obj_rdma::S3RdmaControlPlane>(custom_params);
            if (cp->valid()) {
                rdmaCp_ = std::move(cp);
            } else {
                why = "the S3 RDMA control plane did not initialize (endpoint or credentials)";
                tokens_.reset();
            }
        }
        httpFallback_ = nixl::getBackendParamDefaulted(custom_params, "rdma_http_fallback", false);
        // libfabric writers are OSDs that hold the token: after a GET with no
        // response, they write until their lease ends (5 s in Ceph) and the
        // gateway's drain (3 s). cuObject keeps its behavior.
        const uint64_t fence_default = (tokens_ && tokens_->name() == "ofi") ? 10000 : 0;
        fence_ = std::chrono::milliseconds(
            nixl::getBackendParamDefaulted(custom_params, "rdma_fence_ms", fence_default));
    }

    // Fail fast: a generic accel client whose fast path is not fully ready can
    // never recover (setExecutor is unsupported and tokens_/rdmaCp_ are set once
    // here), so surface it at construction instead of failing every transfer.
    if (rdma_requested_ && !rdmaReady()) {
        if (why.empty()) {
            why = "no executor";
        }
        throw std::runtime_error("accelerated=true (generic S3-over-RDMA) requested but the "
                                 "fast path is unavailable: " +
                                 why + "; no HTTP fallback");
    }

    NIXL_DEBUG << "S3 Accelerated client initialized (rdma="
               << (rdma_requested_ ? std::string(tokens_->name()) : std::string("off")) << ")";
}

bool
awsS3AccelClient::rdmaReady() const {
    return tokens_ != nullptr && rdmaCp_ != nullptr && executor_ != nullptr;
}

bool
awsS3AccelClient::supportsRdma() const {
    return rdma_requested_ && rdmaReady();
}

void
awsS3AccelClient::putObjectAsync(std::string_view key,
                                 uintptr_t data_ptr,
                                 size_t data_len,
                                 size_t offset,
                                 put_object_callback_t callback) {
    if (!rdma_requested_) {
        awsS3Client::putObjectAsync(key, data_ptr, data_len, offset, callback);
        return;
    }

    if (!tokens_->supportsPut()) {
        // The server reads the payload of a PUT with this token type over
        // HTTP (Ceph RGW with a libfabric token), so the PUT carries no token:
        // a plain S3 PUT, which needs host memory.
        const auto mem = tokens_->memTypeAt(reinterpret_cast<const void *>(data_ptr), data_len);
        if (mem != DRAM_SEG) {
            NIXL_ERROR << "S3 RDMA put: the " << tokens_->name()
                       << " transport sends a PUT payload over HTTP, which needs registered "
                          "host memory; key="
                       << key;
            callback(false);
            return;
        }
        awsS3Client::putObjectAsync(key, data_ptr, data_len, offset, callback);
        return;
    }

    // A single-shot RDMA PUT writes the whole object; there is no offset in the
    // PUT control plane, so a non-zero offset cannot be honored.
    if (offset != 0) {
        NIXL_ERROR << "S3 RDMA put: non-zero offset (" << offset << ") not supported, key=" << key;
        callback(false);
        return;
    }

    // Capture the transfer state by value (bucket, the token provider, and a
    // shared control plane) rather than `this`, so the task is self-contained
    // and safe even if this client is destroyed while it runs.
    const bool submitted = executor_->Submit([tokens = tokens_,
                                              cp = rdmaCp_,
                                              bucket = std::string(bucketName_.c_str()),
                                              k = std::string(key),
                                              data_ptr,
                                              data_len,
                                              callback]() {
        nixl_obj_rdma::S3RdmaClientCtx ctx;
        ctx.bucket = bucket;
        ctx.object = k;
        const ssize_t r = nixl_obj_rdma::rdmaPutWithRetry(
            *tokens, *cp, ctx, reinterpret_cast<void *>(data_ptr), data_len);
        // Success is a complete transfer: the descriptor length is a promise, so a
        // short count is a failure, not a partially-written object.
        const bool ok = (r == static_cast<ssize_t>(data_len));
        if (!ok) {
            NIXL_ERROR << "S3 RDMA put failed (" << r << " of " << data_len
                       << "; accelerated=true, no HTTP fallback), key=" << k;
        }
        callback(ok);
    });
    // A rejected submission never runs the task, so fire the callback here or the
    // transfer's future would never complete.
    if (!submitted) {
        NIXL_ERROR << "S3 RDMA put: executor rejected task, key=" << key;
        callback(false);
    }
}

void
awsS3AccelClient::getObjectAsync(std::string_view key,
                                 uintptr_t data_ptr,
                                 size_t data_len,
                                 size_t offset,
                                 get_object_callback_t callback) {
    if (!rdma_requested_) {
        awsS3Client::getObjectAsync(key, data_ptr, data_len, offset, callback);
        return;
    }

    // With rdma_http_fallback, the body of a declined GET goes straight into a
    // host buffer. A GPU buffer cannot take it, so it keeps the hard error.
    void *body_dst = nullptr;
    if (httpFallback_ &&
        tokens_->memTypeAt(reinterpret_cast<const void *>(data_ptr), data_len) == DRAM_SEG) {
        body_dst = reinterpret_cast<void *>(data_ptr);
    }

    // See putObjectAsync: capture the transfer state by value so the task is
    // self-contained and safe even if this client is destroyed while it runs.
    const bool submitted = executor_->Submit([tokens = tokens_,
                                              cp = rdmaCp_,
                                              fence = fence_,
                                              warned = warnedFallback_,
                                              bucket = std::string(bucketName_.c_str()),
                                              k = std::string(key),
                                              data_ptr,
                                              data_len,
                                              offset,
                                              body_dst,
                                              callback]() {
        nixl_obj_rdma::S3RdmaClientCtx ctx;
        ctx.bucket = bucket;
        ctx.object = k;
        const ssize_t r = nixl_obj_rdma::rdmaGetWithRetry(
            *tokens, *cp, ctx, reinterpret_cast<void *>(data_ptr), data_len, offset, body_dst);
        // A full read is required: the server clamps to min(requested, servable),
        // so a short count (r < data_len) leaves the buffer tail unfilled and must
        // not be reported as success.
        const bool ok = (r == static_cast<ssize_t>(data_len));
        if (!ok) {
            NIXL_ERROR << "S3 RDMA get failed (" << r << " of " << data_len << " over "
                       << tokens->name() << "), key=" << k;
            // With no response, a writer can still be writing into the buffer:
            // report the failure only once no write of this GET can land.
            if (nixl_obj_rdma::fenceFailedGet(ctx, fence)) {
                NIXL_WARN << "S3 RDMA get: held back the failure for " << fence.count()
                          << " ms after the request, until no write of it can land, key=" << k;
            }
        } else if (ctx.httpBody && !warned->exchange(true)) {
            NIXL_WARN << "S3 RDMA get: the server declined RDMA, and the data came over HTTP "
                         "(rdma_http_fallback=true); key="
                      << k;
        }
        callback(ok);
    });
    // A rejected submission never runs the task, so fire the callback here or the
    // transfer's future would never complete.
    if (!submitted) {
        NIXL_ERROR << "S3 RDMA get: executor rejected task, key=" << key;
        callback(false);
    }
}
