# overlaybd Data Layout Optimization (EXT4)

[简体中文](apply-data-layout_zh.md)

`overlaybd-apply` converts an OCI tar layer into an overlaybd layer via **e2fsprogs**. Rearranging where the file data payload and the ext4 metadata land inside the layer substantially improves the locality of metadata access.

## Opt in

The compact layout is disabled by default. Use `--compact-layout` (or
`--compact-layout=true`) to enable it for a plain Append writable layer:

```bash
overlaybd-create layer.data layer.index 64
overlaybd-apply --mkfs --compact-layout layer.tar image.json
overlaybd-commit --seal layer.data layer.index
```

Here `image.json` points its `upper.data` and `upper.index` to the created files.
Omitting the flag or setting `--compact-layout=false` keeps the original write
path. The flag applies only to that command and is not saved in the layer.
Sparse, Hybrid, raw, TurboOCI and qcow2 conversion do not support this option;
explicitly enabling it for those modes fails before filesystem writes.

`overlaybd-create --mkfs --compact-layout layer.data layer.index 64` also merges
repeated mkfs writes and appends only the final metadata blocks. This requires
`--mkfs`; it produces a usable empty ext4 filesystem. To compact a later apply,
pass `--compact-layout` to that command too.

Both commands use temporary sparse files in `$TMPDIR`, or `/tmp` if it is unset
or empty. Reserve scratch space for written metadata and, for apply, tar headers.
The temporary files are unlinked immediately and removed on close.

## New layout

For a fresh layer formatted and populated in one apply, the sealed layout is:

```text
old: | Header | data | fsmeta | data | fsmeta copy | data | ... | index | Trailer |

new: | Header 4 KiB | payload: 4 KiB aligned per file | fsmeta: live metadata | index | Trailer 4 KiB |
                     [4096, payload_end)                [payload_end, index_offset)
```

- **payload**: the data of every regular file, staged in tar order; each non-empty file starts at a 4 KiB aligned offset and occupies a 4 KiB aligned length with the tail zeroed. Empty files take no space.
- **fsmeta**: the metadata blocks that are live at the end, appended compactly in ascending logical order, with no duplicate copies.
- **index / Trailer**: appended in place by `overlaybd-commit --seal`; the data is never rearranged.

The result is still a standard sealed LSMT — no new format version, no new tag, no external payload file.

Existing bytes are not rearranged. For example, formatting with create and then
applying to the same writable layer leaves
`Header | mkfs metadata | new payload | new metadata`. Repeated applies can also
leave earlier prefixes; compact layout orders only the bytes added by each
operation. Use a fresh upper layer to obtain the complete layout shown above.

## How it works

```text
tar stream   → append payload sequentially (aligned per file, no mapping yet) + temp tar headers (recording payload offsets)
temp headers → ext4 replays metadata only → FsMetaFile (sparse file, original logical offsets)
                     └─ regular files: fallocate + FIEMAP → pending ext4-block-to-payload mappings
close ext4   → drop payload blocks → append live fsmeta compactly → install full-block payload mappings
commit --seal → merge adjacent mappings, append index and Trailer in place
```

`CompactConversion::apply()` owns this lifecycle, including scratch files, the
aligned image view and ext4 close. Its input-finalization callback runs after
payload staging and before opening or formatting ext4, so checksum failures do
not publish payload mappings. `CompactConversion::mkfs()` reuses the same metadata
staging lifetime for create. The command-line tools prepare input and choose the
mode; they do not select the physical upper file or supply an ext4-close callback.

## Measured results

Converted 10 SWE-benchmark images into overlaybd images with a 64GB block size, 182 layers in total. After mounting, `du`/`find` were run to collect and compare the distribution of accessed ranges between the old and the new layout:

| Metric (du+find, MB is decimal) | Old | New | Change |
|---|---:|---:|---:|
| LSMT requested MB | 1949.475 | 1948.631 | nearly unchanged |
| LSMT bytes in the second half | 63.20% | 99.92% | more concentrated |
| LSMT position p5–p95 span | 90.82% | 6.38% | more concentrated |
| LSMT 1 MiB blocks touched | 12851 | 1276 | −90.07% |
| ZFile local requested MB | 110.352 | 103.290 | −6.40% |
| ZFile unique 4 KiB coverage MB | 436.945 | 56.717 | −87.02% |
| ZFile unique 1 MiB coverage MB | 6299.222 | 271.553 | −95.69% |
| Sealed total MB | 44574.161 | 32020.957 | −28.16% |
| ZFile total MB | 17818.462 | 17192.135 | −3.52% |

- **Access is concentrated**: in the new layout 99.95% of the R requested bytes fall in the fsmeta region, which is only 5.80% of the data area; in compressed space the range corresponding to fsmeta accounts for 99.45% of the bytes read.
- **Coverage shrinks sharply at coarser fetch granularities**: requested bytes drop by only 6.40%, while unique coverage aligned to 4 KiB / 1 MiB drops by 87.02% / 95.69%.

## Related code

| File | Contents |
|---|---|
| `src/tools/overlaybd-apply.cpp` | command-line options and input preparation |
| `src/overlaybd/tar/compact_conversion.{h,cpp}` | scratch ownership, ext4 lifecycle, compact mkfs and complete tar conversion |
| `src/overlaybd/tar/libtar.cpp` | TarCore::dump_tar_headers, extract_staged, apply_pending; per-file alignment and zeroed tails |
| `src/overlaybd/tar/fsmeta_file.{h,cpp}` | sparse fsmeta staging, compact append of live blocks |
| `src/overlaybd/lsmt/file.cpp` | StagedData mappings, close_seal and index merging |
