/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NIXL_SRC_UTILS_OBJECT_RDMA_CUOBJ_TOKEN_PROVIDER_H
#define NIXL_SRC_UTILS_OBJECT_RDMA_CUOBJ_TOKEN_PROVIDER_H

// cuObject DC descriptors as tokens, over the process-wide SharedCuObjClient.
// Compiled only when the cuObjClient library is present.

#include <memory>

#include "cuobj_client.h"
#include "token_provider.h"

namespace nixl_obj_rdma {

/** @brief Registers buffers with cuObject and mints its GET and PUT descriptors. */
class cuobjTokenProvider : public iRdmaTokenProvider {
public:
    /**
     * @brief The provider over the process's cuObject client.
     * @return The provider, or nullptr when the RDMA fabric is unavailable.
     */
    [[nodiscard]] static std::unique_ptr<cuobjTokenProvider>
    create();

    [[nodiscard]] std::string_view
    name() const override {
        return "cuobj";
    }

    [[nodiscard]] bool
    isConnected() const override {
        return rdma_->isConnected();
    }

    [[nodiscard]] bool
    supportsMem(nixl_mem_t mem) const override {
        return mem == DRAM_SEG || mem == VRAM_SEG;
    }

    [[nodiscard]] bool
    supportsPut() const override {
        return true;
    }

    [[nodiscard]] nixl_status_t
    registerMemory(void *ptr, size_t len, nixl_mem_t mem, uint64_t dev_id) override;

    nixl_status_t
    deregisterMemory(void *ptr) override;

    [[nodiscard]] std::optional<rdmaToken>
    makeToken(void *ptr, size_t len, rdma_op op) override;

private:
    explicit cuobjTokenProvider(SharedCuObjClient *rdma) : rdma_(rdma) {}

    SharedCuObjClient *rdma_;
};

} // namespace nixl_obj_rdma

#endif // NIXL_SRC_UTILS_OBJECT_RDMA_CUOBJ_TOKEN_PROVIDER_H
