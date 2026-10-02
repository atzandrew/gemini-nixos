# Desktop/session selection — GNOME, gemshell, or the framebuffer console

Last updated: 2026-10-02 (greetd + tuigreet replace GDM — see "Login via
greetd + tuigreet"; 2026-09-12: COSMIC and niri removed, modes are
`gnome|gemshell|console`)

**Status: the selector + GNOME/gemshell/console modes are ON GLASS.**
GNOME (the default) is verified live (AccountsService session set,
sentinel logic in place, GNOME session preserved); gemshell is on glass
since 2026-09-11 (reworked 2026-09-12, docs/gemshell.md); console stays
on the fbcon tty1. **COSMIC and niri were REMOVED 2026-09-12** — their
session modules and packages are no longer built or selectable. The
sections below that discuss them are kept only as the removal record.

## The ask

> Try the COSMIC desktop without breaking GNOME. Can they co-exist? Can
> `gemcli` switch between them and/or set the startup default? A third
> no-desktop mode that just stays in the framebuffer console would be
> useful.

## Current modes (2026-09-12)

| Mode | Panel owner | Start path | Notes |
|---|---|---|---|
| `gnome` | `mutter` on card0 (panfrost/kmsro) | GDM autologin → `gnome` session | **Verified on glass** (docs/gnome-feasibility.md). Default. |
| `gemshell` | the `gemshell` compositor on card0 (a SYSTEM service, User=cjdell — not a GDM session) | display-manager skipped; `gemini-gemshell.service` (ConditionPathExists=/run/gemini-console) | Native Rust Wayland compositor + in-process egui settings panel (docs/gemshell.md). **On glass 2026-09-11; reworked 2026-09-12** (egui UI, gemdata abstraction, present() mirror fix). No tty1 getty (the compositor owns the panel). |
| `console` | none (fbcon tty1) | display-manager skipped; `getty@tty1` | No desktop; serial console (ttyS0) unaffected. |

## Login via greetd + tuigreet (2026-10-02)

**Status: built-but-not-deployed (eval-checked only).** GDM is replaced
by greetd with the tuigreet text greeter (`services/greeter.nix`,
toggle `services.geminiGreeter.enable` in `config/gemini.nix`). Reason:
GDM's greeter is a full gnome-shell, and the goal is to try lighter
desktops quickly — tuigreet's F3 menu lists every installed session
(`services.displayManager.sessionData`), remembers the last one per user,
and runs on the already-rotated fbcon tty1 (`fbcon=rotate:3`).

- The marker modes are unchanged; with the greeter on, `gnome` means
  "show the greeter" (the AccountsService session the applier writes is
  unused). `gemshell`/`console` still skip the display manager.
- greetd's unit is `greetd.service` with the alias
  `display-manager.service` (nixpkgs `greetd.nix`), so the selector puts
  `ConditionPathExists=!/run/gemini-console` on `greetd.service` when the
  greeter is on (a separate `display-manager` definition would collide
  with the alias). Eval-verified: no `display-manager` unit is defined,
  `greetd.service` carries the condition, `gemini-desktop-apply` and
  `gemini-panfrost-load` are ordered before both names.
- No autologin any more (GDM autologin is force-disabled). Rollback:
  `services.geminiGreeter.enable = false` and redeploy.
- Unverified: GNOME's lock screen without GDM.

## Architecture

```
/var/lib/gemini/desktop              "gnome" | "gemshell" | "console"
        |                            (persistent, root-owned; written by
        |                             `gemcli session set`)
        v
gemini-desktop-apply.service         oneshot, Before=display-manager.service,
  (services/scripts/                 WantedBy=multi-user.target
   gemini-desktop-apply)
        |
        | gnome      : AccountsService SetSession/SetSessionType
        |              for cjdell (Wayland) + rm /run/gemini-console
        | gemshell   : touch /run/gemini-console (NO getty, NO chvt —
        |              gemini-gemshell.service starts on that sentinel)
        | console    : touch /run/gemini-console
        |              + start getty@tty1 (autologin cjdell)
        v
display-manager.service (GDM)
  ConditionPathExists=!/run/gemini-console   -> skipped in gemshell/console
  otherwise GDM auto-logs cjdell into the AccountsService session
```

Module: `services/desktop-select.nix` (`services.geminiDesktop.*`),
wired in `config/gemini.nix`.

## Why a marker and not `services.displayManager.defaultSession`

`defaultSession` is emitted as a GDM **preStart** call to
`set-session` (nixpkgs `gdm.nix`: `preStart = "... set-session
$autologinSession"` when `defaultSession != null`). That runs on
**every** display-manager start, so it would overwrite a runtime choice
on the next boot. `services/gnome.nix` therefore leaves it unset
(`null`), and the applier reproduces set-session's mechanism directly
(`AccountsService` `SetSession` + `SetSessionType("wayland")` via
`busctl` — no python in the closure). GDM reads that saved session for
auto-login. `services/desktop-select.nix` `mkForce`-es the option back
to `null` so any future session-package default cannot clobber the
marker.

## gemcli commands

```
gemcli session status          # marker, console sentinel, AccountsService session
gemcli session list            # the three modes
gemcli session set gnome       # persist; takes effect next boot
gemcli session set gemshell --apply    # also update the applier now
gemcli session set console --reboot    # persist + reboot (clean switch)
gemcli session apply           # re-run the applier for the current marker
```

`set` writes the marker atomically. `--apply` runs the same applier the
boot unit runs; `--reboot` uses the healthy `systemctl reboot`
(docs/power-states.md).

The marker lives on the 58 GiB p27 `linux` rootfs, so it survives
reboots and `nixos-rebuild switch`; the compile-time
`services.geminiDesktop.mode` is only the fallback for a fresh install
with no marker.

**Sleep interacts with the marker:** `gemcli sleep on` stops the
*currently running* panel owner, which it detects from the live units —
`display-manager.service` for GNOME, `gemini-gemshell.service` for
gemshell — and restarts exactly what it stopped on `gemcli sleep off`
(docs/power-sleep.md). That is why the sleep stop-list is
desktop-agnostic rather than keyed off the marker.

## Removed sessions (2026-09-12)

**COSMIC** (`services.desktopManager.cosmic.enable`) and **niri**
(`programs.niri.enable`) were removed on 2026-09-12 along with the
nested **LXQt** (services/lxqt.nix) and **Phosh** (services/phosh.nix)
desktops. Their build facts were verified at the flake pin
`dc5d91f84032` (26.11pre1068949) before removal:

| Session | Provided name | aarch64 cache | Verified on glass? |
|---|---|---|---|
| COSMIC 1.6.0 | `cosmic` | CACHED (2026-09-10) | **never** |
| niri 26.04 | `niri` | CACHED (2026-09-11) | **never** |

The selector no longer references them: `services.geminiDesktop.mode`
is an enum of `gnome|gemshell|console`, `gemcli session set` accepts the
same three, and `gemini-desktop-apply` only handles those. Removing
them also removed `pkgs/labwc-geminipda.nix`,
`pkgs/phoc-geminipda.nix`, `services/lxqt.nix`, `services/phosh.nix`
and `config/lxqt/`.

## On-glass checklist

1. Deploy the toplevel (`bin/deploy.sh`) — no reflash.
2. `gemcli session status` — marker/effective session sane.
3. `gemcli session set gemshell --reboot` — verify no GDM, no tty1
   getty, `gemini-gemshell.service` active and the panel shows the
   gemshell scene (its own checklist: docs/gemshell.md);
   `gemcli session set gnome --reboot` for the return trip.
4. `gemcli session set console --reboot` — verify no GDM, fbcon tty1
   autologin; then back to `gnome`.
5. Log the version lines + outcomes in `docs/session-log.md` (rule 0).

## Files

| Path | What |
|---|---|
| `services/desktop-select.nix` | the selector module (`services.geminiDesktop.*`) |
| `services/scripts/gemini-desktop-apply` | boot applier (marker → AccountsService / console sentinel) |
| `config/gemini.nix` | selector wiring + `services.gnomeDesktop.enable` + `services.gemshellDesktop.enable` |
| `services/gnome.nix` | leaves `displayManager.defaultSession` unset (selector owns it) |
| `pkgs/gemshell/crates/gemdata-device/src/session.rs` | `gemcli session` (modes gnome/gemshell/console) |
| `pkgs/gemshell/crates/gemcli/src/main.rs` | `session set <mode>` clap value_parser (gnome/gemshell/console) |
