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

#include "spdk_key.h"

#include <algorithm>

// Ratified NVMe Key-Value inline key window (1..16 bytes).
static constexpr uint8_t kNvmeKvKeyMaxLen = 16;

bool
spdkKvKeyFromBlobId(const std::string &block_id, uint8_t max_key_len, std::vector<uint8_t> &out) {
    if (block_id.empty() || max_key_len == 0) {
        return false;
    }
    const size_t limit = std::min<size_t>(kNvmeKvKeyMaxLen, max_key_len);
    if (block_id.size() > limit) {
        // Reject rather than truncate: a truncated key would alias distinct
        // keys that share a prefix and silently corrupt data.
        return false;
    }
    out.assign(block_id.begin(), block_id.end());
    return true;
}
