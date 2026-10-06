/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef OBJ_PLUGIN_S3_ACCEL_CLIENT_H
#define OBJ_PLUGIN_S3_ACCEL_CLIENT_H

#include <atomic>
#include <chrono>
#include <memory>
#include <string_view>

#include "s3/client.h"
#include "object/rdma/rdma.h"
#include "nixl_types.h"

/**
 * S3 Accelerated Object Client - the generic, protocol-compliant S3-over-RDMA
 * client, and the base for vendor-specific accelerated clients.
 *
 * When `accelerated=true` is requested with no `type` (or `type=s3`), this
 * client moves the object payload out-of-band over the published `x-amz-rdma-*`
 * protocol instead of streaming the body over HTTP: putObjectAsync/getObjectAsync
 * mint a token for the (engine-registered) buffer and drive the RDMA control
 * plane. `rdma_transport` picks the token type: `cuobj` (cuObject DC
 * descriptors) or `ofi` (libfabric ofi1 tokens, which Ceph RGW forwards to its
 * OSDs). The default is `cuobj` in a build with cuObject, `ofi` otherwise.
 * There is no silent HTTP fallback - an unavailable fabric or control plane
 * fails construction, and a server decline is a hard error, unless a GET into
 * host memory opts in with `rdma_http_fallback=true`.
 *
 * Ceph reads the payload of a PUT with a libfabric token over HTTP, so with
 * `ofi` a PUT from host memory is a plain S3 PUT, and a PUT from GPU memory
 * fails.
 *
 * A GET that gets no response, after a timeout or a broken connection, can
 * still have writers that write into the buffer. Its failure is held back
 * until `rdma_fence_ms` after the request was sent (default 10000 for `ofi`,
 * the OSD lease and drain with slack, and 0 for `cuobj`).
 *
 * Vendor clients (selected by an explicit `type`) inherit and manage their own
 * RDMA path; for them the generic fast path stays disabled
 * (isGenericAccelRequested is false), so this client behaves as the plain HTTP
 * base.
 */
class awsS3AccelClient : public awsS3Client {
public:
    awsS3AccelClient(nixl_b_params_t *custom_params,
                     std::shared_ptr<Aws::Utils::Threading::Executor> executor = nullptr);

    ~awsS3AccelClient() override = default;

    void
    putObjectAsync(std::string_view key,
                   uintptr_t data_ptr,
                   size_t data_len,
                   size_t offset,
                   put_object_callback_t callback) override;

    void
    getObjectAsync(std::string_view key,
                   uintptr_t data_ptr,
                   size_t data_len,
                   size_t offset,
                   get_object_callback_t callback) override;

    /**
     * @brief Whether the generic S3-over-RDMA fast path is fully usable
     *        (generic accel requested, cuObject fabric + control plane +
     *        executor all ready). The engine uses this to gate VRAM_SEG
     *        advertisement and buffer pinning.
     */
    [[nodiscard]] bool
    supportsRdma() const;

    /**
     * @brief The token provider of the generic fast path, or nullptr. The
     *        engine registers DRAM and VRAM buffers with it.
     */
    [[nodiscard]] std::shared_ptr<nixl_obj_rdma::iRdmaTokenProvider>
    tokenProvider() const {
        return tokens_;
    }

private:
    [[nodiscard]] bool
    rdmaReady() const;

    std::shared_ptr<Aws::Utils::Threading::Executor> executor_;
    bool rdma_requested_ = false;
    // shared_ptr (not unique_ptr) so an in-flight transfer task can hold the
    // token provider and the control plane alive independently of this
    // client's lifetime.
    std::shared_ptr<nixl_obj_rdma::iRdmaTokenProvider> tokens_;
    std::shared_ptr<nixl_obj_rdma::iS3RdmaControlPlane> rdmaCp_;
    bool httpFallback_ = false;
    std::chrono::milliseconds fence_{0};
    std::shared_ptr<std::atomic<bool>> warnedFallback_ = std::make_shared<std::atomic<bool>>(false);
};

#endif // OBJ_PLUGIN_S3_ACCEL_CLIENT_H
