#!/bin/zsh
# bfsrec.sh DIR SESSION GX GY [STEP] [MAXN] -> prints the route; the snap frame is logged in its own chunk
source ${0:A:h}/env.sh
D=$1; SS=$2
n=$(ls $D | grep -cE '^[0-9]{3}\.pad$'); nxt=$(printf "%s/%03d.pad" $D $n)
pt do $SS "padlog $nxt; snap bfs0; padlog off" --no-shot >/dev/null
.venv/bin/python $S/bfs.py $SS $3 $4 ${5:-24} ${6:-900} 2>&1 | tail -1 > $W/route_last.txt
pt do $SS "restore bfs0" --no-shot >/dev/null
sed 's/^FOUND ([0-9, ]*) //; s/^BEST ([0-9, ]*) //' $W/route_last.txt
