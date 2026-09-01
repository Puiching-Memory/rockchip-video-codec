# 架构

## 唯一运行路径

~~~text
application / rkvc CLI
        |
        v
rkvc_context -> backend registry + model registry + device probe
        |
        v
rkvc_request -> planner -> linear graph
        |
        v
source -> decode -> transform -> encode -> sink
        |
        v
rkvc_job / executor / bounded queues
~~~

核心库只包含上下文、规划、生命周期、帧所有权、执行器、诊断和注册表。
媒体实现通过 backend.h 的版本化工厂接口注册。核心不链接 FFmpeg、MPP、
RGA 或 RKNN 类型。

当前图执行器是一条线性媒体链，不宣称支持任意 DAG。每个请求由 operation
确定必要阶段，每个阶段从已探测工厂中稳定排序；configure/open 失败只替换
失败阶段的候选。节点间队列有界，执行器统一处理背压、EOS、flush 和取消。

## 生命周期

~~~text
context_create
  -> job_create: validate + plan + create + configure
  -> job_start: open + start executor
  -> push/pull or file source/sink
  -> wait/cancel
  -> job_destroy
  -> context_destroy
~~~

configure 不打开设备；open 才允许分配硬件资源。任一步失败都逆序关闭和销毁
已经创建的节点。

## 安装边界

~~~text
bin/rkvc
lib/librkvc.so.0
lib/rkvc/backends/*.so
include/rkvc/*.h
share/rkvc/models/*.rkmodel
~~~

CMake install tree 是包内容的唯一来源。手工复制式打包脚本不属于安装边界。
