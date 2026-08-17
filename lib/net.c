/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file net.c
 * @brief UDP / RTP 码流收发原语实现。
 */

#include "internal.h"
#include "rkvc/net.h"

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

#define RKVC_UDP_FRAG_HEADER  16
#define RKVC_UDP_FRAG_PAYLOAD 65491
#define RKVC_UDP_MAX_PAYLOAD  65507
#define RKVC_UDP_MAX_FRAGS    16
#define RKVC_UDP_MAX_FRAME    ((size_t)RKVC_UDP_MAX_FRAGS * RKVC_UDP_FRAG_PAYLOAD)
/* finish：frag_id=0xffff, frag_total=0, frame_len=0 */
#define RKVC_UDP_FINISH_ID    0xFFFFu
#define RKVC_UDP_FINISH_TOTAL 0

#define RKVC_RTP_HEADER_SIZE  12
#define RKVC_RTP_PAYLOAD_MAX  1400
#define RKVC_RTP_MAX_FRAME    RKVC_UDP_MAX_FRAME

struct rkvc_net {
    rkvc_net_mode mode;
    int           fd;
    int           has_peer;
    int           finished;
    int           timeout_ms;
    struct sockaddr_in peer;
    uint32_t      rtp_ssrc;
    uint8_t       rtp_pt;
    uint16_t      rtp_seq;

    /* UDP reassembly */
    uint8_t      *reasm_data;
    int           reasm_cap;
    int           reasm_size;
    int           reasm_frame_len;
    int           reasm_frag_total;
    uint8_t       reasm_mask[2];
    int64_t       reasm_pts;

    rkvc_net_stats stats;
};

rkvc_net_config rkvc_net_config_defaults(void)
{
    rkvc_net_config c;
    memset(&c, 0, sizeof(c));
    c.mode = RKVC_NET_UDP;
    c.timeout_ms = 1000;
    c.rtp_ssrc = 0x01020304;
    c.rtp_payload_type = 96;
    return c;
}

static int sock_create(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    return fd;
}

static rkvc_err sock_bind(int fd, const char *ip, int port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (ip && ip[0]) {
        if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
            return RKVC_ERR_INVALID;
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return (errno == EACCES || errno == EPERM) ? RKVC_ERR_PERMISSION
                                                   : RKVC_ERR_IO;
    return RKVC_OK;
}

static rkvc_err set_peer(struct sockaddr_in *dst, const char *ip, int port)
{
    if (!ip || !ip[0] || port <= 0 || port > 65535)
        return RKVC_ERR_INVALID;
    memset(dst, 0, sizeof(*dst));
    dst->sin_family = AF_INET;
    dst->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &dst->sin_addr) != 1)
        return RKVC_ERR_INVALID;
    return RKVC_OK;
}

static void apply_timeout(int fd, int timeout_ms)
{
    struct timeval tv;
    if (timeout_ms < 0)
        timeout_ms = 0;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

rkvc_err rkvc_net_open(rkvc_net **out, const rkvc_net_config *cfg)
{
    if (!out || !cfg)
        return RKVC_ERR_INVALID;
    if (cfg->mode != RKVC_NET_UDP && cfg->mode != RKVC_NET_RTP)
        return RKVC_ERR_INVALID;

    int need_bind = cfg->bind_port > 0;
    int need_peer = cfg->peer_ip && cfg->peer_ip[0] && cfg->peer_port > 0;
    if (!need_bind && !need_peer)
        return RKVC_ERR_INVALID;

    *out = NULL;
    rkvc_net *n = rkvc_calloc(1, sizeof(*n));
    if (!n)
        return RKVC_ERR_NOMEM;

    n->mode = cfg->mode;
    n->fd = -1;
    n->timeout_ms = cfg->timeout_ms;
    n->rtp_ssrc = cfg->rtp_ssrc ? cfg->rtp_ssrc : 0x01020304;
    n->rtp_pt = cfg->rtp_payload_type ? cfg->rtp_payload_type : 96;

    n->fd = sock_create();
    if (n->fd < 0) {
        rkvc_free(n);
        return RKVC_ERR_IO;
    }

    if (need_bind) {
        rkvc_err err = sock_bind(n->fd, cfg->bind_ip, cfg->bind_port);
        if (err != RKVC_OK) {
            close(n->fd);
            rkvc_free(n);
            return err;
        }
    }

    if (need_peer) {
        rkvc_err err = set_peer(&n->peer, cfg->peer_ip, cfg->peer_port);
        if (err != RKVC_OK) {
            close(n->fd);
            rkvc_free(n);
            return err;
        }
        n->has_peer = 1;
    }

    if (n->timeout_ms > 0)
        apply_timeout(n->fd, n->timeout_ms);

    RKVC_LOG("net open mode=%s bind=%d peer=%s:%d",
             n->mode == RKVC_NET_UDP ? "udp" : "rtp",
             cfg->bind_port,
             need_peer ? cfg->peer_ip : "-",
             need_peer ? cfg->peer_port : 0);
    *out = n;
    return RKVC_OK;
}

void rkvc_net_close(rkvc_net *net)
{
    if (!net)
        return;
    if (net->fd >= 0)
        close(net->fd);
    rkvc_free(net->reasm_data);
    rkvc_free(net);
}

static ssize_t send_raw(rkvc_net *n, const void *data, size_t len)
{
    if (!n->has_peer)
        return -1;
    return sendto(n->fd, data, len, 0, (struct sockaddr *)&n->peer,
                  sizeof(n->peer));
}

static rkvc_err udp_send_frame(rkvc_net *n, const uint8_t *data, size_t size,
                               int64_t pts)
{
    if (size == 0) {
        uint8_t zero[RKVC_UDP_FRAG_HEADER];
        uint16_t fin_id = htons(RKVC_UDP_FINISH_ID);
        uint16_t fin_tot = htons(RKVC_UDP_FINISH_TOTAL);
        uint32_t fin_len = htonl(0);
        uint64_t fin_pts = htobe64(0);
        memcpy(zero, &fin_id, 2);
        memcpy(zero + 2, &fin_tot, 2);
        memcpy(zero + 4, &fin_len, 4);
        memcpy(zero + 8, &fin_pts, 8);
        if (send_raw(n, zero, sizeof(zero)) < 0)
            return RKVC_ERR_IO;
        return RKVC_OK;
    }

    if (size > RKVC_UDP_MAX_FRAME)
        return RKVC_ERR_INVALID;

    int n_frags = (int)((size + RKVC_UDP_FRAG_PAYLOAD - 1) / RKVC_UDP_FRAG_PAYLOAD);
    if (n_frags < 1 || n_frags > RKVC_UDP_MAX_FRAGS)
        return RKVC_ERR_INVALID;

    size_t off = 0;
    for (int i = 0; i < n_frags; i++) {
        size_t chunk = size - off;
        if (chunk > RKVC_UDP_FRAG_PAYLOAD)
            chunk = RKVC_UDP_FRAG_PAYLOAD;

        uint8_t hdr[RKVC_UDP_FRAG_HEADER];
        uint16_t net_id = htons((uint16_t)i);
        uint16_t net_tot = htons((uint16_t)n_frags);
        uint32_t net_len = htonl((uint32_t)size);
        uint64_t net_pts = htobe64((uint64_t)pts);
        memcpy(hdr, &net_id, 2);
        memcpy(hdr + 2, &net_tot, 2);
        memcpy(hdr + 4, &net_len, 4);
        memcpy(hdr + 8, &net_pts, 8);

        struct iovec iov[2] = {
            { .iov_base = hdr, .iov_len = RKVC_UDP_FRAG_HEADER },
            { .iov_base = (void *)(data + off), .iov_len = chunk },
        };
        struct msghdr msg = {
            .msg_name = &n->peer,
            .msg_namelen = sizeof(n->peer),
            .msg_iov = iov,
            .msg_iovlen = 2,
        };
        ssize_t sent = sendmsg(n->fd, &msg, 0);
        if (sent < 0)
            return RKVC_ERR_IO;
        n->stats.bytes_sent += (uint64_t)sent;
        off += chunk;
    }
    n->stats.pkts_sent++;
    return RKVC_OK;
}

static rkvc_err rtp_send_frame(rkvc_net *n, const uint8_t *data, size_t size,
                               int64_t pts)
{
    if (size > RKVC_RTP_MAX_FRAME)
        return RKVC_ERR_INVALID;

    if (size == 0) {
        uint8_t fin[RKVC_RTP_HEADER_SIZE];
        memset(fin, 0, sizeof(fin));
        fin[0] = 0x80;
        fin[1] = (uint8_t)(0x80 | n->rtp_pt);
        fin[8] = (n->rtp_ssrc >> 24) & 0xFF;
        fin[9] = (n->rtp_ssrc >> 16) & 0xFF;
        fin[10] = (n->rtp_ssrc >> 8) & 0xFF;
        fin[11] = n->rtp_ssrc & 0xFF;
        if (send_raw(n, fin, sizeof(fin)) < 0)
            return RKVC_ERR_IO;
        return RKVC_OK;
    }

    uint32_t ts = (uint32_t)(pts & 0xFFFFFFFFu);
    size_t pos = 0;
    while (pos < size) {
        size_t chunk = size - pos;
        if (chunk > RKVC_RTP_PAYLOAD_MAX)
            chunk = RKVC_RTP_PAYLOAD_MAX;
        int is_last = (pos + chunk >= size);

        uint8_t buf[RKVC_RTP_HEADER_SIZE + RKVC_RTP_PAYLOAD_MAX];
        buf[0] = 0x80;
        buf[1] = (uint8_t)((is_last ? 0x80 : 0) | n->rtp_pt);
        buf[2] = (n->rtp_seq >> 8) & 0xFF;
        buf[3] = n->rtp_seq & 0xFF;
        buf[4] = (ts >> 24) & 0xFF;
        buf[5] = (ts >> 16) & 0xFF;
        buf[6] = (ts >> 8) & 0xFF;
        buf[7] = ts & 0xFF;
        buf[8] = (n->rtp_ssrc >> 24) & 0xFF;
        buf[9] = (n->rtp_ssrc >> 16) & 0xFF;
        buf[10] = (n->rtp_ssrc >> 8) & 0xFF;
        buf[11] = n->rtp_ssrc & 0xFF;
        memcpy(buf + RKVC_RTP_HEADER_SIZE, data + pos, chunk);

        ssize_t sent = send_raw(n, buf, RKVC_RTP_HEADER_SIZE + chunk);
        if (sent < 0)
            return RKVC_ERR_IO;
        n->stats.bytes_sent += (uint64_t)sent;
        n->rtp_seq++;
        pos += chunk;
    }
    n->stats.pkts_sent++;
    return RKVC_OK;
}

rkvc_err rkvc_net_send(rkvc_net *net, const uint8_t *data, size_t size,
                       int64_t pts, int key_frame)
{
    (void)key_frame;
    if (!net || net->fd < 0)
        return RKVC_ERR_INVALID;
    if (!net->has_peer)
        return RKVC_ERR_INVALID;
    if (size > 0 && !data)
        return RKVC_ERR_INVALID;

    if (net->mode == RKVC_NET_UDP)
        return udp_send_frame(net, data, size, pts);
    return rtp_send_frame(net, data, size, pts);
}

rkvc_err rkvc_net_finish(rkvc_net *net)
{
    if (!net)
        return RKVC_ERR_INVALID;
    net->finished = 1;
    if (!net->has_peer)
        return RKVC_OK;
    return rkvc_net_send(net, NULL, 0, 0, 0);
}

static int wait_readable(int fd, int timeout_ms)
{
    if (timeout_ms < 0)
        return 1;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready = poll(&pfd, 1, timeout_ms);
    if (ready == 0)
        return 0;
    if (ready < 0)
        return -1;
    return 1;
}

static void reasm_reset(rkvc_net *n, int frag_total, size_t frame_len, int64_t pts)
{
    if ((size_t)n->reasm_cap < frame_len) {
        rkvc_free(n->reasm_data);
        n->reasm_data = rkvc_malloc(frame_len);
        n->reasm_cap = n->reasm_data ? (int)frame_len : 0;
    }
    n->reasm_size = 0;
    n->reasm_frame_len = n->reasm_data ? (int)frame_len : 0;
    n->reasm_frag_total = frag_total;
    n->reasm_pts = pts;
    memset(n->reasm_mask, 0, sizeof(n->reasm_mask));
    if (n->reasm_data && n->reasm_frame_len > 0)
        memset(n->reasm_data, 0, (size_t)n->reasm_frame_len);
}

static int reasm_complete(const rkvc_net *n)
{
    if (n->reasm_frag_total <= 0)
        return 0;
    for (int i = 0; i < n->reasm_frag_total; i++) {
        if (!(n->reasm_mask[i / 8] & (1u << (i % 8))))
            return 0;
    }
    return 1;
}

static rkvc_err udp_recv_frame(rkvc_net *n, rkvc_buffer **out, int timeout_ms)
{
    uint8_t buf[RKVC_UDP_MAX_PAYLOAD];
    struct sockaddr_in src;

    for (;;) {
        int wr = wait_readable(n->fd, timeout_ms);
        if (wr == 0)
            return RKVC_ERR_AGAIN;
        if (wr < 0)
            return RKVC_ERR_IO;

        socklen_t slen = sizeof(src);
        ssize_t nr = recvfrom(n->fd, buf, sizeof(buf), 0, (struct sockaddr *)&src,
                              &slen);
        if (nr < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return RKVC_ERR_AGAIN;
            return RKVC_ERR_IO;
        }
        if (nr < RKVC_UDP_FRAG_HEADER)
            continue; /* 截断噪声，忽略 */

        uint16_t frag_id, frag_total;
        uint32_t frame_len;
        uint64_t pts_be;
        memcpy(&frag_id, buf, 2);
        memcpy(&frag_total, buf + 2, 2);
        memcpy(&frame_len, buf + 4, 4);
        memcpy(&pts_be, buf + 8, 8);
        frag_id = ntohs(frag_id);
        frag_total = ntohs(frag_total);
        frame_len = ntohl(frame_len);
        int64_t pts = (int64_t)be64toh(pts_be);

        /* 显式结束信号：frag_id=0xffff, frag_total=0, frame_len=0 */
        if (frag_id == RKVC_UDP_FINISH_ID && frag_total == RKVC_UDP_FINISH_TOTAL &&
            frame_len == 0)
            return RKVC_ERR_EOF;

        if (frag_total < 1 || frag_total > RKVC_UDP_MAX_FRAGS)
            continue;
        if (frag_id >= frag_total)
            continue;
        if ((size_t)frame_len > RKVC_UDP_MAX_FRAME)
            continue;
        /* 与分片数一致的上界：末片可短，但总长不得超过 n*payload */
        if ((size_t)frame_len > (size_t)frag_total * RKVC_UDP_FRAG_PAYLOAD)
            continue;

        int payload_len = (int)nr - RKVC_UDP_FRAG_HEADER;
        if (payload_len <= 0)
            continue;

        if (frag_id == 0) {
            if (!n->reasm_data || n->reasm_frag_total != (int)frag_total ||
                n->reasm_frame_len != (int)frame_len || n->reasm_pts != pts)
                reasm_reset(n, (int)frag_total, (size_t)frame_len, pts);
            if (!n->reasm_data || n->reasm_cap <= 0 || n->reasm_frame_len <= 0)
                return RKVC_ERR_NOMEM;
        }

        /* 等首片建立组装缓冲；乱序先到的非 0 片丢弃 */
        if (!n->reasm_data || n->reasm_frag_total <= 0 || n->reasm_frame_len <= 0)
            continue;

        if (n->reasm_frag_total != (int)frag_total || n->reasm_pts != pts)
            continue;
        if (n->reasm_frame_len != (int)frame_len)
            continue;
        if (n->reasm_mask[frag_id / 8] & (1u << (frag_id % 8)))
            continue;

        int offset = (int)frag_id * RKVC_UDP_FRAG_PAYLOAD;
        if (offset < 0 || offset >= n->reasm_frame_len)
            continue;
        int expected = (frag_id + 1 == n->reasm_frag_total)
                           ? (n->reasm_frame_len - offset)
                           : RKVC_UDP_FRAG_PAYLOAD;
        if (expected <= 0 || payload_len != expected)
            continue;
        memcpy(n->reasm_data + offset, buf + RKVC_UDP_FRAG_HEADER,
               (size_t)expected);
        n->reasm_size += expected;
        n->reasm_mask[frag_id / 8] |= (uint8_t)(1u << (frag_id % 8));
        n->stats.bytes_recv += (uint64_t)nr;

        if (!reasm_complete(n))
            continue;
        if (n->reasm_size != n->reasm_frame_len)
            continue;

        rkvc_err err = rkvc_buffer_alloc_bitstream(out, n->reasm_data,
                                                   (size_t)n->reasm_frame_len, 1);
        if (err != RKVC_OK)
            return err;
        rkvc_buffer_set_pts(*out, n->reasm_pts);
        n->stats.pkts_recv++;
        n->reasm_frag_total = 0;
        n->reasm_size = 0;
        n->reasm_frame_len = 0;
        return RKVC_OK;
    }
}

static rkvc_err rtp_recv_frame(rkvc_net *n, rkvc_buffer **out, int timeout_ms)
{
    uint8_t pkt[RKVC_UDP_MAX_PAYLOAD];
    uint8_t *frame = NULL;
    size_t frame_cap = 0;
    size_t frame_size = 0;
    int64_t pts = 0;
    int got_marker = 0;
    struct sockaddr_in src;

    while (!got_marker) {
        int wr = wait_readable(n->fd, timeout_ms);
        if (wr == 0) {
            rkvc_free(frame);
            return RKVC_ERR_AGAIN;
        }
        if (wr < 0) {
            rkvc_free(frame);
            return RKVC_ERR_IO;
        }

        socklen_t slen = sizeof(src);
        ssize_t nr = recvfrom(n->fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&src,
                              &slen);
        if (nr < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (n->finished && frame_size == 0) {
                    rkvc_free(frame);
                    return RKVC_ERR_EOF;
                }
                rkvc_free(frame);
                return RKVC_ERR_AGAIN;
            }
            rkvc_free(frame);
            return RKVC_ERR_IO;
        }
        if (nr < RKVC_RTP_HEADER_SIZE)
            continue;

        int marker = pkt[1] & 0x80;
        int payload_len = (int)nr - RKVC_RTP_HEADER_SIZE;
        pts = ((int64_t)pkt[4] << 24) | ((int64_t)pkt[5] << 16) |
              ((int64_t)pkt[6] << 8) | ((int64_t)pkt[7]);
        /* empty marker-only finish packet (exactly 12B header) */
        if (payload_len == 0 && marker) {
            rkvc_free(frame);
            return RKVC_ERR_EOF;
        }

        if (payload_len <= 0)
            continue;

        if (frame_size + (size_t)payload_len > frame_cap) {
            if (frame_size + (size_t)payload_len > RKVC_RTP_MAX_FRAME) {
                rkvc_free(frame);
                return RKVC_ERR_INVALID;
            }
            size_t nc = frame_cap ? frame_cap * 2 : 4096;
            while (nc < frame_size + (size_t)payload_len)
                nc *= 2;
            uint8_t *nf = rkvc_malloc(nc);
            if (!nf) {
                rkvc_free(frame);
                return RKVC_ERR_NOMEM;
            }
            if (frame && frame_size)
                memcpy(nf, frame, frame_size);
            rkvc_free(frame);
            frame = nf;
            frame_cap = nc;
        }
        memcpy(frame + frame_size, pkt + RKVC_RTP_HEADER_SIZE,
               (size_t)payload_len);
        frame_size += (size_t)payload_len;
        n->stats.bytes_recv += (uint64_t)nr;
        if (marker)
            got_marker = 1;
    }

    rkvc_err err = rkvc_buffer_alloc_bitstream(out, frame, frame_size, 1);
    rkvc_free(frame);
    if (err != RKVC_OK)
        return err;
    rkvc_buffer_set_pts(*out, pts);
    n->stats.pkts_recv++;
    return RKVC_OK;
}

rkvc_err rkvc_net_recv(rkvc_net *net, rkvc_buffer **out, int timeout_ms)
{
    if (!net || !out || net->fd < 0)
        return RKVC_ERR_INVALID;
    *out = NULL;
    if (timeout_ms < 0)
        timeout_ms = net->timeout_ms;

    if (net->mode == RKVC_NET_UDP)
        return udp_recv_frame(net, out, timeout_ms);
    return rtp_recv_frame(net, out, timeout_ms);
}

rkvc_err rkvc_net_get_stats(const rkvc_net *net, rkvc_net_stats *out)
{
    if (!net || !out)
        return RKVC_ERR_INVALID;
    *out = net->stats;
    return RKVC_OK;
}
