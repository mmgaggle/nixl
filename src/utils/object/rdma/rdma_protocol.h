/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NIXL_SRC_UTILS_OBJECT_RDMA_RDMA_PROTOCOL_H
#define NIXL_SRC_UTILS_OBJECT_RDMA_RDMA_PROTOCOL_H

// Generic S3-over-RDMA wire protocol helpers.
//
// This header is intentionally free of any AWS SDK or cuObject dependency so the
// protocol logic can be unit-tested on its own. It encodes the published S3 RDMA
// convention (the `x-amz-rdma-*` headers) used by NVIDIA cuObject and implemented
// by any compliant S3 endpoint (transparently usable against AWS S3 if/when it
// adopts the same convention). Nothing here is vendor-specific.

#include <sys/types.h> // ssize_t

#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace nixl_obj_rdma {

// S3 RDMA protocol headers (AWS S3 RDMA spec, compatible with NVIDIA aws-c-s3).
inline constexpr const char *amz_rdma_token = "x-amz-rdma-token";
inline constexpr const char *amz_rdma_reply = "x-amz-rdma-reply";
inline constexpr const char *amz_rdma_bytes_transferred = "x-amz-rdma-bytes-transferred";

// SigV4 payload hash sentinel for body-less RDMA control-plane requests.
inline constexpr const char *unsigned_payload = "UNSIGNED-PAYLOAD";

// RDMA reply status codes (carried in x-amz-rdma-reply, aligned with HTTP codes).
inline constexpr int rdma_reply_success = 200; // transfer completed (PUT/GET)
inline constexpr int rdma_reply_no_content =
    204; // No Content (S3 DELETE semantics); the RDMA GET/PUT data path uses 200/206
inline constexpr int rdma_reply_partial_content = 206; // partial transfer (ranged GET)
inline constexpr int rdma_reply_not_implemented =
    501; // server declined RDMA (under accelerated=true: hard error)

// Return-code sentinels shared by rdmaPut/rdmaGet (negative => caller errors; no
// HTTP fallback). >0 is the number of bytes transferred on success.
inline constexpr ssize_t rdma_not_supported = -2; // server declined RDMA
inline constexpr ssize_t rdma_error = -1; // transport / unexpected failure

// One transient retry: a fresh token mint + control-plane attempt recovers from
// a transient cuObject token-acquisition or transport hiccup. (NIC-aware failover
// is a future enhancement; it requires binding the control-plane socket to the
// token's source NIC, which the AWS SDK HTTP client does not expose today.)
inline constexpr int rdma_max_attempts = 2;

// Aggressive control-plane timeouts (seconds) so a transport stall surfaces fast
// and the retry path can take over.
inline constexpr long rdma_connect_timeout_secs = 5;
inline constexpr long rdma_timeout_secs = 10;

/**
 * @brief Map the server's x-amz-rdma-reply header value to a transfer outcome.
 *
 * This drives both the GET and PUT paths and decline detection. A GET success
 * carries x-amz-rdma-reply: 200/206 and a PUT success carries 200; its absence
 * (a non-RDMA server never sets it) is read as a decline, which stops a server
 * that ignored the token from being mistaken for a completed transfer.
 * @param reply The raw x-amz-rdma-reply header value (may be empty/absent).
 * @return The reply code (200/204/206) on RDMA success; 0 for an unparsable or
 *         out-of-range value (caller treats as failure); rdma_not_supported (-2)
 *         when the reply is "501" or absent/empty (server declined RDMA).
 */
[[nodiscard]] inline int
parseRdmaReply(const std::string &reply) {
    if (reply.empty() || reply == "501") {
        return static_cast<int>(rdma_not_supported);
    }
    // Require the ENTIRE value to be a valid integer. std::stoi would accept
    // trailing junk ("200xyz" -> 200), which could mask a malformed reply as a
    // success code; from_chars rejects it.
    int value = 0;
    const char *begin = reply.data();
    const char *end = begin + reply.size();
    auto [parsed_end, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || parsed_end != end) {
        return 0; // malformed -> caller treats as failure
    }
    // Only HTTP-style status codes are meaningful here; anything outside the
    // range (e.g. a negative value that would alias rdma_not_supported) is
    // treated as malformed so it cannot masquerade as a decline or success.
    if (value < 100 || value > 599) {
        return 0;
    }
    return value;
}

/** @brief How a control-plane GET ended. */
enum class get_outcome {
    rdma, ///< the server wrote the range out of band
    http_body, ///< the server declined RDMA and sent the range in the body
    declined, ///< the server declined RDMA, and the caller kept no body
    error, ///< any other response
};

/** @brief The outcome of a control-plane GET, and the bytes it delivered. */
struct getReplyClass {
    get_outcome outcome = get_outcome::error;
    uint64_t bytes = 0;
};

/**
 * @brief Decide the outcome of a control-plane GET from its response.
 *
 * Out of band, the HTTP status and x-amz-rdma-reply must agree: 200 with 200
 * for a whole object, 206 with 206 for a range. A reply of 501, or none (a
 * server that does not know the token), is a decline. After a decline, a
 * caller that kept the body (rdma_http_fallback) has the data from the body.
 * @param http_status The status code of the response.
 * @param reply x-amz-rdma-reply, empty when the header is absent.
 * @param bytes_transferred x-amz-rdma-bytes-transferred, empty when absent.
 *        The server sends it only for a transfer of more than 0 bytes.
 * @param requested The size of the range the request asked for.
 * @param body_bytes The size of the body the caller kept, or nullopt when it
 *        kept none.
 */
[[nodiscard]] inline getReplyClass
classifyGetReply(int http_status,
                 const std::string &reply,
                 const std::string &bytes_transferred,
                 uint64_t requested,
                 std::optional<uint64_t> body_bytes) {
    const bool ok_status = http_status == 200 || http_status == 206;
    const int code = parseRdmaReply(reply);
    if (code == static_cast<int>(rdma_not_supported)) {
        if (!ok_status) {
            return {get_outcome::error, 0};
        }
        if (body_bytes.has_value() && *body_bytes <= requested) {
            return {get_outcome::http_body, *body_bytes};
        }
        return {get_outcome::declined, 0};
    }
    const bool accepted = (http_status == 200 && code == rdma_reply_success) ||
        (http_status == 206 && code == rdma_reply_partial_content);
    if (!accepted) {
        return {get_outcome::error, 0};
    }
    if (bytes_transferred.empty()) {
        return {get_outcome::rdma, 0};
    }
    uint64_t n = 0;
    const char *begin = bytes_transferred.data();
    const char *end = begin + bytes_transferred.size();
    auto [parsed_end, ec] = std::from_chars(begin, end, n);
    if (ec != std::errc{} || parsed_end != end || n > requested) {
        return {get_outcome::error, 0};
    }
    return {get_outcome::rdma, n};
}

} // namespace nixl_obj_rdma

#endif // NIXL_SRC_UTILS_OBJECT_RDMA_RDMA_PROTOCOL_H
