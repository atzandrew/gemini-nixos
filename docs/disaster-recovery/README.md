# Disaster Recovery — Gemini PDA (MT6797) full-flash-erase playbook

> **Status:** 🟡 Planning + artifact audit done 2026-09-07 (host-side);
> ported into gemini-nixos as part of the golden-repo pivot (AGENTS.md
> migration M2). Nothing has been destroyed to test these drills. Every
> preloader-mode (Level 1) read/write is PROVEN on this unit (2026-08-30:
> `printgpt`, readbacks, idempotent `w para`). The BROM-mode (Level 2)
> path is 🔴 **unverified on this unit** — BROM entry (`0e8d:0003`) must
> be validated while the device is healthy (`gather.md` step 5).
> **Last updated:** 2026-09-10

## Purpose

If the unit's flash is corrupted or erased (accidental "Format all",
bad GPT surgery, `dd` of the wrong device, …), this folder is the
playbook to get back to a state where **TWRP boots** — the staging
ground from which `boot` (NixOS/Gemian/Android) and everything else can
be re-flashed. It restores *bootability and identity*, not user data.
This repo is the golden home of the playbook; `GeminiPDA` (legacy)
holds the bulk blobs until migration M3 completes.

## The boot chain this defends (receipts: `docs/boot-process.md` §1;
original LK source receipts in the legacy `GeminiPDA/docs/boot-chain.md`
§8c + `GeminiPDA/repos/gemini-lk`, port pending)

```
BootROM (in-SoC, unerasable)
  → preloader ("boot0" — eMMC hardware BOOT1 area, OUTSIDE the GPT)
  → LK (p20, 512 KiB)
  → p1 recovery  (RECOVERY mode)  |  p22 boot (NORMAL mode)   [GPT lookups]
```

- LK RECOVERY: `para[0..32] == "boot-recovery"` → loads **p1 `recovery`**
  (`app/mt_boot/mt_boot.c:1527-1528`, `platform/mt6797/recovery.c:107`
  strict strcmp). TWRP lives there (16 MiB partition).
- LK NORMAL: loads **p22 `boot`** (`mt_boot.c:1484-1485`). `boot2`/`boot3`
  are **never** loaded by stock LK — do not count them as boot slots.
- No-para escape: **RTC FAC_RESET** — hold power + side button ~10 s →
  RECOVERY even with para cleared (checked before the POC path).
- POC trap: USB attached + no power-key press at LK → charging kernel
  (`0e8d:2008`), not a console. Hold the power key to force NORMAL.
- **2026-09-10:** the repartition left **TWRP (p1) + NixOS (p27 `linux`)**
  as the only systems; `boot2`/`boot3` and the Android/Debian rootfs are
  gone, so "NORMAL" now always means the NixOS `boot` image. GPT backups
  for both the pre- and post-repartition layouts are in
  `stock-dump/repartition-20260910/` (`inventory.md`).

## Failure levels

| Lvl | What died | Device behaviour | Recovery channel | Status |
|---|---|---|---|---|
| 0 | p22 `boot` only (bad image, hang) | no software path back once para is cleared | preloader mode `w boot`, or TWRP via RTC FAC_RESET | ✅ proven |
| 1 | GPT + user-area partitions wiped; preloader (BOOT1) intact | still enumerates **preloader `0e8d:2000`** on every power-on with USB (~9 s window) | preloader-mode mtkclient (`printgpt` → `w lk` → `w recovery` → `w para`) | ✅ proven write path |
| 2 | everything, incl. eMMC BOOT1 (preloader) | **BROM `0e8d:0003`** only (SoC BootROM survives — it is in-chip) | BROM mode + DA upload; needs a preloader image, raw GPT, scatter | 🔴 unverified — close gather.md steps 1–3, 5, 7 |

## Doc map

| Doc | Contents |
|---|---|
| `README.md` | this index + decision tree |
| `inventory.md` | **what is archived, where, sha256** (ledger + migration copy status); stock firmware; tooling; the missing list |
| `gather.md` | **what to collect from a working device** + exact commands + verification (the gaps in `inventory.md`) |
| `drills.md` | Level 1 and Level 2 restore procedures, write order, safety rules |

## Golden rules (DR-specific; AGENTS rule 0 still applies)

0. **Close the gaps while the device works.** Every row of
   `inventory.md` "missing" is one power-on away from being
   irreplaceable (preloader dump especially — Level 2 recovery depends
   on it).
1. **Never run "Format all" / `mtk f`** on a device whose full inventory
   set is not present and hash-verified. The live flash + `stock-dump/`
   IS the backup.
2. **Never write `nvram`/`proinfo`/`protect*`** except to restore the
   exact readbacks. `stock-dump/nvram.bin` holds the IMEI — private,
   never commit.
3. **Battery first.** A low unit browns out mid-recovery (observed
   2026-09-01: LK "buzz" then power loss). Charge to full (≥3.8 V,
   TWRP dmesg `[PE+]Ibat=`) before any drill.
4. **Write order = bootable intermediate at every step.** Level 2:
   preloader (BOOT1) → GPT → lk (+lk2) → para (full `para-boot-recovery.bin`
   — the 512 KiB image preserves the LK env window at para 0x20000) →
   p1 recovery (TWRP). **Only then** p22 `boot`. Never leave the unit
   with para cleared and an unverified `boot` (that is how boots get
   stuck with no software way back).
5. **Version line per operation** (rule 0): what was written, from which
   file, sha256, outcome → `docs/session-log.md`.
6. **Read-only first.** `printgpt` and readbacks before any `w`; verify
   partition names/offsets against `stock-dump/gpt-2026-08-30.txt`.
7. **Two channels agree.** For any dump that matters, take it via two
   independent channels (DA readback + Linux/TWRP `dd`) and compare
   sha256 before trusting either.

## Where the restore images come from

- GPT (raw primary + backup, both the pre- and the post-2026-09-10
  layouts): `stock-dump/repartition-20260910/gpt-*.bin` — see
  `inventory.md` "Post-repartition map".
- Boot-critical dumps: **`stock-dump/` in this repo** (gitignored;
  boot-critical set copied from the legacy repo 2026-09-07 — ledger +
  copy status in `inventory.md`). Bulk blobs (android images, the
  firmware zip, the 88-image boot backup history) remain in
  `GeminiPDA/stock-dump/` until migration M3; `inventory.md` lists both.
- NixOS `boot.img`/rootfs: built by this flake, flashed via
  `bin/flash-nixos.sh` once TWRP is up — that is this repo's normal
  job, not a DR task.

## Related (this repo + legacy pointers)

- `AGENTS.md` — golden-repo rules; migration status (M1–M7)
- `docs/boot-process.md` — plain-language boot explainer (receipt pointers)
- `docs/repartition-android-space.md` — partition map + para layout §2
- `bin/run-mtk.sh` — patched-mtkclient launcher (preloader/BROM mode;
  **Linux host only** — no USB passthrough into Apple's container);
  `bin/usb-watch.sh` — USB-state watcher (device classification; Linux
  lsusb / macOS system_profiler via `bin/lib/host.sh`)
- `docs/usb-network.md` — the USB gadget NIC (CDC-ECM since 2026-09-17)
  and the host-side Linux/macOS helpers used by the DR scripts
- `keys/` — the device SSH identity, committed: `gemini_ed25519` is both
  the admin login key and the **pinned sshd host key**, so a rebuild from
  scratch needs no key provisioning and does not change the device's
  identity (`keys/README.md` has the rationale + the accepted trade)
- Legacy receipts until M1: `GeminiPDA/docs/{flashing,boot-chain,hardware}.md`
- Legacy session history until M7: `GeminiPDA/docs/session-log.md`
