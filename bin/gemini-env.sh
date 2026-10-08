# gemini-env.sh — shared variables + safe copy/flash helpers for the Gemini work.
#
# Source it from ~/.bashrc on Hydra, Dragon and the Gemini (see "Install" below).
# When the network changes: edit the three *_IP lines here (or put overrides
# in ~/.gemini-env.local on a machine), then `gemenv_push` from Hydra and open
# new shells (or run `. ~/.bashrc`).
#
# Install (once):
#   Hydra:  echo '. ~/Build/gemini-nixos/bin/gemini-env.sh' >> ~/.bashrc && . ~/.bashrc
#           gemenv_push            # copies it to Dragon + Gemini and hooks their ~/.bashrc
#
# Commands (run `gemenv` any time to see values + this list):
#   Hydra:   to_dragon FILE [NAME]   copy FILE to Dragon:$FLASHDIR/NAME (old file removed first), sha256 both sides
#            to_gemini FILE [DEST]   copy FILE to the Gemini (default: ~/<basename>; old file removed first)
#            from_gemini PATH [DIR]  copy PATH from the Gemini into DIR (default ~/Build/; old file removed first)
#            gssh / dssh             ssh to the Gemini / Dragon
#   Dragon:  flash_boot NAME         mtk w boot $FLASHDIR/NAME
#            flash_all BOOT USERDATA mtk multi "w boot …;w userdata …;reset"
#            backup_boot NAME        mtk r boot $BACKUPDIR/NAME (refuses to overwrite)
#            lsflash                 list $FLASHDIR with sizes + sha256

# ---- machine addresses (edit here) -----------------------------------------
GEMINI_IP=10.0.20.63  # target device (Wi-Fi)                  [other network; home: 192.168.0.139]
HYDRA_IP=10.0.20.128   # dev/build machine (no SSH server)      [other network; home: 192.168.0.218]
DRAGON_IP=10.0.20.129    # flashing machine (USB, mtkclient)      [other network; home: 192.168.0.8]

GUSER=atzero
FLASHDIR=/home/atzero/readytoflash      # images to flash, on Dragon (absolute: mtk wants it)
BACKUPDIR=/home/atzero/gemini-backup    # partition backups, on Dragon
MTK=${MTK:-mtk}

# per-machine overrides (optional), e.g. GEMINI_IP=192.168.0.139 at home
[ -f "$HOME/.gemini-env.local" ] && . "$HOME/.gemini-env.local"

GEMINI=$GUSER@$GEMINI_IP
DRAGON=$GUSER@$DRAGON_IP
export GEMINI_IP HYDRA_IP DRAGON_IP GUSER GEMINI DRAGON FLASHDIR BACKUPDIR MTK

# ---- helpers ---------------------------------------------------------------
_gerr() { echo "gemini-env: $*" >&2; return 1; }

gemenv() {
    echo "GEMINI    = $GEMINI"
    echo "DRAGON    = $DRAGON"
    echo "HYDRA_IP  = $HYDRA_IP"
    echo "FLASHDIR  = $FLASHDIR   (on Dragon)"
    echo "BACKUPDIR = $BACKUPDIR   (on Dragon)"
    echo "Hydra:  to_dragon FILE [NAME] | to_gemini FILE [DEST] | from_gemini PATH [DIR] | gssh | dssh | gemenv_push"
    echo "Dragon: flash_boot NAME | flash_all BOOT USERDATA | backup_boot NAME | lsflash"
}

gssh() { ssh "$GEMINI" "$@"; }
dssh() { ssh "$DRAGON" "$@"; }

# to_dragon FILE [NAME] — NAME is required when FILE is a nix store path
to_dragon() {
    local src="$1" name="${2:-}"
    [ -n "$src" ] || { _gerr "usage: to_dragon FILE [NAME]"; return 1; }
    [ -f "$src" ] || { _gerr "no such file: $src"; return 1; }
    if [ -z "$name" ]; then
        case "$src" in /nix/store/*) _gerr "nix store path: give a name, e.g. to_dragon \$IMG boot-foo-$(date +%Y%m%d).img"; return 1;; esac
        name=$(basename "$src")
    fi
    name=$(basename "$name")
    ssh "$DRAGON" "mkdir -p '$FLASHDIR' && rm -f '$FLASHDIR/$name'" || return 1
    scp "$src" "$DRAGON:$FLASHDIR/$name" || return 1
    ssh "$DRAGON" "chmod u+w '$FLASHDIR/$name'"
    local a b
    a=$(sha256sum "$src" | cut -d' ' -f1)
    b=$(ssh "$DRAGON" "sha256sum '$FLASHDIR/$name'" | cut -d' ' -f1)
    echo "Hydra : $a"
    echo "Dragon: $b  $FLASHDIR/$name"
    [ "$a" = "$b" ] && echo "OK — on Dragon run:  flash_boot $name" || _gerr "CHECKSUM MISMATCH — do not flash"
}

# to_gemini FILE [DEST] — DEST is a path on the Gemini (default ~/<basename>)
to_gemini() {
    local src="$1" dest="${2:-}"
    [ -n "$src" ] || { _gerr "usage: to_gemini FILE [DEST]"; return 1; }
    [ -f "$src" ] || { _gerr "no such file: $src"; return 1; }
    [ -n "$dest" ] || dest="/home/$GUSER/$(basename "$src")"
    case "$dest" in */) dest="$dest$(basename "$src")";; esac
    ssh "$GEMINI" "rm -f '$dest'" || return 1
    scp "$src" "$GEMINI:$dest" || return 1
    ssh "$GEMINI" "chmod u+w '$dest'" && echo "copied → $GEMINI:$dest"
}

# from_gemini PATH [DIR]
from_gemini() {
    local src="$1" dir="${2:-$HOME/Build}"
    [ -n "$src" ] || { _gerr "usage: from_gemini PATH [DIR]"; return 1; }
    rm -f "$dir/$(basename "$src")"
    scp "$GEMINI:$src" "$dir/" && echo "copied → $dir/$(basename "$src")"
}

# gemenv_push — copy this file to Dragon + Gemini as ~/.gemini-env.sh and hook ~/.bashrc
gemenv_push() {
    local self="${BASH_SOURCE[0]:-$HOME/Build/gemini-nixos/bin/gemini-env.sh}" h
    for h in "$DRAGON" "$GEMINI"; do
        ssh "$h" "rm -f ~/.gemini-env.sh" &&
        scp "$self" "$h:.gemini-env.sh" &&
        ssh "$h" "grep -q gemini-env.sh ~/.bashrc || echo '. ~/.gemini-env.sh' >> ~/.bashrc" &&
        echo "pushed → $h"
    done
}

# ---- Dragon side -----------------------------------------------------------
_flashpath() {   # NAME or path → absolute path in $FLASHDIR (must exist)
    local p="$1"
    case "$p" in /*) ;; *) p="$FLASHDIR/$(basename "$p")";; esac
    [ -f "$p" ] || { _gerr "not found: $p   (lsflash to list)"; return 1; }
    echo "$p"
}

lsflash() {
    ls -l "$FLASHDIR"/ && (cd "$FLASHDIR" && sha256sum -- *)
}

flash_boot() {
    local p; p=$(_flashpath "${1:-}") || return 1
    echo "sha256: $(sha256sum "$p" | cut -d' ' -f1)"
    echo ">> $MTK w boot $p"
    $MTK w boot "$p"
}

flash_all() {
    local b u
    b=$(_flashpath "${1:-}") || return 1
    u=$(_flashpath "${2:-}") || return 1
    echo ">> $MTK multi \"w boot $b;w userdata $u;reset\""
    $MTK multi "w boot $b;w userdata $u;reset"
}

backup_boot() {
    local n="${1:-}"
    [ -n "$n" ] || { _gerr "usage: backup_boot NAME.img"; return 1; }
    local p="$BACKUPDIR/$(basename "$n")"
    [ -e "$p" ] && { _gerr "exists, pick another name: $p"; return 1; }
    mkdir -p "$BACKUPDIR"
    echo ">> $MTK r boot $p"
    $MTK r boot "$p"
}
