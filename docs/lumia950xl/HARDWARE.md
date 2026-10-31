# Lumia 950 XL hardware notes (measured)

Facts measured on the phone under XNU, with where they came from. Register maps follow LineageOS's
`msft/cityman` kernel (msm8994 device trees, `clock-cpu-8994.c`, `qpnp-smbcharger.c`, `qpnp-fg.c`,
`spm-regulator.c`, `cpr-regulator`, `mem-acc-regulator.c`, `synaptics_i2c_rmi4.c`, `i2c-msm-v2.c`).

## Firmware hand-off (Lumia950XLPkg UEFI 2.x)

| | |
| --- | --- |
| Exception level | EL1; PSCI through **HVC** (`/psci` method `hvc`); no SYSTEM_RESET |
| Framebuffer | GOP 1080×1920, BGRA, stride 1080, at 0x00400000; the panel (1440×2560) scales it |
| Timer | CNTFRQ 19.2 MHz, virtual timer PPI **20** |
| GIC | QGIC2 at 0xF9000000 / 0xF9002000; GICD_PIDR2 reads 0 |
| CPUs | MPIDR 0x000-0x003 (A53), 0x100-0x103 (A57); banked GICD_ITARGETSR0 gives each core's target bit |
| eMMC | SDCC1 at 0xF9824900 (+ core 0xF9824000); the card does not answer after ExitBootServices: re-identify from CMD0 |

## CPU clocks (APCS)

| Block | Address | Notes |
| --- | --- | --- |
| A53 PLLs (c0) | 0xF9015000 | PLL0 at +0x00, PLL1 ("PLLA") at +0x40: MODE, L (+4), ALPHA (+8), USER_CTL (+0x10), CONFIG_CTL (+0x14), STATUS (+0x1C), TEST_CTL (+0x20/+0x24) |
| A57 PLLs (c1) | 0xF9016000 | same layout |
| CCI PLL | 0xF9011000 | |
| A53 / A57 mux | 0xF900D054 / 0xF900F054 | low-frequency select [2:1], high-frequency select [4:3] |

- Early output = L × 19.2 MHz; main output = early ÷ post-divider (USER_CTL [9:8]: 1 = ÷2, 3 = ÷4).
- Firmware state: both PLL0s at L 80 (1536 MHz); A53 main ÷2 = 768 MHz, A57 main ÷4 = 384 MHz; both
  clusters on the low-frequency mux (select 2 = PLL0 main). PLL1s idle.
- v2 low-frequency mux: 0 XO, 1 PLL1 main, 2 PLL0 main, 3 aux. High-frequency mux: 0 = LF, 1 = PLL1, 3 = PLL0.
- PLL1 VCO range 1209.6-1996.8 MHz; v2 config CONFIG_CTL 0x004D6968, TEST_CTL_LO 0x80000000, TEST_CTL_HI 0x1.
- Measured with a chain of dependent adds (one cycle each on A53 and A57) against CNTVCT.

## CPU rails, CPR and limits

- Rails on PM8994 (SPMI SID 1), FTS2.5 (type 0x1C, subtype 0x09), range 0: **V = 80 mV + 5 mV × VSET**
  (VSET at +0x41). S2 (MX) 0x1700, S8 (A53) 0x2900, S9/S10/S11 (A57, three-phase, S11 leads) 0x2C00/0x2F00/0x3200.
- Firmware state: A53 1000 mV, A57 900 mV, MX 1035 mV. The SAW2 blocks that would carry voltage requests
  (CCI SAW 0xF9065000 for the A53 rail, A57 L2 SAW 0xF9013000) have SPM_CTL 0: the rails take direct writes.
- CPR fuses in QFPROM (0xFC4BC000 + row × 8): open-loop voltage per fuse corner = reference + signed
  (sign-magnitude, 6-bit) steps × 10 mV + the v2 adjustment, clamped to floor/ceiling. A53 rows 138 bits
  0/6/12, A57 row 138 bits 54/60 and row 139 bit 2. Speed bin: row 22 bits 0-2. Redundancy select: row 140
  bits 61-63. On our unit (fuse revision 4, speed bin 0): A53 780/855/885/1115 mV, A57 810/810/1000/1080 mV.
- A57 fuse corners: 1-2 cover up to 633.6 MHz, 3 covers 768-960 MHz, 4 covers 1248-1958 MHz.
  **Above fuse corner 2 Linux also moves the A57 memory-assist registers** (0xF900F084-0x90: 0x02 → 0x06)
  through `scm_io_write` (the secure monitor), and wants MX at nominal (corner 3) or turbo (corner 4).
  Without that, 960 MHz ran ~6 minutes and then the SoC reset (PS_HOLD).

## PMIC buses (SPMI arbiter 0xFC4C0000)

- Command channel at +0xF800 (v1 command format: opcode << 27 | SID << 20 | PID << 12 | offset << 4),
  status +0xF808, read data +0xF818, write data +0xF810. One user at a time (`native_spmi_read` in NativeTouch).
- PM8994 SID 0: PON 0x800 (real-time status 0x810: bit 0 KPDPWR power, bit 1 RESIN volume down), RTC 0x6000,
  GPIO *n* at 0xC000 + 0x100 × (n − 1), status bit 0 = level (volume up GPIO 3, camera GPIO 4, focus GPIO 5,
  inputs pulled up, low = pressed).
- PM8994 SID 1: regulators (L22 touch 0x5500, LVS1 0x8000, SMPS above).
- PMI8994 SID 2: charger CHGR 0x1000 (status 0x0E: bits 2:1 = 0 none, 1 pre, 2 fast, 3 taper), BAT_IF 0x1200,
  USB_CHGPTH 0x1300 (RT status bit 2 = input present, IL_CFG 0xF2 input limit index), MISC 0x1600,
  fuel gauge SOC 0x4000 (monotonic SOC at +0x09, 0-255). Firmware leaves the input limit at 1500 mA in
  high-current mode (500 mA on a PC port) and charges on its own.

## Touch (Synaptics S3708AR, RMI4)

- I2C address 0x4B on BLSP1 QUP1 (0xF9923000); SDA/SCL TLMM GPIO 2/3 (function 3), reset GPIO 38, IRQ GPIO 77.
- Clocks: GCC 0xFC400000, BLSP1 AHB vote bit 17 at 0x1484, QUP1 I2C CBCR 0x648, RCG at 0x660 (XO).
- QUP v2 tags, FIFO mode polled; 400 kHz = HT 14 / FS 28 at 19.2 MHz.
- PDT: F34, F01 (data 0x06: interrupt status at data + 1), F12 (data 0x08, 10 objects × 8 bytes, object
  type 1 finger / 2 stylus / 5 glove), sensor 1440×2560 = panel pixels.

## Storage

- Samsung eMMC (CID 15010042…), 61,071,360 × 512 B, EXT_CSD rev 7. Host version 0x3C02, capabilities
  0x742DC8B2 (ADMA2, 64-bit), base clock 200 MHz (the MSM host ignores the SDHCI divider; the GCC SDCC1 RCG
  sets the card clock).
- 8-bit HS at 50 MHz: PIO ~10 MB/s; ADMA2 (32-bit descriptors, buffers below 4 GiB, manual `dc civac`
  around each transfer: the host is not cache-coherent) ~27 MB/s with 64 KiB commands.
