#!/usr/bin/env python3
# PS5 DevLoop - the client of the resident input and screen-capture payload.
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later
"""Press buttons on the console and see what its screen shows.

Commands:
  load [ELF]            send the payload to the console's ELF loader (once a boot)
  ping                  is the payload there
  raw WORDS...          send one command line as it is, print the answer
  open [USERID]         create the virtual pad, give it to the foreground user, close
                        the prompt the console raises, and disconnect any real
                        controller so that titles read the pad
  close                 remove it
  status                the pad's handle, device, user and last insert result
  press BUTTONS [--ms]  press and release: "cross", "l1+r1", "down"
  hold BUTTONS [--lx ..] hold buttons, sticks (0-255, 128 centre) and triggers
  release               let everything go
  seq STEPS...          steps in order: a button press ("down", "cross:300" for
                        300 ms), or "wait:500"
  shot [FILE]           the whole screen, system UI included, through the console's
                        own screenshot: about 3 s. For the home screen and dialogs
  grab [FILE]           ask the running title for its frame (--title, from
                        titles.json): about 1 s. Use it whenever a title runs
  latest [FILE]         fetch the newest capture without pressing anything
  shots                 list the console's newest captures

The console's address comes from PS5_HOST in the environment, then from .env in
this project (.env.example). It is never printed.
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PORT = int(os.environ.get("PS5LOOP_PORT", "9112"))
KLOG_PORT = int(os.environ.get("KLOG_PORT", "3232"))
LOADER_PORT = int(os.environ.get("PS5_LOADER_PORT", "9021"))
# What a picture is scaled down to, so that it can be looked at cheaply.
VIEW_WIDTH = 1280

# Titles that answer a request file with a picture of their frame (AGENTS.md,
# "Giving a title grab"): the file the title looks for, and the folder it writes
# the picture into.
TITLES = json.loads((ROOT / "titles.json").read_text(encoding="utf-8"))
NO_HOOK = ("the title did not answer: is it running, and does it have the grab hook? "
           "(AGENTS.md, \"Giving a title grab\"); shot takes the whole screen meanwhile")

BUTTONS = {
    "create": 0x1, "l3": 0x2, "r3": 0x4, "options": 0x8,
    "up": 0x10, "right": 0x20, "down": 0x40, "left": 0x80,
    "l2": 0x100, "r2": 0x200, "l1": 0x400, "r1": 0x800,
    "triangle": 0x1000, "circle": 0x2000, "cross": 0x4000, "square": 0x8000,
    "ps": 0x10000, "touchpad": 0x100000,
}


def env_file_host(text):
    for raw in text.splitlines():
        key, _, value = raw.strip().partition("=")
        if key.strip() == "PS5_HOST" and value.strip():
            return value.strip()
    return None


def console_host():
    host = os.environ.get("PS5_HOST")
    if host:
        return host
    local = ROOT / ".env"
    if local.is_file():
        host = env_file_host(local.read_text(encoding="utf-8"))
        if host:
            return host
    raise SystemExit("PS5_HOST is not set: export it, or copy .env.example to .env")


def command(line, timeout=30.0, payload=False):
    """Send one command. Returns the answer line, and the bytes after it for a file."""
    try:
        connection = socket.create_connection((console_host(), PORT), timeout=10)
    except OSError as error:
        raise SystemExit(f"ps5loop: the payload did not answer ({error.strerror or error}); "
                         "run: ps5loop.py load") from None
    with connection:
        connection.settimeout(timeout)
        connection.sendall(line.encode() + b"\n")
        data = b""
        while b"\n" not in data:
            chunk = connection.recv(65536)
            if not chunk:
                break
            data += chunk
        answer, _, rest = data.partition(b"\n")
        answer = answer.decode("utf-8", "replace")
        if not payload or not answer.startswith("ok file"):
            return answer, b""
        size = int(dict(word.split("=", 1) for word in answer.split() if "=" in word)["size"])
        while len(rest) < size:
            chunk = connection.recv(1 << 20)
            if not chunk:
                raise SystemExit(f"ps5loop: the file ended at {len(rest)} of {size} bytes")
            rest += chunk
        return answer, rest[:size]


def klog_listen():
    """A connection to the console's klog, or None. Opened before the pad, so
    that what the console logs as the pad arrives is not missed."""
    try:
        return socket.create_connection((console_host(), KLOG_PORT), timeout=5)
    except OSError:
        return None


def other_pads(connection, device, seconds=2.5):
    """The other controllers the console lists, from the table its klog prints
    when a controller changes owner: "[CIM] [0] 0x<device> 0x<user> ..."."""
    found = set()
    if connection is None:
        return None
    with connection:
        connection.settimeout(0.5)
        data = b""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                chunk = connection.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                break
            data += chunk
    # The last table is the current one.
    table = data.decode("utf-8", "replace").rsplit("[CIM]  Pad", 1)
    if len(table) < 2:
        return None
    for line in table[1].splitlines():
        words = line.split()
        if len(words) >= 4 and words[0].endswith("[CIM]") and words[2].startswith("0x"):
            pad = int(words[2], 16)
            if pad not in (device, 0xffffffffffffffff):
                found.add(pad)
    return found


def open_pad(args):
    """Create the pad, close the console's prompt, and make the pad its user's first."""
    klog = None if args.beside else klog_listen()
    answer, _ = command(f"pad open {args.user}".strip())
    if not answer.startswith("ok") or "already open" in answer:
        return say(answer)
    print(answer)
    device = int(answer.split("device=")[1].split()[0], 16)
    if not args.keep_prompt:
        # The console asks who the new controller belongs to although it is bound
        # already; Circle, from the pad itself, closes the question.
        time.sleep(1.5)
        command(f"pad tap {BUTTONS['circle']:x} 80")
    if args.beside:
        return 0
    # A title reads its user's first controller, so a real one that is connected
    # has to go: PS on it brings it back.
    pads = other_pads(klog, device)
    if pads is None:
        print("klog did not list the controllers: if a real one is connected, titles "
              "read it and not this pad (pad evict DEVICE)")
        return 0
    for pad in sorted(pads):
        print(command(f"pad evict {pad:x}")[0])
    if not pads:
        print("ok no other controller is connected")
    return 0


def say(answer):
    print(answer)
    return 0 if answer.startswith("ok") else 1


def mask(names):
    value = 0
    for name in names.lower().replace(",", "+").split("+"):
        if name not in BUTTONS:
            raise SystemExit(f"unknown button {name!r}: {', '.join(BUTTONS)}")
        value |= BUTTONS[name]
    return value


def load(args):
    elf = Path(args.elf) if args.elf else ROOT / "build/ps5loop.elf"
    if not elf.is_file():
        raise SystemExit(f"missing payload: {elf} (run tools/build.sh)")
    with socket.create_connection((console_host(), LOADER_PORT), timeout=10) as connection:
        connection.sendall(elf.read_bytes())
    print(f"sent {elf.name} ({elf.stat().st_size} bytes) to the loader")
    if elf.name != "ps5loop.elf":
        return 0
    for _ in range(20):
        time.sleep(0.5)
        try:
            return say(command("ping", timeout=5)[0])
        except SystemExit:
            continue
    raise SystemExit("ps5loop: loaded, but the payload does not answer; read klog")


def to_png(data, target):
    """Write a capture as a PNG to look at, whatever the console saved it as."""
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    if data[:3] == b"II\xbc":
        # JPEG XR, what the console writes with HDR on.
        raw = target.with_suffix(".jxr")
        raw.write_bytes(data)
        tiff = target.with_suffix(".tif")
        # Asked for 24-bit RGB first: left to itself the decoder writes the home
        # screen's 10-bit pictures as a packed TIFF that nothing reads back
        # correctly. A title's 8-bit pictures refuse that conversion and decode
        # as they are.
        for conversion in (["-c", "9"], []):
            tiff.unlink(missing_ok=True)
            decoded = subprocess.run(["JxrDecApp", "-i", str(raw), "-o", str(tiff), *conversion],
                                     capture_output=True, text=True, check=False)
            if decoded.returncode == 0 and tiff.is_file():
                break
        if decoded.returncode != 0 or not tiff.is_file():
            print(f"kept {raw}: JxrDecApp could not decode it ({decoded.stdout.strip()[-200:]})")
            return raw
        source = tiff
    else:
        source = target.with_suffix(".png" if data[:4] == b"\x89PNG" else ".jpg")
        if source == target:
            source = target.with_suffix(".full.png")
        source.write_bytes(data)
    # Only the console's HDR pictures need their levels stretched to be readable.
    levels = ["-auto-level"] if source.suffix == ".tif" else []
    scaled = subprocess.run(["magick", str(source), *levels, "-resize", f"{VIEW_WIDTH}x>",
                             "-depth", "8", str(target)],
                            capture_output=True, text=True, check=False)
    if scaled.returncode != 0:
        print(f"kept {source}: magick could not convert it ({scaled.stderr.strip()[-200:]})")
        return source
    if source.suffix == ".tif":
        source.unlink()
    return target


def fetch(line, args):
    answer, data = command(line, timeout=60, payload=True)
    if not answer.startswith("ok file"):
        return say(answer)
    name = args.file or ROOT / "captures" / time.strftime("shot-%Y%m%d-%H%M%S.png")
    print(f"{answer.split(' path=')[0]} -> {to_png(data, name)}")
    return 0


def sequence(args):
    for step in args.steps:
        name, _, value = step.partition(":")
        if name == "wait":
            time.sleep(int(value or 500) / 1000)
            continue
        answer, _ = command(f"pad tap {mask(name):x} {int(value or args.ms)}")
        if not answer.startswith("ok"):
            return say(answer)
        time.sleep(args.gap / 1000)
    print(f"ok {len(args.steps)} steps")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("load").add_argument("elf", nargs="?")
    commands.add_parser("ping")
    commands.add_parser("raw").add_argument("words", nargs="+")
    opening = commands.add_parser("open")
    opening.add_argument("user", nargs="?", default="")
    opening.add_argument("--keep-prompt", action="store_true",
                         help="leave the console's \"Who's using this controller?\" prompt up")
    opening.add_argument("--beside", action="store_true",
                         help="leave a real controller connected (titles then read it, not the pad)")
    commands.add_parser("close")
    commands.add_parser("status")
    press = commands.add_parser("press")
    press.add_argument("buttons")
    press.add_argument("--ms", type=int, default=80)
    hold = commands.add_parser("hold")
    hold.add_argument("buttons", nargs="?", default="")
    for axis, default in (("lx", 128), ("ly", 128), ("rx", 128), ("ry", 128), ("l2", 0), ("r2", 0)):
        hold.add_argument(f"--{axis}", type=int, default=default)
    commands.add_parser("release")
    steps = commands.add_parser("seq")
    steps.add_argument("steps", nargs="+")
    steps.add_argument("--ms", type=int, default=80, help="how long each press is held")
    steps.add_argument("--gap", type=int, default=250, help="the pause after each press")
    shot = commands.add_parser("shot")
    shot.add_argument("file", nargs="?")
    shot.add_argument("--hold", type=int, default=0, help="how long Create is held, in ms")
    grabbing = commands.add_parser("grab")
    grabbing.add_argument("file", nargs="?")
    grabbing.add_argument("--title", default=next(iter(TITLES)), choices=sorted(TITLES),
                          help="a title from titles.json")
    commands.add_parser("latest").add_argument("file", nargs="?")
    commands.add_parser("shots")
    args = parser.parse_args()

    if args.command == "load":
        return load(args)
    if args.command == "open":
        return open_pad(args)
    if args.command == "seq":
        return sequence(args)
    if args.command == "shot":
        return fetch(f"shot {args.hold}" if args.hold else "shot", args)
    if args.command == "grab":
        title = TITLES[args.title]
        status = fetch(f"grab {title['request']} {title['folder']}", args)
        if status != 0:
            print(NO_HOOK)
        return status
    if args.command == "latest":
        return fetch("latest", args)
    if args.command == "press":
        return say(command(f"pad tap {mask(args.buttons):x} {args.ms}")[0])
    if args.command == "hold":
        buttons = mask(args.buttons) if args.buttons else 0
        return say(command(f"pad set {buttons:x} {args.lx} {args.ly} {args.rx} {args.ry} "
                           f"{args.l2} {args.r2}")[0])
    if args.command == "release":
        return say(command("pad set 0 128 128 128 128 0 0")[0])
    line = {"ping": "ping", "shots": "shots", "status": "pad status", "close": "pad close",
            "raw": " ".join(getattr(args, "words", []))}[args.command]
    return say(command(line)[0])


if __name__ == "__main__":
    sys.exit(main())
