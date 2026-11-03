#!/bin/zsh
# stage.sh DIR SESSION SAVE CMDS... : one chunk, align masks, replay onto the late pads if needed
source /private/tmp/claude-501/-Users-brianpugh-projects-ebsrc/fb0daf9a-52d7-45c3-a277-26fd307b6f4d/scratchpad/swA/env.sh
D=$1; SS=$2; SV=$3; shift 3
tot=$(cat $D/[0-9][0-9][0-9].pad 2>/dev/null | wc -l | tr -d ' ')
$S/chunk.sh $D $SS "$@" 2>&1 | grep -E "^STATE|FIN|DONE|STUCK|NO PROG" | tail -2 | cut -c1-150
AA_MIN=$tot .venv/bin/python $S/autoalign.py $D 12 | grep -v "^  " | tail -2 | cut -c1-150
v=$(rec $D verify $SS | tail -1)
if [ "$v" != "SAME" ]; then
  pt stop $SS >/dev/null 2>&1; rec $D replay $SS $SV >/dev/null 2>&1; pt do $SS "padlog off" --no-shot >/dev/null
  echo "REPLAYED -> $(pt do $SS state --no-shot | grep -m1 '^STATE' | cut -c1-150)"
else echo SAME; fi
