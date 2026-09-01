# 模型签名信任根（trust root）

`.rkmodel` 容器的 Ed25519 验签公钥在编译期固定进 `librkvc`：

- **dev 根**：`dev-root.pub`（64 字符 hex，首行注释为 key_id）。
  缺省时 CMake 配置期自动生成临时开发根到 `<build>/trust/`（仅本机构建
  有效，不入库）。本仓不提交真实 dev 根；团队共享 dev 根时由维护者放置。
- **prod 根**：不入库。发布流程离线生成（HSM/KMS 或气隙机），经
  `-DRKVC_TRUST_PRODUCTION=ON -DRKVC_TRUST_PUBKEY_HEX=<64 hex>` 传入；
  缺失即配置失败。prod 构建中 unsigned/开发签名模型一律 untrusted。

工具（主机侧，需 libsodium 运行库）：

```bash
PYTHONPATH=tools python3 -m rkvc_build.rkmodel keygen keys/dev-root   # 生成密钥对
PYTHONPATH=tools python3 -m rkvc_build.rkmodel sign m.rkmodel --key keys/dev-root.sec
PYTHONPATH=tools python3 -m rkvc_build.rkmodel verify m.rkmodel --pubkey keys/dev-root.pub
```

`.sec` 私钥以 0600 落盘，**永不提交**。密钥轮换 = 更换编译期公钥 +
重签模型，二者随同一发布列车交付。
