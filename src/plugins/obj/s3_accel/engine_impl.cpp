/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "engine_impl.h"

#include <algorithm>
#include <memory>
#include <string>

#include "object/rdma/rdma.h"
#include "obj_engine_registry.h"
#include "common/nixl_log.h"

namespace {

// Registered-memory metadata for the accel engine. It records what to clean up
// on deregister: an OBJ_SEG key mapping, or a DRAM/VRAM buffer registered with
// the token provider, which it keeps alive until then.
class accelObjMetadata : public nixlBackendMD {
public:
    accelObjMetadata(nixl_mem_t nixl_mem, uint64_t dev_id, std::string obj_key)
        : nixlBackendMD(true),
          nixlMem(nixl_mem),
          devId(dev_id),
          objKey(std::move(obj_key)) {}

    accelObjMetadata(nixl_mem_t nixl_mem,
                     uintptr_t addr,
                     std::shared_ptr<nixl_obj_rdma::iRdmaTokenProvider> tokens)
        : nixlBackendMD(true),
          nixlMem(nixl_mem),
          localAddr(addr),
          rdmaTokens(std::move(tokens)) {}

    ~accelObjMetadata() = default;

    nixl_mem_t nixlMem;
    uint64_t devId = 0;
    std::string objKey;
    uintptr_t localAddr = 0;
    std::shared_ptr<nixl_obj_rdma::iRdmaTokenProvider> rdmaTokens;
};

// Register the standard-S3 engine under "s3"; obj_backend normalizes a missing
// `type` to "s3", so `accelerated=true` with no type resolves here too.
objAccelEngineRegistrar reg_s3_accel(
    "s3",
    [](const nixlBackendInitParams *p) { return std::make_unique<S3AccelObjEngineImpl>(p); },
    [](const nixlBackendInitParams *p, std::shared_ptr<iS3Client> s3, std::shared_ptr<iS3Client>) {
        return std::make_unique<S3AccelObjEngineImpl>(p, std::move(s3));
    });

} // namespace

S3AccelObjEngineImpl::S3AccelObjEngineImpl(const nixlBackendInitParams *init_params)
    : DefaultObjEngineImpl(init_params) {
    s3Client_ = std::make_shared<awsS3AccelClient>(init_params->customParams, executor_);
    NIXL_INFO << "Object storage backend initialized with S3 Accel client";
}

S3AccelObjEngineImpl::S3AccelObjEngineImpl(const nixlBackendInitParams *init_params,
                                           std::shared_ptr<iS3Client> s3_client)
    : DefaultObjEngineImpl(init_params, s3_client, nullptr) {
    if (!s3_client) {
        s3Client_ = std::make_shared<awsS3AccelClient>(init_params->customParams, executor_);
    }
    NIXL_INFO << "Object storage backend initialized with S3 Accel client (injected)";
}

iS3Client *
S3AccelObjEngineImpl::getClient() const {
    return s3Client_.get();
}

std::shared_ptr<nixl_obj_rdma::iRdmaTokenProvider>
S3AccelObjEngineImpl::rdmaTokens() const {
    const auto *client = dynamic_cast<const awsS3AccelClient *>(s3Client_.get());
    if (client == nullptr || !client->supportsRdma()) {
        return nullptr;
    }
    return client->tokenProvider();
}

bool
S3AccelObjEngineImpl::rdmaEngReady() const {
    return rdmaTokens() != nullptr;
}

nixl_mem_list_t
S3AccelObjEngineImpl::getSupportedMems() const {
    // VRAM_SEG (GPU-direct) is advertised only when the RDMA fast path is ready
    // and its token provider can register GPU memory, so a GPU pointer can
    // never reach the HTTP path.
    const auto tokens = rdmaTokens();
    if (tokens && tokens->supportsMem(VRAM_SEG)) {
        return {DRAM_SEG, OBJ_SEG, VRAM_SEG};
    }
    return {DRAM_SEG, OBJ_SEG};
}

nixl_status_t
S3AccelObjEngineImpl::registerMem(const nixlBlobDesc &mem,
                                  const nixl_mem_t &nixl_mem,
                                  nixlBackendMD *&out) {
    const nixl_mem_list_t supported_mems = getSupportedMems();
    if (std::find(supported_mems.begin(), supported_mems.end(), nixl_mem) == supported_mems.end()) {
        return NIXL_ERR_NOT_SUPPORTED;
    }

    if (nixl_mem == OBJ_SEG) {
        auto obj_md = std::make_unique<accelObjMetadata>(
            nixl_mem, mem.devId, mem.metaInfo.empty() ? std::to_string(mem.devId) : mem.metaInfo);
        devIdToObjKey_[mem.devId] = obj_md->objKey;
        out = obj_md.release();
        return NIXL_SUCCESS;
    }

    // DRAM_SEG / VRAM_SEG: register the buffer with the token provider so the
    // client can mint tokens for it. The accelerated path is RDMA-only, so when
    // the fast path is ready a registration failure is a hard error rather than
    // a silent success that fails later at transfer time.
    if (auto tokens = rdmaTokens()) {
        const nixl_status_t status = tokens->registerMemory(
            reinterpret_cast<void *>(mem.addr), mem.len, nixl_mem, mem.devId);
        if (status != NIXL_SUCCESS) {
            NIXL_ERROR << "RDMA buffer registration (" << tokens->name()
                       << ") failed; the accelerated path has no HTTP fallback";
            return status;
        }
        out = std::make_unique<accelObjMetadata>(nixl_mem, mem.addr, std::move(tokens)).release();
        return NIXL_SUCCESS;
    }

    // Not RDMA-ready (e.g. an injected non-accel client in tests): VRAM is never
    // advertised, so this is DRAM, which needs no pinning.
    out = nullptr;
    return NIXL_SUCCESS;
}

nixl_status_t
S3AccelObjEngineImpl::deregisterMem(nixlBackendMD *meta) {
    auto *obj_md = static_cast<accelObjMetadata *>(meta);
    if (obj_md) {
        std::unique_ptr<accelObjMetadata> obj_md_ptr(obj_md);
        if (obj_md->nixlMem == OBJ_SEG) {
            devIdToObjKey_.erase(obj_md->devId);
        } else if (obj_md->rdmaTokens) {
            obj_md->rdmaTokens->deregisterMemory(reinterpret_cast<void *>(obj_md->localAddr));
        }
    }
    return NIXL_SUCCESS;
}
