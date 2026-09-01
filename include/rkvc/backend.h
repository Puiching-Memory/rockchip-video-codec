/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef RKVC_BACKEND_H
#define RKVC_BACKEND_H

/**
 * @file backend.h
 * @brief Versioned ABI used by trusted rkvc backend DSOs.
 *
 * This is an SDK extension surface, not an application-level graph API.
 * A backend owns all nodes it creates and must keep its descriptor and factory
 * arrays alive until the context that loaded the DSO is destroyed.
 */

#include <stddef.h>
#include <stdint.h>

#include "rkvc/api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RKVC_BACKEND_QUERY_SYMBOL "rkvc_backend_query"

typedef struct rkvc_queue rkvc_queue;
typedef struct rkvc_port rkvc_port;
typedef struct rkvc_node rkvc_node;
typedef struct rkvc_node_factory rkvc_node_factory;
typedef struct rkvc_backend rkvc_backend;
typedef struct rkvc_graph rkvc_graph;

struct rkvc_port {
    const char      *name;
    int              is_input;
    rkvc_frame_spec  fmt;
    rkvc_frame_spec  desired;
    rkvc_queue      *queue;
};

typedef struct rkvc_node_ops {
    const char *id;
    int (*configure)(rkvc_node *node, rkvc_diag **diag);
    int (*open)(rkvc_node *node, rkvc_diag **diag);
    /**
     * The input reference is borrowed for the duration of this call. The core
     * releases it after process() returns. A backend that forwards or retains
     * the frame must first call rkvc_frame_retain().
     */
    int (*process)(rkvc_node *node, rkvc_frame *input, rkvc_diag **diag);
    int (*flush)(rkvc_node *node, rkvc_diag **diag);
    void (*close)(rkvc_node *node);
    void (*destroy)(rkvc_node *node);
} rkvc_node_ops;

typedef enum rkvc_node_state {
    RKVC_NODE_CREATED = 0,
    RKVC_NODE_CONFIGURED,
    RKVC_NODE_OPEN,
    RKVC_NODE_RUNNING,
    RKVC_NODE_CLOSED,
    RKVC_NODE_FAILED,
} rkvc_node_state;

struct rkvc_node {
    const rkvc_node_ops *ops;
    void                *priv;
    int                  state;
    int                  in_count;
    rkvc_port           *in_ports;
    int                  out_count;
    rkvc_port           *out_ports;
    rkvc_graph           *graph; /* core-owned; backend must not inspect */
    size_t                idx;
};

typedef enum rkvc_node_stage {
    RKVC_NODE_STAGE_SOURCE = 0,
    RKVC_NODE_STAGE_DECODE,
    RKVC_NODE_STAGE_TRANSFORM,
    RKVC_NODE_STAGE_ENCODE,
    RKVC_NODE_STAGE_SINK,
} rkvc_node_stage;

enum {
    RKVC_BACKEND_CAP_MPP_DECODE = 1u << 0,
    RKVC_BACKEND_CAP_MPP_ENCODE = 1u << 1,
    RKVC_BACKEND_CAP_RGA        = 1u << 2,
    RKVC_BACKEND_CAP_RKNN       = 1u << 3,
};

struct rkvc_node_factory {
    const char *id;
    const char *backend_id;
    rkvc_node_stage stage;
    int priority;
    int (*matches)(rkvc_operation op, rkvc_codec codec,
                   const rkvc_device_caps *caps);
    int (*score)(const rkvc_request *request,
                 const rkvc_device_caps *caps, void *create_ctx);
    rkvc_node *(*create)(const rkvc_node_factory *factory,
                         const rkvc_request *request, void *create_ctx);
    void *create_ctx;
};

struct rkvc_backend {
    uint32_t abi_version;
    const char *id;
    uint32_t capability_flags;
    int (*probe)(const rkvc_device_caps *caps, void *probe_ctx,
                 rkvc_diag **diag);
    const rkvc_node_factory *(*factories)(void *probe_ctx, size_t *count);
    void *probe_ctx;
};

typedef const rkvc_backend *(*rkvc_backend_query_fn)(void);
typedef void (*rkvc_backend_frame_release_fn)(void *release_ctx);

/** Add a backend-specific reason to the diagnostic chain. */
void rkvc_diag_push(rkvc_diag **diag, rkvc_status status, int stage,
                    const char *subject, const char *reason);

rkvc_port *rkvc_node_get_port(rkvc_node *node, const char *name, int as_input);
void rkvc_port_set_desired(rkvc_port *port, const rkvc_frame_spec *spec);
/**
 * Transfers one frame reference to the graph on success. On failure, including
 * cancellation or EOS, ownership remains with the backend caller.
 */
int rkvc_node_emit(rkvc_node *node, size_t output_index, rkvc_frame *frame);

/**
 * Create a core-owned frame handle for backend output. The callback runs once
 * when the last reference is released and may return MPP/RGA/RKNN resources.
 */
rkvc_status rkvc_backend_frame_create(
    const rkvc_frame_desc *desc,
    rkvc_backend_frame_release_fn release_fn,
    void *release_ctx,
    rkvc_frame **out);

/**
 * Duplicate a backend-produced DMA-BUF fd into a core-owned frame. The core
 * closes its duplicate on final release, so the frame may outlive the backend
 * node, context and DSO that produced it.
 */
rkvc_status rkvc_backend_frame_create_dmabuf(
    const rkvc_frame_desc *desc,
    rkvc_frame **out);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BACKEND_H */
