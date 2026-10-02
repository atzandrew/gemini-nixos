# Minimal root-scanning initramfs for the Gemini PDA.
#
# WHY NOT MOBILE NIXOS STAGE-1 (docs R1, headline risk)
# -----------------------------------------------------
# Measured on this kernel/config: the Mobile NixOS stage-1 initrd is
# 9.66 MiB xz (39 MiB unpacked: loader + mruby + systemd-minimal + udev
# + lvgui stack). With the bring-up kernel payload (13.45 MiB gz + DTB)
# the boot image totals ~23.3 MiB — far over the 16 MiB `boot`
# partition cap.
#
# This initrd follows the verified bring-up design instead
# (GeminiPDA/build/initramfs-6.6/init, ~1.3 MiB gzip): the stock
# MediaTek LK copies the *compressed* ramdisk verbatim to 0x45000000
# and passes it to the kernel via ATAG_INITRD2; the kernel
# (CONFIG_RD_GZIP=y) decompresses it itself. So a plain busybox
# cpio.gz is exactly the contract, and gzip matches the verified
# reference format.
#
# DUAL-BOOT (2026-09-07, docs/repartition-android-space.md §5): the
# initrd is the NixOS/Debian selector. LK boots it only in NORMAL mode —
# when the para command @ offset 0 is exactly "boot-recovery" LK goes to
# p1 TWRP instead and never loads this ramdisk. So the remaining command
# values select the OS:
#
#   "boot-debian\0"+zeros  -> Debian branch: ext4 rootfs with
#                             /etc/os-release (p29), replicating the
#                             proven GeminiPDA handoff (A72 opt-in +
#                             fstab fix) -> switch_root /sbin/init.
#   "" (zeros) / unknown   -> NixOS branch (the DEFAULT): the Mobile
#                             NixOS store-only image (p32 userdata,
#                             ext4 label NIXOS_SYSTEM); by-label symlink
#                             + generation lookup -> switch_root <gen>/init.
#   mode target missing    -> fallback: boot the first rootfs of the
#                             OTHER kind, so out-of-order states (e.g.
#                             para cleared before p32 is flashed) boot
#                             Debian instead of a dead shell.
#
# The initrd scans partitions by content, never by name: the mmcblk
# numbering is not stable on this unit (mmcblk0 vs mmcblk1), and the
# para partition is found as p2 of the LARGEST whole mmcblk (the same
# rule the host-side bin/ scripts use). Everything else (udev,
# networking, GPU, ...) happens in stage-2 (the NixOS systemd system).
#
# BUILD NOTE: this deliberately does NOT use nixpkgs makeInitrd, which
# archives the *nix-store closure* of its inputs (a static busybox
# still drags in the whole glibc package via its derivation
# references, ~12 MiB). Instead it stages a flat tree of plain file
# copies and makes the cpio directly — the same approach as the
# verified bring-up script. The output contains no store paths.
{ pkgs, ... }:
let
  lib = pkgs.lib;

  # Static: no dynamic libc/libgcc to ship at all.
  busybox = pkgs.busybox.override { enableStatic = true; };

  # Static e2fsck (e2fsprogs 1.47.x, musl-static, ~1.3 MB) for the
  # boot-time ext4 auto-repair in the init script (2026-09-09). Busybox
  # has no ext4 fsck, and the kernel's mount-time journal replay cannot
  # recover a torn orphan chain (the 2026-09-09 boot-panic incident).
  # Plain file copy like busybox — no store paths in the cpio; the
  # boot.img stays well under the 16 MiB partition cap.
  e2fsck = (pkgs.pkgsStatic.e2fsprogs.overrideAttrs (oldAttrs: {
    postPatch = (oldAttrs.postPatch or "") + ''
      sed -i 's/blocksize=4096/blocksize=16384/g' tests/m_minrootdir/script
      sed -i 's/ -b 4096/ -b 16384/g' tests/m_minrootdir/script
      sed -i 's/blocksize=4096/blocksize=16384/g' tests/m_rootdir/script
      sed -i 's/ -b 4096/ -b 16384/g' tests/m_rootdir/script
    '';
    # pkgsStatic (musl) e2fsprogs is not on cache.nixos.org, so it always
    # builds locally — and its test suite fails on SELinux build hosts
    # (e.g. Fedora Asahi): every test file carries a security.selinux
    # xattr that `mke2fs -d` cannot copy, breaking m_rootdir. We only
    # ship the e2fsck binary in the initrd; skip the host-dependent tests.
    doCheck = false;
  })).bin;

  # The applets the init script uses. `sh` is busybox ash; the shebang
  # in /init is `#!/bin/busybox sh`.
  applets = [
    "sh"
    "mount"
    "umount"
    "mkdir"
    "mknod"
    "cat"
    "ls"
    "echo"
    "uname"
    "dmesg"
    "grep"
    "sed"
    "awk"
    "sleep"
    "switch_root"
    "losetup"
    "true"
    "cp"
    "rm"
    "ln"
    "chmod"
    "find"
    "blkid"
    "readlink"
    "dd"
    "cmp"
    "basename"
  ];

  init = pkgs.writeText "gemini-initrd-init" ''
    #!/bin/busybox sh
    # Gemini PDA dual-boot initramfs (para-selected rootfs).
    #
    # Booted by the stock MediaTek LK in NORMAL mode (para command @
    # offset 0 != "boot-recovery"; that exact command sends LK to p1
    # TWRP and never reaches this ramdisk — see initrd.nix header and
    # docs/repartition-android-space.md §5).

    echo "==> gemini-nixos initramfs: starting (kernel $(uname -r))"

    mount -t proc proc /proc 2>/dev/null
    mount -t sysfs sysfs /sys 2>/dev/null
    mount -t devtmpfs devtmpfs /dev 2>/dev/null
    mkdir -p /dev/pts /newroot /tmp
    mount -t devpts devpts /dev/pts 2>/dev/null

    echo "==> waiting for eMMC block devices"
    i=0
    while [ "$i" -lt 30 ]; do
        i=$((i + 1))
        ready=""
        for blk in /dev/mmcblk[0-9]*; do
            [ -b "$blk" ] || continue
            case "$blk" in
            *boot0|*boot1|*rpmb) continue ;;
            esac
            # a whole eMMC whose GPT is scanned shows up as ...p2 (para)
            if [ -b "$blk"p2 ]; then ready=1; break; fi
        done
        [ -n "$ready" ] && break
        sleep 1
    done

    # ---- boot selector: the 32-byte para command @ offset 0 ------------
    # para = p2 of the LARGEST whole mmcblk (mmcblk numbering shifts
    # between kernel builds; boot0/boot1/rpmb have no p2). Byte-exact
    # compare against the reference marker — shell vars cannot hold NULs,
    # so cmp on files (dd captures the raw 32 bytes first).
    PARA=""
    bs=0
    for blk in /dev/mmcblk[0-9]*; do
        [ -b "$blk" ] || continue
        case "$blk" in
        *boot0|*boot1|*rpmb) continue ;;
        esac
        [ -b "$blk"p2 ] || continue
        s=0
        # (basename keeps the block name for the sysfs size read; shell
        # param expansion is avoided here — nix string interpolation)
        read -r s < "/sys/class/block/$(basename "$blk")/size" 2>/dev/null || s=0
        if [ "$s" -gt "$bs" ]; then bs=$s; PARA="$blk"p2; fi
    done

    # zeros (or anything unreadable/unknown) -> NixOS default
    MODE=nixos
    if [ -n "$PARA" ]; then
        dd if="$PARA" of=/tmp/para bs=32 count=1 2>/dev/null || true
        # reference: "boot-debian\0" + 20 NULs — exactly what bin/ and
        # gemini-boot-debian write (32 bytes).
        printf 'boot-debian\0' | dd of=/tmp/ref-debian bs=32 count=1 conv=sync 2>/dev/null || true
        if [ -f /tmp/para ] && cmp -s /tmp/para /tmp/ref-debian 2>/dev/null; then
            MODE=debian
        fi
    fi
    echo "==> boot selector: $MODE (para=$PARA)"

    # ---- rootfs scan ------------------------------------------------------
    # NixOS store-only image: /nix/store + a registration file (first
    # boot, removed after store re-hydration) or a system profile (later
    # boots). Debian rootfs: /etc/os-release at the filesystem root.
    # A DIRTY ext4 (unclean journal after a crash / mid-write eMMC abort)
    # cannot mount -o ro (EUCLEAN — replay needs rw): fall back to an rw
    # probe, which replays the journal and mounts.
    ROOT=""      # the mode target
    KIND=""      # nixos | debian
    FALLBACK=""  # first valid rootfs of the OTHER kind (out-of-order safety)
    echo "==> scanning for rootfs (mode: $MODE)"
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
        for blk in /dev/mmcblk*; do
            [ -b "$blk" ] || continue
            case "$blk" in
            *boot0|*boot1|*rpmb) continue ;;
            esac
            if mount -t ext4 -o ro,noload "$blk" /newroot 2>/dev/null || \
               mount -t ext4 -o rw "$blk" /newroot 2>/dev/null; then
                is_nixos=""
                if [ -d /newroot/nix/store ]; then
                    # nixos only counts with a resolvable system profile OR the
                    # first-boot registration file. RESOLVE the profile chain by
                    # hand: the final target is an ABSOLUTE /nix/store path, so
                    # plain `[ -e ]` on the symlink fails inside this initrd
                    # (no /nix/store here) — NixOS could only ever boot once
                    # per flash (first boot via nix-path-registration), every
                    # later boot silently fell back to Debian. [fixed 2026-09-07]
                    # Chain: system -> system-N-link -> /nix/store/gen.
                    p=$(readlink /newroot/nix/var/nix/profiles/system 2>/dev/null) || p=""
                    case "$p" in
                        /nix/store/*) : ;;
                        *)
                            q=$(readlink "/newroot/nix/var/nix/profiles/$p" 2>/dev/null)
                            case "$q" in
                                /nix/store/*) p="$q" ;;
                            esac
                        ;;
                    esac
                    if { [ -f /newroot/nix-path-registration ] ||
                         case "$p" in /nix/store/*) [ -x "/newroot$p/init" ] ;; *) false ;; esac
                       }; then
                        is_nixos=1
                    fi
                fi
                is_debian=""
                [ -f /newroot/etc/os-release ] && is_debian=1

                if { [ "$MODE" = nixos ] && [ -n "$is_nixos" ]; } || \
                   { [ "$MODE" = debian ] && [ -n "$is_debian" ]; }; then
                    ROOT="$blk"; KIND="$MODE"
                    umount /newroot
                    echo "==> rootfs found: $ROOT ($KIND)"
                    break 2
                fi
                # remember the first other-kind rootfs for the fallback
                if [ -z "$FALLBACK" ]; then
                    if { [ "$MODE" = nixos ] && [ -n "$is_debian" ]; } || \
                       { [ "$MODE" = debian ] && [ -n "$is_nixos" ]; }; then
                        FALLBACK="$blk"
                    fi
                fi
                umount /newroot 2>/dev/null
            fi
        done
        sleep 1
    done

    if [ -z "$ROOT" ] && [ -n "$FALLBACK" ]; then
        ROOT="$FALLBACK"
        # the mode target was missing — the other kind is what we found
        if [ "$MODE" = nixos ]; then KIND=debian; else KIND=nixos; fi
        echo "!! $MODE rootfs not found — falling back to the $KIND rootfs on $ROOT"
    fi
    if [ -z "$ROOT" ]; then
        echo "!! no usable rootfs found — dropping to a shell"
        exec /bin/sh
    fi

    # ---- ext4 auto-repair before the rw mount (2026-09-09) --------------
    # A hard power-off can leave the rootfs needing journal replay, or a
    # torn orphan chain that the KERNEL's mount-time replay cannot recover
    # (oops/panic every boot — the 2026-09-09 incident; offline e2fsck -y
    # in TWRP was the fix). Run the same offline repair HERE, before the
    # kernel ever replays the journal. e2fsck -y on a HEALTHY fs skips in
    # milliseconds (1.47 check_if_skip: no full 5-pass scan unless
    # VALID_FS/ERROR_FS/orphan/RECOVER signals or mount-count/interval
    # forcing — all absent/disabled on this fs); on a dirty fs it replays
    # the journal; on a damaged fs it runs the full repair. Unattended-safe
    # (-y), offline (fs unmounted), applies to both the NixOS and Debian
    # branches.
    echo "==> e2fsck -y $ROOT (auto-repair; skips fast when clean)"
    e2fsck -y "$ROOT" 2>&1 || echo "!! e2fsck -y returned an error — continuing to mount"

    echo "==> mounting $ROOT rw"
    if ! mount -t ext4 -o rw "$ROOT" /newroot 2>/dev/null; then
        echo "!! rw mount of $ROOT failed — e2fsck -y repair, then retry"
        e2fsck -y "$ROOT" 2>&1 || true
        mount -t ext4 -o rw "$ROOT" /newroot 2>&1 || { echo "!! mount failed after repair — shell"; exec /bin/sh; }
    fi

    if [ "$KIND" = debian ]; then
        # ---- Debian handoff ---------------------------------------------
        # Replicates the proven GeminiPDA initramfs
        # (GeminiPDA/build/initramfs-6.6/init) exactly, so Debian boots
        # unchanged through this ramdisk.
        [ -x /newroot/sbin/init ] || { echo "!! no /sbin/init on $ROOT — shell"; exec /bin/sh; }

        # A72 opt-in: the automatic a72-up.service fires early in boot and
        # the A72-cluster power-on WEDGES the box while boot is still busy
        # (RCU stall / eMMC+i2c timeouts). A72 bring-up is opt-in from a
        # settled system (manual cl2-up.sh ~5+ min after boot). Enforce
        # before systemd (belt: remove the enable symlink; braces: neuter
        # an empty cl2-up.sh).
        if [ -f /newroot/etc/systemd/system/a72-up.service ]; then
            rm -f /newroot/etc/systemd/system/multi-user.target.wants/a72-up.service
            echo "==> a72-up.service disabled (opt-in A72 bring-up)"
        fi
        if [ -f /newroot/root/cl2-up.sh ]; then
            [ -f /newroot/etc/systemd/system/a72-up.service ] || \
                { grep -q "a72-up.service disabled" /newroot/root/cl2-up.sh 2>/dev/null || \
                  { echo '#!/bin/sh' > /newroot/root/cl2-up.sh; chmod +x /newroot/root/cl2-up.sh; \
                    echo "==> cl2-up.sh neutered"; }; }
        fi

        # the Debian rootfs fstab hardcodes /dev/mmcblk0p29; the eMMC may
        # be mmcblk1 on this unit — fix the / entry to the mounted device
        if [ -f /newroot/etc/fstab ]; then
            sed -i "s|^[^ ]*[[:space:]]/[[:space:]]|$ROOT / |" /newroot/etc/fstab 2>/dev/null || true
            echo "==> fstab root -> $ROOT"
        fi

        echo "==> switching root to Debian /sbin/init"
        exec switch_root /newroot /sbin/init
    fi

    # ---- NixOS branch (store-only image) --------------------------------
    # NixOS's fstab references / by label (/dev/disk/by-label/NIXOS_SYSTEM).
    # A normal NixOS initrd provides that symlink via udev; this one has
    # no udev, so create it on devtmpfs before handing over — systemd
    # resolves the fstab / entry against it at mount time.
    label=$(blkid -s LABEL -o value "$ROOT" 2>/dev/null)
    if [ -n "$label" ]; then
        mkdir -p /dev/disk/by-label
        ln -sf "$ROOT" "/dev/disk/by-label/$label"
        echo "==> /dev/disk/by-label/$label -> $ROOT"
    else
        echo "!! could not read ext4 label of $ROOT — systemd may not find /"
    fi

    # Locate the nixos-system generation.
    GEN=""
    # First boot: the closure registration file (removed after the store
    # is re-hydrated on first boot).
    p=$(grep "^/nix/store/[a-z0-9]*-nixos-system-" /newroot/nix-path-registration 2>/dev/null | sed -n 1p)
    # Later boots: the system profile (system -> system-N-link -> store path).
    if [ -z "$p" ]; then
        p=$(readlink /newroot/nix/var/nix/profiles/system 2>/dev/null)
        case "$p" in
            /nix/store/*) :
            ;;
            *)
                q=$(readlink "/newroot/nix/var/nix/profiles/$p" 2>/dev/null)
                case "$q" in
                    /nix/store/*) p="$q" ;;
                esac
            ;;
        esac
    fi
    if [ -n "$p" ] && [ -x "/newroot$p/init" ]; then
        GEN="$p"
    fi

    if [ -z "$GEN" ]; then
        echo "!! no NixOS generation found in rootfs — dropping to a shell"
        exec /bin/sh
    fi

    echo "==> switching root to $GEN/init"
    exec switch_root /newroot "$GEN/init"
  '';
in
# cpio/gzip run on the build host (cross-build); the inputs are only
# used to stage the tree, never referenced from the output.
pkgs.runCommand "gemini-minimal-initrd" {
  nativeBuildInputs = with pkgs.buildPackages; [ cpio gzip ];
  busyboxBinary = "${busybox}/bin/busybox";
  initScript = init;
} ''
  mkdir -p $out root/bin root/dev root/proc root/sys

  # Plain file copy: the cpio must contain no nix/store paths.
  cp $busyboxBinary root/bin/busybox
  chmod 555 root/bin/busybox
  # static e2fsck (boot-time auto-repair, see init) — plain copy, self-contained
  cp ${e2fsck}/bin/e2fsck root/bin/e2fsck
  chmod 555 root/bin/e2fsck
  for app in ${lib.escapeShellArgs applets}; do
    ln -s busybox root/bin/$app
  done

  cp $initScript root/init
  chmod 555 root/init

  (cd root && find . -print0 | cpio --quiet -o -H newc -R +0:+0 --reproducible --null | gzip -9n > $out/initrd)
  ln -s initrd $out/initrd.gz
''
