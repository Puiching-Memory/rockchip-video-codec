/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file graph_internal.h
 * @brief 0.4 通用图内核内部定义（非公共 ABI）。
 *
 * 图内核只依赖公共 ABI 头（context/request/job/frame/diagnostic）与
 * libc 分配器；不含任何 FFmpeg / MPP / RGA / RKNN 类型。媒体实现位于
 * 后端 DSO，通过节点接口进入图。
 *
 * 单测（RKVC_STANDALONE_TEST）把分配器映射到 libc，可独立编译。
 */

#ifndef RKVC_GRAPH_INTERNAL_H
#define RKVC_GRAPH_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rkvc/api.h"
#include "rkvc/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 分配器（单测映射到 libc） ───────────────────────────────────── */
#if defined(RKVC_STANDALONE_TEST)
#  include <stdlib.h>
#  define rkvc_g_calloc(n, s) calloc((n), (s))
#  define rkvc_g_free(p)      free(p)
#else
void *rkvc_calloc(size_t nmemb, size_t size);
void rkvc_free(void *ptr);
#  define rkvc_g_calloc rkvc_calloc
#  define rkvc_g_free   rkvc_free
#endif

/* ── 帧（内部表示；公共 ABI 句柄 opaque） ─────────────────────────── */
struct rkvc_frame {
    uint32_t       refcount;
    rkvc_frame_spec spec;
    void          *data;
    int            fd;
    void         (*free_fn)(void *);
    void          *free_ctx;
};

/** 内部构造帧（不经过公共 ABI 的身份/格式校验路径）。 */
rkvc_frame *rkvc_frame_internal_alloc(const rkvc_frame_spec *spec, void *data,
                                      int fd, void (*free_fn)(void *),
                                      void *free_ctx);

/* ── 诊断链内部构造 ──────────────────────────────────────────────── */
/**
 * @brief 在诊断链首追加一个节点。
 * @param diag  指向链首指针（可为空链）。失败时记为 NOMEM 节点并仍返回原链。
 * @param status 关联状态码。
 * @param stage  阶段编码。
 * @param subject 主体（后端/模型/端口名）。
 * @param reason  静态原因。
 */
void rkvc_diag_push(rkvc_diag **diag, rkvc_status status, int stage,
                    const char *subject, const char *reason);

/* ── 端口 ─────────────────────────────────────────────────────────── */
typedef struct rkvc_port_interp rkvc_port;
typedef struct rkvc_queue        rkvc_queue;

/** @brief 端口方向与格式（端口是节点的 I/O 面，连接被建模为 rkvc_queue）。 */
struct rkvc_port_interp {
    const char    *name;      /**< 端口名（如 input/output/preview） */
    int            is_input;  /**< 从节点视角看是否为输入 */
    rkvc_frame_spec fmt;      /**< 协商后的数据格式 */
    rkvc_frame_spec desired;  /**< 期望/初始要求格式 */
    rkvc_queue     *queue;    /**< 关联的连接队列（输出=push 面，输入=pull 面） */
};

/* ── 节点 ─────────────────────────────────────────────────────────── */
typedef struct rkvc_node rkvc_node;

/**
 * @brief 节点操作表。
 *
 * `configure` 只协商格式（不打开设备/分配大块内存）；`open` 才打开设备并
 * 分配大块内存。二者任一步失败，图内核按逆序关闭已打开对象。
 * `process` 收到 in（可为空表示 EOS），可经 rkvc_node_emit 产出 0..N 帧。
 */
typedef struct rkvc_node_ops {
    const char *id;
    int (*configure)(rkvc_node *n, rkvc_diag **diag);
    int (*open)(rkvc_node *n, rkvc_diag **diag);
    int (*process)(rkvc_node *n, rkvc_frame *in, rkvc_diag **diag);
    int (*flush)(rkvc_node *n, rkvc_diag **diag);
    void (*close)(rkvc_node *n);
    void (*destroy)(rkvc_node *n);
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
    void  *priv;              /**< 节点私有数据（由工厂 create 分配） */
    int    state;
    int    in_count;
    rkvc_port *in_ports;
    int    out_count;
    rkvc_port *out_ports;
    struct rkvc_graph *graph;
    size_t idx;               /**< 在图中索引（决定确定性顺序） */
};

/* ── 节点工厂与后端注册表 ─────────────────────────────────────────── */
typedef struct rkvc_backend rkvc_backend;
typedef struct rkvc_node_factory rkvc_node_factory;

/**
 * @brief 节点工厂：声明节点类型与筛选（是否匹配 op/codec/caps），并创建节点。
 */
struct rkvc_node_factory {
    const char *id;
    const char *backend_id;
    int (*matches)(rkvc_operation op, rkvc_codec codec,
                   const rkvc_device_caps *caps);
    rkvc_node *(*create)(const rkvc_node_factory *factory,
                         const rkvc_request *req, void *create_ctx);
    void *create_ctx;
};

/**
 * @brief 后端：ABI 版本化，导出能力探测与节点工厂列表。
 *
 * 后端 DSO 通过 `rkvc_backend_query()` 返回本结构；load 失败只淘汰候选。
 */
struct rkvc_backend {
    uint32_t abi_version;
    const char *id;
    int (*probe)(const rkvc_device_caps *caps, void *probe_ctx,
                 rkvc_diag **diag);
    const rkvc_node_factory *(*factories)(void *probe_ctx, size_t *count);
    void *probe_ctx;
};

/* ── 计划与图 ─────────────────────────────────────────────────────── */
typedef struct rkvc_plan_step {
    const rkvc_node_factory *factory;
    rkvc_request request;      /**< 每步可携带微调后的请求（如 color range） */
} rkvc_plan_step;

typedef struct rkvc_plan {
    rkvc_plan_step *steps;
    size_t           step_count;
} rkvc_plan;

typedef struct rkvc_graph rkvc_graph;

struct rkvc_graph {
    rkvc_node **nodes;
    size_t      node_count;
    rkvc_queue **queues;       /**< 连接队列（node_count-1 条线性链） */
    size_t      queue_count;
    size_t      queue_capacity;/**< 每条连接的容量（背压上限） */
    void       *exec;          /**< 执行器（opaque，见 executor.c） */
    rkvc_plan   plan;
    int         state;         /**< 0=构建 1=已实例化 2=运行 3=已关闭 */
};

/* ── 执行器（executor.c） ─────────────────────────────────────────── */
typedef struct rkvc_exec rkvc_exec;
rkvc_exec *rkvc_exec_create(rkvc_graph *g, size_t worker_threads);
void    rkvc_exec_destroy(rkvc_exec *e);
int     rkvc_exec_run(rkvc_exec *e, rkvc_diag **diag);   /* 阻塞到 EOS/错误/取消 */
void    rkvc_exec_cancel(rkvc_exec *e);
int     rkvc_exec_push(rkvc_exec *e, rkvc_frame *frame); /* 推入图输入 */
int     rkvc_exec_pull(rkvc_exec *e, rkvc_frame **frame);/* 图输出 */
int     rkvc_exec_eos(rkvc_exec *e);                     /* 输入 EOS */

/* ── 图生命周期（graph.c） ─────────────────────────────────────────── */
rkvc_graph *rkvc_graph_new(void);
void rkvc_graph_free(rkvc_graph *g);

/* 从计划实例化：创建节点、连接端口（协商格式）、open 全部；失败逆序回滚 */
int rkvc_graph_build(rkvc_graph *g, const rkvc_plan *plan, rkvc_diag **diag);

/* 运行器（起线程→跑→join），阻塞；返回 0 成功 / <0 错误 */
int rkvc_graph_run(rkvc_graph *g, rkvc_diag **diag);
void rkvc_graph_cancel(rkvc_graph *g);
void rkvc_graph_teardown(rkvc_graph *g);

/* 规划器（registry.c）：从注册表+请求+能力 生成线性计划，确定性且可解释 */
int rkvc_plan_build(const rkvc_context *ctx, const rkvc_request *req,
                    const rkvc_device_caps *caps, rkvc_plan *plan,
                    rkvc_diag **diag);
void rkvc_plan_release(rkvc_plan *plan);

/* ── 注册表（registry.c） ─────────────────────────────────────────── */
rkvc_status rkvc_registry_add_backend(rkvc_context *ctx, const rkvc_backend *be);
const rkvc_backend *rkvc_registry_backend(const rkvc_context *ctx, size_t idx);
size_t rkvc_registry_backend_count(const rkvc_context *ctx);
const rkvc_node_factory *rkvc_registry_find_factory(const rkvc_context *ctx,
                                                    const char *id);

/* ── 节点/端口工具（node.c） ────────────────────────────────────────── */
rkvc_port *rkvc_node_get_port(rkvc_node *n, const char *name, int as_input);
int rkvc_node_connect(rkvc_node *a, const char *out_name,
                      rkvc_node *b, const char *in_name);
void rkvc_port_set_desired(rkvc_port *p, const rkvc_frame_spec *spec);
int rkvc_node_emit(rkvc_node *n, int out_index, rkvc_frame *frame);

/* ── 连接队列（executor.c 内部；此处仅声明给 graph.c 用） ────────── */
rkvc_queue *rkvc_queue_create(size_t capacity);
void rkvc_queue_destroy(rkvc_queue *q);
/* push 阻塞至有空间（背压），返回 0 或被取消返回 -13；pop 返回 1=帧 / 0=EOS / <0=取消 */
int rkvc_queue_push(rkvc_queue *q, rkvc_frame *f, void *exec);
int rkvc_queue_pop(rkvc_queue *q, rkvc_frame **f, void *exec);
void rkvc_queue_set_eos(rkvc_queue *q);

/* 图可调项 */
void rkvc_graph_set_queue_capacity(rkvc_graph *g, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_GRAPH_INTERNAL_H */
