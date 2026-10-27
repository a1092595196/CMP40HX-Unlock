#!/bin/sh
# uninstall-efi.sh — 完全移除 NVPermissiveEFI (回到工厂状态, 下次重启生效)
set -e
ESP=${ESP:-/boot/efi}
F=${F:-/etc/grub.d/40-cmp40hx}

rm -f "$F"
rm -rf "$ESP/EFI/UNLOCK"
if command -v update-grub >/dev/null 2>&1; then
    update-grub
fi
echo "已移除: $F, $ESP/EFI/UNLOCK, 并已重新生成 grub.cfg"
echo "下次重启即恢复工厂状态 (寄存器由 GSP 每次启动重新初始化)。"
