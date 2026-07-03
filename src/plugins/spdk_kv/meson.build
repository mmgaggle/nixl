# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# ---------------------------------------------------------------------------
# SPDK plugin: a clean-sheet, transport-agnostic, backend-agnostic NIXL
# backend that speaks the ratified NVMe Key-Value command set over the SPDK
# NVMe driver (lib/nvme). WRITE -> KV Store, READ -> KV Retrieve. It ships its
# OWN generic host shim (spdk_kv_shim.{h,c}); it does NOT depend on any external
# host shim, so the -Dspdk_kv_shim_dir option is unused here.
# ---------------------------------------------------------------------------

# Location of the (already-built) SPDK tree we link against.
spdk_root = get_option('spdk_root')

spdk_build_lib = spdk_root / 'build' / 'lib'
spdk_build_inc = spdk_root / 'build' / 'include'
spdk_inc = spdk_root / 'include'
dpdk_lib = spdk_root / 'dpdk' / 'build' / 'lib'
vfu_lib = spdk_root / 'build' / 'libvfio-user' / 'usr' / 'local' / 'lib'

# The plugin is gated on the prebuilt SPDK NVMe library (which carries the KV
# command set) and the KV public header. Skip cleanly when they are absent.
# These checks need no C compiler, so run them before acquiring one: a default
# `meson setup` reaches this file (SPDK is in all_plugins), and on a host with
# no SPDK build the plugin must skip rather than fail configuration.
if not fs.exists(spdk_build_lib / 'libspdk_nvme.a')
    warning('SPDK plugin disabled: SPDK nvme lib not found under ' + spdk_build_lib)
    subdir_done()
endif
if not fs.exists(spdk_inc / 'spdk' / 'nvme_kv.h')
    warning('SPDK plugin disabled: spdk/nvme_kv.h not found under ' + spdk_inc)
    subdir_done()
endif

# The top-level project() declares only C++ (and optionally CUDA); it does NOT
# enable the C language. This plugin compiles its own C shim (spdk_kv_shim.c),
# so enable the host C compiler here. This keeps the SPDK plugin self-contained: it
# does not rely on another plugin having flipped the project languages to include C.
# add_languages is idempotent, so this is a no-op if C was already enabled by a
# co-built plugin or the top-level project. Request it non-fatally and degrade to
# the same clean skip when no C compiler is present, rather than hard-failing the
# whole tree.
if not add_languages('c', native: false, required: false)
    warning('SPDK plugin disabled: no C compiler available for the C shim')
    subdir_done()
endif

cc = meson.get_compiler('c')

# Include dirs: the plugin's own headers, plus the SPDK C headers (needed by
# the C shim and by the C++ TUs that include spdk_kv_shim.h).
spdk_kv_local_inc = include_directories('.')
spdk_kv_spdk_inc = include_directories(spdk_inc, spdk_build_inc, is_system: true)

# Build the C shim (spdk_kv_shim.c) into its own static lib so the C compile
# flags and the C-only SPDK headers stay isolated from the C++ plugin TUs.
spdk_kv_shim_lib = static_library(
    'nixl_spdk_kv_shim',
    ['spdk_kv_shim.c', 'spdk_kv_fence.c'],
    c_args: ['-D_GNU_SOURCE', '-w'],
    include_directories: [spdk_kv_local_inc, spdk_kv_spdk_inc],
    install: false,
)

# SPDK whole-archive set. The ordering and the whole-archive wrapping around the
# SPDK + DPDK static libs is required so the env/transport registration
# constructors are pulled in. (This is the SPDK KV host-test link line.)
spdk_libs = [
    'spdk_sock_posix', 'spdk_nvme', 'spdk_keyring', 'spdk_sock', 'spdk_trace',
    'spdk_rpc', 'spdk_jsonrpc', 'spdk_json', 'spdk_dma', 'spdk_vfio_user',
    'spdk_vmd', 'spdk_util', 'spdk_log',
]

dpdk_libs = [
    'rte_argparse', 'rte_bus_pci', 'rte_cryptodev', 'rte_dmadev', 'rte_eal',
    'rte_ethdev', 'rte_hash', 'rte_kvargs', 'rte_log', 'rte_mbuf',
    'rte_mempool', 'rte_mempool_ring', 'rte_meter', 'rte_net', 'rte_pci',
    'rte_power', 'rte_power_acpi', 'rte_power_amd_pstate', 'rte_power_cppc',
    'rte_power_intel_pstate', 'rte_power_intel_uncore', 'rte_power_kvm_vm',
    'rte_rcu', 'rte_ring', 'rte_telemetry', 'rte_timer', 'rte_vhost',
]

spdk_link_args = ['-L' + spdk_build_lib, '-L' + vfu_lib, '-L' + dpdk_lib]
# Link the SPDK libs by their explicit .a paths (not -l) so we statically link
# them even when the SPDK build dir also ships .so files (in which case -l would
# prefer the .so and leave the binary with an unsatisfiable runtime dependency).
# This matches how the DPDK archives are given explicitly below.
spdk_link_args += ['-Wl,--whole-archive', '-Wl,--no-as-needed']
foreach l : spdk_libs
    spdk_link_args += [spdk_build_lib / ('lib' + l + '.a')]
endforeach
spdk_link_args += ['-Wl,--no-whole-archive']
spdk_link_args += [spdk_build_lib / 'libspdk_env_dpdk.a']
spdk_link_args += ['-Wl,--whole-archive']
foreach l : dpdk_libs
    spdk_link_args += [dpdk_lib / ('lib' + l + '.a')]
endforeach
spdk_link_args += ['-Wl,--no-whole-archive']

# isa-l static archives provide the crc/xor symbols referenced by spdk_util.
foreach isal : [spdk_root / 'isa-l' / '.libs' / 'libisal.a',
                spdk_root / 'isa-l-crypto' / '.libs' / 'libisal_crypto.a']
    if fs.exists(isal)
        spdk_link_args += [isal]
    endif
endforeach

# Tail system deps required by the SPDK/DPDK static archives. fuse3/fuse
# satisfy the CUSE code pulled in by statically linking libspdk_nvme.a.
foreach syslib : ['numa', 'dl', 'rt', 'uuid', 'ssl', 'crypto', 'm', 'lz4',
                  'keyutils', 'aio', 'rados', 'rbd', 'vfio-user', 'json-c',
                  'fuse3', 'fuse']
    dep = cc.find_library(syslib, required: false)
    if dep.found()
        spdk_link_args += ['-l' + syslib]
    endif
endforeach

spdk_kv_sources = [
    'spdk_kv_backend.cpp',
    'spdk_kv_backend.h',
    'spdk_kv_key.cpp',
    'spdk_kv_key.h',
    'spdk_kv_plugin.cpp',
]

plugin_deps = [nixl_infra, nixl_common_dep]
spdk_kv_inc = [nixl_inc_dirs, utils_inc_dirs, spdk_kv_local_inc, spdk_kv_spdk_inc]

if 'SPDK' in static_plugins
    spdk_kv_backend_lib = static_library(
        'SPDK',
        spdk_kv_sources,
        dependencies: plugin_deps,
        link_with: spdk_kv_shim_lib,
        link_args: spdk_link_args,
        include_directories: spdk_kv_inc,
        install: false,
        name_prefix: 'libplugin_')
else
    spdk_kv_backend_lib = shared_library(
        'SPDK',
        spdk_kv_sources,
        dependencies: plugin_deps,
        link_with: spdk_kv_shim_lib,
        link_args: spdk_link_args,
        cpp_args: ['-fPIC'],
        include_directories: spdk_kv_inc,
        install: true,
        name_prefix: 'libplugin_',
        install_dir: plugin_install_dir,
        install_rpath: '$ORIGIN/..')
    if get_option('buildtype') == 'debug'
        run_command('sh', '-c',
            'echo "SPDK=' + spdk_kv_backend_lib.full_path() + '" >> ' + plugin_build_dir + '/pluginlist',
            check: true
        )
    endif
endif

if 'SPDK' in static_plugins
    spdk_kv_backend_interface = declare_dependency(link_whole: spdk_kv_backend_lib)
else
    spdk_kv_backend_interface = declare_dependency(link_with: spdk_kv_backend_lib)
endif

# Direct-engine round-trip tests (link the engine TUs + shim against a live SPDK
# target). Enabled by -Dspdk_kv_build_test=true. The KV test drives the OBJ_SEG
# Store/Retrieve path; the block test drives the BLK_SEG LBA read/write path.
if get_option('spdk_kv_build_test')
    # Pure-logic unit test for the timeout/orphan/quarantine fence. It links only
    # the SPDK-free spdk_kv_fence.c (no lib/nvme, no live target, no hugepages),
    # so it exercises the stale-orphan discard, poison latch, and staging-buffer
    # quarantine accounting standalone.
    executable(
        'spdk_kv_fence_test',
        ['spdk_kv_fence_test.c', 'spdk_kv_fence.c'],
        c_args: ['-D_GNU_SOURCE'],
        include_directories: [spdk_kv_local_inc],
        install: false,
    )
    executable(
        'spdk_kv_roundtrip_test',
        ['spdk_kv_roundtrip_test.cpp', 'spdk_kv_backend.cpp', 'spdk_kv_backend.h',
         'spdk_kv_key.cpp', 'spdk_kv_key.h'],
        dependencies: plugin_deps,
        link_with: spdk_kv_shim_lib,
        link_args: spdk_link_args,
        include_directories: spdk_kv_inc,
        install: false,
    )
    executable(
        'spdk_kv_block_roundtrip_test',
        ['spdk_kv_block_roundtrip_test.cpp', 'spdk_kv_backend.cpp', 'spdk_kv_backend.h',
         'spdk_kv_key.cpp', 'spdk_kv_key.h'],
        dependencies: plugin_deps,
        link_with: spdk_kv_shim_lib,
        link_args: spdk_link_args,
        include_directories: spdk_kv_inc,
        install: false,
    )
endif
