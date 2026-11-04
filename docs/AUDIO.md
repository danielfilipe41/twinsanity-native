# The sound

How the port makes the game's sound, why it is built the way it is, and what to check when it misbehaves.

The game's own sound code (`src/game/sound.cpp`, `soundobjects.cpp`) is retail's and was not touched. It drives a fixed
interface, `Platform::Audio` (`include/platform/audio.h`), which on the PS2 is a thin wrapper over MultiStream, the EE client of
the disc's `STREAM.IRX` sound and streaming module. On the PC the same interface is answered by a reconstruction of that module
and of the sound processor it drives.

## The three layers

| Layer | Files | What it is |
|---|---|---|
| The sound processor | `spu2.{h,cpp}` | The SPU2 as a device: 2 cores of 24 voices, ADPCM out of 2 MB of 16 bit words, an envelope and a 15 bit volume per voice, a dry and an effect sum per core, a reverb network, and two master volumes. |
| The module | `soundmodule.{h,cpp}`, `soundmemory.{h,cpp}` | The side of the bargain `STREAM.IRX` makes: a table of the sounds in memory, a channel per voice with the key status the game polls, the volume groups, an effect per core, the memory's compaction, and the music streams. |
| The platform | `audio.cpp`, `audio.h` | `Platform::Audio` over the module, the module's mix handed to SDL, and the movie's own PCM mixed on top. |

Everything under the module is the hardware's arithmetic; everything at the module is the game's contract. Keeping those two
separate is what makes both testable: `ninja audio-proof` runs the hardware side with no disc and no game.

## Why it is built this way

The game **never re-feeds** anything. It plays a sound or starts a track, then only polls. Three consequences fall out of that,
and each was a real bug in the host's first mixer:

- `IsVoiceFree` is the **only** way the game ever frees a voice (`sound.cpp` clears `voicesInUse` on that signal alone). A voice
  that never reports free is a voice lost for the session, and 48 of those is a silent game. So a key-on always advances its
  envelope (a rate of zero holds nothing here), and a sound's end frees its voice within a block of where its data ends.
- `IsMusicPlaying` is the **only** way the game learns a track ended (`UpdateMusic` treats a false as "track over", stops it, and
  never restarts it). So it goes true at `StreamMusic` and stays true — through a pause, through a movie, through a chunk load —
  until the track's data genuinely runs out or the game stops it.
- Music must keep streaming **without the game's help**. So the module owns the ring in the processor's memory and tops it up
  itself.

## The retail contract, and where it came from

The MultiStream EE client in `src/platform/ps2/multistream/` encodes most of it: the stream state machine and what
`MsGetStreamStatus` reports, that `MsIsChannelFree` is a cached key status, that a play keys exactly the voice it is given, that
`ReleaseVoice` is a rate followed by a key off, and that music buffers are `0x8000` bytes a player. What the IOP module does
internally is not in this repository; the processor's own behaviour is taken from PCSX2's SPU2, which is the reference for the
chip, and from the disc's own data.

Three things the disc settled, which the first mixer had guessed:

- **The bank header is `0x40` bytes** (`MSVp`, the size, the rate, the sound's name), and the first ADPCM block is the one right
  after it. The first mixer skipped 48, which left a block of silence and a misaligned start on every sound in the game.
- **The effect register sets** are Sony's own, out of the disc's `LIBSD.IRX`, thirty two words a mode.
- **A track's data carries its own loop and end flags.** The module clears them as it lays blocks into the ring and writes its
  own, or a streamed track would stop or jump at its authored loop point instead of where the module loops the ring.

## The music's ring

A player reserves `0x8000` bytes of processor memory; an interleaved track takes half of it a side (`0x4000`, the module's
`MusicStreamBuffer`). The module reads the track's region into the host's memory once, at `StreamMusic`, and then lays blocks of
it into the ring behind the voice's read pointer, keeping 128 blocks clear. Two properties hold it together:

- the ring's last block carries end+loop, so the voice comes back to the ring's first block by itself;
- a track that plays once has its final block carry end alone, so the voice stops there and the module reports the stream over;
- a track that loops simply starts its data again — the ring never notices, because it only ever sees the sequence of blocks.

Two things the ring cannot be trusted to do by itself, both of which cost a side of a track in a playthrough's log:

- **A ring is primed before any voice is let loose on it**, and that includes the ring of an interleaved track's *other side*: the
  game prepares and plays only the parent's channel, and the module keys the second voice on itself. A side started on a ring
  nobody had written played the memory that was there — heard as a burst of the wrong sound — and then read on until a stale
  block's flags stopped it, so that side was silent for the rest of the session.
- **A looping track's region starts over when it is spent** (`WriteStreamBlock` resets to the region's beginning). A filler that
  stops at the region's end leaves the ring half written, and the voice reaches memory nobody wrote.

Nothing is decoded to PCM up front and no disc read happens on the audio thread: `audio.cpp` holds the lock for a mix chunk,
which is the same bargain the module makes with the I/O processor.

## Pauses and movies

`PauseAll` is **not** a mute of the processor. It lets the game's sounds go — they release and fade out, and their voices free as
they would on a release — and holds the music where it is: the stream voices keep their read position, their envelopes and their
volumes, and simply say nothing until `ResumeAll`. A sound played *during* a pause is heard, which matters because the frontend
plays its menu sounds while the module is in exactly that state.

The reason it is built that way is that the game does not always pair the two calls. `PauseSound()` is called when a movie starts
and when the game pauses; `ResumeSound()` is called when play starts again (`StartingPlay`) and when the game resumes. A movie
that ends into the title screen goes `MovieEnded` → `StateLoadingTitle`, which never reaches `StartingPlay` — so the module is
left paused for the rest of the session. On the PS2 that costs nothing, because a paused processor still lets a key-on be heard
through it. A module that muted its whole output on one unmatched pause silenced every menu sound in the frontend, which is
precisely the bug that was reported as "menu sounds don't work".

`LendToMovie` is separate and takes away key-ons and nothing else: no channel is stopped and no track cleared, so a track that
was playing is still playing when the module comes back — which is what the game's music player assumes, since it will not
restart a track it believes is playing.

The movie's sound is its own path, as on the PS2 where it goes through libsdr into the processor's input rather than through a
voice: `audio.cpp` mixes its PCM on top of the module's output and saturates.

## Compaction

The game asks for `CompactSoundMemory` every 36 frames. The memory's blocks are moved down over the gaps released sounds left.

**The samples themselves are moved**, not just the table: the arena knows only where a sound's data *should* be, and a table
that says a sound is at a new address while the processor's memory still holds its old occupant's bytes is every moved sound
playing the wrong data — heard as a snatch of another sound, or as noise, from the first compaction of a session onwards. The
moves go in the arena's own order, ascending, and each destination is at or below its own block, so no block still waiting to
be copied is overwritten on the way.

**Every voice reading a moved block is moved with it** — its start, its loop point, its end and its read position. The start
alone is not enough: a loop point left behind sends a looping sound back to where its data used to be, and an end left behind
lets a one-shot read on past its own samples into whatever is under them.

A block released back onto the memory's end is **no longer a hole in the arena's list**: it is the free space the arena grows
into. Left in both places, the next sound that did not fit a hole was handed the released block's own address and two live
sounds sat on top of each other.

## Checking it

```sh
ninja audio-proof && ./build/host/audio_proof
```

126 checks, no disc needed. They are the numbers a change to the sound has to keep: the interpolation table against the
published one, the ADPCM filter's coefficients and its history across blocks, the volume registers' other phase, no envelope
rate stalling, a one shot freeing its voice at its block's end, loops surviving and then stopping on release, four voices at
full saturating rather than wrapping, the reverb's tail, the memory's compaction (the samples moved, a voice's start, loop
point and end carried with it, and the sound table's addresses updated so a play after a compaction reads the moved block) and the arena never giving two live sounds the same words, the module's
channel states through play/release/pause/movie, the pause that lets its sounds go and still plays a menu over itself, a play
of no passes being one pass and never leaving its own data, a counted play going round exactly that many times, a re-bound
channel letting its old voice go, a channel re-bound *while* that old voice is still being let go of playing its new track
rather than being refused for data it has, a group's volume reaching a track (at its start and while it plays), a side the game
never prepared still having a ring under it, a track's voice not being the game's to take while the track plays and being the
game's again once it is let go of, the streams' rings (a track longer than its ring never falls silent, a once-track ends where
its data does, an interleaved pair keeps all four of its voices, a track's ring splitting its chunks), and the claim the streaming
rests on: **a track streamed through the ring is sample-for-sample the same as the same bytes played as a bank sound.**

Each of these checks has been seen to fail: a check that cannot fail is not a check, and two of them could not until they were
rebuilt — the compaction one used a pattern whose period was exactly the distance the memory moves by, so a voice reading the
wrong blocks read the same bytes.

## When it sounds wrong

```sh
cd disc
TS_AUDIOLOG=1 ../build/host/sdl3/game-with-data 2> audio.log
```

The log answers for what the module is asked and what it did. `TS_AUDIOLOG` turns the game's own sound log on as well as the
module's, so one file holds both sides of a sound that went missing: whether the game ever asked for it, and what the module did
with it.

- `sound: play id <n> loaded <n> size <n> offset <n> group <n> kind <n> last <n>` — the game handing a sound over; the sound's
  name comes with the voice it landed on (`sound: voice <n> mixer id <n> (<name>) ...`), because a bank read keeps no names.
- `sound: placed id <n> volume <n> pitch <n> group <n> kind <n> last <n>` / `sound: by-id ...` / `sound: instance-by-id ...` —
  the same handover for the other ways the game asks for a sound: at a place in the world, by an id alone, or as an instance
  (lit, looping or followed) sound.
- `sound: play id <n> no free voice (kind <n>)` — the game dropped it before the module saw it: every voice of that kind was
  busy. Nothing in the module can cause this on its own, but a voice the module never reports free will.
- `sound: placed id <n> NOT HEARD (... drawn <n> now <n>)` / `WITHOUT SAMPLES` — a sound with a position in the world that the
  game turned down itself: its chunk was not drawn in the last two frames, or its samples are not loaded.
- `sound: placed id <n> INAUDIBLE at <x> <y> <z> (volume <n>)` — the distance and the panning left nothing to hear (retail's
  `SoundAtPlace` says so at 0.005 of the volume), which is the one drop that happens after a voice was found.
- `sound: dosound slot <s> of <n>, slot value <v>, sound <id>` / `... (THE SLOT IS EMPTY)` — a sound's command picking one of its
  slots and what that slot held: an empty slot asks for nothing at all, and is the one way a sound goes missing with no play
  line to show for it.
- `sound: gem <n> picked up (frame <n>)` / `sound: crystal picked up (frame <n>)` — a marker in the run of log lines, so the
  requests that follow it belong to the pickup exactly.
- `load sound <id> <bytes> -> <words> at <address> (header <n>, memory <free>/<total>)` — what a bank read put where, and how
  much memory is left. A load that fails to find room shows up as a play that then does nothing.
- `play sound <id> voice <n> vol <L>/<R> pitch <p> attack <a> release <r> loops <n> group <g> from <address>` — exactly what
  the game handed the module, and `ON A VOICE ALREADY PLAYING` when a play landed on a sounding voice. The address is the
  game's own call site, so a sound nobody can account for is traced rather than guessed at:
  `addr2line -f -e build/host/sdl3/game-with-data <address>` names the function that asked for it.
- `play sound <id> voice <n> REFUSED (...)` — the module turned it down: its samples are not in the memory, or a movie has the
  module.
- `voice <n> cut to silence (was <L>/<R>, sound <s>)` — the game took a sounding voice to zero, which is how a sound stops in the
  middle of itself.
- `stream channel <n> file <f> offset <o> size <s> rate pitch <p> voice <n> loop <l> ring <r>` — a track's start.
- `stream channel <n> has no data to play (nothing was read: <n> bytes in memory of <n>, ring <n> words, file <n>)` — the
  region's read came back empty, so no voice is keyed on and the game is told the track is over rather than left believing it
  plays. The numbers say which of the two it was: nothing in memory (the read found no file) or nothing to write into (the ring).
- `stream channel <n> read its region again at the play (<n> bytes)` — the set-up's read found no file, so the region was taken
  again as the track was played. Retail reads the region as it plays, so a bank that was not open at the set-up costs it
  nothing; this port reads the whole region up front, which is why the play repeats it when it is missing.
- `stream channel <n> ended` / `stopped (<n> of 48 voices still sounding)` / `STOPPED ON ITS OWN` (said once per side: a looping
  track's voice stopping by itself is a fault, since its ring always has a block that comes back round).
- `compacted the memory: <n> block(s) moved, <n> voice(s) followed, <n> words used` — only when something actually moved. A voice
  that should have followed one and did not is the shape of a noise burst.

`TS_AUDIODUMP=<file>` writes the mixed stream's raw s16 stereo at 48 kHz, which is what to compare by ear or by measurement.

## What is not settled

- **The one that went quiet was found, and it was two faults.** A playthrough's log named it: `stream channel 5 voice 3 STOPPED
  ON ITS OWN` — the *second side* of the context player, going silent a few seconds in and staying silent for the session. The
  game only ever prepares and plays the parent's channel, and the module keyed the second voice on anyway, onto a ring nobody
  had written: it played the memory that was there (heard as a burst of the wrong sound) and then met a stale block whose flags
  stopped it. Both sides are primed now, before either voice is keyed on. The same log showed the second fault behind it: the
  game asks `IsVoiceFree` every frame for every voice and marks the ones it is told about as unplaying, so a track's voice
  reported free was one the game would hand to a sound.
- **A second audit of the whole module found six**, and **the gem and extra-life chimes found a seventh**; each is now fixed and each with a check that has been seen to fail
  without it: the memory compaction moved the table but not the samples (every moved sound played its old occupant's data, from
  the first compaction of a session onwards — the noise class of failure, and a playthrough's log shows one such compaction with
  27 blocks in it); the sound table's addresses were never updated by a compaction, so any play after one read the address a sound
  used to be at (every such play read whichever sound had moved down onto it — **this killed the gem and extra-life chimes**, which
  are the same sound 266, and is the same class as the "hogs sounding like noise" complaint); a voice followed a move with its start only, leaving its loop point and its end behind; the arena could hand
  two live sounds the same words after a release; a pass count of 0 left a voice that carried its own loop flag going round for
  good; `PlayMusic` could key a voice on with nothing written under it; and a pause could hold a voice the track had already let
  go of and the game had been given back.
- **The envelope question, deliberately left alone.** With sustain level 15 and the fastest decay, the level drops by half one
  frame after the attack reaches full and then holds — the same registers on the hardware put the level in the same place, so a
  sound sits 6 dB under its register value and there is a step of one envelope bit at the top of every key-on. It is uniform, so
  it cannot make one sound louder than another; if a playthrough ever finds everything uniformly quiet or everything clicking,
  this is the place to look. The stream envelope's release rate was 0 (the chip's fastest, a click) and is now the same 0x0D the
  module lets a track go at.
- **Two sounds measured by ear, not yet by number**: the gear sounds (reported loud) and the hogs (reported like noise). Hogs
  are the compaction class of bug above, so they should be measurably better now. The gears are **a repeating instance sound** —
  a playthrough's log shows the same sound re-played every pass on a fresh voice with the volume creeping up as the player
  approaches, which is `soundobjects.cpp`'s `StateWaiting` re-key. Whether that is simply what the game asks for is not
  answerable from the log alone; the play line now shows the sound's id, volume, pitch and the game's call site, and the banks
  keep no names, so an id is what to compare against retail by ear.
- **The gem's chime is scripted, and where it is is now known**: a gem's own pickup path (`AddGemCommand::ExecuteOn` →
  `ShowGemWidget` → `OLEG::PlayPickupEffect`) is a HUD sparkle and nothing else, which the retail binary confirms — its `AddGem`
  command's only call is the visual effect. The chime is a `DoSound` command instead: the six-gem table in `CRASH.BD`
  (`0x592f04`–`0x592fa4`) is followed at `0x592fc4` by one whose flags (`0x007800f1`) name a single slot and the random-slot
  kind, and whose `soundSlots[0]` is `1` — the picked-up object's second sound slot. Our command logic matches retail's for that
  path, so the open question is whether the object the command runs on has that slot, and whether the command runs at all: the
  `sound: gem <n> picked up` marker, the `sound: dosound slot ...` line and the drops below it (`NOT HEARD`, `WITHOUT SAMPLES`,
  `no free voice`, `INAUDIBLE`, `(THE SLOT IS EMPTY)`) answer both between them.
- **The effect's depth, delay and feedback mapping** (the game's knobs over libsd's preset registers) is this port's reading of
  what the module did with them; the register sets themselves are the disc's.
- **The volume ramp** is ten milliseconds either way, which is the module's smooth volume opcode's job, not a number read off the
  hardware.

## The audit

The sound was rebuilt against retail's contract, but a contract is not a guarantee: the failures that survived the first
playthrough were the ones nobody had thought to state. This is the list of the ways the module can differ from what the game
expects, found by going through every call the game makes and asking what the retail module would have done, what this one does,
and what happens when the two part.

### The one that was actually wrong

**A sound's end is its length, not its flags.** The chip only knows a block's flags, and it reads on until it meets an end flag.
Not every region carries one where it would have to: the module's own load line counts them per sound, and the disc's sounds
come in both shapes — `0 end 0 loop` on some, `1 end 0 loop` and `2 end 1 loop` on most. A region with no end flag at all is one
the chip would read straight through, on into whatever the memory held next. The retail module knows each sound's size — that is
what its sound table *is* — so the voice is stopped at the samples' end either way, and a looping region is what it is because
the module was told the sound loops. The rebuilt module had no such notion, so a voice reading a flag-less sound walked on into
the next sound's samples.

What that looked like from a controller: menu sounds playing a few times and then stopping entirely (their voices were never let
go of, so the game's own voice table filled and `FindFreeVoice` had nothing left to hand out), sounds stuck repeating, a noise
burst at a moment that always repeated, and audio degrading the longer a session went on. One missing field, five symptoms.

The device now carries a voice's data end, set by the module from the sound's own length when it plays. The check that pins it
is `a sound with no end flag is still let go of at its length`.

### The other two

**A pause is not a mute.** The game pauses the module for a movie and resumes it when play starts again — but a movie that ends
into the title screen goes `MovieEnded` → `StateLoadingTitle`, which never reaches the `StartingPlay` that would call
`ResumeSound`. The module was left paused for the rest of the session with its output memset to zero, so every menu sound
played, logged, and said nothing. Nothing in the log looked wrong: the plays were all there, at full volume, on free voices.
The tells were the pause line with no resume after it, and a mix that never went above zero afterwards. The pause now lets the
sounds go and holds only the music (`a sound played during an unpaired pause is still heard`).

**A voice must not keep the last play's loop point.** `Voice::Start` reset the read position, the history, the FIFO and the
flags, but not the loop point — which a streamed track sets to somewhere inside its own ring. A looping sound re-keyed onto that
voice wrapped into memory that was not its own: a snatch of another sound, at the wrong pitch, arriving on its own. That is the
"menu sound but at another pitch" that played with nothing pressed. The loop point is now the play's own start, and only the
data can name another (`a voice does not loop into memory it does not own`).

Ordering matters here and cost an hour: a key-on resets the voice, so the data window has to be handed over *after* it. A
duplicate `KeyOn` in the play path undid the window every time, which showed up as a repeat that stopped after one pass.

### Every other call, and where it could part from retail

| The game asks | Retail module | This one | Where they could part |
|---|---|---|---|
| `PlaySound(id, voice, vol, pitch, attack, release, loops)` | Keys the given voice, its table's block for the samples, and counts the passes the game asked for | Same, and hands the voice the sound's end and whether it is to go round it | A `loops` of 0 or 1 is a single pass: the voice stops at the end of its data, and the game is free to key it again — which is what a repeatable sound is, `soundobjects.cpp`'s instance sounds waiting for the voice to stop and then playing it again. Only a count above one is a count of passes, and the voice goes round its own data that many times. Looping the `0` case here would stall the game's own state machine and pile copies up with nothing able to stop them. **The module's pass count is never 0 for that same reason**: most of the disc's sounds carry a loop flag on their last block, the processor follows that flag on its own, and a count of 0 meant the module never keyed the voice off, so a repeatable sound went round for good and its voice never came back |
| `ReleaseVoice(voice, rate)` | Release rate into ADSR2, then key off | Same | A rate of 0x1F holds the envelope; the game only ever sends 0xD |
| `IsVoiceFree(voice)` | Cached key status: `Off` only when the IOP says so | `Off` once the device says the voice is stopped | A voice that never stops never frees — the failure above. Every path that ends a sound now ends it: the flags, the length, or the game's own release. **A track's voice is never free while the track is the module's**, which is retail's `MsPlayStream` marking the music's channel taken: reported free, the game marked the voice unplaying and then treated it as stealable, which is how a sound came to be played onto a track's voice and a side of the track was lost |
| `SetVoiceVolume` | Volume register, smoothed | Same, ramped over ~10 ms | A per-frame volume from a moving instance sound lags the ramp slightly. Retail's slide is the same kind of unit, at a rate this port chose |
| `SetGroupVolume` | Each voice in the group rewritten | Same, in the level domain | Group volume is applied on top of the game's own register value; a group at full is exactly the register's value (checked). **A stream's voice is not one of the game's channels**, so it is scaled whenever a stream is bound to it — without that the music volume option reached nothing and a track played at its own level however low the option was set |
| `SetReverb(mode, depth, delay, feedback)` | libsd's preset registers, area reserved at the top of memory | Same | The depth/delay/feedback mapping is this port's reading of the knobs |
| `ReserveSoundMemory(0x8000)` | Bumps the bank pointer, returns bytes | A block from the arena, returned in bytes | The game adds `0x4000` to it for an interleaved side, so the units have to be bytes; they are |
| `ReleaseSound(id)` | Frees the block | Frees it, **and lets go of any voice still playing it** | Retail's compaction would move the samples under the voice; this refuses to leave a voice reading freed memory (the noise class of the bug above) |
| `CompactSoundMemory` | Moves the samples down and rewrites the table | Same, **and carries every voice reading a moved block** | The samples have to move with the table: a table that says a sound is at a new address while the memory still holds the old occupant's bytes is every moved sound playing another sound's data. A voice whose start is inside a moved block follows it — start, loop point, end and read position all, or it wraps to where its data used to be. A voice reading a *freed* block has none to follow, which the release above prevents |
| `ReserveSounds(0x100)` | A table of 0x100 slots in IOP memory | A fixed table of 0x10000 ids | Nothing: the bigger table only accepts more |
| `ReadMusic`/`StreamMusic`/`PlayMusic`/`StopMusic`/`IsMusicPlaying` | A stream the module keeps; `IsMusicPlaying` false only at a real end or a stop | Same | A stream's end is its ring's end flag; a once-track's last block carries end alone. A channel given a new track **lets the one it had go first**, its other side with it: a re-bound channel that left the old voice looping its ring went on sounding under the new track, a copy per track change, with nothing in the game left that knew it was there to stop. **Both sides' rings are primed before either voice is keyed on**, including the side the game never prepares |
| `PauseAll`/`ResumeAll` | The SPU2 paused: voices and envelopes held | The sounds are let go of and the music is held, positions kept, and the mix keeps running | The game does not always resume (a movie into the title screen). Retail tolerates that; a module that muted everything did not. Only a track the module still holds, whose voice it actually keyed on, is held: holding by the stream's recorded voice alone could freeze a voice the track had already been let go of and the game had been given back. A resume clears every hold on the processor rather than asking the streams, since a track re-bound while paused is no longer on the voice its channel names. See "Pauses and movies" |
| `LendToMovie`/`ReclaimFromMovie` | Takes the key-on callback away, touches no stream state | Same: key-ons refused, nothing cleared | The movie's own PCM is mixed outside the module, as retail's goes through libsdr. **The game lends the sound late, not early**: retail's `MoviePlayer::Start` lends on the way out of a start-up that worked, past both of its own failed-open returns, so this port now opens the movie first and lends only then — lending first left a player that failed to open holding the sound and the music's buffer for the rest of the session, which refuses every game sound and takes the music's buffer out of the game's hands, and the readers' `ReclaimMovieSound` net only fires on a movie that actually ran |

### Side effects worth knowing about

- **Voices are the game's to hand out.** The module never picks a voice and never steals one. A play on a voice that is still
  sounding re-keys it, which is what the module does too — the log says `ON A VOICE ALREADY PLAYING` when it happens.
- **The memory is the game's to ask about.** Nothing is compacted or moved behind its back; the only moves are the ones its own
  36-frame `CompactSoundMemory` asks for, and each one says what it moved.
- **A track's own loop point is not ours.** The module clears the region's flags as it lays blocks into a ring and writes its
  own, or a streamed track would stop or jump where its data happens to be authored to.
- **A stream reads the disc from the game's thread, once per track.** Retail's module reads it on the I/O processor as it plays.
  The audible difference is that a track's setup is a read rather than a preload, which is the game's `Preparing` state.
- **Two music players really can play at once** (main and context slots). The frontend does exactly that; the two tracks are
  different material, and they are meant to be heard together.

## Checking a sound against the disc

`ninja audio-disc` builds a parser for the disc's own audio:

```sh
./build/host/audio_disc disc/CRASH6/ENGLISH.MB <offset> <size> --rate 32000 --wav /tmp/sound.wav
./build/host/audio_disc disc/CRASH6/MUSIC.MB   <offset> <size> --rate 32000
```

It reports whether the region opens with the bank tools' header, how many blocks it is, **how many blocks carry an end or loop
flag** (the number that would have shown the bug above immediately), whether the block headers are plausible, the region's RMS
and peak, its length at the rate it was made at, and its energy over time; `--wav` writes what it sounds like. The offsets come
from the port's own log: `bank <id> from file <n> (<path>) at <offset>, <size> bytes` and
`stream channel <c> file <f> offset <o> size <s>`. Use the **path** the log names, not a guess at which `.mb` it was: the port's
file ids are not the disc's order, and a region parsed out of the wrong file reads as noise (the tool says so, with the count of
block headers that are not a plausible shift and filter) and tells you nothing about the sound.

