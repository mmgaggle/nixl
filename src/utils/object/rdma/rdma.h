/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NIXL_SRC_UTILS_OBJECT_RDMA_RDMA_H
#define NIXL_SRC_UTILS_OBJECT_RDMA_RDMA_H

// Generic S3-over-RDMA data path for the object backend.
//
// RDMA is NOT a separate engine or a vendor plugin. It is an optimization of
// the normal S3 GET/PUT path on the standard client, enabled per backend via
// `accelerated=true` (generic S3-over-RDMA): the client issues an out-of-band
// RDMA transfer over the published `x-amz-rdma-*` protocol. Under
// `accelerated=true` an RDMA decline/failure is a hard
// error — there is no silent HTTP fallback, because a server that ignores
// the token (instead of returning `x-amz-rdma-reply: 501`) would accept a
// body-less PUT as a 0-byte object (see s3/client.cpp). A GET into host
// memory can opt in to taking the body of a declined GET
// (`rdma_http_fallback=true`), since a GET cannot store anything. The protocol is an AWS
// S3 convention (not vendor-specific), so the same code works against any
// compliant S3 endpoint that adopts it (including AWS S3 itself).
//
// This is the public umbrella header for the S3-over-RDMA utilities. It composes
// units with a single responsibility each:
//   - rdma_protocol.h         pure wire-protocol helpers (no AWS/transport deps)
//   - token_provider.h        the token interface: registration + minting
//   - cuobj_token_provider.h  cuObject DC descriptors (needs cuObjClient)
//   - ofi_token_provider.h    libfabric ofi1 tokens (needs ofi-rma)
//   - control_plane.h         the control-plane interface (no AWS deps)
//   - s3_control_plane_http.h the signed control-plane GET/PUT over the AWS SDK
//   - rdmaRetry.h             token-lifecycle + one-transient-retry wrappers
// A token provider is built only when its library is present: cuObject sets
// HAVE_CUOBJ_CLIENT, ofi-rma sets HAVE_OFI_RMA. The rest is built when either
// is present.

#include "rdma_protocol.h"
#include "token_provider.h"
#include "control_plane.h"
#include "s3_control_plane_http.h"
#include "rdmaRetry.h"
#ifdef HAVE_CUOBJ_CLIENT
#include "cuobj_client.h"
#include "cuobj_token_provider.h"
#endif
#ifdef HAVE_OFI_RMA
#include "ofi_token_provider.h"
#endif

#endif // NIXL_SRC_UTILS_OBJECT_RDMA_RDMA_H
