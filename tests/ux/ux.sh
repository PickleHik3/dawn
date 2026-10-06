#!/bin/bash
# Drive dawn like a user in a tmux pty. Start a session first, with a scratch HOME so real notes
# and config stay untouched, and (optionally) the mock TAI from mock_tai.py:
#
#   python3 -I tests/ux/mock_tai.py 8765 &
#   mkdir -p /tmp/dawn-ux/home/.launcherctl && echo http://127.0.0.1:8765 > /tmp/dawn-ux/home/.launcherctl/endpoint
#   tmux new-session -d -s dawnux -x 100 -y 40 \
#     "env HOME=/tmp/dawn-ux/home XDG_DATA_HOME=/tmp/dawn-ux/data XDG_CONFIG_HOME=/tmp/dawn-ux/config \
#          XDG_CACHE_HOME=/tmp/dawn-ux/cache TERM=xterm-256color ./build/dawn"
#
# Then:  ux.sh k <tmux keys>      send keys (C-s, Escape, Enter, Up ...)
#        ux.sh t <text>           type literal text
#        ux.sh seq '<escaped>'    raw bytes to the pty, e.g. a dictation mark:
#                                 ux.sh seq '\x1b]7727;phrase;id=1\x07\x1b[200~Hello\x1b[201~'
#                                 or a theme report: ux.sh seq '\x1b[?997;2n'
#        ux.sh shot <name>        capture the screen to shots/<name>.txt (+ .ansi with colours)
# Mock knobs: curl -XPOST localhost:8765/_mock/reply_mode/{plain|tool|edit|error409|notools|slow}
#             curl -XPOST localhost:8765/_mock/speak_seconds/3.0
SESS=${DAWN_UX_SESSION:-dawnux}; OUT=${DAWN_UX_SHOTS:-shots}
case "$1" in
  k) shift; tmux send-keys -t "$SESS" "$@"; sleep 0.35;;
  t) shift; tmux send-keys -t "$SESS" -l -- "$*"; sleep 0.35;;
  seq) shift; hex=$(python3 -c "import sys; print(' '.join('%02x'%b for b in sys.argv[1].encode().decode('unicode_escape').encode('latin1')))" "$*"); tmux send-keys -t "$SESS" -H $hex; sleep 0.5;;
  shot) shift; mkdir -p "$OUT"; tmux capture-pane -t "$SESS" -p > "$OUT/$1.txt"; tmux capture-pane -t "$SESS" -p -e > "$OUT/$1.ansi"; sed 's/[[:space:]]*$//' "$OUT/$1.txt" | awk 'NF{b=0} !NF{b++} b<2';;
  *) sed -n 2,20p "$0"; exit 1;;
esac
