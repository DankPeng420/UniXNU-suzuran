// loader.c - suzuran XNU loader. v14: PM8994 GPIO 4 (camera, NativeTouch's Home) and GPIO 5 (focus) set up as the
// Sony kitakami DTS does: input, vin S4, 30 uA pull-up, enabled. S1 leaves them pulled down, so the camera key read as
// held forever. v13: also starts the A57 cluster (CPUs 4-7, MPIDR 0x100-0x103): PM8994 S11 on via SPMI
// (gang leader of S9/S10, firmware leaves it at 900 mV but off), cluster-1 L2/SCU power-up as power_on_l2_msm8994, then the
// same SCM cold-boot + ACC unclamp + LDO (vref-ret 0x3e) per core; spin-table slot = aff1 * 4 + aff0 (proven by smptest2).
// v12: touch reset GPIO 109 is ACTIVE HIGH (Sony DTS reset state = output-high,
// active = output-low): pulse high, then hold low (running) + 200 ms (delay_after_hw_reset). v11: powers the ClearPad touch controller for NativeTouch (Z5C values from the
// Sony kitakami DTS: L22 3.0 V on, reset GPIO 109 low -> high, SDA/SCL GPIO 27/28 to function 3 (BLSP1 QUP6),
// IRQ GPIO 42 input); NativeTouch is pointed at QUP6 / address 0x2c by kernel patch probe8. v10: starts A53 cores 1-3 before the handoff (SCM cold-boot address MC (SMC32) +
// ACC unclamp + scm_io L2 bit + APC LDO config, as msm-3.10 qcom,8994-arm-cortex-acc; proven by smptest) and parks them
// in a spin table: core N polls the 64-byte slot N at PA(MB_VA) (MMU off, uncached) and jumps to the entry the patched
// kernel writes there (ArmCpu::startCPU's PSCI CPU_ON smc -> probe7 cave: str entry, dc civac, sev), x0 = 0.
// MB_VA is the zero-filled tail of the kernel's __DATA (after __bss). v9: eMMC VCC (L20) also voted 800 mA (RPM key "ma") so the RPM puts the LDO in high-power
// mode; in low-power mode the card browned out during writes (stopped answering CMD13 for good after a CMD25). v8: payload type 4 = trust cache bundle (make-trustcache-bundle.py), copied after the
// ADT (align 4K) and announced in the ADT property "TrustCache" (/chosen/memory-map: u64 PA, u64 size), as DarwinBoot does.
// Error 7 = trust cache given but the ADT has no TrustCache placeholder. v7: + legacy clocking in SDHCI VENDOR_SPEC (hc+0x10c: MCLK default, SELECT_IN off,
// no clock power-save), DLL reset+off (hc+0x100), no DLL power-save (hc+0x1b0). S1 leaves HS400 clocking selected and
// the vendor registers live in the SDHCI window on MSM8994 (NativeSdhci writes them at core+ offsets, as on MSM8992).
// emmctest2: card answers CMD1 (OCR 0xC0FF8080) only with core reset + this. v6: + eMMC VCC (L20 2.95 V) voted on, IO pads switched to 1.8 V
// (SDHCI+0x10C VENDOR_SPEC: IO_PAD_PWR_SWITCH_EN | IO_PAD_PWR_SWITCH, as sdhci-msm does). v5 made commands
// complete (CMD0 ok) but the card never answered CMD1. v5: re-initialise SDCC1 (eMMC host) like Linux sdhci-msm before the handoff:
// core SW reset, HC_MODE (SDHCI mode), SDHCI reset-all, bus power on (1.8 V) with every PWRCTL request acked.
// S1 leaves the host with CMD-inhibit stuck and a BUS_ON power request pending; NativeSdhci never acks PWRCTL.
// v4: quiesce interrupts before the handoff (GIC distributor + CPU interface off,
// all SPIs/PPIs disabled, pending/active cleared, EL1 timers off). The S1 bootloader leaves interrupts armed;
// XNU took an IRQ in machine_startup before any interrupt controller driver existed and panicked.
// v3: the framebuffer moves INTO the kernel window, right after the ADT
// (fb = align64K(tree + adt size), top = align16K(fb + 2944*1280)), so it is inside TopOfKernelData and
// reachable through the kernel's own linear mapping (VA = virtbase + PA - physbase) once the MMU is on.
// Payload + Mach-O are parsed and copied BEFORE the display comes up. v2: window PA = 0x20000000 + (kernel base mod 2 MiB), like DarwinBoot
// (v1 put base at 0x20000000 exactly; XNU needs PA == VA mod 2 MiB for its block mappings).
// (step 1) Display bring-up from stage6c (self-refreshing panel),
// then a DarwinBoot-style handoff:
//   payload = the boot.img "ramdisk" (found via /chosen linux,initrd-start), format SZPK:
//     "SZPK", u32 version 1, u32 count, u32 window_mb, entries[count] { u32 type, u32 0, u64 offset, u64 size }
//     type 1 = kernel Mach-O (arm64 MH_EXECUTE), 2 = device tree (ADT), 3 = command line (text)
//   kernel window at 0x20000000 (window_mb MiB): segments at PA = 0x20000000 + (vmaddr - base),
//   boot_args (rev 2 / ver 2, 0x480 bytes) at align16K(end), ADT at boot_args + 4 KiB, top = align16K(ADT end)
//   enter at EL1, MMU/caches off, x0 = boot_args PA.
// Screen status: white bar at the top = loader running; on error a RED bar at the top with N white
// squares under it (N = error code), then power-off after 60 s:
//   1 no payload / bad magic   2 no kernel entry   3 bad Mach-O   4 kernel too big for window
//   5 no device tree entry     6 payload overlaps the kernel window
// Built on:
// stage6c: no overlapping frames. stage6b showed a byte-misaligned, wrapped image (channels rotated,
// half-line and vertical offset) because autorefresh + START were issued while a kicked frame was
// still in flight. Now: display on (0x29) first via DSI DMA, then MDP setup with autorefresh already
// configured, ONE flush + ONE start, nothing else touches CTL0 afterwards.
// stage6b: PP0 SYNC_CONFIG_HEIGHT = 1478 (vtotal) instead of TWRP's 0xfff0. With the internal vsync counter
// the frame period is height x 216 clks / 19.2 MHz: 0xfff0 gave ~0.74 s per frame (~1.4 fps, as observed);
// 1478 gives 16.6 ms (60 Hz). Also enables the instruction cache (SCTLR_EL1.I) for faster drawing.
// stage6: PP0 tear check on with the INTERNAL vsync counter (no TE pin) + AUTOREFRESH every frame,
// one CTL START, then the CPU only writes memory: a yellow bar moves down the bottom half without kickoffs.
// If the bar moves smoothly, XNU can draw into the framebuffer and the panel follows by itself.
// stage5b: unpack 0x03020001 so u32 0xAARRGGBB (bytes B,G,R,A) shows correctly (stage5 had R/B swapped).
// suzuran stage5 (built on stage4): RGB1 fetches a 736-stride ARGB8888 test pattern from DDR at 0x03400000
// (same pipe/SMP/VBIF setup TWRP uses), AXI at 150 MHz. Pattern: top half 8 vertical bars
// (white yellow cyan green magenta red blue black), bottom half 80 px grid, 4 px white border, red square top-left.
// Codes as stage4 (30 = finished). Earlier header:
// suzuran stage4 (built on stage3): DSI command-mode host init, panel init DCS commands via DSI DMA,
// MDP solid-fill (white) on RGB0 -> MIXER0 -> CTL0 -> PP0 (tear check off) -> INTF1 -> DSI0, display on,
// WLED backlight strings 0/1. Screen should be WHITE from ~3 s until the red flash.
// STAGE4 report codes:
//   30  success path finished (look at the screen before this!)
//   12  MDP clock switch to GPLL0 85.71 MHz failed     15  DSI DMA never completed (first command)
//   18  DSI DMA failed later in the sequence             21  WLED SPMI write failed
//   33  stage2 part failed   36  stage3 part failed      57  exception
// stage3 adds panel power: RPM LDO votes (L2 1.25 V, L12 1.8 V, L14 2.0 V, L28 1.0 V),
// PMI8994 LAB/IBB +-5.5 V in LCD mode over SPMI (PMIC arbiter v1, channel 0, SID 3, LAB 0xDE00, IBB 0xDC00),
// then the panel reset sequence on TLMM GPIO 78 (0/10 ms, 1/12 ms, 0/1 ms, 1/10 ms).
// STAGE3 report codes (replace the stage2 table):
//    9  everything OK: LDOs voted, LAB and IBB report VREG_OK, GPIO 78 reads back high
//   12  SPMI write/read failed or denied        15  LAB peripheral type wrong (not 0x24)
//   18  LAB never reported VREG_OK              21  IBB never reported VREG_OK
//   24  RPM regulator vote got no reply         27  GPIO 78 readback wrong
//   30  stage2 part failed (MDSS / DSI / PLL / link clocks)   57  exception
// suzuran stage3 (built on stage2): MDSS up (probe13 sequence incl. TZ restore_sec_cfg), then the DSI0 link:
//   DSI PHY sw reset -> PHY regulator (captured values) -> PHY lanes/timing/strength (panel dtsi)
//   -> 20nm PLL to VCO 821.52 MHz (byte 102.69 MHz, pclk 68.46 MHz) -> lock -> post dividers
//   -> byte0/pclk0 RCG on DSI0 PLL, esc0 on XO -> branch clocks on.
// Register sequences follow the Sony msm8994 kernel (mdss-dsi-20nm-pll-util.c, msm_mdss_io_8974.c).
// Red-flash report (seconds after start):
//    9  PLL locked AND byte0/pclk0/esc0 branches running         <- success
//   12  PLL locked, but a link branch clock is stuck off
//   15  PLL never set RESET_SM bit5 (no lock)    18  bit5 set but bit6 (ready) never came
//   21  DSI0 / PHY / PLL registers did not answer (bus error)
//   24  MDSS unlock (TZ) failed                   54  clocks/GDSC failed   57  exception
//   6..21 from smd/rpm setup (rare, see probe13 codes)
typedef unsigned long u64; typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8;
#define R32(a) (*(volatile u32 *)(u64)(a))
#define W32(a,v) (*(volatile u32 *)(u64)(a) = (u32)(v))

#define SMEM_BASE   0x06A00000UL
#define SMEM_SIZE   0x00200000UL
#define IPC_REG     0xF900D008UL       /* APCS_GCC IPC, bit 0 = RPM SMD */
#define HOST_APPS   0
#define HOST_RPM    6
#define ITEM_ALLOC_TBL   13
#define ITEM_ALLOC_TBL_2 266
#define ITEM_SMD_BASE    14
#define ITEM_SMD_BASE_2  138
#define ITEM_FIFO_BASE   338
#define ITEM_FIFO_BASE_2 202
#define SS_CLOSED 0
#define SS_OPENING 1
#define SS_OPENED 2
#define SS_CLOSING 4

#define MMSS 0xFD8C0000UL
#define GCC_MMSS_NOC_CFG_AHB_CBCR 0xFC40024CUL
#define AHB_CMD_RCGR          (MMSS+0x5000)
#define MMSSNOC_AHB_CBCR      (MMSS+0x5024)
#define MMSSNOC_BTO_AHB_CBCR  (MMSS+0x5028)
#define MDP_CMD_RCGR          (MMSS+0x2040)
#define AXI_CMD_RCGR          (MMSS+0x5040)
#define MDSS_BCR              (MMSS+0x2300)
#define GDSC_MDSS             (MMSS+0x2304)
#define MMSS_MISC_AHB_CBCR    (MMSS+0x502C)
#define MMSS_S0_AXI_CBCR      (MMSS+0x5064)
#define MMSS_MMSSNOC_AXI_CBCR (MMSS+0x506C)
#define MDSS_AHB_CBCR         (MMSS+0x2308)
#define MDSS_AXI_CBCR         (MMSS+0x2310)
#define MDSS_MDP_CBCR         (MMSS+0x231C)
#define MDSS_VSYNC_CBCR       (MMSS+0x2328)
#define MDSS    0xFD900000UL
#define PS_HOLD 0xFC4AB000UL
#define WDOG_EN 0xF9017008UL

extern void install_vectors2(void);
static u64 t0; static volatile int in_mdss_read;
static inline u64 cntpct(void){ u64 v; __asm__ volatile("isb; mrs %0, cntpct_el0":"=r"(v)); return v; }
static void udelay(u64 us){ u64 s=cntpct(); while (cntpct()-s < us*192/10) {} }
static void report(u64 secs){ while (cntpct()-t0 < secs*19200000ULL) {} W32(PS_HOLD, 0); for(;;) __asm__ volatile("wfe"); }
static volatile int faulted, probing; static long smc_ret; static volatile u32 last_esr; static int gcc_bridge_was_off;
void exc_handler2(u64 esr, u64 far, u64 *frame){
    (void)far; (void)frame;
    u32 ec = (u32)(esr >> 26) & 0x3f;
    last_esr = (u32)esr;
    if (probing && ec != 0x25){
        u64 elr; __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
        elr += 4; __asm__ volatile("msr elr_el1, %0; isb" :: "r"(elr));
        faulted = 2; return;
    }
    if (probing && ec == 0x25){ /* data abort */                      /* data abort on a probe read: skip it */
        u64 elr; __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
        elr += 4; __asm__ volatile("msr elr_el1, %0; isb" :: "r"(elr));
        faulted = 1; return;
    }
    report(57);                                      /* unexpected exception */
}
/* one 32-bit read that may fault; returns 1 if the address answered */
static int __attribute__((noinline)) probe_read(u64 a){
    faulted = 0; probing = 1;
    u32 v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
    __asm__ volatile("" :: "r"(v));
    probing = 0;
    return faulted == 0;
}

/* ---------------- SMEM ---------------- */
static u64 part_base;   /* apps<->RPM partition header, 0 if none */

static void smem_find_partition(void){
    u64 toc = SMEM_BASE + SMEM_SIZE - 0x1000;
    if (R32(toc) != 0x434f5424) return;                  /* "$TOC" */
    u32 n = R32(toc + 8);
    if (n > 32) n = 32;
    for (u32 i = 0; i < n; i++){
        u64 e = toc + 32 + i*48;                          /* struct smem_toc_entry, 48 bytes */
        u32 off = R32(e+0), hosts = R32(e+12);
        u32 h0 = hosts & 0xffff, h1 = hosts >> 16;
        if (((h0==HOST_APPS && h1==HOST_RPM) || (h0==HOST_RPM && h1==HOST_APPS)) && off){
            u64 hdr = SMEM_BASE + off;
            if (R32(hdr) == 0x54525024) { part_base = hdr; return; }   /* "$PRT" */
        }
    }
}
/* returns item address, *size in bytes; 0 if not found */
static u64 smem_get(u32 id, u32 *size){
    if (part_base){
        u64 hdr = part_base;
        u64 end = hdr + R32(hdr + 12);                    /* offset_free_uncached */
        for (u64 a = hdr + 32; a < end; ){
            u32 w0 = R32(a), sz = R32(a+4), w2 = R32(a+8);
            if ((w0 & 0xffff) != 0xa5a5) break;           /* canary */
            u32 pad_data = w2 & 0xffff, pad_hdr = w2 >> 16;
            if ((w0 >> 16) == id){ *size = sz - pad_data; return a + 16 + pad_hdr; }
            a += 16 + pad_hdr + sz;
        }
    }
    /* legacy (non-secure) heap */
    if (R32(SMEM_BASE + 64 + 128) != 1) return 0;        /* heap_info.initialized */
    u64 e = SMEM_BASE + 208 + (u64)id*16;
    if (!R32(e)) return 0;
    *size = R32(e+8);
    u64 aux = R32(e+12) & 0xfffffffcu;
    return (aux ? aux : SMEM_BASE) + R32(e+4);
}

/* ---------------- SMD (word-access half channels: 11 words each) ---------------- */
enum { H_STATE=0, H_DSR=1, H_CTS=2, H_CD=3, H_RI=4, H_FHEAD=5, H_FTAIL=6, H_FSTATE=7, H_FBLOCK=8, H_TAIL=9, H_HEAD=10 };
static u64 tx, rx, txf, rxf; static u32 fsz;
#define HW(h,f)    R32((h) + (f)*4)
#define HSET(h,f,v) W32((h) + (f)*4, v)
static void notify(void){ W32(IPC_REG, 1); }
static void set_state(u32 s){
    u32 on = (s == SS_OPENED);
    HSET(tx,H_DSR,on); HSET(tx,H_CTS,on); HSET(tx,H_CD,on);
    HSET(tx,H_STATE,s); HSET(tx,H_FSTATE,1); notify();
}
static int smd_find(void){
    smem_find_partition();
    u32 tbl_sz = 0; u64 tbl = smem_get(ITEM_ALLOC_TBL, &tbl_sz);
    int which = 0;
    if (!tbl && !R32(SMEM_BASE + 64 + 128) && !part_base) return 6;
    u32 cid = 0xffffffff;
    for (int t = 0; t < 2 && cid == 0xffffffff; t++){
        if (t == 1){ tbl = smem_get(ITEM_ALLOC_TBL_2, &tbl_sz); }
        if (!tbl) continue;
        for (u32 i = 0; i + 32 <= tbl_sz; i += 32){       /* struct smd_alloc_elm: name[20], cid, type, ref */
            u64 el = tbl + i;
            u32 n0 = R32(el), n1 = R32(el+4), n2 = R32(el+8), n3 = R32(el+12);
            /* "rpm_requests\0" = 'rpm_' 'requ' 'ests' '\0...' */
            if (n0 == 0x5f6d7072 && n1 == 0x75716572 && n2 == 0x73747365 && (n3 & 0xff) == 0
                && (R32(el+24) & 0xff) == 15){ cid = R32(el+20); which = t; break; }
        }
    }
    if (!tbl && cid == 0xffffffff) return 9;
    if (cid == 0xffffffff) return 12;
    u32 csz = 0, ffsz = 0;
    u64 ch = smem_get((which ? ITEM_SMD_BASE_2 : ITEM_SMD_BASE) + cid, &csz);
    u64 ff = smem_get((which ? ITEM_FIFO_BASE_2 : ITEM_FIFO_BASE) + cid, &ffsz);
    if (!ch || !ff || ffsz < 64) return 15;
    tx = ch; rx = ch + 44; fsz = (ffsz & ~31u) / 2; txf = ff; rxf = ff + fsz;
    return 0;
}
static int smd_open(void){
    u32 s = HW(tx,H_STATE);
    if (s == SS_OPENED && HW(rx,H_STATE) == SS_OPENED) return 0;   /* bootloader left it open */
    if (s == SS_CLOSED || s == SS_CLOSING){
        HSET(rx,H_TAIL,0); HSET(tx,H_HEAD,0); HSET(tx,H_FBLOCK,0);
        set_state(SS_OPENING);
    }
    u64 start = cntpct();
    while (cntpct() - start < 19200000ULL){
        u32 r = HW(rx,H_STATE);
        if ((r == SS_OPENING || r == SS_OPENED) && HW(tx,H_STATE) == SS_OPENING) set_state(SS_OPENED);
        if (r == SS_OPENED && HW(tx,H_STATE) == SS_OPENED) return 0;
        udelay(100);
    }
    return 18;
}
static void fifo_put(u32 *head, u32 w){ W32(txf + *head, w); *head += 4; if (*head >= fsz) *head = 0; }
static int rpm_send(u32 set, u32 type, u32 id, u32 key, u32 val){
    static u32 msg_id = 1;
    u32 pkt[15] = {
        40, 0, 0, 0, 0,                                 /* SMD packet header: payload length */
        0x00716572, 32,                                 /* 'req', request_len = msg hdr 20 + kvp 12 */
        msg_id++, set, type, id, 12,                    /* rpm_message_header */
        key, 4, val };               /* kvp */
    u32 rx_head0 = HW(rx,H_HEAD);
    u32 head = HW(tx,H_HEAD);
    for (int i = 0; i < 15; i++) fifo_put(&head, pkt[i]);
    __asm__ volatile("dsb sy");
    HSET(tx,H_HEAD,head); HSET(tx,H_FHEAD,1); notify();
    u64 start = cntpct();
    while (cntpct() - start < 5*19200000ULL){ if (HW(rx,H_HEAD) != rx_head0) return 0; udelay(100); }
    return 21;
}

/* ---------------- MDSS power + clocks (stage 1c) ---------------- */
static void rcg_xo(u64 cmd){ W32(cmd+4, 0); W32(cmd, R32(cmd)|1); for(int i=0;i<1000&&(R32(cmd)&1);i++) udelay(1); }
static void cbcr_on(u64 r){ W32(r, R32(r) | 1); }
static int  running(u64 r){ for (int i=0;i<1000;i++){ if (!(R32(r)>>31)) return 1; udelay(1);} return 0; }

static void smc_call(u64 fn, long *r0, long *r1){
    register u64 x0 __asm__("x0") = fn;
    register u64 x1 __asm__("x1") = 2;              /* SCM_ARGS(2) */
    register u64 x2 __asm__("x2") = 1;              /* SEC_DEVICE_MDSS */
    register u64 x3 __asm__("x3") = 0;              /* spare */
    register u64 x4 __asm__("x4") = 0, x5 __asm__("x5") = 0, x6 __asm__("x6") = 0;
    do {
        x1 = 2; x2 = 1; x3 = 0;
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         : "r"(x4), "r"(x5), "r"(x6)
                         : "x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
        if (x0 == 1) x0 = fn;                          /* SCM_INTERRUPTED: retry */
        else break;
    } while (1);
    *r0 = (long)x0; *r1 = (long)x1;
}

#define DSI0      0xFD998000UL
#define DSI0_PLL  0xFD998300UL
#define DSI0_PHY  0xFD998500UL
#define DSI0_REG  0xFD998780UL
#define DSI1_PLL  0xFD9A0300UL
#define MMSS_MISC 0xFD828000UL
#define PCLK0_CMD_RCGR  (MMSS+0x2000)
#define BYTE0_CMD_RCGR  (MMSS+0x2120)
#define ESC0_CMD_RCGR   (MMSS+0x2160)
#define MDSS_PCLK0_CBCR (MMSS+0x2314)
#define MDSS_BYTE0_CBCR (MMSS+0x233C)
#define MDSS_ESC0_CBCR  (MMSS+0x2344)

static int mdss_up(void){
    int rc = smd_find();          if (rc) return rc;
    rc = smd_open();              if (rc) return rc;
    rc = rpm_send(1, 0x306b6c63, 2, 0x62616E45, 1);  if (rc) return rc;
    rc = rpm_send(0, 0x306b6c63, 2, 0x62616E45, 1);  if (rc) return rc;
    rc = rpm_send(0, 0x316b6c63, 3, 0x007A484B, 40000); if (rc) return rc;
    cbcr_on(GCC_MMSS_NOC_CFG_AHB_CBCR);
    for (int i=0;i<1000 && (R32(GCC_MMSS_NOC_CFG_AHB_CBCR)>>31);i++) udelay(1);
    W32(AHB_CMD_RCGR+4, 0); W32(AHB_CMD_RCGR, R32(AHB_CMD_RCGR) | 2 | 1);
    for (int i=0;i<1000&&(R32(AHB_CMD_RCGR)&1);i++) udelay(1);
    cbcr_on(MMSSNOC_AHB_CBCR); cbcr_on(MMSSNOC_BTO_AHB_CBCR);
    rcg_xo(AXI_CMD_RCGR);
    cbcr_on(MMSS_MISC_AHB_CBCR); cbcr_on(MMSS_S0_AXI_CBCR); cbcr_on(MMSS_MMSSNOC_AXI_CBCR);
    rcg_xo(MDP_CMD_RCGR);
    cbcr_on(MDSS_AHB_CBCR); cbcr_on(MDSS_AXI_CBCR); cbcr_on(MDSS_MDP_CBCR); cbcr_on(MDSS_VSYNC_CBCR);
    udelay(10);
    {   u32 g = R32(GDSC_MDSS);
        g &= ~((1u<<1) | (1u<<2));
        g &= ~((0xFu<<20) | (0xFu<<16) | (0xFu<<12));
        g |= (0x2u<<20) | (0x8u<<16) | (0x2u<<12);
        W32(GDSC_MDSS, g); W32(GDSC_MDSS, g & ~1u); }
    int on = 0; for (int i=0;i<10000;i++){ if (R32(GDSC_MDSS)>>31){ on=1; break; } udelay(1); }
    W32(MDSS_AXI_CBCR, R32(MDSS_AXI_CBCR) | (1u<<14)); udelay(1);
    W32(MDSS_AXI_CBCR, R32(MDSS_AXI_CBCR) | (1u<<13)); udelay(1);
    W32(MDSS_MDP_CBCR, R32(MDSS_MDP_CBCR) | (1u<<14)); udelay(1);
    W32(MDSS_MDP_CBCR, R32(MDSS_MDP_CBCR) | (1u<<13)); udelay(2);
    W32(MDSS_BCR, R32(MDSS_BCR) | 1); udelay(20);
    W32(MDSS_BCR, R32(MDSS_BCR) & ~1u); udelay(100);
    if (!on || !running(MDSS_MDP_CBCR) || !running(MDSS_AHB_CBCR)) return 54;
    long a, b;
    smc_call(0x42000C02UL, &a, &b);
    if (a || b) smc_call(0x02000C02UL, &a, &b);
    udelay(100);
    if (!probe_read(MDSS)) return 24;
    return 0;
}

/* ---- PHY (msm_mdss_io_8974.c, 20nm) ---- */
static const u8 lanecfg[45] = {
    0x02,0x00,0xEF,0x00,0x20,0x00,0x00,0x01,0xFF,
    0x02,0x00,0xEF,0x00,0x40,0x00,0x00,0x01,0xFF,
    0x02,0x00,0xEF,0x40,0x20,0x00,0x00,0x01,0xFF,
    0x02,0x00,0xEF,0x40,0x00,0x00,0x00,0x01,0xFF,
    0x00,0x00,0x45,0x80,0x00,0x00,0x00,0x01,0x97 };
static const u8 phy_timing[12] = { 0xD6,0x34,0x22,0x00,0x62,0x60,0x2E,0x36,0x28,0x03,0x04,0x00 };

static void dsi_phy_init(void){
    /* phy sw reset (ctrl + 0x12c) */
    W32(DSI0+0x12c, 1); udelay(1000); W32(DSI0+0x12c, 0); udelay(100);
    /* regulator: values captured from Linux (TWRP) */
    W32(DSI0_REG+0x04, 0x03); W32(DSI0_REG+0x08, 0x03); W32(DSI0_REG+0x0c, 0x00);
    W32(DSI0_REG+0x10, 0x20); W32(DSI0_REG+0x18, 0x01);
    W32(DSI0_PHY+0x1dc, 0x00);              /* LDO_CNTRL */
    W32(DSI0_REG+0x00, 0x03);
    /* config */
    W32(DSI0_PHY+0x184, 0x77);              /* STRENGTH_CTRL_0 */
    W32(DSI0_PHY+0x1d4, 0x00);              /* GLBL_TEST_CTRL */
    for (int ln = 0; ln < 5; ln++)
        for (int i = 0; i < 9; i++) W32(DSI0_PHY + ln*0x40 + i*4, lanecfg[ln*9+i]);
    for (int i = 0; i < 12; i++) W32(DSI0_PHY + 0x140 + i*4, phy_timing[i]);
    W32(DSI0_PHY+0x174, 0);                 /* CTRL_1 */
    __asm__ volatile("dsb sy");
    W32(DSI0_PHY+0x170, 0x7f);              /* CTRL_0: enable */
}

/* ---- 20nm PLL (mdss-dsi-20nm-pll-util.c), VCO 821.52 MHz from 19.2 MHz ref ---- */
#define P(o,v) W32(DSI0_PLL+(o), v)
static void pll_common_block_1(u64 base){
    W32(base+0x04, 0x82); W32(base+0x30, 0x2a); W32(base+0x30, 0x2b); W32(base+0x54, 0x02);
}
static void pll_powerdown(u64 base){
    W32(base+0x00, 0x00); W32(base+0x08, 0x01); W32(base+0x04, 0x82); W32(base+0x30, 0x02); W32(base+0x54, 0x06);
}
static int pll_wait(u32 bit){
    for (int i = 0; i < 2000; i++){ if (R32(DSI0_PLL+0x150) & bit) return 1; udelay(50); }   /* RESET_SM, ~100 ms */
    return 0;
}
/* returns 0 locked, 15 no bit5, 18 no bit6 */
static int pll_lock(void){
    pll_common_block_1(DSI1_PLL);
    pll_common_block_1(DSI0_PLL);
    /* common block 2 */
    P(0x00,0x40); P(0x0C,0x0F); P(0x10,0x0F); P(0x18,0x08); P(0x1C,0x0E);
    P(0x2C,0x08); P(0x48,0x4A); P(0x60,0x00); P(0x64,0x01); P(0x14,0x07);
    P(0x70,0x1f); P(0x68,0x8A); P(0x80,0x10); P(0xB4,0x00); P(0xFC,0x0C);
    P(0x110,0x0a); P(0x13C,0x0f); P(0x08,0x00);
    /* loop bandwidth */
    P(0x24,0x03); P(0x34,0x3F); P(0x38,0x03); P(0x3C,0x1F); P(0x114,0x77);
    /* VCO rate: dec_start 21, frac 412876, lock compare 4380 */
    P(0x100,0xcc); P(0x104,0x99); P(0x108,0x59);
    P(0xAC,0x95);  P(0x10C,0x02);
    P(0x90,0x1c);  P(0x94,0x11); P(0x98,0x00);
    P(0x9C,0x01);                                   /* PLLLOCK_CMP_EN */
    /* init lock: resetsm config + vco start */
    P(0x74,0x00); P(0xA8,0x00); P(0x4C,0x24); P(0x50,0x07);
    P(0x04,0x03); P(0x54,0x02); udelay(10); P(0x54,0x03);
    if (!pll_wait(1u<<5)) return 15;
    if (!pll_wait(1u<<6)) return 18;
    pll_powerdown(DSI1_PLL);
    /* post dividers: bypass_lp_div mux direct (sel 0), hr_oclk2 /4, hr_oclk3 /6 */
    P(0x140, R32(DSI0_PLL+0x140) | 0x80);
    P(0x144, 4-1);
    P(0x148, 6-1);
    return 0;
}

static void rcg_set(u64 cmd, u32 cfg){
    W32(cmd+4, cfg); W32(cmd, R32(cmd) | 1);
    for (int i=0;i<1000&&(R32(cmd)&1);i++) udelay(1);
}


/* ---------- RPM regulator votes ---------- */
#define RPM_LDOA 0x616f646cu   /* "ldoa" */
#define KEY_SWEN 0x6e657773u   /* "swen" */
#define KEY_UV   0x00007675u   /* "uv"   */
#define KEY_MA   0x0000616du   /* "ma"   */
static int ldo_on(u32 n, u32 uv){
    if (rpm_send(0, RPM_LDOA, n, KEY_UV, uv)) return 24;
    if (rpm_send(0, RPM_LDOA, n, KEY_SWEN, 1)) return 24;
    return 0;
}

/* ---------- SPMI PMIC arbiter v1 (core 0xFC4CF000, channel 0) ---------- */
#define ARB   0xFC4CF000UL
#define CHN   (ARB + 0x800)
static int spmi_wait(void){
    for (int i = 0; i < 1000; i++){
        u32 st = R32(CHN + 0x08);
        if (st){ return (st & 1) && !(st & 0xE) ? 0 : 12; }
        udelay(1);
    }
    return 12;
}
static int spmi_w(u32 sid, u32 addr, u32 v){
    W32(CHN + 0x10, v & 0xff);
    W32(CHN + 0x00, (0u<<27) | (sid<<20) | (addr<<4) | 0);      /* EXT_WRITEL, 1 byte */
    return spmi_wait();
}
static int spmi_r(u32 sid, u32 addr, u32 *v){
    W32(CHN + 0x00, (1u<<27) | (sid<<20) | (addr<<4) | 0);      /* EXT_READL, 1 byte */
    int rc = spmi_wait(); if (rc) return rc;
    *v = R32(CHN + 0x18) & 0xff; return 0;
}
#define PMI 3
#define LAB 0xDE00
#define IBB 0xDC00
static int labibb_on(void){
    u32 v; int rc;
    if ((rc = spmi_r(PMI, LAB + 0x04, &v))) return rc;
    if (v != 0x24) return 15;
    /* LCD mode on both (secure register: unlock first) */
    if ((rc = spmi_w(PMI, LAB + 0xD0, 0xA5))) return rc;
    if ((rc = spmi_w(PMI, LAB + 0x44, 0x00))) return rc;
    if ((rc = spmi_w(PMI, IBB + 0xD0, 0xA5))) return rc;
    if ((rc = spmi_w(PMI, IBB + 0x44, 0x00))) return rc;
    /* 5.5 V each: LAB (5.5-4.6)/0.1 = 9, IBB (5.5-1.4)/0.1 = 41, override bit 7 */
    if ((rc = spmi_w(PMI, LAB + 0x41, 0x80 | 9)))  return rc;
    if ((rc = spmi_w(PMI, IBB + 0x41, 0x80 | 41))) return rc;
    /* module ready */
    if ((rc = spmi_w(PMI, LAB + 0x45, 0x80))) return rc;
    if ((rc = spmi_w(PMI, IBB + 0x45, 0x80))) return rc;
    /* LCD mode: enabling IBB sequences LAB too */
    if ((rc = spmi_w(PMI, IBB + 0x46, 0x80))) return rc;
    udelay(20000);
    int lab_ok = 0, ibb_ok = 0;
    for (int i = 0; i < 20 && !(lab_ok && ibb_ok); i++){
        if ((rc = spmi_r(PMI, LAB + 0x08, &v))) return rc; lab_ok = (v >> 7) & 1;
        if ((rc = spmi_r(PMI, IBB + 0x08, &v))) return rc; ibb_ok = (v >> 7) & 1;
        udelay(5000);
    }
    if (!lab_ok) return 18;
    if (!ibb_ok) return 21;
    return 0;
}

/* ---------- TLMM GPIO 78 (panel reset) ---------- */
#define GPIO_CFG(n)   (0xFD511000UL + 0x10*(n))
#define GPIO_INOUT(n) (0xFD511004UL + 0x10*(n))
static void gpio_set(u32 n, u32 v){ W32(GPIO_INOUT(n), v ? 2 : 0); }


/* ---------- DSI command DMA ---------- */
static u32 dmabuf[16] __attribute__((aligned(64)));
static int dsi_dma(u32 hdr_word){
    dmabuf[0] = hdr_word;
    __asm__ volatile("dsb sy");
    W32(DSI0+0x110, R32(DSI0+0x110) | 1);          /* clear CMD_DMA_DONE status */
    W32(DSI0+0x48, (u32)(u64)dmabuf);
    W32(DSI0+0x4c, 4);
    W32(DSI0+0x90, 1);                              /* trigger */
    for (int i = 0; i < 20000; i++){ if (R32(DSI0+0x110) & 1){ W32(DSI0+0x110, R32(DSI0+0x110) | 1); return 0; } udelay(1); }
    return 1;
}
#define HDR_LAST  (1u<<31)
static int dcs1(u32 cmd, u32 p){ return dsi_dma(HDR_LAST | (0x15u<<16) | ((p & 0xff)<<8) | (cmd & 0xff)); }
static int dcs0(u32 cmd){ return dsi_dma(HDR_LAST | (0x05u<<16) | (cmd & 0xff)); }
static const u8 initcmds[13][2] = {
    {0xBA,0x01},{0xC2,0x08},{0xFB,0x01},{0xFF,0x05},{0x02,0x8E},{0xFB,0x01},{0xFF,0xEE},
    {0x12,0x50},{0x13,0x02},{0x6A,0x60},{0xFB,0x01},{0xFF,0x00},{0x35,0x00} };

static void dsi_host_init(void){
    W32(DSI0+0x40, 0x8);                 /* cmd mode: dst format RGB888 */
    W32(DSI0+0x44, 0x13c2c);             /* insert DCS, wr_mem_start 0x2c, continue 0x3c */
    W32(DSI0+0x84, 0x80000004);          /* trigger: te_sel, dma sw trigger */
    W32(DSI0+0xb0, 0);                   /* lane swap */
    W32(DSI0+0xc4, 0xf28);               /* t_clk_post / t_clk_pre */
    W32(DSI0+0xcc, 1);                   /* tx EOT append */
    W32(DSI0+0x10c, 0x03f03fc0);         /* err int mask */
    W32(DSI0+0x110, 0);                  /* no interrupts, we poll */
    W32(DSI0+0x11c, 0x23f);              /* clocks on */
    W32(DSI0+0x58, 0x08710039); W32(DSI0+0x60, 0x08710039);
    W32(DSI0+0x5c, 0x050002d0); W32(DSI0+0x64, 0x050002d0);
    W32(DSI0+0x3c, (1u<<28) | (1u<<26)); /* DMA: embedded, LP */
    W32(DSI0+0x04, 0x135);               /* enable, cmd mode, lanes 0/1, clk lane */
    udelay(100);
}

/* ---------- MDP ---------- */
static const u32 top_regs[][2] = {
    {0x14,0x1000},{0x80,0x111010},{0x84,0x121211},{0x88,0x101616},{0x8c,0x111010},{0x90,0x121111},{0x94,0x161212},
    {0x98,0x11616},{0x9c,0x130704},{0x130,0x111010},{0x134,0x121211},{0x138,0x101616},{0x13c,0x111010},
    {0x140,0x121111},{0x144,0x161212},{0x148,0x11616},{0x14c,0x130704},{0x178,0x20},{0x17c,0xffff},{0x184,0xff00},
    {0x1e0,0xa4},{0x1e8,0x38},{0x1ec,0x1},{0x2ac,0xc0000ccc},{0x2b0,0x4c3c},{0x2b4,0xc0000ccc},{0x2b8,0x4c3c},
    {0x2bc,0xcccccc},{0x2c0,0x18c3c},{0x2c4,0xcc},{0x2c8,0xc3c},{0x2e0,0x11111},{0x384,0x2},{0x394,0x2},
    {0x3a8,0xcccc0c0},{0x3b0,0xccccc0c0},{0x3b8,0xccccc0c0},{0x3bc,0x800000},{0x3d0,0xccc000},{0x3d8,0xe4},
    {0x3dc,0xe4},{0x3f8,0x1} };
static const u32 lm_regs[][2] = {
    {0x0,1},{0x4,0x50002d0},{0x20,0x100},{0x24,0xff},{0x50,0x100},{0x54,0xff},{0x80,0x100},{0x84,0xff},
    {0xb0,0x100},{0xb4,0xff},{0x110,0x10fff},{0x114,0x10000},{0x118,0x10000},{0x120,0x10fff},{0x124,0x10000},
    {0x128,0x10000},{0x130,0x10fff},{0x134,0x10000},{0x138,0x10000} };
static const u32 intf_regs[][2] = {
    {0x4,0x40000},{0x90,0x213f},{0x108,1},{0x10c,0xff0000},{0x110,0x400040},{0x118,0xff},{0x11c,0x24},
    {0x120,0x200},{0x128,0x200},{0x130,0x200},{0x134,0xff},{0x138,0xff},{0x13c,0xff},{0x140,0xff},
    {0x144,0xff},{0x148,0xff},{0x164,1} };
static const u32 pp_regs[][2] = {
    {0x4,0x1800d8},{0x8,0xfff0},{0xc,0x505},{0x10,0x500},{0x14,0x1d00558},{0x18,0x40004},{0x1c,0x500},{0x20,0x501} };
#define N(a) (sizeof(a)/sizeof(a[0]))
#define TOP   (MDSS+0x1000)
#define CTL0  (MDSS+0x2000)
#define RGB0  (MDSS+0x15000)
#define LM0   (MDSS+0x45000)
#define INTF1 (MDSS+0x6b800)
#define PP0   (MDSS+0x71000)
#define VBIF  0xFD924000UL
#define RGB1  (MDSS+0x17000)
static u64 g_fb = 0x03400000UL;
#define FB     g_fb
#define FBW    736
#define STRIDE (FBW*4)
static const u32 vbif_regs[][2] = {
    {0x20,0x404},{0x24,0x5555959},{0x28,0xaaaaaaa},{0x2c,0xffffbfb},{0x70,0x13fff},{0x74,0xa4},{0xa8,0x3fff},
    {0xb0,0x101010},{0xb4,0x10001010},{0xb8,0x101010},{0xbc,0x1010},{0xc0,0x10000000},{0xc4,0x100000},
    {0xc8,0x10000000},{0xd0,0x2020},{0xd4,0x2020},{0xd8,0x707},{0xe4,0x1},{0xf0,0x30},{0x100,0x76543210},
    {0x104,0xdcba98},{0x124,0x3},{0x160,0x22222222},{0x164,0x222222},{0x170,0x3fff},{0x178,0x3fff},
    {0x17c,0x3fff3fff},{0x204,0x3fff0000},{0x20c,0x30} };
static const u32 rgb1_regs[][2] = {
    {0x0,0x50002d0},{0x4,0x50002e0},{0x8,0},{0xc,0x50002d0},{0x10,0},{0x30,0x236ff},{0x34,0x3020001},{0x38,0},
    {0x48,0x87},{0x4c,0x707},{0x50,0x78},{0x54,0xf0},{0x58,0x168},{0xd0,0x8400},{0xe0,0x8400} };
static u32 pat(u32 x, u32 y){
    static const u32 bars[8] = { 0xFFFFFFFF,0xFFFFFF00,0xFF00FFFF,0xFF00FF00,0xFFFF00FF,0xFFFF0000,0xFF0000FF,0xFF000000 };
    if (x < 4 || x >= 716 || y < 4 || y >= 1276) return 0xFFFFFFFF;
    if (x >= 16 && x < 80 && y >= 16 && y < 80) return 0xFFFF0000;
    if (y < 640) return bars[(x * 8) / 720];
    return ((x % 80) < 2 || (y % 80) < 2) ? 0xFFFFFFFF : 0xFF202060;
}
static void draw_row(u32 y, int bar){
    volatile u32 *row = (volatile u32 *)(FB + (u64)y*STRIDE);
    for (u32 x = 0; x < FBW; x++) row[x] = (bar && x >= 4 && x < 716) ? 0xFFFFFF00 : pat(x, y);
}
static void draw_pattern(void){
    for (u32 y = 0; y < 1280; y++) draw_row(y, 0);
    __asm__ volatile("dsb sy");
}
static void mdp_setup(void){
    for (u32 i=0;i<N(top_regs);i++)  W32(TOP  + top_regs[i][0],  top_regs[i][1]);
    for (u32 i=0;i<N(lm_regs);i++)   W32(LM0  + lm_regs[i][0],   lm_regs[i][1]);
    for (u32 i=0;i<N(intf_regs);i++) W32(INTF1+ intf_regs[i][0], intf_regs[i][1]);
    for (u32 i=0;i<N(pp_regs);i++)   W32(PP0  + pp_regs[i][0],   pp_regs[i][1]);
    W32(PP0 + 0x4, (1u<<19) | 0xd8);         /* vsync counter on, internal source (no TE pin), 216 clks/line */
    W32(PP0 + 0x8, 1478);                    /* sync height = vtotal -> 60 Hz from the internal counter */
    W32(PP0 + 0x0, 1);                       /* tear check on */
    /* VBIF (captured) */
    for (u32 i=0;i<N(vbif_regs);i++) W32(VBIF + vbif_regs[i][0], vbif_regs[i][1]);
    /* RGB1: fetch ARGB8888 from FB (captured TWRP setup, our address) */
    for (u32 i=0;i<N(rgb1_regs);i++) W32(RGB1 + rgb1_regs[i][0], rgb1_regs[i][1]);
    W32(RGB1+0x14, FB); W32(RGB1+0x24, STRIDE);
    /* CTL0: RGB1 at base stage of LM0, cmd mode on INTF1 */
    W32(CTL0+0x00, 1u<<12);
    W32(CTL0+0x14, 0x00020020);
}
static void mdp_kick(void){
    W32(CTL0+0x18, (1u<<3) | (1u<<4) | (1u<<6) | (1u<<17) | (1u<<30));
    W32(CTL0+0x1c, 1);
}

/* ---------- WLED (PMI8994 SID 3, ctrl 0xD800, sink 0xD900), strings 0 and 1 ---------- */
static int wled_on(u32 level){
    int rc; u32 v;
    for (u32 n = 0; n < 2; n++){
        u32 b = 0xD900 + 0x50 + n*0x10;
        if ((rc = spmi_w(PMI, b + 0x0, 0x80))) return 21;           /* MOD_EN */
        if ((rc = spmi_w(PMI, b + 0x2, 25000/2500))) return 21;     /* full-scale 25 mA */
        if ((rc = spmi_w(PMI, b + 0x7, level & 0xff))) return 21;   /* brightness LSB */
        if ((rc = spmi_w(PMI, b + 0x8, (level >> 8) & 0x0f))) return 21;
    }
    if ((rc = spmi_w(PMI, 0xD900 + 0x47, 0x0F))) return 21;        /* sync */
    if ((rc = spmi_w(PMI, 0xD900 + 0x47, 0x00))) return 21;
    if ((rc = spmi_w(PMI, 0xD900 + 0x46, 0x3u << 4))) return 21;    /* current sinks 0,1 */
    if ((rc = spmi_r(PMI, 0xD800 + 0x46, &v))) return 21;
    if ((rc = spmi_w(PMI, 0xD800 + 0x46, (v & 0x7f) | 0x80))) return 21;   /* module enable */
    return 0;
}

static void fb_clear(u32 c);
static void display_init(void){
    W32(WDOG_EN, 0);
    t0 = cntpct();
    install_vectors2();
    { u64 sc; __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sc)); sc |= (1u<<12); __asm__ volatile("ic iallu; dsb sy; msr sctlr_el1, %0; isb" :: "r"(sc)); }
    int rc = mdss_up();                 if (rc) report(33);
    if (!probe_read(DSI0) || !probe_read(DSI0_PHY) || !probe_read(DSI0_PLL) || !probe_read(DSI0_REG)) report(33);
    if (ldo_on(14, 2000000) || ldo_on(12, 1800000) || ldo_on(2, 1250000) || ldo_on(28, 1000000)) report(36);
    udelay(2000);
    W32(MMSS_MISC+0x14, R32(MMSS_MISC+0x14) & ~((1u<<9) | (1u<<15)));
    W32(MMSS_MISC+0x108, 0);
    dsi_phy_init();
    if (pll_lock()) report(33);
    rcg_set(BYTE0_CMD_RCGR, (1u<<8) | 1);
    rcg_set(PCLK0_CMD_RCGR, (1u<<8) | 1);
    rcg_set(ESC0_CMD_RCGR,  (0u<<8) | 1);
    cbcr_on(MDSS_ESC0_CBCR); cbcr_on(MDSS_BYTE0_CBCR); cbcr_on(MDSS_PCLK0_CBCR);
    udelay(100);
    if (!running(MDSS_BYTE0_CBCR) || !running(MDSS_PCLK0_CBCR) || !running(MDSS_ESC0_CBCR)) report(33);
    /* MDP core clock -> GPLL0 / 7 = 85.71 MHz (Linux rate) */
    W32(MDP_CMD_RCGR+4, 0x50d); W32(MDP_CMD_RCGR, R32(MDP_CMD_RCGR) | 1);
    { int ok = 0; for (int i=0;i<1000;i++){ if (!(R32(MDP_CMD_RCGR)&1)){ ok=1; break; } udelay(1);} if (!ok) report(12); }
    W32(AXI_CMD_RCGR+4, 0x507); W32(AXI_CMD_RCGR, R32(AXI_CMD_RCGR) | 1);
    { int ok = 0; for (int i=0;i<1000;i++){ if (!(R32(AXI_CMD_RCGR)&1)){ ok=1; break; } udelay(1);} if (!ok) report(12); }
    fb_clear(0xFF000000);
    dsi_host_init();
    if (labibb_on()) report(36);
    gpio_set(78, 0);
    W32(GPIO_CFG(78), 1u<<9);
    gpio_set(78, 0); udelay(10000);
    gpio_set(78, 1); udelay(12000);
    gpio_set(78, 0); udelay(1000);
    gpio_set(78, 1); udelay(10000);
    /* panel init (LP mode) */
    for (int i = 0; i < 13; i++){ if (dcs1(initcmds[i][0], initcmds[i][1])) report(i ? 18 : 15); }
    if (dcs0(0x11)) report(18);          /* sleep out */
    udelay(100000);
    if (dcs0(0x29)) report(18);          /* display on (panel shows its RAM, not yet written) */
    udelay(40000);
    mdp_setup();
    W32(PP0 + 0x30, (1u<<31) | 1);       /* autorefresh every vsync, set BEFORE the single start */
    W32(CTL0+0x18, (1u<<3) | (1u<<4) | (1u<<6) | (1u<<17) | (1u<<30));
    W32(CTL0+0x1c, 1);
    if ((rc = wled_on(2048))) report(rc);
    return;
}

static void fb_fill(u32 x0, u32 y0, u32 w, u32 h, u32 c){
    for (u32 y = y0; y < y0 + h; y++){
        volatile u32 *row = (volatile u32 *)(FB + (u64)y*STRIDE);
        for (u32 x = x0; x < x0 + w; x++) row[x] = c;
    }
}
static void fb_clear(u32 c){ fb_fill(0, 0, FBW, 1280, c); __asm__ volatile("dsb sy"); }
static void fail(int code){
    fb_fill(0, 0, 720, 120, 0xFFE00000);
    for (int k = 0; k < code; k++) fb_fill(20 + k*60, 160, 40, 40, 0xFFFFFFFF);
    __asm__ volatile("dsb sy");
    report(60);
}

/* ---- tiny FDT reader: /chosen linux,initrd-start / -end ---- */
static u32 be32(u64 p){ volatile u8 *b = (volatile u8 *)p; return ((u32)b[0]<<24)|((u32)b[1]<<16)|((u32)b[2]<<8)|b[3]; }
static int streq(u64 a, const char *b){ volatile u8 *p = (volatile u8 *)a; int i = 0; for (; b[i]; i++) if (p[i] != (u8)b[i]) return 0; return p[i] == 0; }
static u64 fdt_cell(u64 p, u32 len){ return len == 8 ? ((u64)be32(p) << 32) | be32(p + 4) : be32(p); }
static int fdt_initrd(u64 dtb, u64 *start, u64 *end){
    if (!dtb || be32(dtb) != 0xd00dfeed) return 0;
    u64 st = dtb + be32(dtb + 8), strs = dtb + be32(dtb + 12);
    int depth = 0, in_chosen = 0, got = 0;
    for (u64 p = st;;){
        u32 tok = be32(p); p += 4;
        if (tok == 1){                       /* BEGIN_NODE */
            depth++;
            if (depth == 2) in_chosen = streq(p, "chosen");
            u32 n = 0; while (((volatile u8 *)p)[n]) n++;
            p += (n + 4) & ~3u;
        } else if (tok == 2){ if (depth == 2) in_chosen = 0; depth--; }
        else if (tok == 3){                  /* PROP */
            u32 len = be32(p), nameoff = be32(p + 4); u64 val = p + 8;
            if (in_chosen && depth == 2){
                if (streq(strs + nameoff, "linux,initrd-start")){ *start = fdt_cell(val, len); got |= 1; }
                if (streq(strs + nameoff, "linux,initrd-end"))  { *end   = fdt_cell(val, len); got |= 2; }
            }
            p = val + ((len + 3) & ~3u);
        } else if (tok == 4) {}
        else break;                           /* END or garbage */
    }
    return got == 3;
}

static u32 rd32(u64 p){ return *(volatile u32 *)p; }
static u64 rd64(u64 p){ return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static void mcpy(u64 d, u64 s, u64 n){
    if (!((d | s | n) & 7)){ for (u64 i = 0; i < n; i += 8) *(volatile u64 *)(d + i) = *(volatile u64 *)(s + i); return; }
    for (u64 i = 0; i < n; i++) *(volatile u8 *)(d + i) = *(volatile u8 *)(s + i);
}
static void mzero(u64 d, u64 n){
    u64 i = 0;
    for (; i < n && ((d + i) & 7); i++) *(volatile u8 *)(d + i) = 0;
    for (; i + 8 <= n; i += 8) *(volatile u64 *)(d + i) = 0;
    for (; i < n; i++) *(volatile u8 *)(d + i) = 0;
}

#define WIN_ALIGNED 0x20000000ULL
static u64 WIN_BASE = WIN_ALIGNED;   /* set to WIN_ALIGNED + (kernel base & 2 MiB-1): PA == VA mod 2 MiB, as DarwinBoot does */
struct seg { u64 va, vsize, off, fsize; };

extern void darwin_enter(u64 args, u64 entry, u64 win, u64 size);

#define GICD 0xF9000000UL
#define GICC 0xF9002000UL
#define SDC_CORE 0xF9824000UL
#define SDC_HC   0xF9824900UL
#define GCC_SDCC1_APPS_CBCR 0xFC4004C4UL
#define GCC_SDCC1_AHB_CBCR  0xFC4004C8UL
static void W8(u64 a, u32 v){ *(volatile unsigned char *)a = (unsigned char)v; }
static u32 R8(u64 a){ return *(volatile unsigned char *)a; }
static u32 sdc_log[8];
static void pwrctl_ack(u32 ms){
    u64 s = cntpct(), quiet = cntpct();
    while (cntpct() - s < ms * 19200ULL){
        u32 st = R32(SDC_CORE + 0xdc);
        if (st){
            W32(SDC_CORE + 0xe4, st);                         /* PWRCTL_CLEAR */
            u32 ctl = 0;
            if (st & 3)   ctl |= 1;                           /* BUS_ON/BUS_OFF -> BUS_SUCCESS */
            if (st & 0xc) ctl |= 4;                           /* IO_LOW/IO_HIGH -> IO_SUCCESS */
            W32(SDC_CORE + 0xe8, ctl);
            quiet = cntpct();
        } else if (cntpct() - quiet > 19200ULL) break;      /* 1 ms without a new request */
    }
}
static void sdcc1_reinit(void){
    cbcr_on(GCC_SDCC1_AHB_CBCR); cbcr_on(GCC_SDCC1_APPS_CBCR);
    udelay(10);
    sdc_log[0] = R32(SDC_HC + 0x24); sdc_log[1] = R32(SDC_CORE + 0xdc); sdc_log[2] = R32(SDC_CORE + 0x78);
    W32(SDC_CORE + 0x0, R32(SDC_CORE + 0x0) | (1u << 7));    /* CORE_POWER.SW_RST */
    for (int i = 0; i < 10000 && (R32(SDC_CORE + 0x0) & (1u << 7)); i++) udelay(1);
    W32(SDC_CORE + 0x78, R32(SDC_CORE + 0x78) | 1u | (1u << 13));   /* HC_MODE_EN | FF_CLK_SW_RST_DIS */
    W32(SDC_HC + 0x10c, R32(SDC_HC + 0x10c) | (1u << 15) | (1u << 16));   /* IO pad power switch: enable, 1.8 V */
    W32(SDC_CORE + 0xe0, 0xf);                               /* PWRCTL_MASK: all four requests */
    pwrctl_ack(5);
    W8(SDC_HC + 0x2f, 1);                                    /* SDHCI SOFTWARE_RESET: all */
    for (int i = 0; i < 10000 && (R8(SDC_HC + 0x2f) & 1); i++) udelay(1);
    pwrctl_ack(10);
    W8(SDC_HC + 0x29, (5u << 1) | 1);                        /* POWER_CONTROL: 1.8 V, bus power on */
    pwrctl_ack(20);
    W32(SDC_HC + 0x10c, R32(SDC_HC + 0x10c) | (1u << 15) | (1u << 16));   /* again, after reset-all */
    {   u32 vs = R32(SDC_HC + 0x10c);                         /* legacy clocking: undo S1's HS400 selection */
        vs &= ~((3u << 8) | (1u << 18) | (7u << 19) | (1u << 1));
        vs |= 2u << 8;
        W32(SDC_HC + 0x10c, vs);
        W32(SDC_HC + 0x1b0, R32(SDC_HC + 0x1b0) & ~(1u << 3));
        W32(SDC_HC + 0x100, R32(SDC_HC + 0x100) | (1u << 30));
        W32(SDC_HC + 0x100, R32(SDC_HC + 0x100) | (1u << 29));
    }
    sdc_log[3] = R32(SDC_HC + 0x24); sdc_log[4] = R32(SDC_CORE + 0xdc); sdc_log[5] = R8(SDC_HC + 0x29);
}
static void quiesce_irqs(void){
    __asm__ volatile("msr daifset, #0xf");
    __asm__ volatile("msr cntp_ctl_el0, xzr; msr cntv_ctl_el0, xzr; isb");
    u32 lines = (R32(GICD + 0x4) & 0x1f) + 1;          /* 32 interrupts per line */
    W32(GICD + 0x0, 0);                                  /* distributor off */
    for (u32 i = 0; i < lines; i++){
        W32(GICD + 0x180 + 4*i, 0xffffffff);            /* ICENABLER */
        W32(GICD + 0x280 + 4*i, 0xffffffff);            /* ICPENDR */
        W32(GICD + 0x380 + 4*i, 0xffffffff);            /* ICACTIVER */
    }
    for (int k = 0; k < 64; k++){                        /* drain anything the CPU interface still holds */
        u32 iar = R32(GICC + 0x0c);
        if ((iar & 0x3ff) >= 1020) break;
        W32(GICC + 0x10, iar);
    }
    W32(GICC + 0x0, 0);                                  /* CPU interface off */
    __asm__ volatile("dsb sy; isb");
}

/* ===================== v10: secondary cores parked in a spin table ===================== */
/* v15: if the ADT carries /psci "spin-table-mailbox" (u64 placeholder), the slots are park_slots in loader memory and
   their PA is written into that property (source-built kernel, ArmCpu spin-table release); otherwise MB_VA as in v10-v14
   (the 2026.11.01 release kernel with the probe10 cave). */
#define MB_VA   0xfffffff007c43c00ULL      /* in __DATA after __bss (zero-filled by us), 8 x 64-byte slots */
#define ACC(c)  (0xF908B000UL + (u64)(c) * 0x10000)
#define LDOB(c) (0xF9070000UL + (u64)(c) * 0x1000)
#define L2_PWR_BIT 0xF911210CUL
volatile u64 park_mb;                        /* PA of slot 0 */
/* v15: slots of our own (loader memory, outside the kernel window), announced to the kernel in the ADT property
   /psci "spin-table-mailbox" when the ADT has that placeholder; the source-built ArmCpu releases cores through it. */
static volatile u64 park_slots[64] __attribute__((aligned(512)));
volatile u32 park_seen[8];
extern void park_entry(void);
__asm__(
"   .text\n"
"   .balign 8\n"
"   .globl park_entry\n"
"park_entry:\n"
"   msr  daifset, #0xf\n"
"   mrs  x0, mpidr_el1\n"
"   ubfx x5, x0, #8, #8\n"
"   and  x0, x0, #0xff\n"
"   add  x0, x0, x5, lsl #2\n"
"   adrp x3, park_seen\n"
"   add  x3, x3, :lo12:park_seen\n"
"   movz w4, #0x5ec0, lsl #16\n"
"   orr  w4, w4, w0\n"
"   str  w4, [x3, x0, lsl #2]\n"
"   dsb  sy\n"
"   adrp x1, park_mb\n"
"   ldr  x1, [x1, :lo12:park_mb]\n"
"   add  x1, x1, x0, lsl #6\n"
"1: mov  x6, #2000\n"                 /* v19: poll, no wfe: on the source-built kernel the parked cores did not wake */
"2: subs x6, x6, #1\n"                 /*      on its SEV (boots 19-39: cpu 1 up only after the 1 s wait) */
"   b.ne 2b\n"
"   ldr  x2, [x1]\n"
"   cbz  x2, 1b\n"
"   mov  x0, #0\n"
"   br   x2\n");

static u64 smcx(u64 x0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 *r1){
    register u64 r0_ __asm__("x0") = x0, r1_ __asm__("x1") = a1, r2_ __asm__("x2") = a2,
                 r3_ __asm__("x3") = a3, r4_ __asm__("x4") = a4, r5_ __asm__("x5") = a5;
    register u64 r6_ __asm__("x6") = 0;
    __asm__ volatile("smc #0" : "+r"(r0_), "+r"(r1_), "+r"(r2_), "+r"(r3_), "+r"(r4_), "+r"(r5_), "+r"(r6_)
                     :: "x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
    if (r1) *r1 = r1_;
    return r0_;
}
#define SIP(svc, cmd) (0x02000000u | ((svc) << 8) | (cmd))
static u32 extbuf32[8] __attribute__((aligned(64)));
static void apc_ldo_config_v(u32 c, u32 vref);
static void apc_ldo_config(u32 c){ apc_ldo_config_v(c, 0x2a); }
static void apc_ldo_config_v(u32 c, u32 vref){
    u64 b = LDOB(c);
    W32(b + 0x28, (R32(b + 0x28) & 0xffffff00) | 0x12);
    W32(b + 0x0c, (R32(b + 0x0c) & 0xffffff00) | 0xc2);
    W32(b + 0x0c, (R32(b + 0x0c) & 0xffff00ff) | (0xf8 << 8));
    W32(b + 0x10, (R32(b + 0x10) & 0xffffff00) | 0x60);
    W32(b + 0x10, (R32(b + 0x10) & 0xff00ffff) | (0x4a << 16));
    W32(b + 0x04, R32(b + 0x04) & ~(1u << 16));
    W32(b + 0x04, (R32(b + 0x04) & 0xffff80ff) | ((vref & 0x7f) << 8));
    W32(b, 0); W32(b, 1); __asm__ volatile("dsb sy"); udelay(2); W32(b, 0); __asm__ volatile("dsb sy");
}
static void acc_w(u64 a, u32 v, u32 us){ W32(a, v); __asm__ volatile("dsb sy"); if (us) udelay(us); }

#define L2CCC1 0xF900F000UL
static int a57_cluster_up(void){
    u32 st = 0;
    spmi_w(1, 0x3246, 0x80);                 /* S11 (A57 rail, gang leader of S9/S10) on; firmware left it at 900 mV */
    udelay(2000);
    if (spmi_r(1, 0x3208, &st) || !(st & 0x80)) return 0;
    const u32 pon = (1u << 9) | (1u << 28);
    if ((R32(L2CCC1 + 0x14) & pon) == pon) return 1;
    W32(L2CCC1 + 0x210, 0x2); __asm__ volatile("dsb sy");     /* Q2S: qchannel ignore, no SPM legacy mode */
    W32(L2CCC1 + 0x284, 0); __asm__ volatile("dsb sy");       /* L1 invalidation by h/w */
    W32(L2CCC1 + 0x0c, 0x00400000); __asm__ volatile("dsb sy");
    W32(L2CCC1 + 0x14, 0x00029716); __asm__ volatile("dsb sy"); udelay(8);
    W32(L2CCC1 + 0x14, 0x00023716); __asm__ volatile("dsb sy");
    W32(L2CCC1 + 0x14, 0x0002371E); __asm__ volatile("dsb sy"); udelay(8);
    W32(L2CCC1 + 0x14, 0x0002371C); __asm__ volatile("dsb sy"); udelay(4);
    W32(L2CCC1 + 0x14, 0x0002361C); __asm__ volatile("dsb sy"); udelay(2);
    W32(L2CCC1 + 0x14, 0x00022218); __asm__ volatile("dsb sy"); udelay(4);
    W32(L2CCC1 + 0x14, 0x10022218); __asm__ volatile("dsb sy");
    W32(L2CCC1 + 0x0c, 0); __asm__ volatile("dsb sy");
    return 1;
}
static int start_secondaries(u64 mbpa){
    park_mb = mbpa;
    for (int i = 0; i < 8; i++){ park_seen[i] = 0; *(volatile u64 *)(mbpa + 64 * i) = 0; }
    __asm__ volatile("dsb sy");
    apc_ldo_config(0);
    int up = 0;
    for (u32 c = 1; c <= 7; c++){
        if (c == 4 && !a57_cluster_up()) break;
        u32 cl = c >> 2, core = c & 3;
        extbuf32[0] = 1; extbuf32[1] = ~0u; extbuf32[2] = 0x3;    /* aff2 mask, reserved, COLDBOOT_MC | HLOS */
        __asm__ volatile("dsb sy");
        if (smcx(SIP(1, 0x11), 6, (u64)park_entry, 1u << core, 1u << cl, (u64)extbuf32, 0)) continue;
        u64 a = ACC(c);
        acc_w(a + 0x14, 0x01, 1); acc_w(a + 0x14, 0x03, 1);
        acc_w(a + 0x04, 0x79, 2); acc_w(a + 0x04, 0x7D, 2);
        acc_w(a + 0x04, 0x3D, 0); acc_w(a + 0x04, 0x3C, 1);
        acc_w(a + 0x04, 0x0C, 0); acc_w(a + 0x04, 0x8C, 0);
        u64 v = 0; smcx(SIP(5, 1) | 0x80000000u, 1, L2_PWR_BIT, 0, 0, 0, &v);
        smcx(SIP(5, 2) | 0x80000000u, 2, L2_PWR_BIT, (u32)v & ~1u, 0, 0, 0);
        apc_ldo_config_v(c, cl ? 0x3e : 0x2a);
        for (int t = 0; t < 20000 && (park_seen[c] >> 16) != 0x5ec0; t++) udelay(1);
        if ((park_seen[c] >> 16) == 0x5ec0) up++;
    }
    return up;
}

/* ===================== v16: GPU (Adreno 430) powered and released by TrustZone ===================== */
/* What gputest4 proved (2026-11-11), up to the point where the GPU's own registers answer: VDD_GFX corner + the
   RPM-owned gfx3d clock source, OxiLi CX/GX GDSCs and branch clocks, the secure page-table pool (PTBL_SIZE said
   0x25000) and restore_sec_cfg(18) for the kgsl SMMU. The kernel's NativeAdreno430 does the rest. The pool at
   GPU_PTBL_PA belongs to TrustZone afterwards; it is outside the kernel window. Returns 0 when the GPU is ready. */
#define GPU_PTBL_PA 0x60000000UL
static int gpu_gdsc_on(u64 r){
    u32 g = R32(r);
    g &= ~((0xFu<<20) | (0xFu<<16) | (0xFu<<12));
    g |= (0x2u<<20) | (0x8u<<16) | (0x2u<<12);
    W32(r, g); W32(r, g & ~1u);
    for (int i = 0; i < 20000; i++){ if (R32(r) >> 31) return 1; udelay(1); }
    return 0;
}
static int gpu_power(void){
    if (rpm_send(0, 0x62706d73u, 2, 0x6e726f63u, 3)) return 1;      /* smpb 2 (PMI8994 S2) "corn" = SVS SOC */
    if (rpm_send(0, 0x62706d73u, 2, KEY_SWEN, 1)) return 1;
    if (rpm_send(0, 0x326b6c63u, 1, 0x007A484Bu, 300000)) return 1; /* clk2 1 (gfx3d_clk_src) 300 MHz */
    udelay(5000);
    cbcr_on(MMSS + 0x403C);                                          /* oxilicx_ahb */
    if (!gpu_gdsc_on(MMSS + 0x4034)) return 2;                       /* CX */
    if (!gpu_gdsc_on(MMSS + 0x4024)) return 2;                       /* GX */
    udelay(10);
    cbcr_on(MMSS + 0x4028);                                          /* oxili_gfx3d */
    rcg_xo(MMSS + 0x4090); cbcr_on(MMSS + 0x40B0);                   /* rbbmtimer */
    udelay(100);
    if (!running(MMSS + 0x4028) || !running(MMSS + 0x403C)) return 3;
    u64 size = 0;
    if (smcx(SIP(0xC, 5), 2, 300u << 20, 0, 0, 0, 0)) return 4;      /* IOMMU_SET_CP_POOL_SIZE */
    if (smcx(SIP(0xC, 3), 1, 0, 0, 0, 0, &size)) return 4;           /* IOMMU_SECURE_PTBL_SIZE */
    if (!size || size > (64u << 20)) return 4;
    if (smcx(SIP(0xC, 4), 0x23, GPU_PTBL_PA, size, 0, 0, 0)) return 5; /* IOMMU_SECURE_PTBL_INIT */
    if (smcx(SIP(0xC, 2), 2, 18, 0, 0, 0, 0)) return 6;              /* restore_sec_cfg(kgsl_iommu = 18, ctx 0) */
    return 0;
}

/* ===================== v11: touch controller power + pins ===================== */
#define TLMM_CFG(n)   (0xFD510000UL + 0x1000 + 0x10 * (u64)(n))
#define TLMM_IO(n)    (0xFD510004UL + 0x1000 + 0x10 * (u64)(n))
static void touch_power(void){
    W32(TLMM_CFG(109), 1u << 9);          /* reset: GPIO function, 2 mA, no pull, output */
    W32(TLMM_IO(109), 1u << 1);           /* reset is active high: assert while the supply comes up */
    ldo_on(22, 3000000);                  /* touch_vdd = PM8994 L22 3.0 V (touch_vio = S4 1.8 V is always on) */
    rpm_send(0, RPM_LDOA, 22, KEY_MA, 100);
    W32(TLMM_CFG(42), 0);                 /* interrupt: GPIO input, no pull */
    W32(TLMM_CFG(27), 3u << 2);           /* SDA: BLSP1 QUP6, 2 mA, no pull */
    W32(TLMM_CFG(28), 3u << 2);           /* SCL */
    udelay(10000);
    W32(TLMM_IO(109), 0);                 /* release reset (low = running) */
    udelay(200000);                       /* delay_after_hw_reset */
    __asm__ volatile("dsb sy");
}

void cmain(u64 dtb){

    u64 rs = 0, re = 0;
    if (!fdt_initrd(dtb, &rs, &re)) { rs = 0x50000000ULL; re = 0; }
    if (rd32(rs) != 0x4b505a53 /* "SZPK" */ || rd32(rs + 4) != 1) fail(1);
    u32 count = rd32(rs + 8), win_mb = rd32(rs + 12);
    u64 win_size = (u64)win_mb << 20;
    if (win_size < 0x10000000ULL || (win_size & 0x1fffff)) fail(1);
    u64 kpa = 0, ksz = 0, dpa = 0, dsz = 0, cpa = 0, csz = 0, tpa = 0, tsz = 0, apa = 0, asz = 0;
    for (u32 i = 0; i < count; i++){
        u64 e = rs + 16 + i * 24;
        u32 type = rd32(e); u64 off = rd64(e + 8), sz = rd64(e + 16);
        if (type == 1){ kpa = rs + off; ksz = sz; }
        if (type == 2){ dpa = rs + off; dsz = sz; }
        if (type == 3){ cpa = rs + off; csz = sz; }
        if (type == 4){ tpa = rs + off; tsz = sz; }
        if (type == 5){ apa = rs + off; asz = sz; }   /* v17: addon archive (NativeAddon) */
    }
    if (!kpa) fail(2);
    if (!dpa) fail(5);
    u64 pend = re ? re : (dpa + dsz > kpa + ksz ? dpa + dsz : kpa + ksz);

    /* ---- Mach-O (arm64 MH_EXECUTE, LC_SEGMENT_64 + LC_UNIXTHREAD) ---- */
    if (rd32(kpa) != 0xfeedfacf || rd32(kpa + 4) != 0x0100000c || rd32(kpa + 12) != 2) fail(3);
    u32 ncmds = rd32(kpa + 16), sizeofcmds = rd32(kpa + 20);
    u64 p = kpa + 32, cend = p + sizeofcmds;
    struct seg segs[32]; int nseg = 0; u64 entry = 0; int has_entry = 0;
    for (u32 i = 0; i < ncmds; i++){
        if (cend - p < 8) fail(3);
        u32 cmd = rd32(p), len = rd32(p + 4);
        if (len < 8 || (len & 7) || len > cend - p) fail(3);
        if (cmd == 0x19){
            u64 vsize = rd64(p + 32);
            if (vsize){
                if (nseg == 32) fail(3);
                segs[nseg].va = rd64(p + 24); segs[nseg].vsize = vsize;
                segs[nseg].off = rd64(p + 40); segs[nseg].fsize = rd64(p + 48);
                if (segs[nseg].fsize > vsize || segs[nseg].off + segs[nseg].fsize > ksz) fail(3);
                nseg++;
            }
        } else if (cmd == 5){
            if (len != 288 || rd32(p + 8) != 6 || rd32(p + 12) != 68) fail(3);
            entry = rd64(p + 272); has_entry = 1;
        }
        p += len;
    }
    if (!has_entry || !nseg) fail(3);
    u64 base = ~0ULL, end = 0;
    for (int i = 0; i < nseg; i++){
        if (segs[i].va < base) base = segs[i].va;
        if (segs[i].va + segs[i].vsize > end) end = segs[i].va + segs[i].vsize;
    }
    if ((base & 0xfff) || (entry & 3) || entry < base || entry >= end) fail(3);
    if (end - base > win_size - 0x200000) fail(4);
    WIN_BASE = WIN_ALIGNED + (base & 0x1fffffULL);
    if (rs < WIN_BASE + win_size && pend > WIN_BASE) fail(6);

    /* ---- layout (same as DarwinBoot) ---- */
    u64 args = (WIN_BASE + (end - base) + 0x3fff) & ~0x3fffULL;
    u64 tree = args + 0x1000;
    u64 tcpa = (tree + dsz + 0xfff) & ~0xfffULL;
    u64 adpa = (tcpa + tsz + 0xfff) & ~0xfffULL;   /* v17: the addon archive, announced as /chosen/memory-map NativeAddon */
    u64 fbpa = (adpa + asz + 0xffff) & ~0xffffULL;
    u64 top  = (fbpa + (u64)STRIDE * 1280 + 0x3fff) & ~0x3fffULL;
    g_fb = fbpa;
    if (top - WIN_BASE > win_size - 0x200000) fail(4);
    mzero(WIN_BASE, top - WIN_BASE);
    for (int i = 0; i < nseg; i++) mcpy(WIN_BASE + (segs[i].va - base), kpa + segs[i].off, segs[i].fsize);
    mcpy(tree, dpa, dsz);
    if (tsz){
        mcpy(tcpa, tpa, tsz);
        static const char name[] = "TrustCache";
        int found = 0;
        for (u64 q = tree; q + 52 <= tree + dsz && !found; q += 4){
            int m = 1;
            for (int k = 0; k < 32 && m; k++) if (*(volatile u8 *)(q + k) != (u8)(k < 10 ? name[k] : 0)) m = 0;
            if (m && (rd32(q + 32) & 0x7fffffff) == 16){
                W32(q + 36, (u32)tcpa); W32(q + 40, (u32)(tcpa >> 32)); W32(q + 44, (u32)tsz); W32(q + 48, (u32)(tsz >> 32)); found = 1;   /* 4-byte aligned only */
            }
        }
        if (!found) fail(7);
    }
    if (asz){
        mcpy(adpa, apa, asz);
        static const char name[] = "NativeAddon";   /* 11 chars */
        int found = 0;
        for (u64 q = tree; q + 52 <= tree + dsz && !found; q += 4){
            int m = 1;
            for (int k = 0; k < 32 && m; k++) if (*(volatile u8 *)(q + k) != (u8)(k < 11 ? name[k] : 0)) m = 0;
            if (m && (rd32(q + 32) & 0x7fffffff) == 16){
                W32(q + 36, (u32)adpa); W32(q + 40, (u32)(adpa >> 32)); W32(q + 44, (u32)asz); W32(q + 48, (u32)(asz >> 32)); found = 1;
            }
        }
        if (!found) fail(8);   /* addon given but the ADT has no NativeAddon placeholder */
    }
    int own_slots = 0;
    {
        static const char name[] = "spin-table-mailbox";   /* 18 chars */
        for (u64 q = tree; q + 44 <= tree + dsz && !own_slots; q += 4){
            int m = 1;
            for (int k = 0; k < 32 && m; k++) if (*(volatile u8 *)(q + k) != (u8)(k < 18 ? name[k] : 0)) m = 0;
            if (m && (rd32(q + 32) & 0x7fffffff) == 8){
                u64 pa = (u64)park_slots;
                W32(q + 36, (u32)pa); W32(q + 40, (u32)(pa >> 32)); own_slots = 1;
            }
        }
    }

    /* boot_args, 0x480 bytes */
    *(volatile u16 *)(args + 0x0) = 2;               /* Revision */
    *(volatile u16 *)(args + 0x2) = 2;               /* Version */
    *(volatile u64 *)(args + 0x08) = base;           /* VirtualBase */
    *(volatile u64 *)(args + 0x10) = WIN_BASE;       /* PhysicalBase */
    *(volatile u64 *)(args + 0x18) = win_size;       /* MemorySize */
    *(volatile u64 *)(args + 0x20) = top;            /* TopOfKernelData */
    *(volatile u64 *)(args + 0x28) = FB;             /* Video: base */
    *(volatile u64 *)(args + 0x30) = 1;              /*        display */
    *(volatile u64 *)(args + 0x38) = STRIDE;         /*        row bytes */
    *(volatile u64 *)(args + 0x40) = 720;            /*        width */
    *(volatile u64 *)(args + 0x48) = 1280;           /*        height */
    *(volatile u64 *)(args + 0x50) = 32;             /*        depth */
    *(volatile u64 *)(args + 0x60) = base + (tree - WIN_BASE);   /* DeviceTree (kernel VA) */
    *(volatile u32 *)(args + 0x68) = (u32)dsz;       /* DeviceTreeLength */
    if (csz > 1023) csz = 1023;
    mcpy(args + 0x6c, cpa, csz);                      /* CommandLine (rest zeroed above) */
    *(volatile u64 *)(args + 0x478) = win_size;      /* MemorySizeActual */

    display_init();                                                    /* panel scans out from inside the window */
    fb_fill(0, 0, 720, 24, 0xFFFFFFFF);  __asm__ volatile("dsb sy");   /* white bar: loader alive */
    fb_fill(0, 0, 720, 24, 0xFF00FF00);  __asm__ volatile("dsb sy");   /* green bar: handing over */
    ldo_on(20, 2950000);                 /* eMMC VCC */
    rpm_send(0, RPM_LDOA, 20, KEY_MA, 800); /* high-power mode for writes (Linux: vdd-current-level 800 mA) */
    touch_power();
    for (u32 g = 4; g <= 5; g++){            /* PM8994 GPIO g at SID0 0xC000 + (g-1)*0x100 */
        u32 b = 0xC000 + (g - 1) * 0x100;
        spmi_w(0, b + 0x40, 0x00);           /* mode: digital input */
        spmi_w(0, b + 0x41, 0x02);           /* vin: S4 (1.8 V) */
        spmi_w(0, b + 0x42, 0x00);           /* pull: 30 uA up */
        spmi_w(0, b + 0x46, 0x80);           /* master enable */
    }
    udelay(5000);
    {   int g = gpu_power();                                             /* v16 */
        fb_fill(200, 4, 24, 16, g ? 0xFFFF0000 : 0xFF00C000);           /* bar: green block = GPU ready, red = not */
        for (int i = 0; i < g; i++) fb_fill(230 + i * 8, 4, 4, 16, 0xFFFF0000); }   /* + one red tick per failing step */
    sdcc1_reinit();
    quiesce_irqs();
    if (own_slots || (MB_VA >= base && MB_VA + 512 <= base + (top - WIN_BASE))){
        int up = start_secondaries(own_slots ? (u64)park_slots : WIN_BASE + (MB_VA - base));
        for (int i = 0; i < up; i++) fb_fill(8 + i * 20, 4, 12, 16, i < 3 ? 0xFF0000FF : 0xFFFF00FF);   /* small cores blue, big cores magenta */   /* blue ticks in the bar: cores parked */
        __asm__ volatile("dsb sy");
    }
    darwin_enter(args, WIN_BASE + (entry - base), WIN_BASE, top - WIN_BASE);
}
