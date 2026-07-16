/**
 * @file ffmpeg_util.c
 * @brief FFmpeg libavutil 集成：日志、时间、字典/选项、哈希。
 */

#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

/* ── 日志 (av_log_set_callback) ───────────────────────────────────── */

static int s_rkvc_log_level = AV_LOG_INFO;
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;

void rkvc_set_log_level(int level)
{
    pthread_mutex_lock(&s_log_lock);
    s_rkvc_log_level = level;
    av_log_set_level(level);
    pthread_mutex_unlock(&s_log_lock);
}

int rkvc_get_log_level(void)
{
    int level;

    pthread_mutex_lock(&s_log_lock);
    level = s_rkvc_log_level;
    pthread_mutex_unlock(&s_log_lock);
    return level;
}

void rkvc_log_print(int level, const char *fmt, ...)
{
    va_list ap;

    pthread_mutex_lock(&s_log_lock);
    if (level > s_rkvc_log_level) {
        pthread_mutex_unlock(&s_log_lock);
        return;
    }

    flockfile(stderr);
    fprintf(stderr, "[rkvc] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    funlockfile(stderr);
    pthread_mutex_unlock(&s_log_lock);
}

static void rkvc_av_log_callback(void *avcl, int level, const char *fmt,
                                 va_list vl)
{
    const char *prefix = "ffmpeg";

    if (level > av_log_get_level())
        return;

    if (avcl) {
        const AVClass *cls = *(const AVClass **)avcl;
        if (cls && cls->class_name)
            prefix = cls->class_name;
    }

    pthread_mutex_lock(&s_log_lock);
    flockfile(stderr);
    fprintf(stderr, "[rkvc:%s] ", prefix);
    vfprintf(stderr, fmt, vl);
    funlockfile(stderr);
    pthread_mutex_unlock(&s_log_lock);
}

void rkvc_ffmpeg_utils_init(void)
{
    av_log_set_callback(rkvc_av_log_callback);
    av_log_set_level(s_rkvc_log_level);
}

/* ── 时间 (av_gettime_relative) ───────────────────────────────────── */

int64_t rkvc_now_us(void)
{
    return av_gettime_relative();
}

/* ── 字典 / 选项 (AVDictionary, av_opt_set) ─────────────────────────── */

rkvc_err rkvc_dict_parse_opts(AVDictionary **dict, const char *opts)
{
    if (!dict)
        return RKVC_ERR_INVALID;
    if (!opts || opts[0] == '\0')
        return RKVC_OK;

    int ret = av_dict_parse_string(dict, opts, "=", ":", 0);
    return ret < 0 ? rkvc_from_averror(ret) : RKVC_OK;
}

void rkvc_dict_free(AVDictionary **dict)
{
    av_dict_free(dict);
}

rkvc_err rkvc_dict_set_int(AVDictionary **dict, const char *key, int64_t val)
{
    if (!dict || !key)
        return RKVC_ERR_INVALID;

    int ret = av_dict_set_int(dict, key, val, 0);
    return ret < 0 ? rkvc_from_averror(ret) : RKVC_OK;
}

static void rkvc_log_unused_dict(const char *ctx_name, AVDictionary **dict)
{
    const AVDictionaryEntry *e = NULL;

    if (!dict || !*dict)
        return;

    while ((e = av_dict_get(*dict, "", e, AV_DICT_IGNORE_SUFFIX)) != NULL) {
        rkvc_log_print(AV_LOG_WARNING,
                       "unused %s option: %s=%s", ctx_name, e->key, e->value);
    }
}

rkvc_err rkvc_opt_set_dict(void *obj, AVDictionary **dict)
{
    if (!obj)
        return RKVC_ERR_INVALID;
    if (!dict || !*dict)
        return RKVC_OK;

    int ret = av_opt_set_dict(obj, dict);
    return ret < 0 ? rkvc_from_averror(ret) : RKVC_OK;
}

rkvc_err rkvc_codec_open2(AVCodecContext *ctx, const AVCodec *codec,
                          AVDictionary **opts, const char *ctx_name)
{
    int ret;

    if (!ctx || !codec)
        return RKVC_ERR_INVALID;

    ret = avcodec_open2(ctx, codec, opts);
    if (ret < 0)
        return rkvc_from_averror(ret);

    rkvc_log_unused_dict(ctx_name ? ctx_name : "codec", opts);
    return RKVC_OK;
}

rkvc_err rkvc_format_open_input(AVFormatContext **fmt, const char *path,
                                AVDictionary **opts)
{
    int ret;

    if (!fmt || !path)
        return RKVC_ERR_INVALID;

    ret = avformat_open_input(fmt, path, NULL, opts);
    if (ret < 0)
        return rkvc_from_averror(ret);

    rkvc_log_unused_dict("demux", opts);
    return RKVC_OK;
}

/* ── 哈希 (av_hash_*) ──────────────────────────────────────────────── */

rkvc_err rkvc_hash_buffer(const char *algo, const uint8_t *data, size_t len,
                          char *out_hex, size_t out_size)
{
    struct AVHashContext *ctx = NULL;
    int hash_size;
    rkvc_err err = RKVC_OK;

    if (!algo || !data || !out_hex || out_size == 0)
        return RKVC_ERR_INVALID;

    if (av_hash_alloc(&ctx, algo) < 0)
        return RKVC_ERR_INVALID;

    hash_size = av_hash_get_size(ctx);
    if (hash_size <= 0 || (size_t)hash_size * 2 + 1 > out_size) {
        err = RKVC_ERR_INVALID;
        goto done;
    }

    av_hash_init(ctx);
    av_hash_update(ctx, data, len);
    av_hash_final_hex(ctx, (uint8_t *)out_hex, hash_size * 2 + 1);

done:
    av_hash_freep(&ctx);
    return err;
}

rkvc_err rkvc_hash_file(const char *path, const char *algo,
                        char *out_hex, size_t out_size)
{
    FILE *fp;
    struct AVHashContext *ctx = NULL;
    uint8_t buf[65536];
    int hash_size;
    rkvc_err err = RKVC_OK;

    if (!path || !algo || !out_hex || out_size == 0)
        return RKVC_ERR_INVALID;

    fp = fopen(path, "rb");
    if (!fp)
        return RKVC_ERR_IO;

    if (av_hash_alloc(&ctx, algo) < 0) {
        err = RKVC_ERR_INVALID;
        goto done;
    }

    hash_size = av_hash_get_size(ctx);
    if (hash_size <= 0 || (size_t)hash_size * 2 + 1 > out_size) {
        err = RKVC_ERR_INVALID;
        goto done;
    }

    av_hash_init(ctx);
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), fp);
        if (n > 0)
            av_hash_update(ctx, buf, n);
        if (n < sizeof(buf)) {
            if (ferror(fp))
                err = RKVC_ERR_IO;
            break;
        }
    }

    if (err == RKVC_OK)
        av_hash_final_hex(ctx, (uint8_t *)out_hex, hash_size * 2 + 1);

done:
    av_hash_freep(&ctx);
    fclose(fp);
    return err;
}
