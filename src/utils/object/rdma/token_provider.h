/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NIXL_SRC_UTILS_OBJECT_RDMA_TOKEN_PROVIDER_H
#define NIXL_SRC_UTILS_OBJECT_RDMA_TOKEN_PROVIDER_H

// The source of the x-amz-rdma-token on the S3-over-RDMA data path.
//
// A token provider registers the buffers that a server writes into (GET) or
// reads from (PUT), and mints a token for a range of one of them. The control
// plane sends the token verbatim; only the provider knows its format. Two
// token types exist: cuObject DC descriptors and libfabric ofi1 tokens. Every
// token starts with the same "<addr hex>:<size hex>" fields, so a server can
// read the window size without knowing the type.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "nixl_types.h"

namespace nixl_obj_rdma {

/** @brief What a token lets the server do with the buffer. */
enum class rdma_op {
    GET, ///< the server writes object data into the buffer
    PUT, ///< the server reads object data from the buffer
};

/**
 * @brief One minted token, alive for one control-plane request.
 *
 * Destroying the token, or calling reset(), releases what the provider holds
 * for the request. For cuObject that is the token itself. For libfabric it
 * orders the caller's reads of the buffer after the writes that the server
 * placed. So destroy the token after the response arrives, and before the
 * transfer is reported complete.
 */
class rdmaToken {
public:
    /**
     * @param value The token, sent verbatim as x-amz-rdma-token.
     * @param release Runs once, when the token is destroyed or reset.
     */
    explicit rdmaToken(std::string value, std::function<void()> release = {})
        : value_(std::move(value)),
          release_(std::move(release)) {}

    ~rdmaToken() {
        reset();
    }

    rdmaToken(const rdmaToken &) = delete;
    rdmaToken &
    operator=(const rdmaToken &) = delete;

    rdmaToken(rdmaToken &&other) noexcept
        : value_(std::move(other.value_)),
          release_(std::exchange(other.release_, {})) {}

    rdmaToken &
    operator=(rdmaToken &&other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::move(other.value_);
            release_ = std::exchange(other.release_, {});
        }
        return *this;
    }

    /** @return The token, as sent in x-amz-rdma-token. */
    [[nodiscard]] const std::string &
    value() const {
        return value_;
    }

    /** @brief Release what the provider holds for the request, now. */
    void
    reset() {
        if (release_) {
            std::exchange(release_, {})();
        }
    }

private:
    std::string value_;
    std::function<void()> release_;
};

/**
 * @brief Registers buffers and mints tokens for one token type.
 *
 * Implementations are thread-safe: transfers mint tokens concurrently with
 * each other and with registration.
 */
class iRdmaTokenProvider {
public:
    virtual ~iRdmaTokenProvider() = default;

    /** @return The transport name, as Ceph's rgw_rdma_transports lists it: "cuobj" or "ofi". */
    [[nodiscard]] virtual std::string_view
    name() const = 0;

    /** @return Whether the transport is ready to register memory. */
    [[nodiscard]] virtual bool
    isConnected() const = 0;

    /** @return Whether a buffer of this memory type can be registered. */
    [[nodiscard]] virtual bool
    supportsMem(nixl_mem_t mem) const = 0;

    /**
     * @return Whether the provider mints PUT tokens. Ceph RGW reads the PUT
     *         payload over HTTP for a libfabric token, so "ofi" mints none.
     */
    [[nodiscard]] virtual bool
    supportsPut() const = 0;

    /**
     * @brief Register [ptr, ptr + len) for remote access.
     * @param mem DRAM_SEG or VRAM_SEG.
     * @param dev_id The device ordinal of a VRAM buffer; ignored for DRAM.
     * @return NIXL_SUCCESS, NIXL_ERR_NOT_SUPPORTED for a memory type the
     *         provider cannot register, or another error.
     */
    [[nodiscard]] virtual nixl_status_t
    registerMemory(void *ptr, size_t len, nixl_mem_t mem, uint64_t dev_id) = 0;

    /** @brief Release the registration that registerMemory() made at ptr. */
    virtual nixl_status_t
    deregisterMemory(void *ptr) = 0;

    /**
     * @brief Mint a token for [ptr, ptr + len).
     *
     * The range must lie within one registration. The token's address field
     * names ptr, so the server places byte i of the transfer at ptr + i.
     * @return The token, or nullopt when no registration covers the range,
     *         the operation is not supported, or the transport failed.
     */
    [[nodiscard]] virtual std::optional<rdmaToken>
    makeToken(void *ptr, size_t len, rdma_op op) = 0;

    /**
     * @brief The memory type of the registration that holds [ptr, ptr + len).
     *
     * A client that sends a PUT payload over HTTP, because the provider
     * mints no PUT tokens, uses it to keep a GPU buffer off that path.
     * @return DRAM_SEG or VRAM_SEG, or nullopt when no registration holds
     *         the range or the provider does not track memory types.
     */
    [[nodiscard]] virtual std::optional<nixl_mem_t>
    memTypeAt(const void *ptr, size_t len) const {
        (void)ptr;
        (void)len;
        return std::nullopt;
    }
};

} // namespace nixl_obj_rdma

#endif // NIXL_SRC_UTILS_OBJECT_RDMA_TOKEN_PROVIDER_H
