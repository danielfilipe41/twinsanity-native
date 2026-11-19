#!/usr/bin/env python3
"""The runtime data check, end to end: build both ways, load the disc's data, compare it word for word.

What it asserts, in order:
  1. The split's own data objects hold the disc's bytes (tools/retail_parity.py --retail).
  2. A DATA=runtime build loads SLES_525.68 from the disc and fills its image (--verify-data).
  3. Every word of that image matches what the baked build's objects hold, and every pointer the split made resolves to
     the same symbol plus the same offset (tools/retail_parity.py).

    python3 tools/check_runtime_data.py            # disc/
    python3 tools/check_runtime_data.py "my.iso"   # a disc image

Exit status 0 when everything matches. It leaves the tree configured for the ordinary (data-bearing) build.
"""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
DISC = sys.argv[1] if len(sys.argv) > 1 else "disc"
BINARY = HERE / "build" / "host" / "sdl3" / "game-with-data"
DUMP = HERE / "build" / "host" / "retail-image.txt"


def run(command, data_runtime=False):
    environment = dict(os.environ)
    environment.setdefault("PLATFORM", "sdl3")
    if data_runtime:
        environment["DATA"] = "runtime"
    else:
        environment.pop("DATA", None)
    shown = " ".join(f"{key}={value}" for key, value in sorted(environment.items())
                     if key in ("PLATFORM", "TARGET", "DATA"))
    print(f"$ {shown} {' '.join(str(part) for part in command)}")
    return subprocess.run([str(part) for part in command], cwd=HERE, env=environment).returncode


def main():
    python = sys.executable
    # 1. The baked build's objects, which are the split's own data, and the disc's bytes behind them
    if run([python, "configure.py"]) != 0 or run(["ninja", "game"]) != 0:
        print("check: the data-bearing build does not build", file=sys.stderr)
        return 1
    if run([python, "tools/retail_parity.py", "--retail"]) != 0:
        print("check: the split's objects do not hold the disc's bytes", file=sys.stderr)
        return 1

    # 2. The build without the data, loading the disc's own
    if run([python, "configure.py"], data_runtime=True) != 0 or run(["ninja", "game"]) != 0:
        print("check: the DATA=runtime build does not build", file=sys.stderr)
        run([python, "configure.py"])
        return 1
    status = 0
    if run([BINARY, "--disc", DISC, "--verify-data"]) != 0:
        print("check: the disc's data does not load", file=sys.stderr)
        status = 1
    # 3. And that image against the objects
    elif run([BINARY, "--disc", DISC, "--dump-data", DUMP]) != 0:
        print("check: the image does not dump", file=sys.stderr)
        status = 1
    elif run([python, "tools/retail_parity.py", BINARY, DUMP]) != 0:
        print("check: the image differs from the baked data", file=sys.stderr)
        status = 1

    # The tree goes back to the ordinary build
    if run([python, "configure.py"]) != 0:
        print("check: the data-bearing configuration was not restored", file=sys.stderr)
        status = 1
    print("check: the runtime data matches the disc and the baked build" if status == 0 else "check: FAILED")
    return status


if __name__ == "__main__":
    sys.exit(main())
