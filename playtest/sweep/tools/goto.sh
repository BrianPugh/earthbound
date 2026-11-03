#!/bin/zsh
# goto.sh DIR SESSION SAVE GX GY [TRIES]: BFS + record + verify/replay until within 16 px
source /private/tmp/claude-501/-Users-brianpugh-projects-ebsrc/fb0daf9a-52d7-45c3-a277-26fd307b6f4d/scratchpad/swA/env.sh
D=$1; SS=$2; SV=$3; GX=$4; GY=$5
for t in $(seq 1 ${6:-6}); do
  v=$(rec $D verify $SS | tail -1)
  if [ "$v" != "SAME" ]; then pt stop $SS >/dev/null 2>&1; rec $D replay $SS $SV >/dev/null 2>&1; pt do $SS "padlog off" --no-shot >/dev/null; fi
  stl=$(pt do $SS "state" --no-shot | grep -m1 "^STATE")
  if echo "$stl" | grep -q "BATTLE\|idle=battle"; then
    echo "fight"; $S/stage.sh $D $SS $SV fight "state" 2>&1 | grep -E "FIN|SAME|DIFF" | tail -2; continue
  fi
  p=$(echo "$stl" | grep -m1 -o "pos=([0-9]*,[0-9]*)" | tr -d 'pos=()')
  x=${p%,*}; y=${p#*,}
  dx=$((x-GX)); dy=$((y-GY)); [ ${dx#-} -le 16 ] && [ ${dy#-} -le 16 ] && { echo "AT $p"; exit 0; }
  P=$($S/bfsrec.sh $D $SS $GX $GY 24 500)
  echo "try $t from $p: $(head -c 60 $S/route_last.txt)"
  $S/stage.sh $D $SS $SV "steps:$P" "state" 2>&1 | grep -E "^STATE|SAME|DIFF" | tail -2 | cut -c1-120
done
exit 1
