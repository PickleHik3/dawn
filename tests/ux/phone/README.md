# Driving dawn on the phone over adb

Prerequisites on the device (Termux Launcher): `tmux` installed in Termux, the test build at
`~/.local/bin/dawn-tl` and the wrapper `~/.local/bin/dawn-ux`, which runs it with scratch XDG
directories under `~/.cache/dawn-ux/` so the user's notes stay untouched while `~/.launcherctl`
and `~/.termux/material-colors-*` still come from the real HOME.

Cross-build: `tlstore/recipes/cross/termux-sysroot.sh libcurl openssl zlib libnghttp2`, then the
cmake invocation from `build-dawn.sh` pointed at this checkout (NDK toolchain, `USE_LIBAI=ON`,
RUNPATH on the Termux prefix). Push with `adb push build/dawn /data/local/tmp/dawn-tl` and copy
it into place with `run-as com.termux`.

Loop: type `tmux new -s ux` into the launcher's terminal (via `ad type`), then drive the pane from
here with `dux`, which pushes a step file and runs `dux.sh` under `run-as com.termux`:

    ./dux <<'STEPS'
    t dawn-ux .cache/dawn-ux/notes/phone-run.md
    k Enter
    s 2
    c
    STEPS

Steps: `k <tmux keys>`, `t <literal>`, `s <seconds>`, `c` capture, `C` capture with colours,
`r <pattern>` row of the first match, `x <shell>`. Taps go through the `ad` helper; a pane row r
(1-based) is at y = 226 + (r - 0.5) * (1363 - 226) / 24 and a column c at
x = 16 + (c - 0.5) * (1064 - 16) / 53 for a 53x24 pane on the A065. Kitty graphics, OSC 66
headings and dictation marks do not pass through tmux: for those, run `dawn-ux` directly in the
terminal and read screenshots.

Pitfalls met: `adb shell` re-splits arguments on spaces (hence the step file); `run-as` cannot
run `pkg install` (it hangs on the pacman lock), so install packages from the terminal itself;
the on-screen Ctrl latch does not apply to adb-injected keys.

## The emulator (x86_64)

The local Android 16 emulator (`ad -s emu`, see the android-control skill) runs Termux Launcher too,
so most taps can be checked there without the phone. Its Termux has no tmux and `~/.local/bin` is
not on PATH, so run dawn by its full path and read screenshots. Recipe, from this repo's root:

    cd ../tlstore/recipes/cross && TL_TERMUX_ARCH=x86_64 TL_SYSROOT=$PWD/sysroot-x86_64 \
        TL_CACHE=$PWD/debs-x86_64 ./termux-sysroot.sh libcurl openssl zlib libnghttp2 && cd -
    TL_SYSROOT=../tlstore/recipes/cross/sysroot-x86_64 tests/ux/emu/build-x86_64.sh
    ad -s emu lease acquire "dawn checks"; ad emu start
    ad -s emu adb push build-emu/dawn-x86_64 /data/local/tmp/dawn-tl
    ad -s emu adb push tests/ux/emu/dawn-ux /data/local/tmp/dawn-ux
    for f in dawn-tl dawn-ux; do ad -s emu adb shell run-as com.termux cp /data/local/tmp/$f files/home/.local/bin/$f; done
    ad -s emu adb shell run-as com.termux chmod 755 files/home/.local/bin/dawn-tl files/home/.local/bin/dawn-ux
    ad -s emu launch com.termux; ad -s emu type '~/.local/bin/dawn-ux'; ad -s emu key ENTER

With the keyboard up the pane is 53x30 (1080x2400 at 420 dpi): a column c is at x = 46 + (c - 0.5) * 18.8
and a row r at y = 248 + (r - 0.5) * 37.8, or simply tap what a screenshot shows (`ad shot`, times 1.2
from the scaled view). `dawn-ux` keeps notes and settings under `~/.cache/dawn-ux/`; delete
`config/dawn/settings.json` there to see the first-run tour again. A note file copied into
`~/.cache/dawn-ux/data/dawn/` is not in history (that is the sessions index): open it by path.
A finger drag reaches dawn as wheel events, not SGR drag motion, so the pill follows a swipe
rather than the finger; a slow drag (800 ms) is taken as a long press and does nothing. The
on-screen Ctrl then a letter key (both via `ad tap`) does send the chord, unlike adb-injected keys.
