# Domain glossary

**Index log**: the append-only file of `SegmentMapping` records behind a writable LSMT layer
(the `.index` file of an upper layer). A mapping is published to the in-memory index only after
the log has accepted it, in log order, so replaying the log on reopen reproduces the index.
"Accepted" means written to the file, or held in the group-commit buffer that is flushed on
fsync, restack, close and close_seal. Implemented by `IndexLog` in `src/overlaybd/lsmt/file.cpp`;
it moves with the writable layer through `stack_files()` and `restack()`.
