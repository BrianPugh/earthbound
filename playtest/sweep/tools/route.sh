#!/bin/zsh
# route.sh DIR SESSION SAVE "X Y" ... : goto each target in turn
source /private/tmp/claude-501/-Users-brianpugh-projects-ebsrc/fb0daf9a-52d7-45c3-a277-26fd307b6f4d/scratchpad/swA/env.sh
D=$1; SS=$2; SV=$3; shift 3
for t in "$@"; do
  echo "== target $t"
  $S/goto.sh $D $SS $SV ${=t} 8 2>&1 | grep -E "try|AT|fight" | cut -c1-130
  pt do $SS state --no-shot | grep -m1 PARTY | cut -c1-130
done
