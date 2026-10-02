# greetd + tuigreet as the display manager (replaces GDM) — 2026-10-02.
#
# Why: GDM's greeter is a whole gnome-shell instance, so even reaching the
# login prompt costs a GNOME startup on the Mali-T880. greetd is a tiny
# daemon; tuigreet is a text UI that runs on the fbcon tty1, which is
# already rotated for the panel (fbcon=rotate:3 in config/gemini.nix
# kernelParams) and is driven by the real keyboard (no OSK needed). The
# point is to make trying different desktops cheap: every session
# registered with `services.displayManager.sessionPackages` (GNOME today,
# anything added later — sway, labwc, …) shows up in tuigreet's F3 session
# menu, and the last pick is remembered per user.
#
# What changes vs the GDM path (services/gnome.nix):
#   - GDM is force-disabled, and so is GDM-style autologin: the greeter is
#     shown on every boot (that is the point — pick a session). For an
#     autologin later, set services.greetd.settings.initial_session.
#   - The GNOME session itself is unchanged (services.desktopManager.gnome
#     stays on; its environment.sessionVariables reach the session through
#     greetd's PAM `login` stack -> pam_env).
#   - greetd's unit is `greetd.service` with the alias
#     `display-manager.service` (nixpkgs greetd.nix), so the existing
#     `systemctl … display-manager.service` users (gemcli sleep's live-unit
#     detection, docs/power-sleep.md) keep working through the alias.
#     services/desktop-select.nix puts its console/gemshell condition on
#     greetd.service when this module is on (a second, separate
#     `display-manager` unit definition would collide with the alias).
#
# Known trade-off (unverified on glass): GNOME's lock screen talks to GDM
# for unlock; without GDM, screen locking may be unavailable or behave
# differently. Sleep here is gemcli/gemini-sleepd, not the GNOME lock.
#
# Rollback: set services.geminiGreeter.enable = false in config/gemini.nix
# and redeploy — GDM + autologin come straight back.
{ config, lib, pkgs, ... }:

let
  cfg = config.services.geminiGreeter;
  # The same session tree GDM would read (wayland-sessions + xsessions of
  # every installed session package).
  sessions = config.services.displayManager.sessionData.desktops;
in
{
  options.services.geminiGreeter = {
    enable = lib.mkEnableOption ''
      greetd + tuigreet as the display manager instead of GDM (text greeter
      on tty1 with a session menu)'';
  };

  config = lib.mkIf cfg.enable {
    # ---- Drop GDM ------------------------------------------------------
    services.displayManager.gdm.enable = lib.mkForce false;
    services.displayManager.autoLogin.enable = lib.mkForce false;

    # ---- greetd + tuigreet ---------------------------------------------
    services.greetd = {
      enable = true;
      # TUI greeter: keep boot messages off tty1 (nixpkgs greetd.nix sets
      # TTYPath/TTYReset/StandardError=journal).
      useTextGreeter = true;
      # tuigreet 0.11.1 at the 26.11pre1068949 pin. Flags checked against
      # its source (crates/tuigreet/src/greeter.rs): --remember +
      # --remember-user-session pre-fill the last user and that user's
      # last session (the two --remember-*session flags are mutually
      # exclusive; per-user is the one that survives a failed login).
      # Session dirs are passed explicitly — greetd's environment has no
      # useful XDG_DATA_DIRS. tuigreet sets XDG_SESSION_TYPE from the dir
      # the session came from (wayland vs x11). Cache dir
      # /var/cache/tuigreet is created by the nixpkgs module.
      settings.default_session.command = lib.concatStringsSep " " [
        (lib.getExe pkgs.tuigreet)
        "--time"
        "--asterisks"
        "--remember"
        "--remember-user-session"
        "--sessions"
        "${sessions}/share/wayland-sessions"
        "--xsessions"
        "${sessions}/share/xsessions"
      ];
    };

    # ---- GPU before the greeter ----------------------------------------
    # services/gnome.nix loads panfrost Before=display-manager.service;
    # name the real unit too so the ordering never depends on alias
    # resolution.
    systemd.services.gemini-panfrost-load =
      lib.mkIf config.services.gnomeDesktop.enable {
        before = [ "greetd.service" ];
      };
  };
}
