# How to get iOS 16.5 up and running on lisa (Xiaomi 11 Lite 5G NE)

This guide documents the exact, working, reproducible RAM-only procedure to boot real iOS 16.5 (20F66) with XNU kernel onto `lisa` via `fastboot`. Nothing is flashed, no storage is overwritten, and Android stays intact.

---

## 1. Prerequisites & Environment Facts

- **Device**: Xiaomi 11 Lite 5G NE (`lisa`, SM7325 / Snapdragon 778G / Kodiak platform).
- **SoC equivalence**: Same peripheral window and GIC map as POCO X5 Pro (`redwood`, SM7325-AE).
- **RAM map**: Framebuffer at `0xE1000000` (1080x2400, stride 4320).
- **Mode**: Temporary RAM boot via `fastboot boot` and resident EFI loader (`xnuloader`).
- **Crucial rule**: **NEVER FLASH ANY PARTITION**. All commands use `fastboot boot`, `fastboot stage`, and `fastboot oem run`.

---

## 2. Directory Layout & Key Artifacts

All lisa artifacts live in `_lisa/`:
- `_lisa/boot-images/hackios-lisa-enter.img` — The resident EFI loader image (`xnuloader`).
- `_lisa/reference/hackios-xnu-kernel/userland/system.apfs` — 2.49 GB iOS 16.5 userspace APFS container (`REST=1`).
- `_lisa/reference/hackios-xnu-kernel/userland/root.apfs` — 4 MB RAM disk root APFS.
- `_lisa/reference/hackios-xnu-kernel/xnuloader/mkpayload.py` — Packs XNU kernel + ramdisk container.
- Latest compiled kernel: Downloaded from GitHub Actions run `hackios-lisa-kernel` (under `_lisa/ci-kernel-*/mach.development.lisa`).

---

## 3. Step-by-Step Bring-up Recipe

### Step 1: Boot into the resident loader
Ensure the phone is in fastboot mode (or run `adb reboot bootloader`):
```bash
fastboot boot _lisa/boot-images/hackios-lisa-enter.img
```
Wait ~12-15 seconds for USB re-enumeration. Check that the loader answers:
```bash
fastboot getvar product
# Expected output: product: xnuloader
```

### Step 2: Stage the system container (one-time per phone boot)
`system.apfs` stays resident in RAM across kernel runs as long as the phone doesn't power-cycle:
```bash
fastboot oem ramdisk
fastboot stage _lisa/reference/hackios-xnu-kernel/userland/system.apfs
# ~2.49 GB takes ~78 seconds over USB (~31 MB/s)
```

### Step 3: Bake the payload
Combine the target XNU kernel and the 4 MB `root.apfs` into the container payload:
```bash
python3 _lisa/reference/hackios-xnu-kernel/xnuloader/mkpayload.py \
  -o _lisa/boot-images/payload.bin \
  kernel=<path-to-mach.development.lisa> \
  ramdisk=_lisa/reference/hackios-xnu-kernel/userland/root.apfs \
  --cmdline "loader_return_after=900 epoch=$(date +%s) sb_delay=8 touchscreen=1 touch_lisa=1 touch_debug=1 cpus=8 bootcpu=7 cpufreq=1"
```
*(Omit `-v` to suppress verbose boot console spam and leave the clean Apple lock/home screen visible).*

### Step 4: Stage and run the payload
```bash
fastboot stage _lisa/boot-images/payload.bin
fastboot oem run payload.bin
```
Wait ~12-15 seconds for XNU to take control.

### Step 5: Verify runtime status
```bash
fastboot getvar result
# Expected output: result: running 0 0
```
- `running 0 0`: XNU and iOS userspace (launchd, backboardd, SpringBoard) are alive!
- Wait ~30 seconds for SpringBoard demo choreography to swipe from lock screen to home screen.

---

## 4. Diagnostics & Remote Viewing

### Capture live screenshot over USB:
```bash
# Get framebuffer geometry
fastboot getvar screen
# Trigger loader capture and download staged image
fastboot oem screenshot
fastboot get_staged _lisa/logs/screen.raw
# Convert BGRX to PNG (note: pass geometry as 3 separate arguments in zsh/bash)
python3 -c "
import sys
from PIL import Image
w, h, stride = 1080, 2400, 4320
data = open('_lisa/logs/screen.raw', 'rb').read()
img = Image.frombytes('RGBA', (w, h), data, 'raw', 'BGRA', stride)
img.save('_lisa/logs/screen.png')
"
```

### Capture kernel and loader console log:
```bash
fastboot oem log
fastboot get_staged _lisa/logs/kernel.log
```

---

## 5. Current Blocker: Touchscreen Driver (Goodix GT9897T)

- **Chip**: Goodix GT9897T on SPI engine QUPv3 SE13 (`0xa94000`).
- **Pins**: MISO=52, MOSI=53, CLK=54, CS=55, INT=81, RESET=105.
- **Power**: PM8350C `ldoc2` (IOVDD 1.8V) + `ldoc7` (AVDD 3.0V). Both power on cleanly via RPMh.
- **Current Blocker**: QUP1 S-AHB clock (`0x18008`) stays halted (`0x8000000a`) despite enable votes in `0x52008` (bit 21) being set. Bitbang fallback on GPIOs 52/53 also returned 0.
- **Code**: `hackios/patches/lisa-goodix.patch` applied onto `hackios-xnu-kernel`. Builds via GitHub Actions (`hackios-lisa-kernel.yml`).
