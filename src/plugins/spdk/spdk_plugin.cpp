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

#include "nixl_types.h"
#include "spdk_backend.h"
#include "backend/backend_plugin.h"

// Plugin type alias for convenience.
using spdk_plugin_t = nixlBackendPluginCreator<nixlSpdkEngine>;

// Local host DRAM source; remote is an OBJ-style key-addressed KV blob (OBJ_SEG)
// or an NVMe block LBA range (BLK_SEG). The op-set is chosen by the remote type.
static const nixl_mem_list_t supported_segments = {DRAM_SEG, OBJ_SEG, BLK_SEG};

#ifdef STATIC_PLUGIN_SPDK
nixlBackendPlugin *
createStaticSPDKPlugin() {
    return spdk_plugin_t::create(
        NIXL_PLUGIN_API_VERSION, "SPDK", "0.1.0", {}, supported_segments);
}
#else
extern "C" NIXL_PLUGIN_EXPORT nixlBackendPlugin *
nixl_plugin_init() {
    return spdk_plugin_t::create(
        NIXL_PLUGIN_API_VERSION, "SPDK", "0.1.0", {}, supported_segments);
}

extern "C" NIXL_PLUGIN_EXPORT void
nixl_plugin_fini() {}
#endif
