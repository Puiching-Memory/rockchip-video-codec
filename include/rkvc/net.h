/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file net.h
 * @brief UDP / RTP 码流收发原语（薄协议，不含 Session / 国标信令）。
 *
 * - **UDP**：16B 分片头（frag_id / frag_total / frame_len / pts）+ 载荷，最多 16 片。
 *   结束信号：`frag_id=0xffff, frag_total=0, frame_len=0`（无载荷）。
 *   接收端校验 `frag_total∈[1,16]` 且 `frame_len ≤ frag_total×payload`。
 * - **RTP**：12B RTP 头（PT=96 默认）+ 载荷分片 ≤1400B，Marker 标帧尾。
 *
 * 应用层负责：Session `output` 码流 ↔ `rkvc_net_send` / `recv`；
 * GB28181 SIP、WebRTC 信令不在本层。
 */

#ifndef RKVC_NET_H
#define RKVC_NET_H

#include "rkvc/types.h"
#include "rkvc/buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 传输模式。 */
typedef enum {
    RKVC_NET_UDP = 0, /**< 自定义分片 UDP（适合任意 Annex-B / 裸码流） */
    RKVC_NET_RTP = 1, /**< RTP over UDP（简化 H.264/H.265 帧分片，非完整 RFC） */
} rkvc_net_mode;

/** @brief 不透明网络端点。 */
typedef struct rkvc_net rkvc_net;

/**
 * @brief 打开配置。
 *
 * - 仅发送：设 `peer_ip`/`peer_port`，`bind_port=0`。
 * - 仅接收：设 `bind_port>0`，`peer_ip=NULL`。
 * - 本机回环可同时 bind + peer（如 127.0.0.1）。
 */
typedef struct {
    rkvc_net_mode mode;       /**< UDP 或 RTP */
    const char   *bind_ip;    /**< 绑定地址，NULL=`INADDR_ANY` */
    int           bind_port;  /**< 绑定端口；0=不 bind（纯发送） */
    const char   *peer_ip;    /**< 对端 IP；NULL=纯接收 */
    int           peer_port;  /**< 对端端口 */
    int           timeout_ms; /**< 默认 recv 超时（毫秒），0=阻塞；可被 `recv` 参数覆盖 */
    uint32_t      rtp_ssrc;   /**< RTP SSRC，0=默认 `0x01020304` */
    uint8_t       rtp_payload_type; /**< RTP PT，0=默认 96 */
} rkvc_net_config;

/** @brief 返回默认配置（UDP，timeout=1000ms，PT=96）。 */
rkvc_net_config rkvc_net_config_defaults(void);

/**
 * @brief 打开网络端点。
 * @return `RKVC_OK`、`RKVC_ERR_INVALID`、`RKVC_ERR_IO`、`RKVC_ERR_PERMISSION`。
 */
rkvc_err rkvc_net_open(rkvc_net **out, const rkvc_net_config *cfg);

/** @brief 关闭并释放。NULL 安全。 */
void rkvc_net_close(rkvc_net *net);

/**
 * @brief 发送一帧码流（自动分片）。
 *
 * @param data 载荷；`size==0` 时发送结束信号（同 `rkvc_net_finish`）。
 * @param pts  时间戳（透传；RTP 取低 32 位作 timestamp）。
 * @param key_frame 非 0 表示关键帧（UDP 头未携带该标志，保留给上层统计）。
 * @return 超过接收端重组上限的帧返回 `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_net_send(rkvc_net *net, const uint8_t *data, size_t size,
                       int64_t pts, int key_frame);

/**
 * @brief 接收并重组一帧，输出为码流缓冲（调用方 `rkvc_buffer_unref`）。
 *
 * @param timeout_ms `<0` 用配置默认；`0` 非阻塞；`>0` 等待毫秒。
 * @return `RKVC_OK`、`RKVC_ERR_AGAIN`（超时/无数据）、`RKVC_ERR_EOF`（对端 finish）、
 *         `RKVC_ERR_INVALID`（RTP 帧超过接收上限）。
 */
rkvc_err rkvc_net_recv(rkvc_net *net, rkvc_buffer **out, int timeout_ms);

/** @brief 通知对端结束（UDP 零头 / RTP 空 Marker 包）。 */
rkvc_err rkvc_net_finish(rkvc_net *net);

/** @brief 收发统计。 */
typedef struct {
    uint64_t pkts_sent;   /**< 逻辑帧发送数 */
    uint64_t pkts_recv;   /**< 逻辑帧接收数 */
    uint64_t bytes_sent;  /**< 含协议头的 UDP 字节 */
    uint64_t bytes_recv;
} rkvc_net_stats;

rkvc_err rkvc_net_get_stats(const rkvc_net *net, rkvc_net_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_NET_H */
