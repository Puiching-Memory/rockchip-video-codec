/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file graph_internal.h
 * @brief 通用图内核内部定义（非公共 ABI）。
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
#include "rkvc/backend.h"
#include "rkvc/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 分配器（单测映射到 libc） ───────────────────────────────────── */
#if defined(RKVC_STANDALONE_TEST)
#  include <stdlib.h>
#  define rkvc_g_calloc(n, s) calloc((n), (s))
#  define rkvc_g_free(p)      free((p))
#else
/** 库内集中分配器（便于将来替换为池化/审计实现）。 */
void *rkvc_calloc(size_t nmemb, size_t size);
/** 释放 rkvc_calloc 分配的内存。 */
void rkvc_free(void *ptr);
#  define rkvc_g_calloc rkvc_calloc
#  define rkvc_g_free   rkvc_free
#endif

/* ── 帧（内部表示；公共 ABI 句柄 opaque） ─────────────────────────── */
/** @brief rkvc_frame 内部表示：引用计数 + 格式/内存句柄 + 释放回调。 */
struct rkvc_frame {
    uint32_t       refcount;     /**< 引用计数（原子增减） */
    rkvc_frame_spec spec;        /**< 格式与内存域 */
    void          *data;         /**< HOST 域指针（可为 NULL） */
    size_t          size;        /**< 载荷字节数 */
    int            fd;           /**< DMABUF fd；否则 -1 */
    int64_t         pts;         /**< 显示时间戳 */
    int64_t         dts;         /**< 解码时间戳 */
    uint32_t        flags;       /**< RKVC_FRAME_FLAG_* */
    rkvc_roi_region *roi_regions; /**< ROI 附属数据的自有深拷贝（可为 NULL） */
    size_t           roi_region_count; /**< roi_regions 元素数 */
    rkvc_encode_control encode;  /**< 逐帧编码控制（0 = 保持现状） */
    void         (*free_fn)(void *); /**< 引用归零时回调（可为 NULL） */
    void          *free_ctx;     /**< free_fn 的上下文 */
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

/* ── 计划与图 ─────────────────────────────────────────────────────── */
/** @brief 计划中的一个步骤：候选工厂按序尝试，失败时回退到次优。 */
typedef struct rkvc_plan_step {
    const rkvc_node_factory *factory; /**< 当前选中的工厂 */
    const rkvc_node_factory **candidates; /**< 按分数/稳定 ID 排序 */
    size_t candidate_count;   /**< candidates 元素数 */
    size_t candidate_index;   /**< 当前选中项在 candidates 中的下标 */
    rkvc_request request;      /**< 每步可携带微调后的请求（如 color range） */
} rkvc_plan_step;

/** @brief 线性执行计划：source → … → sink 的步骤序列。 */
typedef struct rkvc_plan {
    rkvc_plan_step *steps;          /**< 步骤数组 */
    size_t           step_count;    /**< steps 元素数 */
    size_t           fallback_count; /**< 已消耗的回退次数（上界 RKVC_MAX_PLAN_FALLBACKS） */
} rkvc_plan;

typedef struct rkvc_graph rkvc_graph;

/* 前置声明：定义在 context_internal.h（模型注册表选择器归上下文所有）。 */
struct rkvc_context;

/** @brief 已构建/协商/打开的图实例及其计划。 */
struct rkvc_graph {
    rkvc_node **nodes;         /**< 按执行顺序排列的节点 */
    size_t      node_count;    /**< nodes 元素数 */
    rkvc_queue **queues;       /**< 连接队列（node_count-1 条线性链） */
    size_t      queue_count;   /**< queues 元素数 */
    size_t      queue_capacity;/**< 每条连接的容量（背压上限） */
    void       *exec;          /**< 执行器（opaque，见 executor.c） */
    rkvc_plan   plan;          /**< 构建本图的计划（build 成功后移交持有） */
    size_t      failure_step;  /**< 最近 create/configure/open 失败的步骤 */
    int         state;         /**< 0=构建 1=已协商 2=已打开/运行 3=已关闭 */
    const struct rkvc_context *ctx; /**< 规划/模型注册表所属上下文（可空） */
    void       *model_payload; /**< 已交付节点的模型载荷（销毁时释放） */
};

/** 绑定上下文（须在 build 前调用）：bind_model 节点据其选择模型。 */
void rkvc_graph_set_context(rkvc_graph *g, const struct rkvc_context *ctx);

/* ── 执行器（executor.c） ─────────────────────────────────────────── */
typedef struct rkvc_exec rkvc_exec;
/** 创建执行器并分配输入/输出边界队列；worker_threads 为节点线程数。 */
rkvc_exec *rkvc_exec_create(rkvc_graph *g, size_t worker_threads);
/** 销毁执行器：取消、join 全部线程并释放边界队列。 */
void    rkvc_exec_destroy(rkvc_exec *e);
/** 阻塞运行到 EOS/错误/取消；返回 0 成功 / <0 状态码。 */
int     rkvc_exec_run(rkvc_exec *e, rkvc_diag **diag);
/** 请求取消：置位并广播唤醒阻塞中的队列操作。 */
void    rkvc_exec_cancel(rkvc_exec *e);
/** 推入图输入边界队列（非阻塞；满时返回 RKVC_STATUS_AGAIN）。 */
int     rkvc_exec_push(rkvc_exec *e, rkvc_frame *frame);
/** 从图输出边界队列拉取一帧；EOS/取消映射为对应状态码。 */
int     rkvc_exec_pull(rkvc_exec *e, rkvc_frame **frame);
/** 通知图输入已到流结束（flush 语义）。 */
int     rkvc_exec_eos(rkvc_exec *e);

/* ── 图生命周期（graph.c） ─────────────────────────────────────────── */
/** 创建空图（默认队列容量 4）。 */
rkvc_graph *rkvc_graph_new(void);
/** 拆除并释放图与计划（幂等；可为 NULL）。 */
void rkvc_graph_free(rkvc_graph *g);

/* 从计划创建节点并协商格式；不打开设备或分配大块硬件资源。 */
int rkvc_graph_build(rkvc_graph *g, const rkvc_plan *plan, rkvc_diag **diag);

/* 实例化协商后的图：打开设备；失败时逆序回滚，图不可重试。 */
int rkvc_graph_open(rkvc_graph *g, rkvc_diag **diag);

/* 运行器（起线程→跑→join），阻塞；返回 0 成功 / <0 错误 */
int rkvc_graph_run(rkvc_graph *g, rkvc_diag **diag);
/** 请求取消运行中的图（转发给执行器）。 */
void rkvc_graph_cancel(rkvc_graph *g);
/** 逆序释放节点与队列；不释放图本身。 */
void rkvc_graph_teardown(rkvc_graph *g);

/* 规划器（registry.c）：从注册表+请求+能力 生成线性计划，确定性且可解释 */
int rkvc_plan_build(const rkvc_context *ctx, const rkvc_request *req,
                    const rkvc_device_caps *caps, rkvc_plan *plan,
                    rkvc_diag **diag);
/** 释放计划的全部步骤与候选数组。 */
void rkvc_plan_release(rkvc_plan *plan);
/** 切换失败步骤到次优候选；必要时推进其他步骤，返回 1=存在新组合。 */
int rkvc_plan_advance(rkvc_plan *plan, size_t failure_step);

/* ── 注册表（registry.c） ─────────────────────────────────────────── */
/** 注册后端（含 ABI 握手、probe 与工厂校验；重复 id 拒绝）。 */
rkvc_status rkvc_registry_add_backend(rkvc_context *ctx, const rkvc_backend *be);
/** 按注册顺序取第 idx 个后端；越界返回 NULL。 */
const rkvc_backend *rkvc_registry_backend(const rkvc_context *ctx, size_t idx);
/** 已注册后端数。 */
size_t rkvc_registry_backend_count(const rkvc_context *ctx);
/** 按 id 查找工厂；未找到返回 NULL。 */
const rkvc_node_factory *rkvc_registry_find_factory(const rkvc_context *ctx,
                                                    const char *id);

/* ── 节点/端口工具（node.c） ────────────────────────────────────────── */
/** 连接 a 的输出端口到 b 的输入端口（新建队列）；0 成功 / -2 无效端口。 */
int rkvc_node_connect(rkvc_node *a, const char *out_name,
                      rkvc_node *b, const char *in_name);

/* ── 连接队列（executor.c 内部；此处仅声明给 graph.c 用） ────────── */
/** 创建容量 capacity 的有界 FIFO（0 → 4）。 */
rkvc_queue *rkvc_queue_create(size_t capacity);
/** 销毁队列并释放其中剩余帧。 */
void rkvc_queue_destroy(rkvc_queue *q);
/** 阻塞 push：等到有空间（背压）；返回 0 / -13=取消。 */
int rkvc_queue_push(rkvc_queue *q, rkvc_frame *f, void *exec);
/** 非阻塞 push：满时立即返回 RKVC_STATUS_AGAIN。 */
int rkvc_queue_try_push(rkvc_queue *q, rkvc_frame *f, void *exec);
/** 阻塞 pop：返回 1=帧 / 0=EOS / <0=取消。 */
int rkvc_queue_pop(rkvc_queue *q, rkvc_frame **f, void *exec);
/** 标记流结束并唤醒所有等待的消费者。 */
void rkvc_queue_set_eos(rkvc_queue *q);

/* 图可调项 */
/** 设置每条连接的队列容量（须在 build 前调用；0 忽略）。 */
void rkvc_graph_set_queue_capacity(rkvc_graph *g, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_GRAPH_INTERNAL_H */
