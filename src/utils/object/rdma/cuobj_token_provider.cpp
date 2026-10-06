/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cuobj_token_provider.h"

#include <string>

#include "common/nixl_log.h"

namespace nixl_obj_rdma {

std::unique_ptr<cuobjTokenProvider>
cuobjTokenProvider::create() {
    SharedCuObjClient *rdma = SharedCuObjClient::instance();
    if (rdma == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<cuobjTokenProvider>(new cuobjTokenProvider(rdma));
}

nixl_status_t
cuobjTokenProvider::registerMemory(void *ptr, size_t len, nixl_mem_t mem, uint64_t dev_id) {
    (void)dev_id; // cuObject finds the device from the pointer
    if (!supportsMem(mem)) {
        NIXL_ERROR << "S3 RDMA (cuobj): memory type " << mem << " is not supported";
        return NIXL_ERR_NOT_SUPPORTED;
    }
    if (len > CUOBJ_MAX_MEMORY_REG_SIZE) {
        NIXL_ERROR << "S3 RDMA (cuobj): " << len << " bytes exceed the cuObject registration limit "
                   << CUOBJ_MAX_MEMORY_REG_SIZE;
        return NIXL_ERR_INVALID_PARAM;
    }
    return rdma_->registerBuffer(ptr, len) ? NIXL_SUCCESS : NIXL_ERR_BACKEND;
}

nixl_status_t
cuobjTokenProvider::deregisterMemory(void *ptr) {
    rdma_->deregisterBuffer(ptr);
    return NIXL_SUCCESS;
}

std::optional<rdmaToken>
cuobjTokenProvider::makeToken(void *ptr, size_t len, rdma_op op) {
    char *token = rdma_->getToken(ptr, len, 0, op == rdma_op::GET ? CUOBJ_GET : CUOBJ_PUT);
    if (token == nullptr) {
        return std::nullopt;
    }
    // The control plane sends a copy; cuObject keeps the descriptor until it
    // is released, after the response.
    return rdmaToken(std::string(token), [rdma = rdma_, token] { rdma->putToken(token); });
}

} // namespace nixl_obj_rdma
