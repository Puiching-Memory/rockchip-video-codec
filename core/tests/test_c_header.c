/* 纯 C 消费者：rkvc.h 必须能被 C 编译器直接包含并使用。C++ 编译器看不到这类
 * 问题——历史回归是 C++ 专属的 noexcept 泄漏进 extern "C" 原型，只有 C 编译
 * 器报 "expected ';', ',' or ')' before 'noexcept'"，包内 SDK 的 C 冒烟与
 * examples/ 里的纯 C 样板全编不过。 */
#include "rkvc/rkvc.h"

#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
#error "本用例必须由 C 编译器编译（纯 C 消费者检查）"
#endif

static void release_cb(void *release_ctx) {
    (void)release_ctx;
}

int main(void) {
    /* 回调参数要能直接写成 C 的函数指针类型——回归点就在这里。 */
    void (*release)(void *release_ctx) RKVC_NOEXCEPT = release_cb;
    rkvc_context_options opts;
    rkvc_session_request req;
    rkvc_frame_desc desc;

    if (rkvc_status_str(RKVC_OK) == NULL ||
        rkvc_status_str(RKVC_INTEGRITY) == NULL) {
        fputs("rkvc_status_str 返回空\n", stderr);
        return 1;
    }

    /* struct_size/version 是 C ABI 的前向兼容约定，C 侧同样要能填。 */
    rkvc_context_options_init(&opts, sizeof(opts));
    if (opts.struct_size != sizeof(opts) ||
        opts.version != (uint32_t)RKVC_ABI_VERSION ||
        opts.backend_dirs != NULL || opts.backend_dir_count != 0) {
        fputs("rkvc_context_options_init 未按约定初始化\n", stderr);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.struct_size = sizeof(req);
    req.version = (uint32_t)RKVC_ABI_VERSION;
    req.operation = RKVC_OP_ENCODE;
    req.codec = RKVC_CODEC_HEVC;
    req.policy = RKVC_POLICY_BALANCED;
    req.quality.qp = 26;
    req.input.kind = RKVC_ENDPOINT_STREAM;
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = "out.h265";

    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.version = (uint32_t)RKVC_ABI_VERSION;
    desc.spec.fmt = RKVC_FRAME_FMT_NV12;
    desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    desc.fd = -1;
    desc.flags = RKVC_FRAME_FLAG_KEYFRAME;

    /* 只验头文件在 C 语境下的可用性，不建会话（那是 test_c_abi 的事）。 */
    if (req.struct_size != sizeof(req) || req.output.uri == NULL ||
        desc.fd != -1 || !(desc.flags & RKVC_FRAME_FLAG_KEYFRAME)) {
        return 1;
    }
    (void)release;
    return 0;
}
