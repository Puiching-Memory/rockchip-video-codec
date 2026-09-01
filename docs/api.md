# API

应用只需包含 rkvc/rkvc.h。

| 对象         | 责任                                          |
| ------------ | --------------------------------------------- |
| rkvc_context | 设备探测、后端与模型注册表                    |
| rkvc_request | 输入、输出、操作、codec 与约束                |
| rkvc_job     | 一次已规划执行的 start/wait/cancel 与流式 I/O |
| rkvc_frame   | 引用计数的 HOST 或 DMA-BUF 媒体数据           |
| rkvc_diag    | 可输出文本或 JSON 的失败原因链                |

所有可扩展公开结构以 rkvc_header 开头。调用方使用对应 init 函数填入
struct_size 和 api_version。句柄均为 opaque。

文件端点通过 RKVC_ENDPOINT_FILE 和 URI 描述；流式端点使用
rkvc_job_push、rkvc_job_pull 和 rkvc_job_push_eos。RKVC_STATUS_AGAIN
表示有界队列暂时满或空，不是永久错误。

编码输入帧可在 `rkvc_frame_desc` 中携带最多
`RKVC_ROI_MAX_REGIONS` 个 `rkvc_roi_region`。`rkvc_frame_wrap()` 会深拷贝
ROI 数组，像素载荷仍按原契约借用；ROI 坐标使用可见像素，`qp_delta` 范围
为 -51..51。`rkvc_encode_control` 的非零 bitrate/GOP 会从该帧开始生效，
`force_idr` 只作用于该帧。MPP 后端将 ROI 对齐到 16 像素并映射到
`KEY_ROI_DATA`。

后端实现者额外包含 rkvc/backend.h。后端 DSO 只导出
rkvc_backend_query()，并通过 ABI 版本、能力位和节点工厂描述自身。

完整函数、字段和所有权约定以公共头及 Doxygen 输出为准。
