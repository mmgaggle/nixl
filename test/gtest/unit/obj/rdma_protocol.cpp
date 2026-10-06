/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

// Unit tests for the dependency-free S3-over-RDMA wire-protocol helpers. These
// exercise the parts of the RDMA path that do not require the AWS SDK or the
// cuObjClient library, so they run on any build.

#include <gtest/gtest.h>

#include "object/rdma/rdma_protocol.h"

namespace {

using namespace nixl_obj_rdma;

TEST(RdmaProtocol, ParseReplyRejectsOutOfRangeCodes) {
    // Values outside the HTTP status range are malformed, so a negative reply
    // cannot alias rdma_not_supported (-2) nor a huge value look like success.
    EXPECT_EQ(parseRdmaReply("-2"), 0);
    EXPECT_EQ(parseRdmaReply("99"), 0);
    EXPECT_EQ(parseRdmaReply("600"), 0);
}

TEST(RdmaProtocol, ParseReplySuccessCodes) {
    EXPECT_EQ(parseRdmaReply("200"), rdma_reply_success);
    EXPECT_EQ(parseRdmaReply("204"), rdma_reply_no_content);
    EXPECT_EQ(parseRdmaReply("206"), rdma_reply_partial_content);
}

TEST(RdmaProtocol, ParseReplyDeclinedAndAbsentMapToNotSupported) {
    // Explicit decline.
    EXPECT_EQ(parseRdmaReply("501"), static_cast<int>(rdma_not_supported));
    // An absent header (a non-RDMA server never sets it) reads as "declined".
    // This drives GET decline detection; PUT success is decided separately by
    // HTTP 200 + ETag, not by this header.
    EXPECT_EQ(parseRdmaReply(""), static_cast<int>(rdma_not_supported));
    // Garbage parses to 0 (caller treats as failure).
    EXPECT_EQ(parseRdmaReply("not-a-number"), 0);
}

TEST(RdmaProtocol, ParseReplyRejectsTrailingJunk) {
    // A reply with trailing junk must NOT be accepted as a success code: std::stoi
    // would return 200 for "200xyz" and mask a malformed reply as RDMA success.
    EXPECT_EQ(parseRdmaReply("200xyz"), 0);
    EXPECT_EQ(parseRdmaReply("200 "), 0);
    EXPECT_EQ(parseRdmaReply("2 06"), 0);
    EXPECT_EQ(parseRdmaReply("0x200"), 0);
    // "501" with trailing junk is not the exact decline token, so it is malformed.
    EXPECT_EQ(parseRdmaReply("501x"), 0);
}

TEST(RdmaProtocol, ParseReplyRejectsLeadingWhitespaceAndSign) {
    // from_chars does not skip leading whitespace or accept a leading '+'.
    EXPECT_EQ(parseRdmaReply(" 200"), 0);
    EXPECT_EQ(parseRdmaReply("+200"), 0);
    EXPECT_EQ(parseRdmaReply("\t206"), 0);
}

TEST(RdmaProtocol, ProtocolConstantsHaveExpectedValues) {
    // Guard the wire contract: header names and status codes must not drift.
    EXPECT_STREQ(amz_rdma_token, "x-amz-rdma-token");
    EXPECT_STREQ(amz_rdma_reply, "x-amz-rdma-reply");
    EXPECT_STREQ(amz_rdma_bytes_transferred, "x-amz-rdma-bytes-transferred");
    EXPECT_STREQ(unsigned_payload, "UNSIGNED-PAYLOAD");
    EXPECT_EQ(rdma_reply_success, 200);
    EXPECT_EQ(rdma_reply_no_content, 204);
    EXPECT_EQ(rdma_reply_partial_content, 206);
    EXPECT_EQ(rdma_reply_not_implemented, 501);
    EXPECT_EQ(rdma_not_supported, -2);
    EXPECT_EQ(rdma_error, -1);
}

// The GET outcome, row by row: status, reply, byte count, requested, body kept.
TEST(RdmaProtocol, ClassifyGetReply) {
    struct row {
        int status;
        const char *reply;
        const char *bytes;
        uint64_t requested;
        std::optional<uint64_t> body;
        get_outcome outcome;
        uint64_t delivered;
    };

    const row rows[] = {
        // out of band: the status and the reply agree
        {200, "200", "4096", 4096, std::nullopt, get_outcome::rdma, 4096},
        {206, "206", "100", 100, std::nullopt, get_outcome::rdma, 100},
        {206, "206", "", 100, std::nullopt, get_outcome::rdma, 0}, // an empty range
        {206, "206", "80", 100, std::nullopt, get_outcome::rdma, 80}, // clamped by the server
        // they disagree: Ceph before it answered a range with 206
        {206, "200", "100", 100, std::nullopt, get_outcome::error, 0},
        {200, "206", "100", 100, std::nullopt, get_outcome::error, 0},
        // a byte count that is malformed or larger than the request
        {200, "200", "4097", 4096, std::nullopt, get_outcome::error, 0},
        {200, "200", "12x", 4096, std::nullopt, get_outcome::error, 0},
        // a decline, without and with the body kept
        {206, "501", "", 100, std::nullopt, get_outcome::declined, 0},
        {206, "", "", 100, std::nullopt, get_outcome::declined, 0}, // a server without RDMA
        {206, "501", "", 100, 100, get_outcome::http_body, 100},
        {200, "", "", 100, 64, get_outcome::http_body, 64},
        // an error status is an error, whatever the reply says
        {403, "", "", 100, 100, get_outcome::error, 0},
        {500, "501", "", 100, std::nullopt, get_outcome::error, 0},
        {404, "200", "100", 100, std::nullopt, get_outcome::error, 0},
        // a malformed reply
        {200, "2OO", "100", 100, std::nullopt, get_outcome::error, 0},
    };
    for (const row &r : rows) {
        const getReplyClass c = classifyGetReply(r.status, r.reply, r.bytes, r.requested, r.body);
        EXPECT_EQ(c.outcome, r.outcome)
            << r.status << " / '" << r.reply << "' / '" << r.bytes << "'";
        EXPECT_EQ(c.bytes, r.delivered)
            << r.status << " / '" << r.reply << "' / '" << r.bytes << "'";
    }
}

} // namespace
