# gemshell — the native Gemini PDA Wayland compositor + desktop shell.
#
# ONE binary: `gemshell` — the compositor (wayland-server: xdg-shell v1,
# wl_shm, seat keyboard+touch; EGL/GBM through Mesa kmsro->panfrost;
# evdev input; compositor-drawn shell chrome: status bar, launcher,
# workspaces, gestures).
#
# 2026-09-12: the old `gemsettings` wl_shm client (hand-drawn CPU UI) was
# REMOVED. The settings panel is now an in-process **egui** overlay
# (GPU-tessellated meshes, real font shaping — src/shell.rs), fed by the
# shared `gemdata::DataProvider`. The workspace also builds the data
# crates (gemdata / gemdata-device / gemdata-dummy) and the gemcli CLI
# frontend (pkgs/gemcli.nix).
#
# Story + on-glass checklist: docs/gemshell.md.
#
# Display path (same as GNOME's, single-process): gbm device on card0 ->
# eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA) -> GLES3 context ->
# eglSwapBuffers page-flips through geminipda-drm's shadow plane; the
# gbm device fd's flip event paces frames. GL symbols resolve at RUNTIME
# through the /run/opengl-driver libglvnd ICD -> mesa-geminipda (kmsro
# pairs card0 with renderD128/panfrost) — exactly the GNOME path
# (docs/gnome-feasibility.md); nothing GL is baked in here.
#
# Link-time needs (all in the system closure already via the GNOME
# stack): libwayland (server+client), libxkbcommon (the xkbcommon crate
# hard-links #[link(name = "xkbcommon")]), libpng (the `png` crate),
# libgbm + libEGL (the hand-rolled EGL/gbm FFI in src/compositor/
# {gbm,render}.rs; libglvnd provides libEGL, the mesa fork provides
# libgbm).
#
# Build = native aarch64 (this flake's canonical model): rustPlatform
# from eval.pkgs; cargoLock.lockFile pins the crate set (regenerate with
# `cargo generate-lockfile` inside pkgs/gemshell and commit). egui is
# pure Rust (bundled fonts), so no extra native build inputs.
#
# Host-side iteration: `bash bin/gemshell-host-check.sh` (cargo check on
# x86_64 in a nix shell; seconds — the fast loop for this crate).
#
# Platform backends (2026-09-11): the crate also builds natively on macOS
# with the host toolchain (`bash bin/gemshell-nested.sh` → `cargo build`),
# where it runs a Cocoa-window + desktop-GL preview (see
# src/platform/macos.rs and docs/gemshell.md). This derivation is the
# Linux/aarch64 product build; the OS-specific half is behind traits, and
# Cargo.toml only enables libwayland's `system` feature for Linux.
#
# Rpath: the gemdemo receipt (2026-09-08) — stdenv's generic rpath fixup
# only pulled the -dev outputs of wayland, so dlopen("libwayland-*.so.0")
# failed on glass. Pin the REAL lib dirs on the RUNPATH in postFixup
# (the ELF references pull them into the runtime closure automatically).
{ lib, rustPlatform, pkg-config, patchelf, wayland, libxkbcommon, libglvnd, libpng, zlib, mesa, libdrm }:

rustPlatform.buildRustPackage rec {
  pname = "gemshell";
  version = "0.1.0";

  # Exclude the host cargo build tree (571 MB of target/ after a local
  # `cargo check`) so the store source stays small and deterministic.
  src = lib.cleanSourceWith {
    src = ./gemshell;
    filter = path: _type: baseNameOf (toString path) != "target";
  };

  # wayland-server/client/protocols 0.31/0.32 + xkbcommon 0.9 + gl 0.14
  # + png 0.17 + ab_glyph + memmap2 + log — pinned by the committed
  # lockfile.
  cargoLock.lockFile = ./gemshell/Cargo.lock;

  nativeBuildInputs = [ pkg-config patchelf ];
  buildInputs = [ wayland libxkbcommon libglvnd libpng zlib mesa libdrm ];

  # -lwayland-server/-lwayland-client resolve from the store, not the
  # system path (same trick as gemdemo).
  RUSTFLAGS = "-L ${wayland}/lib";

  # libgbm (gbm_create_device) comes from the mesa fork; -lgbm finds its
  # .pc via PKG_CONFIG_PATH from buildInputs.

  postFixup = ''
    # Pin the REAL lib dirs on the RUNPATH (the gemdemo receipt, 2026-09-08:
    # stdenv's rpath fixup only pulled the -dev outputs, so dlopen of the
    # sonames failed on glass). The ELF references pull them into the
    # runtime closure automatically.
    ${patchelf}/bin/patchelf --set-rpath \
      "${wayland}/lib:${libxkbcommon}/lib:${libglvnd}/lib:${mesa}/lib:${libpng}/lib:${libdrm}/lib" \
      $out/bin/gemshell
  '';

  doCheck = false; # no host test suite; the on-glass checklist is the test

  meta = with lib; {
    description = "gemshell — native Gemini PDA Wayland compositor + egui desktop shell";
    longDescription = ''
      A custom, minimal, single-process desktop for the Gemini PDA: a
      Wayland compositor (xdg-shell, wl_shm, keyboard + multitouch) that
      renders through EGL/GBM on the geminipda-drm KMS card (panfrost via
      Mesa kmsro), draws its own shell chrome, and hosts an in-process
      egui settings panel backed by the gemdata::DataProvider abstraction.
      See docs/gemshell.md.
    '';
    license = licenses.mit;
    # x86_64-linux is the nested development build (bin/gemshell-nested.sh).
    platforms = [ "aarch64-linux" "x86_64-linux" ];
    mainProgram = "gemshell";
  };
}
