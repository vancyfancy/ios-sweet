# lisa port dossier (Xiaomi Mi 11 Lite 5G NE)

Groundwork for porting ios-sweet from Redmi Note 10 Pro (`sweet`, SM7150) to
`lisa` (Mi 11 Lite 5G NE, SM7325-Abyss / Snapdragon 778G+, 2021, Android 11+
 factory / MIUI 12.5). Facts below are **observations** from public sources;
guesses are marked. The device itself was not touched — this file is the
no-phone groundwork along with patch `0082-lisa-machine-config.patch`
(kernel `MACHINE_CONFIG=LISA` -> `kernel.development.lisa`).

Sources:

- LineageOS wiki, device `lisa` (official: 11 Lite 5G NE global, 11 Lite NE 5G
  China, Mi 11 LE; recovery VolUp+Power, fastboot VolDown+Power; device
  integrity / snet quirk).
- LineageOS kernel source `android_kernel_xiaomi_sm8350` @ 7ede20c
  (msm-5.4, same kernel generation as sweet): `lisa-sm7325-overlay.dts`
  (`qcom,msm-id = <475 0x10000>`, `qcom,board-id = <0x1000B 0>`,
  `xiaomi,board-id = <0xA 0>`) overlays onto the **yupik** platform DTB
  (`lisa-sm7325-overlay.dtbo-base := yupik.dtb`). Local annotated copy of the
  DTS sources: `_lisa/lisa-kernel/` (not committed).
- MIUI stock OTA `miui_LISAGlobal_V13.0.8.0.SKOMIXM_...zip` (Android 12,
  kernel 5.4) downloaded to `_lisa/rom/` — used to extract the stock `boot.img`
  + DTB (memory map, active panel, boot header format). Keep ROM/boot DTB out
  of git.

## SoC family facts (yupik DTB)

- USB: `usb0: ssusb@a600000` (dwc3 core @0xa600000, same base as SM7150);
  pwr_event IRQ GIC_SPI 130, dp/dm/ss PHY IRQs from PDC (lanes 14/15/17).
  HS PHY `usb2_phy0: hsphy@88e3000` "qcom,usb-hsphy-snps-femto" (different
  phy family from sweet's QUSB2 — check the femto vs qusb2 register surface).
  Second controller `usb1: hsusb@8c00000` exists (not needed).
- UFS: `ufshc_mem: ufshc@1d84000` reg `0x1d84000 0x3000` (+ ICE/HWKM windows),
  `phys = <&ufsphy_mem>`, alias `ufshc1` "Embedded UFS Slot". **Same host
  base as sweet.** ABL leaves it configured exactly like sweet; `vendor`
  partition lives at `soc/1d84000.ufshc`.
- GIC: `intc@17a00000` (GICv3, two security states) — same base as sweet.
- SPMI: `spmi_bus: qcom,spmi@c440000` — same base as sweet.
- RPMh: `apps_rsc: rsc@18200000` — same base as sweet.
- DPU: `mdss_mdp: qcom,mdss_mdp@ae00000` (sweet mdss is at 0x9c000x000
  region — different).
- QUPv3 wrappers: `qupv3_0 @9c0000`, `qupv3_1 @ac0000`.
- Console UART: `qupv3_se5_2uart @994000` (GIC_SPI 606), alias `serial0` —
  like sweet, no exposed UART pins on the phone; framebuffer + USB-ECM stay
  the only console paths.
- CPU: 8x `qcom,kryo`, MPIDR aff0 = 0..7, `enable-method = "psci"` —
  4x A55 (CPU0-3, "SLVR", capacity 1024) + 3x A78 (CPU4-6, GOLD) + 1x prime
  A78 (CPU7). Same homogeneous-SMP shape as sweet (cpus=8, clamp in QCOM.h).
- Memory: **no `memory@` node in the DTS** (bootloader fills it). The
  `mem-offline` node hints at the standard SM7325 banks —
  `offline-sizes = <0x1 0x40000000 0x0 0x40000000>, <0x1 0xc0000000 0x0
  0x80000000>, <0x2 0xc0000000 0x1 0x40000000>, granule = <512>` — i.e. a low
  bank at 0x40000000 (1 GiB) and the rest above 0xc0000000 — **to be
  confirmed** from the stock boot cmdline/DTB.
- PMIC set (SPMI SIDs): PMK8350=0 (PON, ADC, RTC `rtc@6100`), PM7325=1,
  PM8350C=2, PM7325B=3 (battery gauge family, SMB139x charger pumps),
  PMR735A=4. (sweet: PM6150/PM6150L family.)

## Per-device constants: sweet -> lisa mapping

Read from the running sweet sources in `_lisa/buildtree/xnu` (values href
`bsd/dev/sweet_touch.c` etc.) against the lisa DTS.

| subsystem | sweet (SM7150) | lisa (SM7325) | note |
|---|---|---|---|
| touch bus | GENI I2C SE7 @`0xa84000` (`TOUCH_SE_PA`) | GENI **SPI** SE13 @`0xa94000` (wrapper1 SE5; `GCC_QUPV3_WRAP1_S5_CLK`), IRQ GIC_SPI 358 | `lisa-sm7325.dtsi:56`, `yupik-qupv3.dtsi:651` |
| touch IC | Goodix GT9896, I2C addr 0x5d | Goodix **GT9897T** via SPI (`xiaomi,k9-spi`), 6 MHz, max-touch 255 | FW/config blobs `goodix_gt9897t_{fw,cfg}_k9d*.bin`, two panel makers 0x36/0x42 |
| touch GPIOs | vol/INT via `TOUCH_GPIO_CFG` (TLMM NORTH 0x03900000) | INT `tlmm 81` (0x2008 = fall edge), RESET `tlmm 105` | TLMM on SM7325: same NORTH region? (verify from stock DTB) |
| wrapper clock vote | `TOUCH_GCC_VOTE = 0x5200c` + `TOUCH_RCG = 0x18148` (wrapper1 local S1 = global SE7) | GCC offset for `QUPV3_WRAP1_S5_CLK` — **different GCC register offsets, to be read from gcc-yupik.h/stock DTB**; wrapper1 RCG local S5 | clock tree differs per SoC |
| touch rails | L11C (rpmh `ldo11c`) | avdd = **L7C** `pm8350c_l7` (rpmh resource `ldoc7`, 3.008 V HPM); iovdd = **L2C** `pm8350c_l2` (`ldoc2`) | `lahaina-regulators.dtsi:654/561` |
| keys (PON/VolDn) | PM6150 PON periph `0x0810` sid0 (`KEYS_PON_RT`) | PMK8350 PON, periph base `pon@800`, KPDPWR + RESIN (`pon_hlos@1300` exists) | `pmk8350.dtsi` |
| keys (VolUp) | PM6150L GPIO2 sid4 @`0xc110` (`KEYS_GPIO_RT`) | **PM7325 GPIO6** sid1, pinctrl base `@8800` (per-gpio SPMI periph address found by enumeration, like sweet_spmi.c does) | `xiaomi-sm7325-common.dtsi:20` |
| USB core | 0xa600000 + QSCRATCH `0xa6f8800` (`core + 0xf8800`), HS/SS phy ctrl 0x10/0x30 | core 0xa600000 (same); QSCRATCH layout — **probably same core+0xf8800, confirm from stock DTB/livedriver** | `yupik-usb.dtsi:6` |
| USB phy family | QUSB2 + QMP | **snps-femto** HS `@88e3000` + QMP-DP `usb_qmp_dp_phy@88e8000` | femto HS phy ctrl bits must be checked (VBUS override path differs from QUSB2?) |
| panel | DSC panel via mdss @0x9c000x000, 1080x2400 | `dsi_k9d_36_02_0a_dsc_cmd`: cmd-mode DSC **30bpp**, 1080x2400, burst, 4 lanes, 68x152 mm, on DPU @`0xae00000` | `display/dsi-panel-k9d-36-02-0a-dsc-cmd.dtsi` (stock active panel to confirm; mi_ts flags say up to 120 Hz variants exist) |
| PMIC gauge | (sweet: static 100 % battery so far) | PM7325B + SMB139x charger, `/battery` node in xiaomi DTB | real fuel gauge data available via SPMI observer (same approach as SweetBattery, new register map) |
| DDR | 2 banks, upper ~3.95 GiB bank used as physmap | stock map pending (see mem-offline above) | stock `boot.img` cmdline |
| boot image | mkbootimg boot-v2, ABL boots it via `fastboot boot` | stock boot header version **pending** (Android 12 MIUI, kernel 5.4, non-GKI — expect v3/v4) | must match lisa ABL expectations for `fastboot boot` |

## Bring-up plan (mirrors the sweet ladder)

1. **Config** (done, patch 0082): `MACHINE_CONFIG=LISA` builds
   `kernel.development.lisa` byte-identical to sweet except machine name.
2. **Boot shim**: reuse boot-only flow (`fastboot boot`, never flash). The
   Xiaomi ABL hangs after userdata flashes — that path stays forbidden.
   Panic-then-reboot-to-bootloader flag (emergency recovery) still applies.
3. **ADT**: `tools/mkadt.py` gains a lisa machine (SM7325 MMIO list, DDR map
   from the stock DTB, panel canvas from /chosen `framebuffer`, `D22AP` /
   `iPhone10,3` identity kept for MobileGestalt).
4. **Kernel drivers** (each is a small constants change + CI loop; values from
   the table above, verified against the stock DTB):
   - `sweet_early.c` physmap/bootargs — DDR map from stock firmware;
   - `sweet_ufs.c` — host base identical; verify ICE/HWKM quirks (HWKM on
     SM7325 — region `ufs_ice_hwkm` exists);
   - `sweet_usb.c` — verify QSCRATCH + femto HS phy VBUS override;
   - `sweet_touch.c` — new GENI SPI SE path (FIFO mode, no GPI DMA), GT9897T
     protocol (K9 family like GT9896), L7C/L2C votes, new GCC clock offsets;
   - `sweet_keys.c` — PON 0x8x0 sid0 + PM7325 GPIO6 sid1;
   - `sweet_spmi.c`/`sweet_rpmh.c` — PMIC set/name diffs;
   - panel: ABL-provided framebuffer path first (identical), native DSI later.
5. **Userspace**: same iOS 16.7 root flow, IPSW 20H392; image v15->new image
   for lisa rootfs; SMBIOS/identity answers: MobileGestalt identity (iPhone X)
   already carried by ADT; XNU reads SMBIOS from the ADT — synthesizing the
   tables is straightforward.

## Stock-DTB facts (merged boot DTB, extracted 2026-10-22)

From the MIUI V13.0.8.0.SKOMIXM OTA (`_lisa/rom/`, kept out of git):
`payload.bin` → `boot.img`/`dtbo.img`/`vendor_boot.img` via
`tools/payload-peek.py` (note: the payload uses the *modern* protobuf enum;
type 8 = REPLACE_XZ — an early op-label sheet that called it SOURCE_INSERT
was wrong). `boot.img` is **header v3** (kernel 47.5 MB + ramdisk, no DTB
inside); the nine platform base DTBs (~460 KiB each) sit in `vendor_boot.img`
after the vendor ramdisk, and the per-board overlays live in `dtbo.img`
(32 entries; **entry 16** = `lisa-sm7325-overlay`).

- **lisa's compiled DTB = yupik base (#7/entry 0x1c5000, `qcom,yupik`,
  `qcom,msm-id = <0x1db 0x10000 0x203 0x10000>`) ⊕ lisa overlay (entry 16)**,
  merged locally with `fdtoverlay` into `_lisa/rom/parts/merged/lisa-base07.dtb`.
  Only yupik bases 07/08 resolve the overlay's `__symbols__`; 07 = yupik
  (matches the source tree's `.dtbo-base := yupik.dtb`), 08 = yupikp
  (`qcom,yupikp`, msm-id 0x1f3) — the two merged trees differ only in the
  root node. So `mkbootimg --dtb` for lisa should carry the **merged**
  lisa-base07.dtb (single blob), like the sweet shim's boot_unpacked dtb.
- `memory { reg = <0 0 0 0> }` — filled by ABL at boot; ground truth is the
  **live FDT**, not this blob.
- `reserved-memory` starts at **0x80000000** (hyp_mem) and carves through
  0xd8000000 (`pil_trustedvm` ends 0xd7ef7000, neuron shbufs at 0xd7f00000
  / 0xd7f80000 0x80000 each), with **nothing carved in the low bank
  0x40000000–0x7fffffff** per the compiled tree — first-guess kernel RAM =
  0x40000000 + 0x40000000 (1 GiB), pending live-FDT validation;
  `splash_region reg = <0 e1000000 0 0x2300000>` inside it (32 MiB CLADE
  window starts 0xe0000000).
- `splash_memory @ 0xe1000000 size 0x2300000` (32 MiB, "cont_splash_region")
  — **lisa framebuffer base for the shim console = 0xe1000000**, 1080x2400.
  DTS says 0x2400000, stock merged DTB says 0x2300000 (Xiaomi trimmed it).
  Any video=vfb carve in cmdline belongs to the Linux splash fb, not ours.
- TLMM: **`pinctrl@f000000` reg 0xf000000 0x1000000** (16 MiB contiguous,
  per-pin 0x1000 stride — same layout family as sweet's TLMM sections) with
  summary IRQ GIC_SPI 0xd0=208. Touch IRQ tlmm 81 → reg 0xf000000+81*0x1000,
  reset tlmm 105.
- arch timer `arm,armv8-timer` @19.2 MHz with PPI indices 11/12/13/14
  (= INTIDs 27–30); sweet's proven `QCOM_VTIMER_INTID = 19` stands in QCOM.h
  for both machines — **verify which INTID actually fires at first lisa
  boot**; if the timer is silent, flip the LISA variant to an INTID knob.
- `mem-offline` node: `offline-sizes = <1 0x40000000 0 0x40000000>,
  <1 0xc0000000 0 0x80000000>, <2 0xc0000000 1 0x40000000>`, granule 512
  (0x200): consistent with a 1 GiB bank @0x40000000, 2 GiB @0xc0000000 and
  ~4-5 GiB above 0x140000000. Note the *driver never parses offline-sizes*;
  informational only.
- cpus: 8x kryo, mpidr aff0 0..7, psci; caps 0x400 (A55 x4),
  0x79a (A78 gold x3), 0x7c1 (prime) — same homogeneous-SMP shape as sweet.
- Panel: `sde_dsi` primary display with `qcom,dsi-default-panel =
  <dsi_prim_sim_vid>` (the **simulator** panel phandle 0x58e) — Xiaomi
  resolves the *real* panel (k9d variants: 36_02_0a, 42_02_0a, 42_0d_0b) at
  runtime; the compiled tree's default is the simulator. For ios-sweet's
  phase 1 this is moot: console = ABL splash fb; native DPU work comes later
  on hardware.
- vendor cmdline (v3, no dtb in boot.img): `console=ttyMSM0` etc.;
  `androidboot.usbcontroller=a600000.dwc3` — confirms usb0 as the only
  gadget controller, same base as sweet.

## No-phone groundwork done (2026-10-22)

- `tools/mkbootimg.py` vendored (AOSP Apache-2.0, gki-cert module NOT vendored
  → v4 GKI signing raises); builds v2 images standalone on this Mac, verified.
- `shim/build.sh` now toolchain-portable: CC env override with
  aarch64-linux-gnu-gcc → aarch64-elf-gcc fallback; objcopy/nm/readelf derived
  from the triplet; `DTB=path` overrides the boot dtb. Verifies: full shim
  image compiles on macOS (brew `aarch64-elf-gcc` 16.2.0); it intentionally
  fails only when `dtb/boot_unpacked/dtb` is absent — the lisa merged dtb is
  `DTB=_lisa/rom/parts/merged/lisa-base07.dtb` for a local build.
- `tools/payload-peek.py`: minimal payload.bin partition extractor (REPLACE /
  REPLACE_XZ / REPLACE_BZ with hash verification) — pulled boot, dtbo,
  vendor_boot out of the 3.5 GiB OTA without installing xxd-bomb tooling.
- `docs/lisa-port.md` (this file) carries every stock-DTB fact.

## What can/cannot be done without the phone

- Done without phone: everything above (this file), the LISA machine config
  (CI-built), mkbootimg/shim toolchain on macOS, ADT generator branch,
  stock-ROM DTB research, boot-arg `sweetlisa=…` planting.
- Needs the phone: every boot test, the ABL reaction to our boot image
  (header version), PDC/IRQ verification, USB ECM bring-up constants,
  physmap/DDR tuning, touch bring-up.

## Pending (next sessions)

- [ ] Extract `boot.img` from `_lisa/rom/miui_LISAGlobal_ota.zip` (payload.bin
      -> boot.img), parse DTB: memory map, active DSI panel, boot header
      version, TLMM base, list of reserved-memory regions.
- [ ] mkbootimg parameters for lisa (header version chosen from stock).
- [ ] mkadt.py `--machine lisa`.
- [ ] Per-driver LISA constant patches (table above).
- [ ] CI green for MACHINE_CONFIG=LISA (run in progress when this file was
      written; record run IDs in NOTES.md).
