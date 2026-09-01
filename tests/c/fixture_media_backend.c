/* SPDX-License-Identifier: AGPL-3.0-or-later */
/** Test-only DSO used to exercise the installed-shape CLI media path. */

#include "rkvc/backend.h"

#include <stdlib.h>
#include <string.h>

struct fixture_node {
    int encode;
};

static void buffer_free(void *ptr) { free(ptr); }

static int configure(rkvc_node *node, rkvc_diag **diag) {
    rkvc_frame_spec any = {0};
    (void)diag;
    rkvc_port_set_desired(&node->in_ports[0], &any);
    rkvc_port_set_desired(&node->out_ports[0], &any);
    return 0;
}

static int process(rkvc_node *node, rkvc_frame *input, rkvc_diag **diag) {
    struct fixture_node *fixture = node->priv;
    rkvc_frame_desc input_desc, output_desc;
    rkvc_frame *output = NULL;
    void *copy;
    int rc;
    (void)diag;
    if (rkvc_frame_get_desc(input, &input_desc) != RKVC_STATUS_OK ||
        !input_desc.data || !input_desc.size)
        return (int)RKVC_STATUS_FORMAT;
    copy = malloc(input_desc.size);
    if (!copy)
        return (int)RKVC_STATUS_NOMEM;
    memcpy(copy, input_desc.data, input_desc.size);
    rkvc_frame_desc_init(&output_desc, sizeof(output_desc));
    output_desc.spec = input_desc.spec;
    output_desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    if (fixture->encode)
        output_desc.spec.fmt = RKVC_FRAME_FMT_BITSTREAM;
    output_desc.data = copy;
    output_desc.size = input_desc.size;
    output_desc.pts = input_desc.pts;
    output_desc.dts = input_desc.dts;
    output_desc.flags = input_desc.flags;
    if (rkvc_backend_frame_create(&output_desc, buffer_free, copy, &output) !=
        RKVC_STATUS_OK) {
        free(copy);
        return (int)RKVC_STATUS_NOMEM;
    }
    rc = rkvc_node_emit(node, 0, output);
    if (rc != 0)
        rkvc_frame_release(output);
    return rc;
}

static void destroy(rkvc_node *node) {
    if (!node) return;
    free(node->priv);
    free(node->in_ports);
    free(node->out_ports);
    free(node);
}

static const rkvc_node_ops encode_ops = {
    "fixture.encode", configure, NULL, process, NULL, NULL, destroy,
};

static int matches(rkvc_operation operation, rkvc_codec codec,
                   const rkvc_device_caps *caps) {
    (void)caps;
    return operation == RKVC_OPERATION_ENCODE &&
           (codec == RKVC_CODEC_H264 || codec == RKVC_CODEC_HEVC);
}

static rkvc_node *create(const rkvc_node_factory *factory,
                         const rkvc_request *request, void *create_ctx) {
    rkvc_node *node = calloc(1, sizeof(*node));
    struct fixture_node *fixture = calloc(1, sizeof(*fixture));
    (void)factory;
    (void)request;
    (void)create_ctx;
    if (!node || !fixture) {
        free(node);
        free(fixture);
        return NULL;
    }
    fixture->encode = 1;
    node->ops = &encode_ops;
    node->priv = fixture;
    node->in_ports = calloc(1, sizeof(*node->in_ports));
    node->out_ports = calloc(1, sizeof(*node->out_ports));
    if (!node->in_ports || !node->out_ports) {
        destroy(node);
        return NULL;
    }
    node->in_count = 1;
    node->in_ports[0].name = "in";
    node->in_ports[0].is_input = 1;
    node->out_count = 1;
    node->out_ports[0].name = "out";
    return node;
}

static const rkvc_node_factory factories[] = {
    {
        .id = "fixture.encode",
        .backend_id = "fixture-media",
        .stage = RKVC_NODE_STAGE_ENCODE,
        .priority = 100,
        .matches = matches,
        .create = create,
    },
};

static const rkvc_node_factory *factory_list(void *ctx, size_t *count) {
    (void)ctx;
    *count = sizeof(factories) / sizeof(factories[0]);
    return factories;
}

static const rkvc_backend backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "fixture-media",
    .factories = factory_list,
};

const rkvc_backend *rkvc_backend_query(void) { return &backend; }
