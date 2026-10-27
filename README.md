# CMP40HX-Unlock(PVE / GRUB UEFI 前置解锁)

NVIDIA **CMP40HX**(TU106,`0x10de:0x1f0b`)的 UEFI 前置阶段寄存器解锁方案,
面向 **Proxmox VE 9(UEFI 引导)物理主机**,目标:

- 解锁 **Tensor Core / FP16**(HGEMM ~50 TFLOPS)与 PCIe Gen2 x16;
- 与 **vGPU(nvidia-vgpu)共存** —— 所有写操作发生在内核加载之前,
  不触发运行时 FLR / GSP 重启 / 驱动重协商,这是与"运行时重路径"方案
  (会搞挂 vGPU 会话)的本质区别;
- 与 **turing-write 0.3**(`turing_write.targets=all` 内核模块路线)互不冲突,
  实验期间建议只启用其一。

上游来源:**NVPermissiveEFI**(PZH1gdmu/CMP40HX-Unlock,MIT)。
本仓库在其基础上做了三处改动并打包:

1. `AUTO_MODE` 编译开关 —— 免 LoadOptions,直接编译出固定模式的 .efi;
2. BOOT0 架构白名单加入 **Turing `0x16`**(TU106 实卡读回 `0x16xxxxxx`,
   上游只认 `0x17/0x1a/0x0f/0x0fffffff`,真卡会被拒绝);
3. **GRUB 集成**:`install-efi.sh` / `uninstall-efi.sh`,ESP 安装 +
   `grub.d/40-cmp40hx` 菜单项生成(PVE 原条目体抽取 + `exec` + root 保存/恢复)。

寄存器目标值与 ngthaihoc/CMP30HXmodtoGEN2 的公开 spec 交叉核对一致
(`SS0=0x88888888` 等)。

## 模式

| 模式 | 文件 | 行为 |
|---|---|---|
| `inspect`(默认) | `NVPermissiveEFI.efi` | 只读:打印 BOOT0 / 当前寄存器值,不写 |
| `compute` | `NVPermissiveEFI-COMPUTE.efi` | 全套计算寄存器 + **1 次**双重 SBR(推荐起步) |
| `full` | `NVPermissiveEFI-FULL.efi` | compute → graphics → gr-reset → gr-acr,**3 次**双重 SBR(对齐 Windows 完整解锁) |

## 寄存器写入(BAR0)

| 偏移 | 值 | 作用 |
|---|---|---|
| `0x008E1B0` XP3G_PLM | `0xFFFFFFFF` | 功耗/性能锁 PLM 清除 |
| `0x008872C` XVE_OVR | `0x6` | PCIe Gen2 覆盖 |
| `0x409650` FECS_PLM | `0xFFFFFFFF` | FECS 性能锁清除 |
| `0x409664` SS0 | `0x88888888` | Tensor/FP16 使能位(核心) |
| `0x40966C` SS1 | `0x8` | 配套状态位 |
| `0x0088610` XVE_08610 | `0x1001` | 链路配置 |
| `0x409500` / `0x40960C` / `0x409610` / `0x409614` | `0xFFFFFFFF` | consumer chain 清除 |

`graphics` 阶段追加 `0x419B54=0xFF`、`0x600000=0xFFFFFFFF`(显示探测)。
SBR = 两次 PCI 二次总线复位(桥 `CTL 0x3e` 位 `0x40`),间隔 100ms 沉降。

## 构建

依赖:`gcc`、GNU `ld`(支持 `-mi386pep`)、`objcopy`、`python3`(可选,`tools/elf2blob.py`)。

```bash
make                       # -> build/NVPermissiveEFI.efi (inspect, 默认)
make AUTO=REQUEST_COMPUTE  # -> build/NVPermissiveEFI-COMPUTE.efi
make AUTO=REQUEST_FULL     # -> build/NVPermissiveEFI-FULL.efi
```

核心逻辑(`src/core_40hx.c`)编译成 SysV 目标后 `objcopy` 成 blob 嵌入
`.core` 段,`src/main.c` 在 UEFI 里定位 `0x10de:0x1f0b`(PCI RootBridgeIO)、
读 BAR0/BOOT0 校验后运行核心。

仓库内附带三个已构建的 .efi,校验:

```bash
sha256sum -c CHECKSUMS.sha256
```

## 安装(PVE 主机)

前置:**UEFI 引导**;**Secure Boot 必须关闭**(脚本用 `mokutil --sb-state` 检查,
开启会直接拒绝)。

```bash
sh install-efi.sh
```

1. 三个 .efi → `/boot/efi/EFI/UNLOCK/`(可用 `ESP=<挂载点>` 覆盖);
2. 从 `grub.cfg` 抽取 PVE 原条目,生成 `/etc/grub.d/40-cmp40hx`:
   - `PVE + CMP40HX UNLOCK (compute, ...)`
   - `PVE + CMP40HX UNLOCK (full, ...)`
   - 原 PVE 条目保留 = 不解锁(工厂状态),随时可退回;
3. `update-grub`。

**无头机**:不需要在 GRUB 菜单按键,直接把解锁项设为默认:

```bash
echo "GRUB_DEFAULT='PVE + CMP40HX UNLOCK (compute, 最小解锁: 全套寄存器 + 1 次 SBR)'" \
  >> /etc/default/grub && update-grub
```

解锁画面 60 秒无按键自动继续,全程无人值守。

**回退**:

```bash
sed -i '/^GRUB_DEFAULT=/d' /etc/default/grub && update-grub   # 回普通 PVE 启动
sh uninstall-efi.sh                                           # 彻底移除
```

## 验证

```bash
dmesg | grep -iE 'nvidia|pcieport' | head -30
journalctl -b -u nvidia-vgpud | tail -20
nvidia-smi -q | grep -iE 'product name|vgpu|bus id'
```

FP16 吞吐用 CUDA 基准(HGEMM / pytorch `float16 matmul`)实测,
满速预期 ~50 TFLOPS 量级(受 Gen2 x16 ~6.4 GB/s 带宽与显存影响)。

## 安全性说明

- 不写 VBIOS / SPI ROM,不改 eFuse,不改主板 BIOS;
- 寄存器在每次上电/GSP 初始化时重新设定,本方案只是每次开机在
  GSP 之前抢先写入,无持久化;
- 最坏情况:EFI 阶段失败 → 直接跳过解锁正常进 PVE(工厂状态),
  不会卡死、不会变砖;
- SBR 是标准 PCI 二次总线复位,等效拔插卡,发生在内核加载前,
  不影响任何已加载的驱动。

## 许可

MIT。上游 NVPermissiveEFI 版权与许可见 [LICENSE](LICENSE);
寄存器目标值参考公开资料(ngthaihoc/CMP30HXmodtoGEN2 spec),仅数值核对,无代码复制。
