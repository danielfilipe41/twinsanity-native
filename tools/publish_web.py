#!/usr/bin/env python3
"""The page as a site: the files GitHub Pages serves.

The web build writes build/web/game.html, game.js and game.wasm, and copies the page's own font (Montserrat, SIL OFL) and
its licence beside them; a site is those, with game.html as index.html. The disc is never one of them: the page reads the player's own image (the picker, or a drag and drop) or the one the site
hosts, and the repository carries neither the image nor SLES_525.68. The files are listed by name rather than copied with
a glob because a disc image is often sitting beside the build (build/web/pal.iso, a symlink to the real one), and a glob
would publish it.

    python3 tools/publish_web.py            # write build/site
    python3 tools/publish_web.py --push     # ...and put it on the gh-pages branch

The branch is made of the built page alone (an orphan history of its own), so nothing of the source tree is on the site.
GitHub Pages serves a branch's root; see docs/BUILD_WEB.md for turning it on.
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
BUILD = HERE / "build" / "web"
SITE = HERE / "build" / "site"

# What is on the site, and where each file comes from: nothing else is ever copied
FILES = {"index.html": "game.html", "game.js": "game.js", "game.wasm": "game.wasm",
         "montserrat.woff2": "montserrat.woff2", "OFL.txt": "OFL.txt"}
BRANCH = "gh-pages"
# GitHub Pages refuses a file over 100 MB and a site over 1 GB; this build is ~4.7 MB, so anything near that is not it
LIMIT = 100 * 1024 * 1024


def git(*arguments, data=None):
    result = subprocess.run(["git", *arguments], cwd=HERE, input=data, capture_output=True)
    if result.returncode != 0:
        raise SystemExit(f"git {' '.join(arguments)}: {result.stderr.decode().strip()}")
    return result.stdout


def assemble():
    SITE.mkdir(parents=True, exist_ok=True)
    for stale in SITE.iterdir():
        if stale.is_file():
            stale.unlink()
    written = {}
    for name, source in FILES.items():
        path = BUILD / source
        if not path.is_file():
            raise SystemExit(f"{path} is not there: build the page first "
                             "(DATA=runtime PLATFORM=web python3 configure.py && ninja game)")
        data = path.read_bytes()
        if len(data) > LIMIT:
            raise SystemExit(f"{name} is {len(data)} bytes, past what Pages takes")
        if name.endswith(".wasm") and not data.startswith(b"\0asm"):
            raise SystemExit(f"{path} is not a wasm module")
        (SITE / name).write_bytes(data)
        written[name] = len(data)
    # An empty .nojekyll: the branch is served as it is, not run through Jekyll first
    (SITE / ".nojekyll").write_bytes(b"")
    written[".nojekyll"] = 0

    # A last look at what is about to go out: the disc's own names would be a mistake this script exists to prevent
    for path in SITE.iterdir():
        if path.suffix.lower() in (".iso", ".bin", ".img") or path.name.upper().startswith(("SLES", "SLUS", "SCES")):
            raise SystemExit(f"{path} looks like the disc: it does not go on the site")
    return written


def push():
    # A checkout in CI has the sources' branch only: the page's branch is brought first, or the commit made below would
    # have no parent and the push would be turned down for starting a history the remote already has
    try:
        git("fetch", "--depth", "1", "origin", f"+refs/heads/{BRANCH}:refs/heads/{BRANCH}")
    except SystemExit:
        pass

    tree = []
    for path in sorted(SITE.iterdir()):
        blob = git("hash-object", "-w", "--stdin", data=path.read_bytes()).decode().strip()
        tree.append(f"100644 blob {blob}\t{path.name}")
    tree_id = git("mktree", data=("\n".join(tree) + "\n").encode()).decode().strip()

    source = git("rev-parse", "--short", "HEAD").decode().strip()
    message = f"The page, built from {source}\n"
    try:
        parent = git("rev-parse", "--verify", f"refs/heads/{BRANCH}").decode().strip()
    except subprocess.CalledProcessError:
        parent = None
    commit = git("commit-tree", tree_id, *(("-p", parent) if parent else ()), data=message.encode()).decode().strip()

    git("update-ref", f"refs/heads/{BRANCH}", commit)
    git("push", "origin", f"refs/heads/{BRANCH}:refs/heads/{BRANCH}")
    return commit


def site_url():
    try:
        remote = git("remote", "get-url", "origin").decode().strip()
    except subprocess.CalledProcessError:
        return None
    name = remote.removesuffix(".git").rsplit("/", 2)
    if len(name) < 2 or "." in name[-2] or "github.com" not in remote:
        return None
    return f"https://{name[-2]}.github.io/{name[-1]}/"


def main():
    written = assemble()
    print(f"site: {SITE}")
    for name, size in written.items():
        print(f"  {name:14} {size:>9} bytes")
    print(f"  {sum(written.values()):14} {sum(written.values()):>9} bytes in all")
    if "--push" not in sys.argv:
        print("\nnothing published: --push puts these files on the "
              f"{BRANCH} branch, which is what Pages serves")
        return
    commit = push()
    print(f"\npushed {commit[:12]} to {BRANCH}")
    url = site_url()
    if url:
        print(f"the site is {url} once Pages is set to serve the {BRANCH} branch "
              "(Settings, Pages, Deploy from a branch, "
              f"{BRANCH} / (root))")


if __name__ == "__main__":
    main()
