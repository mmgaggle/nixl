/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NIXL_SRC_UTILS_OBJECT_RDMA_CONTROL_PLANE_H
#define NIXL_SRC_UTILS_OBJECT_RDMA_CONTROL_PLANE_H

// The S3-over-RDMA control plane as an interface: the signed, body-less GET or
// PUT that carries an x-amz-rdma-token. It needs no AWS SDK, so code that
// drives it (rdmaRetry.h) can be tested with a fake server.
// s3_control_plane_http.h holds the implementation over the AWS SDK.

#include <chrono>
#include <cstdint>
#include <string>

#include <sys/types.h> // ssize_t

namespace nixl_obj_rdma {

// S3 multipart upload caps a single upload at 10000 parts, so a part number is
// valid only in 1..s3_max_multipart_part_number.
inline constexpr uint32_t s3_max_multipart_part_number = 10000;

/**
 * @brief Per-call context for an RDMA PUT/GET control-plane request.
 *
 * Region and credentials live in the control plane's signer, not here.
 */
struct S3RdmaClientCtx {
    std::string bucket; ///< Target bucket.
    std::string object; ///< Object key.
    std::string uploadId; ///< Multipart upload id; empty for single-shot.
    uint32_t partNumber = 0; ///< Part number 1..10000 when uploadId is set.
    std::string etag; ///< ETag returned by the server; populated on success.
    /** Set by a GET that delivered the data in the HTTP body (a fallback). */
    bool httpBody = false;
    /** Set by a request that got an HTTP response, whatever its status. */
    bool answered = false;
    /** When the last request was sent; rdmaGetWithRetry() sets it. */
    std::chrono::steady_clock::time_point lastSend{};
};

/** @brief The control plane, as the retry wrappers drive it. */
class iS3RdmaControlPlane {
public:
    virtual ~iS3RdmaControlPlane() = default;

    /**
     * @brief Issue the signed control-plane PUT carrying the RDMA token.
     * @param ctx Request context (bucket/object, multipart, etag out).
     * @param token RDMA token, sent verbatim as x-amz-rdma-token.
     * @param size Number of bytes to transfer.
     * @return Bytes transferred (>0) on RDMA success, rdma_not_supported if the
     *         server declined, or rdma_error on transport failure.
     */
    [[nodiscard]] virtual ssize_t
    rdmaPut(S3RdmaClientCtx &ctx, const char *token, uint64_t size) = 0;

    /**
     * @brief Issue the signed control-plane GET carrying the RDMA token.
     * @param ctx Request context (bucket/object, etag out).
     * @param token RDMA token, sent verbatim as x-amz-rdma-token.
     * @param size Number of bytes to fetch.
     * @param offset Byte offset into the object.
     * @param body_dst Host memory of size bytes for the HTTP body, or
     *        nullptr. With it, a server that declines RDMA and sends the
     *        range in the body still completes the GET, with ctx.httpBody
     *        set. Without it, a decline returns rdma_not_supported.
     * @return Bytes delivered (>=0), rdma_not_supported if declined, or
     *         rdma_error on failure.
     */
    [[nodiscard]] virtual ssize_t
    rdmaGet(S3RdmaClientCtx &ctx,
            const char *token,
            uint64_t size,
            uint64_t offset,
            void *body_dst) = 0;
};

} // namespace nixl_obj_rdma

#endif // NIXL_SRC_UTILS_OBJECT_RDMA_CONTROL_PLANE_H
