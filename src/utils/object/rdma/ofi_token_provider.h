/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NIXL_SRC_UTILS_OBJECT_RDMA_OFI_TOKEN_PROVIDER_H
#define NIXL_SRC_UTILS_OBJECT_RDMA_OFI_TOKEN_PROVIDER_H

// libfabric ofi1 tokens for the S3-over-RDMA data path, over ofi-rma.
//
// The client lends its buffers as ofi-rma windows on one libfabric endpoint,
// and sends a token that names the provider, the endpoint and the window's
// memory key:
//
//   <base hex>:<size hex>:ofi1:<provider>:<endpoint name hex>:<key hex>
//
// Ceph RGW forwards the token to its OSDs, which write the object's stripes
// straight into the window over the same provider (tcp, shm, verbs;ofi_rxm,
// or a UET provider). The client keeps no state for the writers. Ceph serves
// a GET this way; a PUT with a libfabric token carries its payload over HTTP,
// so this provider mints GET tokens only.
//
// ROCm memory is lent as a dma-buf that the ROCr runtime exports
// (hsa_amd_portable_export_dmabuf). libfabric's own export of ROCm memory
// needs a kernel built with CONFIG_DMABUF_MOVE_NOTIFY, which some
// distributions lack.

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "token_provider.h"

namespace ofi_rma {
class Endpoint;
}

namespace nixl_obj_rdma {

/**
 * @brief The libfabric token provider's settings, from the backend parameters.
 *
 * | Parameter      | Meaning                                                        |
 * |----------------|----------------------------------------------------------------|
 * | `ofi_provider` | Required. Must equal the OSDs' osd_ofi_provider.               |
 * | `ofi_domain`   | Device or interface, for example mlx5_0, rxe0 or a UET netdev. |
 * | `ofi_node`     | Local address to bind. The OSDs must reach it.                 |
 * | `ofi_service`  | Local port to bind.                                            |
 * | `ofi_hmem`     | GPU memory type for VRAM_SEG: none, cuda, rocr, ze or auto.    |
 *
 * `ofi_hmem=auto`, the default, picks the GPU runtime that NIXL was built with,
 * or none.
 */
struct ofiTokenConfig {
    std::string provider;
    std::string domain;
    std::string node;
    std::string service;
    /** none, cuda, rocr or ze, after auto is resolved. */
    std::string hmem = "none";

    /**
     * @brief Read the settings from backend parameters.
     * @param err Set to the reason on failure.
     * @return The settings, or nullopt when ofi_provider is missing or
     *         ofi_hmem names no known memory type.
     */
    [[nodiscard]] static std::optional<ofiTokenConfig>
    fromParams(const nixl_b_params_t *params, std::string *err);
};

/**
 * @brief Lends registered buffers as ofi-rma windows and mints ofi1 tokens.
 *
 * Providers created with the same settings share one endpoint, because some
 * libfabric providers, the UET reference provider among them, allow one
 * endpoint per process. The endpoint polls itself from a thread of its own,
 * so writes land while a request is open, also on providers that progress
 * manually.
 */
class ofiTokenProvider : public iRdmaTokenProvider {
public:
    /**
     * @brief Open the endpoint, or share the one already open with the same settings.
     * @param err Set to the reason on failure.
     * @return The provider, or nullptr when the endpoint does not open.
     */
    [[nodiscard]] static std::unique_ptr<ofiTokenProvider>
    create(const ofiTokenConfig &cfg, std::string *err);

    ~ofiTokenProvider() override;

    ofiTokenProvider(const ofiTokenProvider &) = delete;
    ofiTokenProvider &
    operator=(const ofiTokenProvider &) = delete;

    [[nodiscard]] std::string_view
    name() const override {
        return "ofi";
    }

    [[nodiscard]] bool
    isConnected() const override;

    [[nodiscard]] bool
    supportsMem(nixl_mem_t mem) const override;

    [[nodiscard]] bool
    supportsPut() const override {
        return false;
    }

    [[nodiscard]] nixl_status_t
    registerMemory(void *ptr, size_t len, nixl_mem_t mem, uint64_t dev_id) override;

    nixl_status_t
    deregisterMemory(void *ptr) override;

    [[nodiscard]] std::optional<rdmaToken>
    makeToken(void *ptr, size_t len, rdma_op op) override;

    [[nodiscard]] std::optional<nixl_mem_t>
    memTypeAt(const void *ptr, size_t len) const override;

    /** @return The endpoint's fabric, domain and memory modes, for logs. */
    [[nodiscard]] std::string
    describe() const;

private:
    struct window {
        uint64_t id = 0;
        char *ptr = nullptr;
        size_t len = 0;
        nixl_mem_t mem = DRAM_SEG;
        int dmabufFd = -1; ///< the dma-buf the window was registered from
    };

    ofiTokenProvider(std::shared_ptr<ofi_rma::Endpoint> ep, const ofiTokenConfig &cfg);

    /** The window that holds [addr, addr + len), or nullptr. Takes mutex_. */
    [[nodiscard]] const window *
    findLocked(uintptr_t addr, size_t len) const;

    /** Release a window: deregister it and close its dma-buf. */
    void
    releaseLocked(const window &w);

    std::shared_ptr<ofi_rma::Endpoint> ep_;
    ofiTokenConfig cfg_;
    mutable std::mutex mutex_;
    /** Registered windows, by start address. */
    std::map<uintptr_t, window> windows_;
};

} // namespace nixl_obj_rdma

#endif // NIXL_SRC_UTILS_OBJECT_RDMA_OFI_TOKEN_PROVIDER_H
