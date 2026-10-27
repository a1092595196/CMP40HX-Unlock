/*
 * core_40hx.c — TU106 (CMP 40HX) permissive core for the UEFI unlock shell.
 *
 * Values are taken from the empirically verified 40HX unlock chain
 * (Linux SEC2-booter ROP, same register family as GA102 NVPermissive):
 *   XP3G_PLM  0x8E1B0  = 0xFFFFFFFF   (compute unlock, verified)
 *   XVE_OVR   0x8872C  = 0x00000006   (PCIe Gen2 unlock, verified)
 *   FECS_PLM  0x409650 = 0xFFFFFFFF   (FECS feature lock open, verified)
 *   SS0       0x409664 = 0x88888888   (SM_SPEED_SELECT full - tensor target)
 *   SS1       0x40966C = 0x00000008   (SM_SPEED_SELECT_1 full)
 *
 * Mode flow mirrors upstream: compute(+SBR) -> graphics(+SBR) -> gr-reset(+SBR)
 * -> gr-acr(no SBR). graphics/gr-acr re-write SS0/SS1 so the final state held
 * after the last SBR is the full-speed override (SBR clears PCI endpoint
 * state, GPU internal security-domain state persists - verify via inspect).
 */
#include "core_abi.h"
#include <stdint.h>

#define WR(g, off, val)                                                    \
    do {                                                                   \
        *(volatile uint32_t *)((uintptr_t)(g)->mmio + (off)) = (uint32_t)(val); \
    } while (0)
#define RD(g, off) (*(volatile uint32_t *)((uintptr_t)(g)->mmio + (off)))

#define XP3G_PLM_OFF 0x008E1B0U
#define XVE_OVR_OFF  0x008872CU
#define FECS_PLM_OFF 0x00409650U
#define FECS_SS0_OFF 0x00409664U
#define FECS_SS1_OFF 0x0040966CU
#define FECS_SS0_RD_OFF 0x00409668U
#define FUSE_DISP_OFF   0x00021C04U
/* v29.56: XVE/HUBMMU-domain 0x88610 empirically WRITABLE + STICKY on 40HX
 * (bridge write 0x1 -> 0x1001 = RTX2070 vBIOS value; readback holds, system
 * stable across seconds). Semantics unpublished; written for 2070 parity. */
#define XVE_08610_OFF   0x00088610U
#define XVE_08610_2070  0x00001001U
/* v29.65: consumer-chain writable points found on-machine (bridge scan
 * 0x409100-0x4096FF, all STICKY, not GSP-covered at runtime). Hypothesis:
 * readout-adjacent group is consumed ONCE at GSP init - EFI-time write may
 * affect the boot verdict even though runtime writes do not. */
#define FECS_09500_OFF  0x00409500U
#define FECS_0960C_OFF  0x0040960CU
#define FECS_09610_OFF  0x00409610U
#define FECS_09614_OFF  0x00409614U
#define CONSUMER_INIT   0xFFFFFFFFU

static void write_full_set(struct gpu_dev *g)
{
    WR(g, XP3G_PLM_OFF, 0xFFFFFFFFU);
    WR(g, XVE_OVR_OFF, 0x00000006U);
    WR(g, FECS_PLM_OFF, 0xFFFFFFFFU);
    WR(g, FECS_SS0_OFF, 0x88888888U);
    WR(g, FECS_SS1_OFF, 0x00000008U);
    WR(g, XVE_08610_OFF, XVE_08610_2070);   /* sticky diff register (2070 parity) */
    /* v29.65: EFI-time consumer-chain injection (runtime writes hold but
     * don't affect verdict - these may be boot-time-consumed instead) */
    WR(g, FECS_09500_OFF, CONSUMER_INIT);
    WR(g, FECS_0960C_OFF, CONSUMER_INIT);
    WR(g, FECS_09610_OFF, CONSUMER_INIT);
    WR(g, FECS_09614_OFF, CONSUMER_INIT);
    g->log(g, "40HX: full set written (XP3G/XVE/FECS/SS0/SS1/08610/consumer-chain)\n");
}

int do_permissive(struct gpu_dev *g, int mode)
{
    if (!g || !g->mmio)
        return -1;

    switch (mode) {
    case NVPERM_MODE_GR_INSPECT: {
        uint32_t boot0 = RD(g, 0x00000000U);
        uint32_t xp3g = RD(g, XP3G_PLM_OFF);
        uint32_t fecs_plm = RD(g, FECS_PLM_OFF);
        uint32_t ss0 = RD(g, FECS_SS0_OFF);
        uint32_t ss0_rd = RD(g, FECS_SS0_RD_OFF);
        uint32_t ss1 = RD(g, FECS_SS1_OFF);
        uint32_t disp = RD(g, FUSE_DISP_OFF);
        /* v29.66: EFI-phase PDISP probe - never measured pre-GSP before */
        uint32_t pdisp0 = RD(g, 0x00600000U);
        uint32_t pdisp1 = RD(g, 0x00610000U);
        uint32_t pd_disp = RD(g, 0x00419B54U);
        g->log(g, "40HX inspect: BOOT0=0x%08x XP3G=0x%08x FECS_PLM=0x%08x\n",
               boot0, xp3g, fecs_plm);
        g->log(g, "40HX inspect: SS0=0x%08x SS0_RD=0x%08x SS1=0x%08x FUSE_DISP=0x%08x\n",
               ss0, ss0_rd, ss1, disp);
        g->log(g, "40HX PDISP-EFI: 0x600000=0x%08x 0x610000=0x%08x 0x419B54=0x%08x\n",
               pdisp0, pdisp1, pd_disp);
        return 0;
    }
    case NVPERM_MODE_COMPUTE:
        write_full_set(g);
        return 0;
    case NVPERM_MODE_GRAPHICS:
        /* post-SBR re-assert: SS0/SS1 + FECS PLM (keep after bus reset) */
        WR(g, FECS_PLM_OFF, 0xFFFFFFFFU);
        WR(g, FECS_SS0_OFF, 0x88888888U);
        WR(g, FECS_SS1_OFF, 0x00000008U);
        /* v29.66: EFI-phase PDISP write attempt (vBIOS display-init values,
         * 2070-identical; harmless if gated, fishing if alive) */
        WR(g, 0x00419B54U, 0x000000FFU);
        WR(g, 0x00600000U, 0xFFFFFFFFU);
        g->log(g, "40HX: graphics re-assert SS0/SS1 + PDISP write attempt\n");
        return 0;
    case NVPERM_MODE_GR_RESET:
        /* TU106 has no GA102-style GR bootstrap needed for the SS override;
         * keep the slot for flow symmetry (a delay lets SBR settle). */
        g->delay_ms(500);
        return 0;
    case NVPERM_MODE_GR_ACR:
        /* final write, no SBR afterwards: this is the state handed to Windows */
        WR(g, FECS_PLM_OFF, 0xFFFFFFFFU);
        WR(g, FECS_SS0_OFF, 0x88888888U);
        WR(g, FECS_SS1_OFF, 0x00000008U);
        g->log(g, "40HX: final SS0/SS1 commit (handoff state)\n");
        return 0;
    default:
        return -1;
    }
}