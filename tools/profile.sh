#!/usr/bin/env bash
# Profiling the port from outside the game: perf(1) records where the game's time goes while you play, and this keeps the
# command line and the output file names the same from run to run.
#
# The port has its own frame counter too (TWINSANITY_FRAME_LOG and TWINSANITY_FPS, see the README): that one says whether the
# picture is on time and
# what a frame costs, which is what a report about "is it running correctly" needs. This one says *where* the time went,
# down to the function, which is what a report about "why is it slow" needs.
#
#   tools/profile.sh                 record the game and print the hottest functions
#   tools/profile.sh --stat          50 seconds of counts (no call stacks): missed vblanks, cache misses, context switches
#   tools/profile.sh --record         record only, then print how to read it back
#   tools/profile.sh --report FILE   read an earlier recording
#   tools/profile.sh --flame          record and write build/profile.folded (flamegraph.pl can turn it into a picture)
#
# The recording goes to build/profile.data; PROFILE_DATA=some/file.data puts it there instead (a relative path is under the
# project root), so a run can be kept by name: PROFILE_DATA=disc/gameplay.data tools/profile.sh --record
#
# Anything else is passed to the game, so `tools/profile.sh --disc /path/to/disc` profiles a run with that disc.
#
# perf needs a kernel setting to read the hardware counters of any process and, on Arch-based systems, is a package of its
# own: `sudo pacman -S perf` (Debian/Ubuntu: `sudo apt install linux-perf`; Fedora: `sudo dnf install perf`).
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
game="${GAME:-$root/build/host/sdl3/game-with-data}"
# Where the recording goes: build/profile.data unless PROFILE_DATA names another file (a relative one is under the project)
data="${PROFILE_DATA:-build/profile.data}"
case "$data" in
    /*) ;;
    *) data="$root/$data" ;;
esac
mkdir -p "$(dirname "$data")" 2>/dev/null
folded="$root/build/profile.folded"

mode=report
case "${1:-}" in
    --stat) mode=stat; shift ;;
    --record) mode=record; shift ;;
    --report) mode=report; shift ;;
    --flame) mode=flame; shift ;;
    --help|-h)
        sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
esac

if [ "$mode" = report ] && [ $# -gt 0 ] && [ -f "$1" ]; then
    data="$1"
    shift
fi

if ! command -v perf >/dev/null 2>&1; then
    cat >&2 <<'EOF'
profile: perf is not installed, and it is the external profiler this uses.

  Arch / CachyOS:  sudo pacman -S perf
  Debian / Ubuntu: sudo apt install linux-perf
  Fedora:          sudo dnf install perf

The port's own frame counter needs none of this: TWINSANITY_FRAME_LOG=1 prints the same line to stderr once a second, and
TWINSANITY_FPS=1 puts it in the corner of the picture. Both are off by default.
EOF
    exit 1
fi

# Whether this system lets perf see anything at all: a container, a sandbox or a paranoid setting answers no, and the game
# should not be started for a recording that could never be written. Counting is enough to tell (recording asks for more)
if ! perf stat -e task-clock true >/dev/null 2>&1; then
    cat >&2 <<'EOF'
profile: perf cannot open any events here, so nothing would be recorded.

  Counting and recording need: sudo sysctl kernel.perf_event_paranoid=1
  (2 allows counted runs only, 3 hides events entirely.) Inside a container or sandbox perf_event is usually disabled
  outright: run the game from a normal terminal there.

The port's own frame counter needs none of this: it is in the corner of the picture and once a second in the log.
EOF
    exit 1
fi

# A recording of a game that has not been built is an empty report: say so rather than recording nothing
if [ "$mode" != report ]; then
    if [ ! -x "$game" ]; then
        echo "profile: no game at $game (build it with: ninja game)" >&2
        exit 1
    fi
    # Frame pointers come first: the host build is compiled with -fno-omit-frame-pointer on purpose (configure.py), and the
    # 32-bit x86 build's DWARF unwinding is what silently produced recordings with no call graph at all
    sample_rate="${PERF_RATE:-999}"
    if [ "$mode" = stat ]; then
        # Counts over a time, not stacks: what the frame budget actually goes on (cycles, instructions, cache and branch
        # misses, the scheduler waking the render thread), with the game's own run in the middle
        perf stat -d -e cycles,instructions,cache-misses,branch-misses,context-switches,task-clock -- "$game" "$@"
        exit $?
    fi

    echo "profile: recording $game (play until you quit it; Ctrl-C ends the recording early)" >&2
    # A call graph asks the kernel for more than sampling does, and a restricted setup refuses the unwinders while still allowing
    # the samples themselves -- which name the functions doing the work, just not who called them. DWARF comes first: the port's
    # own code keeps its frame pointers (configure.py) but bgfx's libraries are built once by bgfx's own flow and do not, so a
    # frame-pointer walk breaks the moment a sample lands inside bgfx -- it produces stacks like `---0xf7f17fcb` instead of
    # failing. DWARF reads the unwind tables every library has. Then frame pointers, then a plain recording; a recording with no
    # call graph at all is reported as such rather than read as if it had one
    recorded=0
    for graph in ${PERF_GRAPH:-dwarf fp none}; do
        rm -f "$data"
        if [ "$graph" = none ]; then
            perf record -e cycles:u -F "$sample_rate" -o "$data" -- "$game" "$@"
        else
            perf record -e cycles:u -F "$sample_rate" -g --call-graph "$graph" -o "$data" -- "$game" "$@"
        fi
        status=$?
        if [ -s "$data" ]; then
            recorded=1
            break
        fi
        echo "profile: no recording from the $graph unwinder; trying the next" >&2
    done

    # An empty file is not a recording: perf opened no events (a recording needs more than counting, and a paranoid setting of
    # 2 or a sandbox refuses it), and a report of nothing would read as if the game had done no work. Checked before the game's
    # own status, which perf's failure sets too
    if [ ! -s "$data" ]; then
        cat >&2 <<'EOF'
profile: perf wrote no recording -- it could not open any events for it.

  sudo sysctl kernel.perf_event_paranoid=1    (0 in some distributions)
  In a container or sandbox, perf_event is usually disabled outright: run the game from a normal terminal.

The port's own frame counter still works: TWINSANITY_FRAME_LOG=1 prints the line to stderr, TWINSANITY_FPS=1 adds the corner text.
EOF
        exit 1
    fi

    if [ $status -ne 0 ]; then
        echo "profile: the game exited with $status (a recording is still kept in $data)" >&2
    fi

    # The binary the recording was made with, beside it: perf stores addresses, and every name a report shows is the *current*
    # binary's for that address. Rebuilding between the recording and the report renames every frame -- it does not fail, it
    # invents a plausible picture (a 15% function read as a 26% one), which is worse than no report. The checksum is what says
    # the two are the same program
    if command -v md5sum >/dev/null 2>&1; then
        md5sum "$game" 2>/dev/null | cut -d' ' -f1 >"$data.build" || rm -f "$data.build"
    fi
fi

# A report of a recording whose binary has moved on: the symbols would be nonsense, so say which build it was made with
if [ -f "$data.build" ] && command -v md5sum >/dev/null 2>&1 && [ -x "$game" ]; then
    now=$(md5sum "$game" 2>/dev/null | cut -d' ' -f1)
    then_build=$(cat "$data.build" 2>/dev/null)
    if [ -n "$now" ] && [ "$now" != "$then_build" ]; then
        echo "profile: WARNING $data was recorded with a different build of $game" >&2
        echo "profile:          its frame names below are the new binary's for the old addresses, so they are not trustworthy" >&2
        echo "profile:          rebuild nothing and re-record, or check out the build the recording was made with" >&2
    fi
fi

if [ "$mode" = flame ] || [ "${1:-}" = "--flame" ]; then
    perf script -i "$data" >"$root/build/profile.script"
    if command -v stackcollapse-perf.pl >/dev/null 2>&1; then
        stackcollapse-perf.pl "$root/build/profile.script" >"$folded"
        echo "profile: folded stacks in $folded (flamegraph.pl $folded > build/flame.svg)"
    else
        echo "profile: folded stacks need the FlameGraph scripts on PATH (github.com/brendangregg/FlameGraph)" >&2
        echo "profile: the raw stacks are in $root/build/profile.script" >&2
    fi
fi

# The report, grouped by function: the top of each stack is the work the frame was in when it was sampled, so the functions
# that head the most samples are the ones to look at first. `perf report` itself is interactive; the stdio form is the same
# table, and is what a terminal log can hold
echo
echo "profile: hottest functions (children >> self means the time is below the function, self the function's own work)"
perf report -i "$data" --stdio --no-children --sort symbol 2>/dev/null | grep -E '^ +[0-9]+\.[0-9]+%' |
    sed -e 's/  */ /g' -e 's/ *$//' -e 's/ - -$//' | cut -c1-150 | head -40

# Who called the work: the same samples walked up the stack instead of listed flat. This is the half of a report that says
# *why* a function is hot (the copy behind a memcpy, the draw behind a submit). A recording made without a call graph has none,
# and says so rather than looking like a report whose stacks are all one deep
callers="$(perf report -i "$data" --stdio --no-children -g 'graph,0.5,caller' --max-stack 8 2>/dev/null)"
if printf '%s\n' "$callers" | grep -qE '^ +\|?-'; then
    echo "profile: callers of the hottest work"
    printf '%s\n' "$callers" | grep -E '^ +[0-9]+\.[0-9]+%|^ +\|?-' | sed 's/ *$//' | cut -c1-150 | head -70
    # A frame-pointer walk through a library built without frame pointers yields a stack of bare addresses rather than a
    # failure: say so, so the table is not read as real callers
    if [ "$(printf '%s\n' "$callers" | grep -cE '^ +\|?-{2,}0x')" -gt 8 ]; then
        echo "profile: those stacks are mostly bare addresses: the unwinder lost the chain (a library without frame" >&2
        echo "profile: pointers). Recording with PERF_GRAPH=dwarf keeps it" >&2
    fi
else
    echo "profile: no call graph in this recording (samples only): the list above names the functions, not their callers." >&2
    echo "profile: recording again is what adds the callers" >&2
fi
echo
echo "profile: full report: perf report -i $data"
