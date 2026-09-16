#!/usr/bin/env python3
"""proxy.py — the tiny CONNECT/HTTP proxy that lets the build VM
reach the internet on a Mac whose own egress the container runtime cannot
reuse.

WHY THIS EXISTS (2026-09-17, receipts in docs/macos-build.md):
on this Mac the default route belongs to a **Tailscale exit node**
(`default … utun4`, `tailscale status` = "grafton-router … active; exit
node"), and Apple's `container` vmnet NAT does not survive that: the VM
reaches its gateway (192.168.64.1) and the Mac itself, but every NAT'd
connection to the internet times out — while the Mac's own curl is fine.
Measured: VM→github:443 timeout, VM→192.168.64.1:22 OK, Mac→github 200.

So the VM borrows the *Mac's* working egress through this proxy instead of
trying to have its own. Nix is proxy-aware end to end:
  - its own downloader (substituters: cache.nixos.org narinfo/nar) reads
    the standard `http_proxy`/`https_proxy`/`all_proxy` environment;
  - nixpkgs' fetchurl declares `impureEnvVars = proxyImpureEnvVars`, so
    the same variables reach fixed-output builders (flake inputs, the
    kernel tarball) even under nix's sandbox.

It is a stdlib-only, single-file, threaded proxy: no packages, nothing
installed, and it listens ONLY on the container bridge gateway
(192.168.64.1 by default) and only accepts clients from the container
subnet — it is not an open proxy on the LAN.

Usage (on the Mac):
  python3 bin/macos/proxy.py                     # defaults: 192.168.64.1:3128
  python3 bin/macos/proxy.py --bind 0.0.0.0 --port 3128 --allow any
  python3 bin/macos/proxy.py --quiet             # no per-connection log

`python3` comes from the flake's darwin devshell when the host has none
(flake-macos.nix: `nix develop`).

Normally you do not run this by hand: `bash bin/build.sh start`
detects the VM's broken egress and starts/stops it automatically (see the
`net` verb there).
"""

import argparse
import ipaddress
import socket
import sys
import threading

BUF = 65536
HEAD_LIMIT = 65536
CONNECT_TIMEOUT = 30


def log(verbose, *args):
    if verbose:
        print("proxy:", *args, file=sys.stderr, flush=True)


def splice(a, b):
    """Copy a -> b until EOF, then half-close both."""
    try:
        while True:
            data = a.recv(BUF)
            if not data:
                break
            b.sendall(data)
    except OSError:
        pass
    finally:
        for s in (a, b):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def read_head(conn):
    """Read request bytes up to and including the header terminator."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            return None
        buf += chunk
        if len(buf) > HEAD_LIMIT:
            return None
    return buf


def target_of(method, raw):
    """(host, port, absolute) for a request line target."""
    if method == b"CONNECT":
        host, _, port = raw.decode("latin-1").partition(":")
        return host, int(port or 443)
    # Plain HTTP proxying: the target is an absolute URL.
    rest = raw.decode("latin-1")
    if not rest.startswith("http://"):
        raise ValueError("only absolute-form HTTP targets are supported")
    authority = rest[len("http://"):].split("/", 1)[0]
    host, _, port = authority.partition(":")
    return host, int(port or 80)


def serve(conn, peer, allow, verbose):
    try:
        conn.settimeout(CONNECT_TIMEOUT)
        head = read_head(conn)
        if head is None:
            return
        header, _, body = head.partition(b"\r\n\r\n")
        line = header.split(b"\r\n", 1)[0]
        parts = line.split(b" ", 2)
        if len(parts) != 3:
            conn.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
            return
        method, raw_target = parts[0], parts[1]

        allow_target = True
        if allow is not None:
            try:
                allow_target = ipaddress.ip_address(peer[0]) in allow
            except ValueError:
                allow_target = False
        if not allow_target:
            log(verbose, "refused client", peer[0])
            conn.sendall(b"HTTP/1.1 403 Forbidden\r\n\r\n")
            return

        try:
            host, port = target_of(method, raw_target)
        except ValueError as exc:
            log(verbose, "bad target", raw_target, exc)
            conn.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
            return

        upstream = socket.create_connection((host, port), CONNECT_TIMEOUT)
        log(verbose, "%s %s:%d" % (method.decode("latin-1"), host, port))

        if method == b"CONNECT":
            conn.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
        else:
            upstream.sendall(header + b"\r\n\r\n" + body)

        t = threading.Thread(target=splice, args=(conn, upstream), daemon=True)
        t.start()
        splice(upstream, conn)
    except OSError as exc:
        log(verbose, "error:", exc)
        try:
            conn.sendall(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")
        except OSError:
            pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bind", default="192.168.64.1",
                    help="listen address (default: the container bridge "
                         "gateway; 0.0.0.0 to listen everywhere)")
    ap.add_argument("--port", type=int, default=3128)
    ap.add_argument("--allow", default="192.168.64.0/24",
                    help="client CIDR allowed to use the proxy; "
                         "empty/'any' for no filter")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    verbose = not args.quiet
    allow = None
    if args.allow and args.allow != "any":
        allow = ipaddress.ip_network(args.allow, strict=False)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(128)
    print("proxy: listening on %s:%d (clients: %s)"
          % (args.bind, args.port, args.allow or "any"), flush=True)

    while True:
        conn, peer = srv.accept()
        threading.Thread(target=serve,
                         args=(conn, peer, allow, verbose),
                         daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
