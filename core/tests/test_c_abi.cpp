// SPDX-License-Identifier: AGPL-3.0-or-later
// C ABI mechanics: structs, frames, failure diagnostics (no codec needed).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>

#include "rkvc/rkvc.h"

TEST_CASE("c abi status strings") {
    CHECK(strcmp(rkvc_status_str(RKVC_OK), "ok") == 0);
    CHECK(strcmp(rkvc_status_str(RKVC_EOF), "end of stream") == 0);
    CHECK(strcmp(rkvc_status_str(RKVC_AGAIN), "try again") == 0);
}

TEST_CASE("c abi context lifecycle") {
    rkvc_context_options opts;
    rkvc_context_options_init(&opts, sizeof(opts));
    CHECK(opts.version == RKVC_ABI_VERSION);
    rkvc_context* ctx = nullptr;
    CHECK(rkvc_context_create(&opts, &ctx) == RKVC_OK);
    CHECK(ctx != nullptr);
    rkvc_caps caps;
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    caps.version = RKVC_ABI_VERSION;
    CHECK(rkvc_probe_device(ctx, &caps) == RKVC_OK);
    rkvc_context_destroy(ctx);
    CHECK(rkvc_context_create(nullptr, &ctx) == RKVC_OK);
    rkvc_context_destroy(ctx);
}

TEST_CASE("c abi session without codec reports not found") {
    rkvc_context* ctx = nullptr;
    CHECK(rkvc_context_create(nullptr, &ctx) == RKVC_OK);
    rkvc_session_request req;
    rkvc_session_request_init(&req, sizeof(req));
    CHECK(req.quality.qp == -1);
    req.operation = RKVC_OP_ENCODE;
    req.codec = RKVC_CODEC_AV1;
    req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.input.fmt = RKVC_FRAME_FMT_NV12;
    req.input.width = 64;
    req.input.height = 64;
    req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    rkvc_session* s = nullptr;
    rkvc_diagnostic* diag = nullptr;
    CHECK(rkvc_session_create(ctx, &req, &s, &diag) == RKVC_NOT_FOUND);
    CHECK(s == nullptr);
    CHECK(diag != nullptr);
    char buf[1024];
    rkvc_diag_fmt_text(diag, buf, sizeof(buf));
    CHECK(strlen(buf) > 0);
    CHECK(strstr(buf, "no candidate") != nullptr);
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
}

TEST_CASE("c abi frame wrap roundtrip") {
    rkvc_frame_desc desc;
    rkvc_frame_desc_init(&desc, sizeof(desc));
    CHECK(desc.fd == -1);
    CHECK(desc.pts == RKVC_FRAME_TS_UNKNOWN);
    desc.spec.width = 16;
    desc.spec.height = 16;
    desc.spec.fmt = RKVC_FRAME_FMT_NV12;
    desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    static uint8_t payload[16 * 16 * 3 / 2];
    memset(payload, 0x42, sizeof(payload));
    desc.data = payload;
    desc.size = sizeof(payload);
    desc.pts = 9000;
    rkvc_frame* f = nullptr;
    CHECK(rkvc_frame_wrap(&desc, &f) == RKVC_OK);
    rkvc_frame_desc back;
    rkvc_frame_desc_init(&back, sizeof(back));
    CHECK(rkvc_frame_get_desc(f, &back) == RKVC_OK);
    CHECK(back.spec.width == 16);
    CHECK(back.data == payload);
    CHECK(back.pts == 9000);
    rkvc_frame_release(f);
    // Undersized payload is rejected.
    desc.size = 10;
    CHECK(rkvc_frame_wrap(&desc, &f) == RKVC_INVALID);
    CHECK(f == nullptr);
}
