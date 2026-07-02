/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SPDK_KV_KEY_H
#define SPDK_KV_KEY_H

#include <cstdint>
#include <string>
#include <vector>

// Map a NIXL block identifier (the OBJ_SEG descriptor's metaInfo blob) to a
// ratified NVMe-KV inline key.
//
// The key is treated as OPAQUE bytes and taken VERBATIM -- no hashing, no
// lineage/position parsing, no backend-specific behavior. Whatever key format
// the caller uses (a flat content hash, a Dynamo-style lineage hash, ...) is
// the caller's concern; this generic plugin only requires that the bytes fit
// the ratified 1..16-byte inline key window.
//
// Validation (no silent mutation):
//   - empty block_id            -> false (no key; caller rejects the descriptor)
//   - max_key_len == 0          -> false
//   - block_id.size() > effective limit min(16, max_key_len) -> false (REJECT;
//     never truncate, since truncation would alias distinct keys that share a
//     prefix and silently corrupt data)
//   - otherwise                 -> true, out = the block_id bytes verbatim
//
// This is intentionally free of any SPDK dependency so it can be unit-tested
// without a KV target.
bool
spdkKvKeyFromBlobId(const std::string &block_id, uint8_t max_key_len, std::vector<uint8_t> &out);

#endif // SPDK_KV_KEY_H
