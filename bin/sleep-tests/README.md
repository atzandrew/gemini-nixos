# sleep-tests — Gemini-side SPM / power experiment scripts

Source of truth for the scripts run on the Gemini during the SPM sleep, connsys and power work
(2026-10-13 → 10-16). Results and the meaning of each experiment (E2–E29) are in the
results in `docs/spm-sleep.md` and `docs/connsys-sleep.md`.

## Deploy to the Gemini
The scripts hard-code `/home/andrew/connsys/` (and `/home/andrew/panel/dcs.sh`), and the
SPM test module at `/home/andrew/mt6797-spm-load.ko` (built from
`devices/planet-geminipda/kernel/modules/mt6797-spm-load`):

    scp bin/sleep-tests/*.sh andrew@<gemini>:connsys/
    ssh andrew@<gemini> 'mkdir -p panel && mv connsys/dcs.sh panel/'
    # dev-only sudo rule the remote runs rely on (NOT part of the Debian image):
    scp bin/sleep-tests/sudoers-spm-3e andrew@<gemini>:/tmp/ && \
      ssh -t andrew@<gemini> 'sudo install -m 440 /tmp/sudoers-spm-3e /etc/sudoers.d/spm-3e'

Run as root in the background, e.g. `sudo -n -b busybox sh /home/andrew/connsys/e29.sh`.
Logs land next to the scripts (`*.log`); they are results, not tracked here.

## Main pieces
- `e16-run.sh` / `e20-run.sh` / `e28-run.sh`: one protected SPM run (DPMS off, CONN off, CPUs 1-9
  offline, DCS 0x28 panel display-off, insmod/rmmod spm-load, DCS 0x29, restore; only CPUs 1-7
  come back). `e20-run` adds the gauge; `e28-run` adds the gemini-sleep-gpio hook (HOOK=1).
- `dcs.sh`: DSI DCS short command via VM_CMD (never send 0x10 — sleep-in leaves the panel black).
- `gpu.sh off|on`: the six MFG domains by hand (stock sequence) — superseded by the scpsys "mfg"
  genpd (d2f4783), kept for reference.
- `pmw.sh ADDR VALUE`: PMIC write through the SPM DVFS pwrap port; `vcore.sh`: Vcore reg readback.
- `e26.sh` / `e27.sh`: awake GPIO load tests (speaker amps ~60 mA, USB1 VBUS trio ~11 mA).
- `soak.sh` + `gemini-soak.service`: unattended overnight sleep series (hangs per hour). Survives
  the PCM-WDT reboots: while armed, greetd autologins to Plasma and the service carries on. Install
  in /home/andrew/soak/ + /etc/systemd/system/; `soak.sh arm HOURS [SLEEP_S] [LABEL]`, `disarm`.
