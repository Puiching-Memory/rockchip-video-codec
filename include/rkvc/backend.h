/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef RKVC_BACKEND_H
#define RKVC_BACKEND_H

/**
 * @file backend.h
 * @brief 可信 rkvc 后端 DSO 使用的版本化 ABI。
 *
 * 这是 SDK 扩展面，不是应用级图 API。后端拥有其创建的全部节点，
 * 其描述符与工厂数组必须存活到装载该 DSO 的 context 销毁为止。
 */

#include <stddef.h>
#include <stdint.h>

#include "rkvc/api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 后端 DSO 必须导出的入口符号；返回其 rkvc_backend。 */
#define RKVC_BACKEND_QUERY_SYMBOL "rkvc_backend_query"

/** 核心持有的有界 FIFO，连接两个端口（见 graph_internal.h）。 */
typedef struct rkvc_queue rkvc_queue;
/** 节点的一个输入或输出端口。 */
typedef struct rkvc_port rkvc_port;
/** 由后端工厂创建（并持有）的节点实例。 */
typedef struct rkvc_node rkvc_node;
/** 规划器条目：描述后端可创建的一种节点。 */
typedef struct rkvc_node_factory rkvc_node_factory;
/** 单个后端 DSO（或内建后端）的描述符。 */
typedef struct rkvc_backend rkvc_backend;
/** 核心持有的管线实例（见 graph_internal.h）。 */
typedef struct rkvc_graph rkvc_graph;

/**
 * 节点的一个输入或输出端口。核心就地重协商格式：`desired` 是
 * configure() 阶段写入的本侧偏好，`fmt` 是与对端协商解析后的取值。
 */
struct rkvc_port {
    const char      *name;      /**< 稳定端口名（"video"、"bitstream"） */
    int              is_input;  /**< 输入端口为非 0 */
    rkvc_frame_spec  fmt;       /**< 当前/已解析格式 */
    rkvc_frame_spec  desired;   /**< 本侧格式偏好 */
    rkvc_queue      *queue;     /**< 核心持有；连接前为 NULL */
};

/**
 * 前置声明：节点虚表中的 bind_model 回调以此指针类型为参。
 * 完整定义见下方 rkvc_model_binding。
 */
typedef struct rkvc_model_binding rkvc_model_binding;

/** 模型容器中单个已校验载荷的视图（见 rkvc_model_binding）。 */
typedef struct rkvc_model_payload_view {
    uint32_t            kind; /**< RKMODEL_PAYLOAD_* */
    const unsigned char *data; /**< 载荷字节（核心持有，只读） */
    size_t              size; /**< 载荷字节数 */
} rkvc_model_payload_view;

/**
 * 节点虚表。所有回调成功返回 0，失败返回负的 rkvc_status；
 * configure() 不得触碰硬件。
 */
typedef struct rkvc_node_ops {
    const char *id; /**< 稳定节点 id（"mpp.decode"、"file.source"） */
    /** 对齐对端格式说明，解析端口格式；不做设备 I/O。 */
    int (*configure)(rkvc_node *node, rkvc_diag **diag);
    /** 打开设备、分配大块资源；由 close() 释放。 */
    int (*open)(rkvc_node *node, rkvc_diag **diag);
    /**
     * 输入帧引用仅在本回调期间借用；核心在 process() 返回后释放。
     * 后端若要转发或保留该帧，必须先调用 rkvc_frame_retain()。
     */
    int (*process)(rkvc_node *node, rkvc_frame *input, rkvc_diag **diag);
    /** 流结束后排空缓冲的输出。 */
    int (*flush)(rkvc_node *node, rkvc_diag **diag);
    /** 释放 open 阶段资源；在 destroy() 之前调用。 */
    void (*close)(rkvc_node *node);
    /** 释放节点及其 priv；恰好调用一次。 */
    void (*destroy)(rkvc_node *node);
    /**
     * 可选：核心在 create 之后、configure 之前交付按请求选中的模型。
     * 实现本回调即声明"本节点必须有模型才能工作"；注册表中无兼容
     * 候选时该节点在图构建期被淘汰（触发候选回退）。载荷指针在节点
     * destroy 之前保持有效，由核心统一释放。
     */
    int (*bind_model)(rkvc_node *node, const rkvc_model_binding *model,
                      rkvc_diag **diag);
} rkvc_node_ops;

/**
 * 交付给节点的模型绑定：容器摘要 + 已校验的载荷字节。
 * `info` 与各载荷指针均为核心/上下文持有的只读存储；节点不得释放。
 *
 * 兼容字段 `payload`/`payload_size` 指向缺省载荷（RKNN；容器未携带时
 * 为首个载荷）。`payloads`/`payload_count` 是同一容器全部载荷的视图，
 * 供需要多载荷（如 MLVC 的 RKNN + 双 PMF 熵表）的节点按 kind 取用；
 * 载荷视图按容器表序排列，节点用 RKMODEL_PAYLOAD_* 匹配 kind。
 */
struct rkvc_model_binding {
    const rkvc_model_info *info;  /**< 容器摘要（id/role/target/trust） */
    const unsigned char   *payload; /**< 缺省载荷字节（见上方约定） */
    size_t                 payload_size; /**< 缺省载荷字节数 */
    const rkvc_model_payload_view *payloads; /**< 全部载荷视图；无载荷为 NULL */
    size_t                 payload_count;   /**< payloads 元素数 */
};
typedef struct rkvc_model_binding rkvc_model_binding;

/** 节点经历的生命周期状态；由核心驱动。 */
typedef enum rkvc_node_state {
    RKVC_NODE_CREATED = 0, /**< 已分配，尚未 configure */
    RKVC_NODE_CONFIGURED,  /**< 端口格式已解析 */
    RKVC_NODE_OPEN,        /**< 设备已打开，资源已持有 */
    RKVC_NODE_RUNNING,     /**< 正在处理帧 */
    RKVC_NODE_CLOSED,      /**< 资源已释放 */
    RKVC_NODE_FAILED,      /**< 终态错误 */
} rkvc_node_state;

/**
 * 节点实例。graph/idx/state 由核心填写并负责连接队列；
 * 后端只访问 ops、priv 与自己的端口。
 */
struct rkvc_node {
    const rkvc_node_ops *ops;      /**< 后端虚表 */
    void                *priv;     /**< 后端私有状态 */
    int                  state;    /**< rkvc_node_state */
    int                  in_count; /**< 输入端口数 */
    rkvc_port           *in_ports; /**< 核心分配的数组 */
    int                  out_count;   /**< 输出端口数 */
    rkvc_port           *out_ports;   /**< 核心分配的数组 */
    rkvc_graph           *graph; /* 核心持有；后端不得检视 */
    size_t                idx;    /**< 在线性图中的位置 */
};

/** 管线位置；规划器每个阶段恰好选一个工厂。 */
typedef enum rkvc_node_stage {
    RKVC_NODE_STAGE_SOURCE = 0, /**< 文件/流读取器 */
    RKVC_NODE_STAGE_DECODE,     /**< 码流 → 原始帧 */
    RKVC_NODE_STAGE_TRANSFORM,  /**< 帧 → 帧（上采样/转换） */
    RKVC_NODE_STAGE_ENCODE,     /**< 原始帧 → 码流 */
    RKVC_NODE_STAGE_SINK,       /**< 文件/流写出器 */
} rkvc_node_stage;

/** 后端上报的能力位；核心会镜像进 caps。 */
enum {
    RKVC_BACKEND_CAP_MPP_DECODE = 1u << 0, /**< VPU 硬解 */
    RKVC_BACKEND_CAP_MPP_ENCODE = 1u << 1, /**< VPU 硬编 */
    RKVC_BACKEND_CAP_RGA        = 1u << 2, /**< RGA 2D 搬运/转换 */
    RKVC_BACKEND_CAP_RKNN       = 1u << 3, /**< RKNN NPU 运行时 */
};

/**
 * 一种节点的规划器条目。matches() 按操作/编码/设备把关候选资格；
 * 最终排序为 priority 加上可选的 score() 加成。
 */
struct rkvc_node_factory {
    const char *id;         /**< 稳定工厂 id */
    const char *backend_id; /**< 所属后端 id */
    rkvc_node_stage stage;  /**< 管线位置 */
    int priority;           /**< 基础分；高者优先 */
    /** 本工厂能否在该设备上服务此操作/编码；非 0 表示可以。 */
    int (*matches)(rkvc_operation op, rkvc_codec codec,
                   const rkvc_device_caps *caps);
    /** 可选：按具体请求计算的 priority 加成。 */
    int (*score)(const rkvc_request *request,
                 const rkvc_device_caps *caps, void *create_ctx);
    /** 实例化节点；仅分配失败时返回 NULL。 */
    rkvc_node *(*create)(const rkvc_node_factory *factory,
                         const rkvc_request *request, void *create_ctx);
    void *create_ctx;       /**< 传给各回调的不透明上下文 */
};

/**
 * rkvc_backend_query() 返回的后端描述符。描述符及其全部指针目标
 * 必须存活到装载该 DSO 的 context 销毁为止。
 */
struct rkvc_backend {
    uint32_t abi_version;      /**< 装载时必须等于 RKVC_ABI_VERSION */
    const char *id;            /**< 稳定后端 id（"mpp"、"fileio"） */
    uint32_t capability_flags; /**< RKVC_BACKEND_CAP_* 位组合 */
    /** 可选设备探测；非 0 淘汰整个后端。 */
    int (*probe)(const rkvc_device_caps *caps, void *probe_ctx,
                 rkvc_diag **diag);
    /** 返回后端持有的工厂数组。 */
    const rkvc_node_factory *(*factories)(void *probe_ctx, size_t *count);
    void *probe_ctx;           /**< 传给 probe/factories 的不透明上下文 */
};

/** rkvc_backend_query() 入口符号的函数类型。 */
typedef const rkvc_backend *(*rkvc_backend_query_fn)(void);
/** 后端帧最后一个引用释放时触发的回调。 */
typedef void (*rkvc_backend_frame_release_fn)(void *release_ctx);

/** 向诊断链追加一条后端侧原因。 */
void rkvc_diag_push(rkvc_diag **diag, rkvc_status status, int stage,
                    const char *subject, const char *reason);

/** 按名查找端口；as_input 选择输入/输出侧；不存在返回 NULL。 */
rkvc_port *rkvc_node_get_port(rkvc_node *node, const char *name, int as_input);
/** 覆盖端口的期望与已解析格式（仅 configure 阶段使用）。 */
void rkvc_port_set_desired(rkvc_port *port, const rkvc_frame_spec *spec);
/**
 * 成功时把一个帧引用转移给图。失败（含取消或 EOS）时所有权仍归
 * 后端调用方。
 */
int rkvc_node_emit(rkvc_node *node, size_t output_index, rkvc_frame *frame);

/**
 * 为后端输出创建核心持有的帧句柄。最后一个引用释放时回调执行一次，
 * 可在其中归还 MPP/RGA/RKNN 资源。
 */
rkvc_status rkvc_backend_frame_create(
    const rkvc_frame_desc *desc,
    rkvc_backend_frame_release_fn release_fn,
    void *release_ctx,
    rkvc_frame **out);

/**
 * 把后端产出的 DMA-BUF fd 复制进核心持有的帧。最后一个引用释放时
 * 核心关闭自己的副本，因此帧可以比产出它的后端节点、context 与 DSO
 * 活得更久。
 */
rkvc_status rkvc_backend_frame_create_dmabuf(
    const rkvc_frame_desc *desc,
    rkvc_frame **out);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BACKEND_H */
