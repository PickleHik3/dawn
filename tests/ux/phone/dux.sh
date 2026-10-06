# on-device driver: reads one step per line from /data/local/tmp/dux-steps.txt
# k <tmux keys> | t <literal text> | s <seconds> | c (capture) | C (capture with colours) | r <pattern> (row index of first match)
export PREFIX=/data/data/com.termux/files/usr HOME=/data/data/com.termux/files/home PATH=/data/data/com.termux/files/usr/bin:/system/bin
while IFS= read -r line || [ -n "$line" ]; do
  op=${line%% *}; arg=${line#* }
  case "$op" in
    k) tmux send-keys -t ux $arg; sleep 0.4;;
    t) tmux send-keys -t ux -l -- "$arg"; sleep 0.4;;
    s) sleep "$arg";;
    c) echo "----- screen"; tmux capture-pane -t ux -p | sed 's/[[:space:]]*$//' | awk 'NF{b=0} !NF{b++} b<2';;
    C) echo "----- screen (ansi)"; tmux capture-pane -t ux -p -e;;
    r) echo "row: $(tmux capture-pane -t ux -p | grep -n -m1 -- "$arg" | cut -d: -f1)";;
    x) eval "$arg";;
  esac
done < /data/local/tmp/dux-steps.txt
