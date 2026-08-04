/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file port.c
 * @brief 有界端口队列 (bounded queue)，存储层使用 AVFifo。
 */

#include "internal.h"

#include <errno.h>
#include <time.h>

struct rkvc_port_queue {
    AVFifo           *fifo;
    int               capacity;
    pthread_mutex_t   lock;
    pthread_cond_t    not_full;
    pthread_cond_t    not_empty;
};

rkvc_port_queue *rkvc_port_queue_create(int capacity)
{
    if (capacity <= 0)
        capacity = RKVC_PORT_QUEUE_DEFAULT;

    rkvc_port_queue *q = rkvc_calloc(1, sizeof(*q));
    if (!q)
        return NULL;

    q->capacity = capacity;
    q->fifo = av_fifo_alloc2((size_t)capacity, sizeof(rkvc_buffer *), 0);
    if (!q->fifo) {
        rkvc_free(q);
        return NULL;
    }

    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

void rkvc_port_queue_destroy(rkvc_port_queue *q)
{
    if (!q)
        return;

    pthread_mutex_lock(&q->lock);
    while (av_fifo_can_read(q->fifo) > 0) {
        rkvc_buffer *b = NULL;
        av_fifo_read(q->fifo, &b, 1);
        rkvc_buffer_unref(b);
    }
    pthread_mutex_unlock(&q->lock);

    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    av_fifo_freep2(&q->fifo);
    rkvc_free(q);
}

rkvc_err rkvc_port_queue_push(rkvc_port_queue *q, rkvc_buffer *buf)
{
    rkvc_buffer *ref;

    if (!q || !buf)
        return RKVC_ERR_INVALID;

    pthread_mutex_lock(&q->lock);
    if (av_fifo_can_write(q->fifo) == 0) {
        pthread_mutex_unlock(&q->lock);
        return RKVC_ERR_AGAIN;
    }

    ref = rkvc_buffer_ref(buf);
    if (!ref) {
        pthread_mutex_unlock(&q->lock);
        return RKVC_ERR_NOMEM;
    }

    if (av_fifo_write(q->fifo, &ref, 1) < 0) {
        rkvc_buffer_unref(ref);
        pthread_mutex_unlock(&q->lock);
        return RKVC_ERR_INTERNAL;
    }

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return RKVC_OK;
}

static int timespec_add_ms(struct timespec *ts, int ms)
{
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
    return 0;
}

rkvc_err rkvc_port_queue_pull(rkvc_port_queue *q, rkvc_buffer **buf,
                              int timeout_ms)
{
    if (!q || !buf)
        return RKVC_ERR_INVALID;

    *buf = NULL;
    pthread_mutex_lock(&q->lock);

    while (av_fifo_can_read(q->fifo) == 0) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&q->lock);
            return RKVC_ERR_AGAIN;
        }

        if (timeout_ms < 0) {
            pthread_cond_wait(&q->not_empty, &q->lock);
            continue;
        }

        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        timespec_add_ms(&deadline, timeout_ms);
        int rc = pthread_cond_timedwait(&q->not_empty, &q->lock, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&q->lock);
            return RKVC_ERR_AGAIN;
        }
    }

    if (av_fifo_read(q->fifo, buf, 1) < 0) {
        pthread_mutex_unlock(&q->lock);
        return RKVC_ERR_INTERNAL;
    }

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return RKVC_OK;
}

rkvc_err rkvc_port_push(rkvc_port *port, rkvc_buffer *buf)
{
    if (!port || !port->queue)
        return RKVC_ERR_INVALID;
    return rkvc_port_queue_push(port->queue, buf);
}

rkvc_err rkvc_port_pull(rkvc_port *port, rkvc_buffer **buf, int timeout_ms)
{
    if (!port || !port->queue)
        return RKVC_ERR_INVALID;
    return rkvc_port_queue_pull(port->queue, buf, timeout_ms);
}
