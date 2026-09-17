# keys/ — the Gemini PDA's SSH identity, committed on purpose

| File | What it is |
|---|---|
| `gemini_ed25519` | ed25519 **private** key (mode 0600) |
| `gemini_ed25519.pub` | its public half |
| `known_hosts` | the device's pinned sshd host key, for verification |

Fingerprint: `SHA256:5P4/WXMpdUQ0G0K5p69d1pCiYwoOwp7EWJykjxBnFKI`

One keypair plays **both** roles (2026-09-17):

1. **the admin login key** — `root` and `cjdell` on the device accept it
   (`config/gemini.nix` → `users.users.*.openssh.authorizedKeys.keyFiles`),
   and every host-side script defaults to it (`bin/lib/host.sh`), so a
   fresh clone of this repo can ssh/flash with no provisioning step;
2. **the device's sshd host key** — an activation script installs it as
   `/etc/ssh/ssh_host_ed25519_key` (0600, root), and `services.openssh.hostKeys`
   is pinned to that path. Because the host key is *fixed* rather than
   generated per install, reinstalling from scratch does not change the
   device's identity: no `REMOTE HOST IDENTIFICATION HAS CHANGED` and no
   stale `known_hosts` entries after a reflash.

## Why committing a private key is deliberate here

**The Gemini PDA is a single-user lab device and security is explicitly
not a concern for it in its current state** (AGENTS.md). The trade made
here is *everything in the repo, nothing manual*: the alternative —
keeping the key in `~/.ssh` on the workstation — meant that a from-scratch
reflash needed a manual re-provisioning step (push the pubkey over a
password login), and that the device's host key changed on every install.

Be aware of the consequences, which are accepted:

- the private key is **public in git**, and it is also copied into the
  **world-readable nix store** on the build host and on the device (the
  activation script installs *from* a store path — that is the only way to
  get a 0600 copy; `environment.etc` would leave a symlink to a 0444 file
  and sshd refuses world-readable host keys);
- anyone with the repo (or read access to the device's `/nix/store`) can
  log in as root over the USB NIC. There is no network exposure beyond the
  device's own Wi-Fi/LAN, which is the only reason this is tolerable.

**If that ever stops being acceptable** (multiple users, a shared network,
a real data-protection need): generate a new keypair outside the repo,
stop committing it, move the secret to an out-of-band store, and rotate
the device's `authorizedKeys` + host key in the same change. Do not leave
this file in place while believing it is private.

## Permissions

Git stores only the executable bit, so a **fresh clone arrives with the
private key at 0644** and `ssh` refuses it. `bin/lib/host.sh`'s
`gemini_ssh_key()` tightens it to 0600 on first use (and says so); by hand:
`chmod 600 keys/gemini_ed25519`.
