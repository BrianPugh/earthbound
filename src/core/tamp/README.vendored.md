Vendored from tamp v1.11.1 (https://github.com/BrianPugh/tamp), Apache-2.0.
Files: common.{c,h}, compressor.{c,h}, decompressor.{c,h},
compressor_find_match_desktop.c (the last is #included by compressor.c on
64-bit desktop builds; on arm-none-eabi the in-tree embedded match finder is
used). Do not edit in place; re-vendor from upstream to update.
