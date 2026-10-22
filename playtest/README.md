# Scripted playtests

Headless, deterministic playtests of the desktop port driven by an input
script (`--input FILE`, see `port/unix/platform/sdl2_input_script.c` for the
command reference).

```
port/unix/build/earthbound --headless --save playtest/saves/24-Museum.srm --input my.play
uv run --no-sync python playtest/run.py NAME playtest/saves/24-Museum.srm playtest/scripts/smoke.play
```

`run.py` boots the save (file 1 -> Continue), plays the script and writes
`playtest/out/NAME/{log.txt,sheet.png}`: the `STATE` lines and a labelled
contact sheet of every `shot`.

## Story saves

`playtest/saves/` is gitignored. It holds 36 emulator SRAM files (8 KB `.srm`,
loaded directly by `--save`) covering the whole story, from
<https://fantasyanime.com/legacy/earthb_saves.htm>; `saves/INDEX` lists each
save's story point. To recreate it, download the zips from that page and copy
each `earthbound.srm` to `playtest/saves/NN-Name.srm`. The last zip ("End")
holds only ZSNES savestates and is skipped.
