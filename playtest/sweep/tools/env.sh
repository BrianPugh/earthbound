S=/private/tmp/claude-501/-Users-brianpugh-projects-ebsrc/fb0daf9a-52d7-45c3-a277-26fd307b6f4d/scratchpad
cd /Users/brianpugh/projects/ebsrc
export EB_BIN=/Users/brianpugh/projects/ebsrc/port/unix/build-native/earthbound
rec(){ .venv/bin/python playtest/sweep/rec.py "$@"; }
go(){ .venv/bin/python playtest/sweep/go.py "$@"; }
pt(){ .venv/bin/python playtest/pt.py "$@"; }
