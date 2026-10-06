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
