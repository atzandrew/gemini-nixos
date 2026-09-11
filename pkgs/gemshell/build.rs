// Link the native libraries the hand-rolled FFI in src/compositor/
// uses. Some of these come from the dependency crates' own
// #[link]/pkg-config machinery, but the crate set is pinned and the
// extern blocks are hand-written — declaring the full set here is
// deterministic and keeps the nix build from drifting on a
// dependency's link emission. Runtime resolution (which mesa/panfrost
// actually serves the GL/EGL calls) goes through the /run/opengl-driver
// libglvnd ICD; these -l flags only satisfy the linker.
fn main() {
    // The `gemshell_check_all` cfg is a type-check escape hatch: compiling
    // with `--cfg gemshell_check_all` on a non-Linux host pulls the Linux
    // backend modules into the build so `cargo check` (which does not
    // link) type-checks them. Declared here so it is not an "unexpected
    // cfg" warning.
    println!("cargo:rustc-check-cfg=cfg(gemshell_check_all)");
    // The link set is platform-specific: Linux links the Wayland/EGL/GLES
    // stack it uses; macOS links the system OpenGL framework (and the
    // pure-Rust wayland backend needs nothing).
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if target_os == "macos" {
        println!("cargo:rustc-link-lib=framework=OpenGL");
        println!("cargo:rustc-link-lib=z");
    } else {
        for lib in [
            // wayland-server crate (system feature): the display + resources
            "wayland-server",
            // wayland-client crate (nested mode): the client protocol
            "wayland-client",
            // hand-declared xkbcommon FFI (src/compositor/input.rs)
            "xkbcommon",
            // hand-declared gbm FFI (src/compositor/gbm.rs)
            "gbm",
            // hand-declared EGL FFI (platform/linux.rs)
            "EGL",
            // hand-declared GL ES FFI (platform/linux.rs)
            "GLESv2",
            // the `png` crate (icon decoding, src/common/icons.rs)
            "png",
            "z",
        ] {
            println!("cargo:rustc-link-lib={lib}");
        }
    }
    println!("cargo:rerun-if-changed=build.rs");
}
