/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ofi_token_provider.h"

#include <cerrno>
#include <utility>

#include <dlfcn.h>
#include <unistd.h>

#include <ofi_rma/ofi_rma.h>

#include "common/backend.h"
#include "common/nixl_log.h"

// The GPU memory type that ofi_hmem=auto picks: the runtime NIXL was built
// with. Meson sets it.
#ifndef NIXL_OFI_HMEM_DEFAULT
#define NIXL_OFI_HMEM_DEFAULT "none"
#endif

namespace nixl_obj_rdma {

namespace {

    [[nodiscard]] bool
    isHmemName(const std::string &name) {
        return name == "none" || name == "cuda" || name == "rocr" || name == "ze";
    }

    [[nodiscard]] ofi_rma::memory_t::iface_t
    ifaceOf(const std::string &hmem) {
        if (hmem == "cuda") {
            return ofi_rma::memory_t::iface_t::cuda;
        }
        if (hmem == "rocr") {
            return ofi_rma::memory_t::iface_t::rocr;
        }
        if (hmem == "ze") {
            return ofi_rma::memory_t::iface_t::ze;
        }
        return ofi_rma::memory_t::iface_t::system;
    }

    /** hsa_amd_portable_export_dmabuf(), whose hsa_status_t is 0 on success. */
    using hsaExportDmabufFn = int (*)(const void *ptr, size_t size, int *fd, uint64_t *offset);

    /**
     * ROCr's dma-buf export, from the runtime the process loaded, or nullptr.
     * NIXL does not link ROCr: a process with ROCm buffers has it loaded.
     * RTLD_NOLOAD never loads a second copy of the runtime.
     */
    [[nodiscard]] hsaExportDmabufFn
    hsaExportDmabuf() {
        constexpr const char *sym = "hsa_amd_portable_export_dmabuf";
        if (void *fn = dlsym(RTLD_DEFAULT, sym)) {
            return reinterpret_cast<hsaExportDmabufFn>(fn);
        }
        if (void *lib = dlopen("libhsa-runtime64.so.1", RTLD_NOW | RTLD_NOLOAD)) {
            void *fn = dlsym(lib, sym);
            dlclose(lib);
            return reinterpret_cast<hsaExportDmabufFn>(fn);
        }
        return nullptr;
    }

    /**
     * The endpoint for these settings: the one already open, or a new one.
     * Some providers allow one endpoint per process, so providers with the
     * same settings share it. An endpoint closes when its last provider and
     * its last token are gone.
     */
    [[nodiscard]] std::shared_ptr<ofi_rma::Endpoint>
    sharedEndpoint(const ofiTokenConfig &cfg, std::string *err) {
        static std::mutex mutex;
        static std::map<std::string, std::weak_ptr<ofi_rma::Endpoint>> endpoints;

        const std::string key = cfg.provider + '\n' + cfg.domain + '\n' + cfg.node + '\n' +
            cfg.service + '\n' + cfg.hmem;
        const std::lock_guard<std::mutex> lock(mutex);
        if (auto ep = endpoints[key].lock()) {
            return ep;
        }

        ofi_rma::config_t c;
        c.provider = cfg.provider;
        c.domain = cfg.domain;
        c.node = cfg.node;
        c.service = cfg.service;
        // Writes land only while the endpoint is polled on providers that
        // progress manually, and the client waits in an HTTP request meanwhile.
        c.progress_thread = true;
        c.hmem = cfg.hmem != "none";
        std::unique_ptr<ofi_rma::Endpoint> opened = ofi_rma::Endpoint::open(c, err);
        if (!opened) {
            return nullptr;
        }
        std::shared_ptr<ofi_rma::Endpoint> ep(std::move(opened));
        endpoints[key] = ep;
        return ep;
    }

} // namespace

std::optional<ofiTokenConfig>
ofiTokenConfig::fromParams(const nixl_b_params_t *params, std::string *err) {
    ofiTokenConfig cfg;
    cfg.provider = nixl::getBackendParamDefaulted(params, "ofi_provider", std::string());
    if (cfg.provider.empty()) {
        *err = "ofi_provider is required: the libfabric provider the OSDs run (osd_ofi_provider)";
        return std::nullopt;
    }
    cfg.domain = nixl::getBackendParamDefaulted(params, "ofi_domain", std::string());
    cfg.node = nixl::getBackendParamDefaulted(params, "ofi_node", std::string());
    cfg.service = nixl::getBackendParamDefaulted(params, "ofi_service", std::string());
    cfg.hmem = nixl::getBackendParamDefaulted(params, "ofi_hmem", std::string("auto"));
    if (cfg.hmem == "auto") {
        cfg.hmem = NIXL_OFI_HMEM_DEFAULT;
    }
    if (!isHmemName(cfg.hmem)) {
        *err = "ofi_hmem='" + cfg.hmem + "' is not one of none, cuda, rocr, ze or auto";
        return std::nullopt;
    }
    return cfg;
}

std::unique_ptr<ofiTokenProvider>
ofiTokenProvider::create(const ofiTokenConfig &cfg, std::string *err) {
    std::shared_ptr<ofi_rma::Endpoint> ep = sharedEndpoint(cfg, err);
    if (!ep) {
        return nullptr;
    }
    if (cfg.hmem != "none" && !ep->hmem()) {
        NIXL_WARN << "S3 RDMA (ofi): provider " << ep->provider()
                  << " offers no FI_HMEM, so VRAM_SEG is not supported";
    }
    NIXL_INFO << "S3 RDMA (ofi): " << ep->describe();
    return std::unique_ptr<ofiTokenProvider>(new ofiTokenProvider(std::move(ep), cfg));
}

ofiTokenProvider::ofiTokenProvider(std::shared_ptr<ofi_rma::Endpoint> ep, const ofiTokenConfig &cfg)
    : ep_(std::move(ep)),
      cfg_(cfg) {}

ofiTokenProvider::~ofiTokenProvider() {
    // the endpoint can outlive this provider, shared with another one or held
    // by a token: give back the windows this one lent
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &[start, w] : windows_) {
        releaseLocked(w);
    }
}

void
ofiTokenProvider::releaseLocked(const window &w) {
    ep_->deregister_window(w.id);
    if (w.dmabufFd >= 0) {
        ::close(w.dmabufFd);
    }
}

bool
ofiTokenProvider::isConnected() const {
    return ep_ && !ep_->unsafe();
}

bool
ofiTokenProvider::supportsMem(nixl_mem_t mem) const {
    if (mem == DRAM_SEG) {
        return true;
    }
    return mem == VRAM_SEG && cfg_.hmem != "none" && ep_->hmem();
}

nixl_status_t
ofiTokenProvider::registerMemory(void *ptr, size_t len, nixl_mem_t mem, uint64_t dev_id) {
    if (ptr == nullptr || len == 0) {
        NIXL_ERROR << "S3 RDMA (ofi): cannot register an empty buffer";
        return NIXL_ERR_INVALID_PARAM;
    }
    if (!supportsMem(mem)) {
        if (mem == VRAM_SEG && cfg_.hmem == "none") {
            NIXL_ERROR << "S3 RDMA (ofi): VRAM_SEG needs ofi_hmem set to cuda, rocr or ze";
        } else if (mem == VRAM_SEG) {
            NIXL_ERROR << "S3 RDMA (ofi): VRAM_SEG needs FI_HMEM, and provider " << ep_->provider()
                       << " offers none";
        } else {
            NIXL_ERROR << "S3 RDMA (ofi): memory type " << mem << " is not supported";
        }
        return NIXL_ERR_NOT_SUPPORTED;
    }
    ofi_rma::memory_t m;
    if (mem == VRAM_SEG) {
        m.iface = ifaceOf(cfg_.hmem);
        m.device = static_cast<int>(dev_id);
    }

    const auto start = reinterpret_cast<uintptr_t>(ptr);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (windows_.count(start) != 0) {
        // deregisterMemory() names a window by its start, so two at one
        // address could not be told apart
        NIXL_ERROR << "S3 RDMA (ofi): a buffer at " << ptr << " is already registered";
        return NIXL_ERR_NOT_ALLOWED;
    }
    ofi_rma::Endpoint::window_t w;
    // ROCm memory: first as a dma-buf that ROCr exports, then through
    // libfabric's own FI_HMEM path.
    if (mem == VRAM_SEG && m.iface == ofi_rma::memory_t::iface_t::rocr) {
        if (const hsaExportDmabufFn export_dmabuf = hsaExportDmabuf()) {
            int fd = -1;
            uint64_t fd_offset = 0;
            if (export_dmabuf(ptr, len, &fd, &fd_offset) == 0 && fd >= 0) {
                ofi_rma::memory_t dmabuf = m;
                dmabuf.dmabuf_fd = fd;
                dmabuf.dmabuf_offset = fd_offset;
                if (ep_->register_window(static_cast<char *>(ptr), len, dmabuf, &w) == 0) {
                    windows_[start] = window{w.id, w.ptr, w.len, mem, fd};
                    return NIXL_SUCCESS;
                }
                NIXL_DEBUG << "S3 RDMA (ofi): dma-buf registration of " << ptr
                           << " failed, trying FI_HMEM: " << ep_->last_error();
                ::close(fd);
            }
        }
    }
    if (const int r = ep_->register_window(static_cast<char *>(ptr), len, m, &w); r < 0) {
        NIXL_ERROR << "S3 RDMA (ofi): registering " << len << " bytes at " << ptr
                   << " failed: " << ep_->last_error();
        // -ENOSYS: the provider offers FI_HMEM, but this libfabric lacks the
        // GPU runtime for the memory, as some distribution packages do
        return (r == -EOPNOTSUPP || r == -ENOSYS) ? NIXL_ERR_NOT_SUPPORTED : NIXL_ERR_BACKEND;
    }
    windows_[start] = window{w.id, w.ptr, w.len, mem, -1};
    return NIXL_SUCCESS;
}

nixl_status_t
ofiTokenProvider::deregisterMemory(void *ptr) {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto it = windows_.find(reinterpret_cast<uintptr_t>(ptr));
    if (it == windows_.end()) {
        NIXL_ERROR << "S3 RDMA (ofi): no buffer is registered at " << ptr;
        return NIXL_ERR_NOT_FOUND;
    }
    releaseLocked(it->second);
    windows_.erase(it);
    return NIXL_SUCCESS;
}

const ofiTokenProvider::window *
ofiTokenProvider::findLocked(uintptr_t addr, size_t len) const {
    // The window that holds the whole range: the nearest one starting at or
    // before it, or, when windows overlap, an earlier one.
    for (auto it = windows_.upper_bound(addr); it != windows_.begin();) {
        --it;
        const uint64_t ofs = addr - it->first;
        if (ofs <= it->second.len && len <= it->second.len - ofs) {
            return &it->second;
        }
    }
    return nullptr;
}

std::optional<nixl_mem_t>
ofiTokenProvider::memTypeAt(const void *ptr, size_t len) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const window *w = findLocked(reinterpret_cast<uintptr_t>(ptr), len);
    if (w == nullptr) {
        return std::nullopt;
    }
    return w->mem;
}

std::optional<rdmaToken>
ofiTokenProvider::makeToken(void *ptr, size_t len, rdma_op op) {
    if (op != rdma_op::GET) {
        NIXL_ERROR << "S3 RDMA (ofi): libfabric tokens serve GET only; a PUT sends its payload "
                      "over HTTP";
        return std::nullopt;
    }
    if (ptr == nullptr || len == 0) {
        NIXL_ERROR << "S3 RDMA (ofi): no token for an empty range";
        return std::nullopt;
    }

    const auto addr = reinterpret_cast<uintptr_t>(ptr);
    const std::lock_guard<std::mutex> lock(mutex_);
    const window *found = findLocked(addr, len);
    if (found == nullptr) {
        NIXL_ERROR << "S3 RDMA (ofi): no registered buffer holds " << len << " bytes at " << ptr;
        return std::nullopt;
    }

    const ofi_rma::Endpoint::window_t w{found->id, found->ptr, found->len};
    std::string token = ep_->window_token(w, addr - reinterpret_cast<uintptr_t>(found->ptr), len);
    if (token.empty()) {
        NIXL_ERROR << "S3 RDMA (ofi): the endpoint gave no token: " << ep_->last_error();
        return std::nullopt;
    }
    // Releasing the token orders the caller's reads after the writes that the
    // progress thread placed; holding the endpoint keeps it open meanwhile.
    return rdmaToken(std::move(token), [ep = ep_] { ep->sync(); });
}

std::string
ofiTokenProvider::describe() const {
    return ep_->describe();
}

} // namespace nixl_obj_rdma
