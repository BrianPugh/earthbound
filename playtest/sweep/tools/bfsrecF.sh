#!/bin/zsh
# bfsrec.sh DIR SESSION GX GY [STEP] [MAXN] -> prints the route; the snap frame is logged in its own chunk
source /private/tmp/claude-501/-Users-brianpugh-projects-ebsrc/fb0daf9a-52d7-45c3-a277-26fd307b6f4d/scratchpad/swA/env.sh
D=$1; SS=$2
n=$(ls $D | grep -cE '^[0-9]{3}\.pad$'); nxt=$(printf "%s/%03d.pad" $D $n)
pt do $SS "padlog $nxt; snap bfsF; padlog off" --no-shot >/dev/null
.venv/bin/python $S/swA/bfsfine.py $SS $3 $4 ${5:-24} ${6:-900} 2>&1 | tail -1 > $S/route_last.txt
pt do $SS "restore bfsF" --no-shot >/dev/null
sed 's/^FOUND ([0-9, ]*) //; s/^BEST ([0-9, ]*) //' $S/route_last.txt
