# Report 10 — The save file corruption bug

Report 7 §7.7 recorded a data-loss bug and left it at "not yet investigated", where it sat while
nine optimisation candidates were catalogued and argued over. The outside triage in
`dragonbox-pyra/docs/optimization-playbook.md` §4 put it back at the top, correctly:

> This is a data-loss bug in the thing the port exists to deliver. It outranks every item below.

This report closes the mechanism, ships a fix for the consequence, and leaves exactly one
question open — with the tool to answer it.

## 10.1 What the symptom constrains

Two observations, from one run:

1. `Failed flush to file ./logs/Ship of Harkinian.log: Bad file descriptor`, **37 times**.
2. A log record written *into* a save file:

```
"vibrato": 0,      [08:27:52.520] [SaveManager.cpp:1132] [info] Save File - fileNum: 0
"vol...
```

Taken together these are much more specific than they look.

`Failed flush to file` is `spdlog::details::file_helper::flush()`, which is `std::fflush(fd_)`.
`EBADF` from `fflush` means the descriptor behind that `FILE*` is **not open** — not that the
directory is unwritable, not that the disk is full. spdlog itself only ever closes `fd_` in
`file_helper::close()`, which sets `fd_ = nullptr` in the same breath, so spdlog cannot reach this
state on its own.

The second observation supplies the other half. A log record appearing between two lines of save
JSON is byte-level interleaving into one file, which means the logger's `FILE*` and the save's
stream were writing to **the same descriptor number**.

One mechanism produces both:

1. Something closes the log's descriptor `N` without telling spdlog.
2. Flushes fail with `EBADF` while `N` is unallocated.
3. The save's `std::ofstream` opens — Linux hands out the lowest free number, so it gets `N`.
4. The logger's next flush writes a log record into the save file.

## 10.2 Reproduced, byte for byte

Not deduced — run. Twenty-five lines, host build, no game involved:

```c
FILE* log = fopen("demo.log", "ab");
fprintf(log, "...logger is healthy\n"); fflush(log);
close(fileno(log));                        // the stray close
fprintf(log, "...Save File - fileNum: 0\n");
fflush(log);                               // -> "Bad file descriptor"
FILE* save = fopen("demo.sav", "w");       // handed the same number
fprintf(save, "{\n \"vibrato\": 0,\n");  fflush(save);
fprintf(log,  "...Save File - fileNum: 0\n"); fflush(log);   // lands in demo.sav
fprintf(save, " \"volume\": 1\n}\n");
```

`demo.sav`:

```
{
 "vibrato": 0,
[00:00:02.000] [info] Save File - fileNum: 0
 "volume": 1
}
```

That is report 7's fragment, including the `"vibrato"` / `"vol…"` neighbours, which are simply
whichever two save keys the interleaving happened to fall between. The mechanism is settled.

## 10.3 Who closes it is still open, and it is not our code

A repo-wide search for a raw `close()` on Linux finds exactly one file — the fork/exec plumbing in
`soh/soh/Extractor/portable-file-dialogs.h`, which is not on the gameplay path and whose closes are
balanced. Nothing in `soh/` or `libultraship/src/` closes a descriptor it did not open. So the
caller is in a library: SDL2, libzip, the audio stack, or the proprietary PowerVR user-mode driver.

That last one is worth naming as the leading suspect only because it is the one component here that
is a binary blob, on a driver generation with a long history of descriptor handling quirks around
`/dev/pvrsrvkm`. It is a suspect, not a finding, and it will stay that way until measured — which is
the mistake §8.16 already catalogued, and not one worth repeating in a report about it.

`scripts/pyra/fdguard.c` answers it. It is an `LD_PRELOAD`, deliberately: it needs no rebuild, so it
runs against the exact binary that corrupted a save. It interposes `close`, `fclose` and `dup2`,
reports any of them landing on a descriptor that currently names the log, with a backtrace; then
remembers that number and reports the next `open`/`fopen` handed it, naming the file that inherited
it. The second line is the corruption caught in the act.

It interposes the `open64`/`openat64`/`fopen64` spellings as well, which is not optional here:
Debian's armhf toolchain enables large-file offsets by default, so libstdc++ — and therefore the
`std::ofstream` that writes the save — calls the 64 variants. Interposing only the unsuffixed names
would have caught the close and missed the theft, and the shim would have looked like it worked.

```sh
./scripts/pyra/build.sh fdguard          # -> build-pyra/fdguard.so, armhf
# on the device, beside the binary:
FDGUARD_MATCH='Ship of Harkinian.log' LD_PRELOAD=./fdguard.so ./soh 2>fdguard.txt
```

Verified against the reproduction above, where it printed both halves and named both files.

**Not yet run on the device.** It needs a session where the Pyra is free, and it needs a save to
happen — the corruption is rare enough that report 7 saw it once.

## 10.4 What was fixed: the consequence, not the cause

You cannot stop another thread closing your descriptor. There is no defensive trick in the logger
that survives it either: once the number has been reused, every option for neutralising the stale
`FILE*` — `fclose`, `freopen`, `dup2` — closes or overwrites the descriptor that the *victim* is now
using, which is worse than the disease. The only defence is to stop the victim from being damaged.

`SaveManager` wrote saves like this: stream JSON at a temp file, `rename()` it over the real save.
Nothing checked what was in the temp file. `rename()` does not care, so a corrupt copy silently
replaced a good save.

It now writes, closes, **reads the file back, and only renames if the bytes match what went out**,
with one retry. A byte compare rather than a JSON re-parse, because a parse would accept several of
the ways this goes wrong — a short write, a truncated tail, a stale file from a failed attempt — and
because comparing 200 KB is cheaper than parsing it.

`SaveManager::SaveGlobal` was worse and is fixed in the same change: it had no temp file at all,
streaming `global.sav` straight over the live copy, so any failed write destroyed it rather than
merely failing to update it.

The result is that a corrupt write costs **this** save, not **the** save.

## 10.5 What is still exposed

Honest list, because the fix above is narrow on purpose.

| Still exposed | Why it matters | Cost to fix |
|---|---|---|
| The log itself dies silently once the descriptor goes | Every later diagnostic in that run is lost, which is how this went uninvestigated for a phase | small — detect and say so on stderr |
| `Config::Save()` has the same defect | `libultraship/src/ship/config/Config.cpp`: `std::ofstream file(mPath); file << ...`, no temp file, no check. Losing `config.json` is annoying rather than fatal, but it is the same bug | small — same helper |
| A failed save is loud in the log and silent on screen | A player who believes the game saved is the remaining bad outcome | needs UI, not plumbing |
| No `fsync` before `rename` | A removable-battery handheld loses power without warning; the rename can land before the data | small, but needs POSIX calls in a file that still builds for three other platforms |
| Whatever *else* holds descriptor `N` when it is closed | The log is the noisiest victim, not the only possible one | none available until the caller is named |

The last row is the reason 10.3 matters more than 10.4. Defending each victim in turn is a losing
game; the fix is to find the close.

## 10.6 Side effect: the host build compiles again

Verifying any of this on the host needed a host build, and there wasn't one:
`soh/soh/Enhancements/custom-message/text.cpp` failed with `'ref' is not a member of 'std'` under
GCC 16. `text.h` uses `std::ref` and includes only `<string>`; newer libstdc++ stopped pulling
`<functional>` in transitively. One include, and the tree links again.

This is not a Pyra issue — the cross container is GCC 14 and was never affected — but it is worth
having, because a working host build is where cheap experiments belong. It is not a *substitute*
for the device: there is no ROM or `oot.o2r` on the host, so the game still cannot be launched
here, and the reproduction in §10.2 deliberately needed neither.

## 10.7 The close is found: the SGX driver's X11 window system closes a descriptor twice (2026-10-02)

**It was never rare.** 54 of the 63 harness run logs on the device carried `Failed flush to file ./logs/Ship
of Harkinian.log: Bad file descriptor`, most within seconds of launch. The log's descriptor was being closed in
nearly every run; a save only gets hurt when it is the next file opened, which is why report 7 saw a
corrupt save once.

**fdguard named it in the first run** (harness binary at the title screen, ~40 s). Two additions first, so a
silence could be trusted: `close_range`/`closefrom` wrappers, and a watcher thread that reports a watched
descriptor disappearing with no wrapper having seen it (a raw syscall or a libc-internal close); both
proved on the host against a plain `close()` and a raw `syscall(SYS_close)`. Built with `-funwind-tables`
and printing the wrapper's own return address through `dladdr`, because on ARM `backtrace()` returned
nothing without unwind tables:

```
FDGUARD: fd 12 opened on watched file ./logs/Ship of Harkinian.log
./dri3_ws.c:843: WSEGL_SwapControlInterval: WSEGL_SwapControlInterval 0
FDGUARD:   called from /lib/libpvrDRI3WSEGL.so+0x154d (?)
FDGUARD: close(12) on watched file /home/pyra/soh-bench2/logs/Ship of Harkinian.log
FDGUARD: fd 12 was the watched file, fopen() just gave it to /etc/powervr.ini
```

**The mechanism**, from the disassembly of `libpvrDRI3WSEGL.so` (the DDK's X11/DRI3 window system, a blob
from 2020, no package owns it):

- Buffer creation (`0x1f9a`-`0x202c`) exports each buffer as a dma-buf with `DRM_IOCTL_PRIME_HANDLE_TO_FD`,
  keeps the number at offset `0x10` of its buffer record, imports it into the SGX (`PVRSRVMapDmaBuf`), and
  passes it to `xcb_dri3_pixmap_from_buffer_checked`. **libxcb owns a descriptor once it is passed**: it
  sends it to X and closes it.
- Buffer destruction (`0x1520`-`0x1560`: `munmap`, `xcb_free_pixmap`, `PVRSRVUnmapDmaBuf`, then
  `close(buf->fd)`, then `DRM_IOCTL_MODE_DESTROY_DUMB`) closes the stored number **again**.

Between the two, the number is free, and whatever opens next gets it: the game's log (opened after the
window's first buffers), or, if the timing falls that way, a save. The buffers are rebuilt when the game sets
its swap interval (`WSEGL_SwapControlInterval 0`, the frame cap's own pacing), which is when the second close
lands. Every EGL program on the Pyra's desktop has the same double close; the game is just the one that
noticed.

**The fix is in the driver**: the second `close()` (`blx close@plt` at `0x1548`, its result unused) becomes two
Thumb NOPs. Tested in a private copy loaded through a per-directory `powervr.ini` first, then installed
system-wide on the owner's decision with the hardware repo's `device/pyra-wsegl-fdfix.sh` (checks the stock
sha256, keeps `/lib/libpvrDRI3WSEGL.so.stock`, replaces by rename because running programs have it mapped;
`rollback` restores):

| run (title screen, ~40 s) | log descriptor | `Bad file descriptor` | process descriptors at +5/15/25/35 s | dma-buf descriptors |
| --- | --- | --- | --- | --- |
| stock module | closed at +5 s | 3 | 20, 20, 20, 20 | 0 |
| patched module | open to exit (spdlog's own `fclose` at quit) | 0 | 21, 21, 21, 21 | 0 |

The flat count is the leak check: had libxcb not closed its copy, removing the driver's close would leave one
descriptor behind per rebuilt buffer. The play install, launched by `pyra-soh` with the system-wide fix: log
open to the end, 0 errors.

## 10.8 The defences finished

The cause is fixed, but a removable-battery handheld still loses power without warning, and the next stray
close would be someone else's. §10.5's list, closed:

- **`fsync` before `rename`** in `WriteJsonVerified` (POSIX builds; Windows, Switch and Wii U unchanged): the
  read-back-verified temp file is synced, renamed, and the directory synced, so after a power cut the save is
  either the old one or the whole new one. A failed file sync counts as a failed attempt; a failed directory
  sync is logged (the save is in place either way).
- **`Config::Save()`** (libultraship) writes a temp file, reads it back, syncs it and renames it over the config,
  instead of streaming over the live file; a failed write leaves the old config. Switch and Wii U keep the
  direct write (rename is unreliable there, as `SaveManager` already notes).
- **A failed save is on screen**: when `WriteJsonVerified` gives up, a notification says the save was not
  written and the previous one is intact. `Notification::Emit` was not safe from the save thread (it pushed
  into the list the UI walks every frame and played a sound through the game's audio state), so it now queues
  under a mutex and the notification window takes the queue on the main thread each frame; existing callers
  are unchanged but for one frame of delay.
- **The log dying silently** is moot with the driver fixed; left as it is.

**Tested on the device** (harness install, the owner saving from the pause menu, 2026-10-02):

| test | result |
| --- | --- |
| a normal save | `file1.sav` replaced by rename (new inode), parses, `Save File Finish ... (written)`; the whole save with both syncs 38 ms |
| a save with the temp path blocked (a non-empty directory at `Save/file1.temp`) | both attempts logged, `NOT WRITTEN`; the red "Save failed:" notification on screen (owner); `file1.sav` byte-identical to the previous save |
| the config | `shipofharkinian.json` replaced by rename at the in-run CVar save and at exit (new inode each time), content unchanged, no `.temp` left |
| the log, whole session, with the driver fix | 0 `Bad file descriptor` |
