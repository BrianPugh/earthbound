#!/bin/zsh
# chunk.sh DIR SESSION  -- opens the next padlog chunk (commands then follow via go2/pt), see usage in notes
source ${0:A:h}/env.sh
D=$1; SS=$2; shift 2
n=$(ls $D | grep -cE '^[0-9]{3}\.pad$'); nxt=$(printf "%s/%03d.pad" $D $n)
pt do $SS "padlog $nxt" --no-shot >/dev/null
for c in "$@"; do
  case "$c" in
    go:*) .venv/bin/python $S/go2.py $SS ${=c#go:} | tail -1 ;;
    fight) .venv/bin/python $S/fin.py $SS auto ;;
    finish) .venv/bin/python $S/fin.py $SS noauto ;;
    steps:*) .venv/bin/python $S/go2.py $SS --steps "${c#steps:}" | tail -1 ;;
    *) pt do $SS "$c" --no-shot | grep -E "^STATE|UNTIL" | tail -2 | cut -c1-160 ;;
  esac
done
pt do $SS "padlog off" --no-shot >/dev/null
rec $D check | head -8 | cut -c1-200
rec $D verify $SS | tail -1
