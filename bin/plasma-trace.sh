#!/usr/bin/env bash
# plasma-trace.sh — capture why Plasma is slow to start and why the Leave
# (shutdown/reboot/logout) screen is slow to appear on the Gemini.
#
# Runs ON THE GEMINI (Debian 13, Plasma 6 Wayland) as the desktop user
# (atzero), over SSH. Uses sudo for the system journal / strace attach.
# Writes everything into one tarball in /tmp and prints its path.
#
# Usage (from Hydra):
#   scp bin/plasma-trace.sh atzero@192.168.0.139:/tmp/
#   ssh -t atzero@192.168.0.139 bash /tmp/plasma-trace.sh boot
#   ssh -t atzero@192.168.0.139 bash /tmp/plasma-trace.sh leave
#   scp atzero@192.168.0.139:/tmp/plasma-trace-*.tar.gz ~/Build/gemini-nixos/logs/
#
# Modes:
#   boot   Read-only. systemd-analyze (system + user), boot/user journals
#          with monotonic timestamps, process start times + CPU time,
#          installed background services (Discover/PackageKit, Baloo,
#          KDE Connect, ...), autostart entries, QML caches. Best run
#          soon after a fresh boot + login (before the A72s come up at
#          ~5 min is fine; note the time).
#   leave  Interactive (you need to be at the Gemini). Opens the Leave
#          screen three times; press ENTER on Hydra when it is fully
#          drawn, then Esc on the Gemini:
#            1) over D-Bus, untraced (clean timing; bypasses power key)
#            2) over D-Bus, with strace on ksmserver + powerdevil
#            3) with the POWER BUTTON, strace still attached
#          Also logs dbus-monitor and when the greeter process appears.
#          1 vs 3 separates power-key-path delay from greeter startup.
#          NOTE (2026-10-04): no Gemini key sends KEY_POWER (Esc/On =
#          KEY_ESC, silver = KEY_SLEEP; mt6351-keys.c), so step 3 never
#          opens anything. The on-screen Leave button = steps 1/2.
#   greeter  Interactive. Profiles the Leave screen's own startup. It is
#          D-Bus-activated by the session dbus-daemon (not ksmserver), a
#          fresh process every time. Opens it twice (ENTER when drawn,
#          Esc on the Gemini each time):
#            1) strace -f on the session dbus-daemon (follows the fork
#               into ksmserver-logout-greeter): exec, file, mmap timing
#            2) system-wide perf record (no strace), reported by
#               process/library/symbol
#
# Added 2026-10-04 (KDE startup / Leave-menu investigation).
set -u

MODE=${1:-}
case "$MODE" in boot|leave|greeter) ;; *) echo "usage: $0 boot|leave|greeter" >&2; exit 2;; esac

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1 ($2)" >&2; exit 1; }; }
need systemd-analyze systemd
need journalctl systemd
need busctl systemd

STAMP=$(date +%Y%m%d-%H%M%S)
OUT=/tmp/plasma-trace-$MODE-$STAMP
mkdir -p "$OUT"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}

sudo -v || { echo "sudo needed" >&2; exit 1; }

run() { # run NAME CMD...   -> $OUT/NAME.txt (stdout+stderr, never fatal)
  local name=$1; shift
  { echo "\$ $*"; "$@"; echo "[rc=$?]"; } >"$OUT/$name.txt" 2>&1
}

common() {
  { date; uname -a; cat /proc/uptime; echo "cpus online: $(cat /sys/devices/system/cpu/online)";
    cat /proc/cmdline; free -m; } >"$OUT/00-context.txt" 2>&1
}

pslist() { # processes: start (s since boot), CPU seconds, pid, cmdline — oldest first
  local hz; hz=$(getconf CLK_TCK)
  for d in /proc/[0-9]*; do
    local cmd; cmd=$(tr '\0' ' ' <"$d/cmdline" 2>/dev/null | cut -c1-150)
    [ -n "$cmd" ] || continue
    sed 's/.*) //' "$d/stat" 2>/dev/null | awk -v hz="$hz" -v pid="${d#/proc/}" -v cmd="$cmd" \
      '{ printf "%9.2f %8.2f %7s %s\n", $20/hz, ($12+$13)/hz, pid, cmd }'
  done | sort -n
}

greeter_watch() { # log when a logout-greeter process appears/disappears (200 ms poll; 50 ms cost ~4 CPU-s of pgrep per run)
  local last=""
  while :; do
    local now; now=$(pgrep -f ksmserver-logout-greeter | tr '\n' ' ')
    if [ "$now" != "$last" ]; then
      echo "$(date +%H:%M:%S.%N) $(cut -d' ' -f1 /proc/uptime) greeter pids: ${now:-none}"
      last=$now
    fi
    sleep 0.2
  done
}

mark() { echo "$1 $(date +%H:%M:%S.%N) $(cut -d' ' -f1 /proc/uptime)" | tee -a "$OUT/04-marks.txt"; }

mode_boot() {
  common
  run 01-analyze           systemd-analyze
  run 02-blame             systemd-analyze blame
  run 03-critical-chain    systemd-analyze critical-chain
  run 04-critical-graphical systemd-analyze critical-chain graphical.target
  run 05-user-analyze      systemd-analyze --user
  run 06-user-blame        systemd-analyze --user blame
  run 07-user-critical     systemd-analyze --user critical-chain plasma-workspace.target
  systemd-analyze plot >"$OUT/08-system-plot.svg" 2>/dev/null
  systemd-analyze --user plot >"$OUT/09-user-plot.svg" 2>/dev/null
  run 10-display-manager   systemctl status --no-pager display-manager.service
  sudo journalctl -b -o short-monotonic --no-pager >"$OUT/11-journal-system.txt" 2>&1
  journalctl --user -b -o short-monotonic --no-pager >"$OUT/12-journal-user.txt" 2>&1
  run 13-user-units        systemctl --user list-units --no-pager --all --type=service
  run 14-user-failed       systemctl --user --failed --no-pager
  run 15-system-failed     systemctl --failed --no-pager
  { echo "# start(s since boot)  cpu(s)  pid  cmdline"; pslist; } >"$OUT/16-procs-start-cpu.txt" 2>&1
  ps -eo pid,ppid,etimes,time,rss,stat,args --sort=-time >"$OUT/17-ps-by-cputime.txt" 2>&1
  top -b -n 2 -d 3 -o %CPU | tail -n +1 >"$OUT/18-top.txt" 2>&1
  dpkg -l | grep -Ei 'discover|packagekit|baloo|akonadi|kdeconnect|plasma-browser|kwallet|kde-config|xdg-desktop-portal|kaccounts|kup|drkonqi|tracker|geoclue|modemmanager|bluedevil|bluez|cups|sddm|greetd|plymouth' \
    >"$OUT/19-pkgs-background.txt" 2>&1
  ls -la /etc/xdg/autostart ~/.config/autostart >"$OUT/20-autostart.txt" 2>&1
  { for f in /etc/xdg/autostart/*.desktop ~/.config/autostart/*.desktop; do
      [ -f "$f" ] || continue; echo "== $f"; grep -E '^(Exec|OnlyShowIn|NotShowIn|Hidden|X-KDE-autostart|X-GNOME-Autostart-enabled|X-systemd-skip)' "$f"; done; } \
    >"$OUT/21-autostart-exec.txt" 2>&1
  (command -v balooctl6 >/dev/null && balooctl6 status) >"$OUT/22-baloo.txt" 2>&1
  { du -sh ~/.cache/* 2>/dev/null | sort -h | tail -40; echo; find ~/.cache -maxdepth 3 -iname '*qml*' -type d 2>/dev/null; } \
    >"$OUT/23-cache.txt" 2>&1
  { cat /etc/environment; echo; env | grep -Ei 'qt|qml|kwin|kde|plasma|mesa|pan_' ; } >"$OUT/24-env.txt" 2>&1
  run 25-kded-modules busctl --user call org.kde.kded6 /kded org.kde.kded6 loadedModules
  run 26-plasma-version plasmashell --version
  run 27-dmesg-tail sudo dmesg --level=err,warn
}

mode_leave() {
  need strace "sudo apt install strace"
  need dbus-monitor "sudo apt install dbus-bin"
  common
  local ks pd greeter
  ks=$(pgrep -u "$(id -u)" -x ksmserver | head -1)
  pd=$(pgrep -u "$(id -u)" -f org_kde_powerdevil | head -1)
  greeter=$(dpkg -L plasma-workspace 2>/dev/null | grep -m1 'ksmserver-logout-greeter$')
  echo "ksmserver=$ks powerdevil=$pd greeter=$greeter" | tee "$OUT/01-pids.txt"
  [ -n "$ks" ] || { echo "ksmserver not running — is Plasma logged in?" >&2; exit 1; }
  { ls -la ~/.cache/ | grep -i -e greeter -e qml; ps -o pid,ppid,args -p "$ks" ${pd:+-p "$pd"}; } >>"$OUT/01-pids.txt" 2>&1

  greeter_watch >"$OUT/05-greeter-pids.txt" 2>&1 &
  local gw=$!
  dbus-monitor --session --monitor >"$OUT/02-dbus-session.txt" 2>&1 &
  local dm=$!
  sleep 1

  echo
  echo "=== PART 1/3: Leave screen via D-Bus, NOT traced (clean timing) ==="
  mark dbus-trigger-untraced
  busctl --user call org.kde.LogoutPrompt /LogoutPrompt org.kde.LogoutPrompt promptAll >>"$OUT/04-marks.txt" 2>&1
  echo "Press ENTER here the moment the Leave screen is fully drawn, then Esc on the Gemini."
  read -r _; mark dbus-shown-untraced
  sleep 4

  local targets=(-p "$ks"); [ -n "$pd" ] && targets+=(-p "$pd")
  sudo strace -f -tt -y -s 160 -e trace=%process,%file,%net -o "$OUT/03-strace.txt" "${targets[@]}" &
  local st=$!
  sleep 2

  echo
  echo "=== PART 2/3: Leave screen via D-Bus, strace attached (slower; that's expected) ==="
  mark dbus-trigger-traced
  busctl --user call org.kde.LogoutPrompt /LogoutPrompt org.kde.LogoutPrompt promptAll >>"$OUT/04-marks.txt" 2>&1
  echo "Press ENTER here the moment the Leave screen is fully drawn, then Esc on the Gemini."
  read -r _; mark dbus-shown-traced
  sleep 4

  echo
  echo "=== PART 3/3: Power button, strace attached ==="
  echo "Press ENTER here, then immediately press the POWER BUTTON on the Gemini (short press)."
  read -r _; mark power-press
  echo "Press ENTER here the moment the Leave screen is fully drawn, then Esc on the Gemini."
  read -r _; mark power-shown
  sleep 3

  sudo pkill -INT -P "$st" strace 2>/dev/null; sudo kill -INT "$st" 2>/dev/null
  kill "$dm" "$gw" 2>/dev/null; wait 2>/dev/null
  journalctl --user -b --since "-15 min" -o short-precise --no-pager >"$OUT/06-journal-user.txt" 2>&1
  sudo journalctl -b --since "-15 min" -o short-precise --no-pager >"$OUT/07-journal-system.txt" 2>&1
  ls -la ~/.cache/ | grep -i -e greeter -e qml >"$OUT/08-cache-after.txt" 2>&1
  sudo chown -R "$(id -u):$(id -g)" "$OUT"
}

mode_greeter() {
  need strace "sudo apt install strace"
  need perf "sudo apt install linux-perf"
  common
  local dd
  dd=$(pgrep -u "$(id -u)" -x dbus-daemon | while read -r p; do
         grep -qa -- '--session' "/proc/$p/cmdline" && echo "$p"; done | head -1)
  [ -n "$dd" ] || dd=$(pgrep -u "$(id -u)" -x dbus-daemon | head -1)
  echo "session dbus-daemon=$dd" | tee "$OUT/01-pids.txt"
  ps -o pid,args -p "$dd" >>"$OUT/01-pids.txt" 2>&1
  greeter_watch >"$OUT/05-greeter-pids.txt" 2>&1 &
  local gw=$!

  sudo strace -f -tt -y -s 200 -e trace=%process,%file,%memory,%net -o "$OUT/03-strace.txt" -p "$dd" &
  local st=$!
  sleep 2
  echo
  echo "=== PART 1/2: Leave screen, strace on its process ==="
  mark trigger-strace
  busctl --user call org.kde.LogoutPrompt /LogoutPrompt org.kde.LogoutPrompt promptAll >>"$OUT/04-marks.txt" 2>&1
  echo "Press ENTER here the moment the Leave screen is fully drawn, then Esc on the Gemini."
  read -r _; mark shown-strace
  sleep 3
  sudo pkill -INT -P "$st" strace 2>/dev/null; sudo kill -INT "$st" 2>/dev/null; wait "$st" 2>/dev/null
  sleep 2

  echo
  echo "=== PART 2/2: Leave screen, perf (no strace) ==="
  sudo perf record -a -g -F 499 -o "$OUT/perf.data" -- sleep 9 >"$OUT/06-perf-record.txt" 2>&1 &
  local pf=$!
  sleep 1
  mark trigger-perf
  busctl --user call org.kde.LogoutPrompt /LogoutPrompt org.kde.LogoutPrompt promptAll >>"$OUT/04-marks.txt" 2>&1
  echo "Press ENTER here the moment the Leave screen is fully drawn, then Esc on the Gemini."
  echo "(perf stops by itself ~8 s after the trigger)"
  read -r _; mark shown-perf
  wait "$pf" 2>/dev/null
  kill "$gw" 2>/dev/null; wait 2>/dev/null
  sudo chown "$(id -u):$(id -g)" "$OUT/perf.data"
  perf report -i "$OUT/perf.data" --no-children --sort comm,dso --stdio 2>/dev/null | head -120 >"$OUT/07-perf-comm-dso.txt"
  local gp; gp=$(grep -Eo 'pids: [0-9]+' "$OUT/05-greeter-pids.txt" | tail -1 | awk '{print $2}')
  echo "greeter pid (perf run): $gp" >>"$OUT/01-pids.txt"
  if [ -n "$gp" ]; then
    perf report -i "$OUT/perf.data" --no-children --pid "$gp" --sort dso --stdio -g none 2>/dev/null | head -80 >"$OUT/08-perf-greeter-dso.txt"
    perf report -i "$OUT/perf.data" --no-children --pid "$gp" --sort comm,dso,sym --stdio -g none 2>/dev/null | head -250 >"$OUT/08-perf-greeter-sym.txt"
    perf script -i "$OUT/perf.data" --pid "$gp" -F comm,tid,time 2>/dev/null | head -1 >"$OUT/08-perf-greeter-first-last.txt"
    perf script -i "$OUT/perf.data" --pid "$gp" -F comm,tid,time 2>/dev/null | tail -1 >>"$OUT/08-perf-greeter-first-last.txt"
  fi
  perf report -i "$OUT/perf.data" --no-children --sort comm --stdio 2>/dev/null | head -60 >"$OUT/09-perf-comm.txt"
  perf script -i "$OUT/perf.data" -F comm,cpu 2>/dev/null | awk '{c=$NF; $NF=""; print c, $0}' | sort | uniq -c | sort -rn | head -60 >"$OUT/10-perf-cpu-per-comm.txt"
  rm -f "$OUT/perf.data"   # big; reports above are enough for a first pass
  journalctl --user -b --since "-10 min" -o short-precise --no-pager >"$OUT/11-journal-user.txt" 2>&1
  sudo chown -R "$(id -u):$(id -g)" "$OUT"
}

"mode_$MODE"
tar -C /tmp -czf "$OUT.tar.gz" "$(basename "$OUT")"
echo
echo "Done: $OUT.tar.gz"
echo "On Hydra:  scp atzero@192.168.0.139:$OUT.tar.gz ~/Build/gemini-nixos/logs/"
