# 授权（1机1码）

rkvc 可选启用基于 **Ed25519 非对称签名** 的离线授权，实现「1机1码」：每台设备绑定唯一机器码，
发码端用私钥签发注册码，库内嵌公钥本地校验，无需联网。

## 原理

```
┌─────────────┐     机器码（用户报告）      ┌──────────────┐
│  目标设备    │ ──────────────────────────→ │  发码端（你） │
│  rkvc_lic   │                              │  持有私钥     │
│ machine-id  │ ←────────────────────────── │  rkvc_lic    │
└─────────────┘     注册码（base64）          │  issue       │
       │                                       └──────────────┘
       ▼
  license.lic ──→ librkvc 校验（签名 + 机器码）
```

- **机器码**：本机硬件指纹经 SHA-256 派生的 64 位十六进制串。
  来源优先级：设备树序列号（`/proc/device-tree/serial-number`）→ Rockchip OTP → 网卡 MAC。仅取硬件级指纹，不含 `/etc/machine-id` 等可复制文件，避免克隆 SD 卡导致两机同码。
- **注册码**：`{magic, product, 机器码}` 的 Ed25519 签名，base64 编码，约 140 字符。授权一经签发永久有效，不含有效期字段。
- **防伪造**：库内只嵌入公钥；私钥离线保管。攻击者反编译拿到公钥也无法伪造注册码。

## 构建

授权模块默认关闭，需显式开启（依赖 **libsodium**，原生支持 Ed25519，随子模块源码静态构建）：

```bash
# 先初始化 libsodium 子模块并构建静态库（仅需一次）
git submodule update --init third_party/libsodium
./scripts/install-libsodium.sh

# 开启授权 SDK：编译授权模块 + rkvc_lic 工具，rkvc_init() 时强制校验，
# 无有效授权则拒绝初始化
cmake --preset default -DRKVC_ENABLE_LICENSE=ON \
      -DRKVC_LICENSE_PUBKEY_FILE=tools/keys/public.key
```

`RKVC_LICENSE_PUBKEY_FILE` 指向 32 字节公钥二进制文件，CMake 配置时自动读取并生成
`license_pubkey.c` 编译进 librkvc。若未指定，使用 `lib/license_pubkey.c` 中的**演示公钥**
（CMake 会发出 WARNING，不可用于生产）。

开启后自动构建 `rkvc_lic` 工具（`.build/release/rkvc_lic`），静态链接 libsodium，可独立部署到发码机。

> **依赖**：`third_party/libsodium` 子模块（1.0.22，原生支持 Ed25519）。
> 无需安装系统 OpenSSL/mbedTLS，libsodium 随项目源码树编译为静态库 `libsodium.a`。

## 使用流程

### 1. 生成密钥对（仅一次）

```bash
mkdir keys && .build/release/rkvc_lic genkey -o keys
```

产出 `keys/secret.key`（64 字节原始私钥，离线保管）、`keys/public.key`（32 字节公钥）
与 `keys/public_key.h`（C 数组，替换 `lib/license_pubkey.c`）。

> ⚠️ **生产部署前务必**：用 `public_key.h` 覆盖 `lib/license_pubkey.c` 并重新编译库。
> 仓库自带的 `tools/keys/demo_secret.key` 是演示密钥，**不可用于生产**。

### 2. 采集设备机器码

在目标设备上运行：

```bash
.build/release/rkvc_lic machine-id
# 44fa96957e0e0fc523ca5afdcb1da8e0fc74512ccb827913f89d3289d496a9a0
```

### 3. 签发注册码

```bash
# 永久授权
.build/release/rkvc_lic issue -m <机器码> -k keys/secret.key -o license.lic
```

### 4. 部署注册码

将 `license.lic` 放到设备默认路径，或用环境变量指定：

```bash
# 默认路径（二选一）
cp license.lic ~/.config/rkvc/license.lic

# 或指定路径
export RKVC_LICENSE_FILE=/etc/rkvc/license.lic
```

### 5. 校验

```bash
# 工具端诊断（-k 可传 public.key 或 secret.key，后者自动派生公钥）
.build/release/rkvc_lic verify -f license.lic -k keys/public.key
```

或通过库 API（`#include <rkvc/rkvc.h>`）：

```c
rkvc_license_info info;
rkvc_err e = rkvc_license_check(&info);
if (e != RKVC_OK) {
    fprintf(stderr, "授权失败: %s\n", rkvc_err_str(e));
    return 1;
}
```

开启 `RKVC_ENABLE_LICENSE` 后，`rkvc_init()` 自动执行此校验，失败则返回 `RKVC_ERR_UNLICENSED` / `RKVC_ERR_LICENSE`。

## API

| 函数 | 说明 |
| --- | --- |
| `rkvc_machine_id(out, size)` | 计算本机机器码（64 hex 字符） |
| `rkvc_license_check(&info)` | 用默认路径校验，返回 `RKVC_OK` / 错误码 |
| `rkvc_license_verify_file(path, &info)` | 校验指定授权文件 |
| `rkvc_license_verify_blob(blob, len, &info)` | 校验已解码的二进制 blob（104 字节） |
| `rkvc_license_default_path(out, size)` | 解析默认授权文件路径 |

## 密钥格式

libsodium 使用 **原始二进制** 密钥（非 PEM）：

- `secret.key`：64 字节 Ed25519 私钥（`crypto_sign_SECRETKEYBYTES`）。
- `public.key`：32 字节 Ed25519 公钥（`crypto_sign_PUBLICKEYBYTES`）。
- 库内嵌公钥为 32 字节 C 数组（`lib/license_pubkey.c`），字节级与标准 Ed25519 公钥一致。

Ed25519 为标准曲线（RFC 8032），公钥/签名字节与所用密码库无关，可跨库互验。

## 安全注意事项

- 私钥（`secret.key` / `*.pem`）已在 `.gitignore` 中排除，切勿提交或部署到设备。
- 机器码以**设备树序列号为主**（RK3588 由 U-Boot 从 OTP 派生，芯片级唯一且重刷不变）。
- OTP 直接读取需 root 权限；设备树序列号普通用户可读，为默认首选源。
- 授权为离线方案，无远程吊销能力。如需在线吊销/续期，需扩展服务端。
