# overlaybd 数据布局优化(EXT4)

[English](apply-data-layout.md)

`overlaybd-apply` 通过 **e2fsprogs** 把 OCI tar 层转换为 overlaybd 层, 通过调整文件数据 payload 和 ext4 元数据在layer中的布局，可有效改善元数据访问的局部性。

## 新布局

```text
旧：| Header | 数据 | fsmeta | 数据 | fsmeta 副本 | 数据 | ... | index | Trailer |

新：| Header 4 KiB | payload：逐文件 4 KiB 对齐 | fsmeta：有效元数据 | index | Trailer 4 KiB |
                   [4096, payload_end)          [payload_end, index_offset)
```

- **payload**：按 tar 顺序暂存所有普通文件的数据；每个非空文件起点和占用长度均按 4 KiB 对齐，尾部补零，空文件不占空间。
- **fsmeta**：最终有效的元数据块，按逻辑地址升序紧凑追加，无重复副本。
- **index / Trailer**：`overlaybd-commit --seal` 原地追加，数据不重排。

结果仍是标准 sealed LSMT，没有新格式版本、tag 或外部 payload 文件。

## 基本原理

```text
tar 流 → 顺序追加 payload（逐文件对齐，不建映射）+ 临时 tar 头部（记录 payload 偏移）
临时头部 → ext4 只回放元数据 → FsMetaFile（稀疏文件，原逻辑偏移）
                └─ 普通文件：fallocate + FIEMAP → 待安装 ext4 块到 payload 的映射
关闭 ext4 → 剔除 payload 块 → 有效 fsmeta 紧凑追加 → 安装完整块 payload 映射
commit --seal → 合并相邻映射，原地追加 index 和 Trailer
```

## 收益实测

将 10 个 SWE评测镜像转换成大小为块大小为 64GB 的 overlaybd 镜像，共 182 层。挂载后跑 du/find，统计并对比新旧布局的访问区间分布：

| 指标（du+find，MB 为十进制） | 旧版 | 新版 | 变化 |
|---|---:|---:|---:|
| LSMT 请求 MB | 1949.475 | 1948.631 | 近乎不变 |
| LSMT 后半段字节占比 | 63.20% | 99.92% | 更集中 |
| LSMT 位置 p5–p95 跨度 | 90.82% | 6.38% | 更集中 |
| LSMT 触及 1 MiB 块 | 12851 | 1276 | −90.07% |
| ZFile 本地请求 MB | 110.352 | 103.290 | −6.40% |
| ZFile 唯一 4 KiB 覆盖 MB | 436.945 | 56.717 | −87.02% |
| ZFile 唯一 1 MiB 覆盖 MB | 6299.222 | 271.553 | −95.69% |
| sealed 合计 MB | 44574.161 | 32020.957 | −28.16% |
| ZFile 合计 MB | 17818.462 | 17192.135 | −3.52% |

- **访问集中**：新版 99.95% 的 R 请求字节落在 fsmeta 区，该区只占数据区 5.80%；压缩空间中 fsmeta 对应区间占读取字节 99.45%。
- **较粗回填粒度的覆盖明显减少**：请求字节仅下降 6.40%，对齐 4 KiB/1 MiB 的唯一覆盖分别下降 87.02%/95.69%。

## 相关代码

| 文件 | 内容 |
|---|---|
| `src/tools/overlaybd-apply.cpp` | 流程选择与编排 |
| `src/overlaybd/tar/libtar.cpp` | stage_tar、extract_staged、apply_pending；逐文件对齐与零尾 |
| `src/overlaybd/tar/fsmeta_file.{h,cpp}` | 稀疏 fsmeta 暂存、有效块紧凑追加 |
| `src/overlaybd/lsmt/file.cpp` | staged RemoteData 映射、close_seal 与索引合并 |
