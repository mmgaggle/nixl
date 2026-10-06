/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NIXL_SRC_UTILS_OBJECT_RDMA_RDMARETRY_H
#define NIXL_SRC_UTILS_OBJECT_RDMA_RDMARETRY_H

// Token-lifecycle + one-transient-retry wrappers that drive a full RDMA PUT/GET:
// mint a token from a token provider (token_provider.h), issue the signed
// control-plane request (control_plane.h), release the token, and retry once on
// a transient failure. They need neither the AWS SDK nor a transport library.

#include <chrono>
#include <cstdint>

#include <sys/types.h> // ssize_t

#include "control_plane.h"
#include "token_provider.h"

namespace nixl_obj_rdma {

/**
 * @brief Mint a token, run rdmaPut, release the token, with one transient retry.
 *
 * The retry covers token-mint and control-plane hiccups. The buffer must
 * already be registered with @p tokens.
 * @param tokens Provider that mints and releases the token.
 * @param cp Control plane that issues the signed request.
 * @param ctx Request context (bucket/object, multipart, etag out).
 * @param buf Source buffer.
 * @param size Number of bytes to transfer.
 * @return >0 bytes transferred (success), rdma_not_supported (server declined),
 *         or rdma_error (failure). The caller treats anything < 0 as an error —
 *         there is no HTTP fallback under accelerated=true.
 */
[[nodiscard]] ssize_t
rdmaPutWithRetry(iRdmaTokenProvider &tokens,
                 iS3RdmaControlPlane &cp,
                 S3RdmaClientCtx &ctx,
                 void *buf,
                 uint64_t size);

/**
 * @brief Mint a token, run rdmaGet, release the token, with one transient retry.
 *
 * The transfer is byte-ranged via @p offset. The buffer must already be
 * registered with @p tokens. Each token is released after its response
 * arrives, and before this returns: for a libfabric token, that orders the
 * caller's reads after the writes the server placed. ctx.lastSend is the
 * time the last attempt was sent.
 * @param tokens Provider that mints and releases the token.
 * @param cp Control plane that issues the signed request.
 * @param ctx Request context (bucket/object, etag out).
 * @param buf Destination buffer.
 * @param size Number of bytes to fetch.
 * @param offset Byte offset into the object.
 * @param body_dst Host memory for the body of a declined GET (the HTTP
 *        fallback), or nullptr. See iS3RdmaControlPlane::rdmaGet().
 * @return >=0 bytes delivered (success), rdma_not_supported (server
 *         declined), or rdma_error (failure).
 */
[[nodiscard]] ssize_t
rdmaGetWithRetry(iRdmaTokenProvider &tokens,
                 iS3RdmaControlPlane &cp,
                 S3RdmaClientCtx &ctx,
                 void *buf,
                 uint64_t size,
                 uint64_t offset,
                 void *body_dst = nullptr);

/**
 * @brief After a failed GET, wait until no write of it can still land.
 *
 * A server that answered has drained its writes: Ceph RGW waits for every OSD
 * write of a GET before it responds. A GET that got no answer, after a timeout
 * or a broken connection, can still have writers that write into the buffer
 * until their lease ends. So this waits until @p fence after ctx.lastSend,
 * and the caller reports the failure only then.
 * @return Whether it waited.
 */
bool
fenceFailedGet(const S3RdmaClientCtx &ctx, std::chrono::milliseconds fence);

} // namespace nixl_obj_rdma

#endif // NIXL_SRC_UTILS_OBJECT_RDMA_RDMARETRY_H
