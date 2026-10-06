# SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

# Token providers for the S3-over-RDMA data path. They need no AWS SDK, so
# they build and test on their own. The libfabric provider (ofi1 tokens)
# needs ofi-rma; ofi_hmem=auto picks the GPU memory type of this build.
if ofi_rma_dep.found()
    ofi_hmem_default = 'none'
    if not is_disabler(rocm_dep)
        if rocm_dep.found()
            ofi_hmem_default = 'rocr'
        endif
    endif
    if ofi_hmem_default == 'none' and cuda_dep.found()
        ofi_hmem_default = 'cuda'
    endif
    obj_rdma_token_lib = static_library('obj_rdma_token',
        ['rdma/token_provider.h', 'rdma/ofi_token_provider.h', 'rdma/ofi_token_provider.cpp'],
        include_directories: [nixl_inc_dirs, utils_inc_dirs],
        dependencies: [nixl_common_dep, ofi_rma_dep, dl_dep],
        cpp_args: ['-DNIXL_OFI_HMEM_DEFAULT="' + ofi_hmem_default + '"'],
        pic: true,
        install: false)
    obj_rdma_token_dep = declare_dependency(
        link_with: obj_rdma_token_lib,
        include_directories: [include_directories('.')],
        dependencies: [nixl_common_dep, ofi_rma_dep, dl_dep])
    summary({'ofi1 tokens (OBJ S3-over-RDMA)': 'ofi-rma, ofi_hmem=auto is ' + ofi_hmem_default},
            section: 'Dependencies')
endif

# Check for AWS SDK dependencies for S3 utilities
aws_s3 = dependency('aws-cpp-sdk-s3', static: false, required: false)
aws_s3_crt = dependency('aws-cpp-sdk-s3-crt', static: false, required: false)
aws_core = dependency('aws-cpp-sdk-core', required: false, static: false)

if aws_s3.found() and aws_s3_crt.found() and aws_core.found()
    # By default aws-cpp-sdk sets c++11 compile flag for the whole project
    partial_aws_s3 = aws_s3.partial_dependency(compile_args: false, includes: true, link_args: true, links: true)
    partial_aws_s3_crt = aws_s3_crt.partial_dependency(compile_args: false, includes: true, link_args: true, links: true)
    partial_aws_core = aws_core.partial_dependency(compile_args: false, includes: true, link_args: true, links: true)

    # Shared S3-over-RDMA infrastructure (NVIDIA cuObject / proposed aws-c-s3
    # protocol), split by responsibility: token providers (buffer registration +
    # token minting: cuobj_token_provider over cuobj_client, and the libfabric
    # ofi_token_provider above), s3_control_plane_http (the signed control-plane
    # GET/PUT), and rdmaRetry (token-lifecycle wrappers). The control plane and
    # the retry wrappers are built when either token provider is; each provider
    # only when its library is found, so they carry no in-source preprocessor
    # guard. rdma_protocol.h is dependency-free and always built, and rdma.h is
    # the public umbrella. These live here (not in the OBJ plugin) so the accel
    # engines — and any future vendor engine — can compose them.
    obj_utils_sources = ['s3/utils.cpp', 's3/utils.h',
                         'rdma/rdma_protocol.h', 'rdma/token_provider.h', 'rdma/control_plane.h']
    obj_utils_deps = [nixl_common_dep, partial_aws_s3, partial_aws_s3_crt, partial_aws_core]
    if cuobj_dep.found() or ofi_rma_dep.found()
        obj_utils_sources += ['rdma/rdma.h',
                              'rdma/s3_control_plane_http.h', 'rdma/s3_control_plane_http.cpp',
                              'rdma/rdmaRetry.h', 'rdma/rdmaRetry.cpp']
    endif
    if cuobj_dep.found()
        obj_utils_sources += ['rdma/cuobj_client.h', 'rdma/cuobj_client.cpp',
                              'rdma/cuobj_token_provider.h', 'rdma/cuobj_token_provider.cpp']
        obj_utils_deps += [cuobj_dep]
    endif
    obj_utils_link_whole = []
    if ofi_rma_dep.found()
        obj_utils_link_whole += [obj_rdma_token_lib]
        obj_utils_deps += [ofi_rma_dep]
    endif

    # Generic object utilities library containing S3 utilities and other object storage utilities
    obj_utils_lib = library('obj_utils',
               obj_utils_sources,
               include_directories: [nixl_inc_dirs, utils_inc_dirs],
               dependencies: obj_utils_deps,
               link_whole: obj_utils_link_whole,
               install: true)

    # rdma.h is part of the public surface: it pulls in nixl_types.h (nixl_common_dep)
    # and, when enabled, <cuobjclient.h> (cuobj_dep). Forward the same public deps the
    # library was built with so downstream consumers compile against the header.
    obj_utils_interface = declare_dependency(
        link_with: obj_utils_lib,
        include_directories: [include_directories('.'), include_directories('s3')],
        dependencies: obj_utils_deps
    )
else
    warning('AWS SDK dependencies not found, skipping obj_utils library build')
endif

