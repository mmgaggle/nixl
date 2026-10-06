/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NIXL_SRC_UTILS_OBJECT_RDMA_S3_CONTROL_PLANE_HTTP_H
#define NIXL_SRC_UTILS_OBJECT_RDMA_S3_CONTROL_PLANE_HTTP_H

// S3 RDMA control plane: the signed, body-less GET/PUT that carries the
// x-amz-rdma-token and negotiates the out-of-band RDMA transfer. This is the
// only component that touches the AWS SDK's low-level HTTP/signing layer; the
// wire-protocol helpers it builds on are in rdma_protocol.h (SDK-free).

#include "control_plane.h"
#include "rdma_protocol.h"

#include <cstdint>
#include <memory>
#include <string>

#include <sys/types.h> // ssize_t

#include "nixl_types.h"

namespace nixl_obj_rdma {

/**
 * @brief S3 RDMA control plane.
 *
 * Owns the AWS SDK primitives (SigV4 signer + HTTP client + resolved endpoint)
 * used to issue the body-less, RDMA-token-carrying GET/PUT that negotiates the
 * out-of-band transfer. This is the only component that touches the AWS SDK's
 * low-level HTTP layer; it is deliberately narrow so the protocol logic around
 * it stays SDK-agnostic and testable.
 */
class S3RdmaControlPlane : public iS3RdmaControlPlane {
public:
    /**
     * @brief Build the control plane from backend params.
     *
     * Resolves the endpoint, region, and credentials. On failure, valid()
     * returns false and the instance is unusable. `rdma_request_timeout_ms`
     * sets the request timeout (default 10000). A server that relays a GET
     * back over HTTP, as Ceph RGW does after waiting out an OSD lease, can
     * need longer.
     * @param custom_params Backend key-value params; may be nullptr.
     */
    explicit S3RdmaControlPlane(const nixl_b_params_t *custom_params);
    ~S3RdmaControlPlane() override;

    /**
     * @brief Whether the control plane initialized successfully.
     * @return true iff the HTTP client and (access + secret) credentials resolved.
     */
    [[nodiscard]] bool
    valid() const {
        return valid_;
    }

    /**
     * @brief Issue the signed control-plane PUT carrying the RDMA token.
     * @param ctx Request context (bucket/object, multipart, etag out).
     * @param token RDMA descriptor (carries the buffer address and size in its
     *        own leading fields; sent verbatim as x-amz-rdma-token).
     * @param size Number of bytes to transfer.
     * @return Bytes transferred (>0) on RDMA success, rdma_not_supported if the
     *         server declined, or rdma_error on transport failure.
     */
    [[nodiscard]] ssize_t
    rdmaPut(S3RdmaClientCtx &ctx, const char *token, uint64_t size) override;

    /**
     * @brief Issue the signed control-plane GET carrying the RDMA token.
     * @param ctx Request context (bucket/object, etag out).
     * @param token RDMA descriptor (carries the buffer address and size in its
     *        own leading fields; sent verbatim as x-amz-rdma-token).
     * @param size Number of bytes to fetch.
     * @param offset Byte offset into the object. The request always carries
     *        a byte range, so a server answers 206 with x-amz-rdma-reply: 206.
     * @param body_dst Host memory for the body of a declined GET, or nullptr.
     *        See iS3RdmaControlPlane::rdmaGet().
     * @return Bytes delivered (>=0), rdma_not_supported if declined, or
     *         rdma_error on failure.
     */
    [[nodiscard]] ssize_t
    rdmaGet(S3RdmaClientCtx &ctx, const char *token, uint64_t size, uint64_t offset, void *body_dst)
        override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool valid_ = false;
};

} // namespace nixl_obj_rdma

#endif // NIXL_SRC_UTILS_OBJECT_RDMA_S3_CONTROL_PLANE_HTTP_H
