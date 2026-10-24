#!/usr/bin/env python3
# Copyright The Overlaybd Authors
# SPDX-License-Identifier: Apache-2.0

"""Exercise compact-layout CLI behavior without mounts or external fixtures."""

import argparse
import errno
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile


class SmokeTest:
    def __init__(self, tools, workdir):
        self.tools = tools
        self.workdir = workdir
        self.commands = 0
        self.payload = b"overlaybd compact-layout smoke\n" + bytes(range(256)) * 17
        self.archive = workdir / "layer.tar"
        with tarfile.open(self.archive, "w", format=tarfile.PAX_FORMAT) as archive:
            directory = tarfile.TarInfo("data")
            directory.type = tarfile.DIRTYPE
            directory.mode = 0o755
            archive.addfile(directory)
            for name, data in (("data/file", self.payload), ("data/empty", b"")):
                entry = tarfile.TarInfo(name)
                entry.mode = 0o644
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
        # Finalizing the checksum must drain padding past tar's end markers,
        # including more than one SHA256CheckedFile trailing-data read.
        with self.archive.open("ab") as archive:
            archive.write(bytes(128 * 1024))
        self.checksum = "sha256:" + hashlib.sha256(self.archive.read_bytes()).hexdigest()

        self.service = workdir / "service.json"
        credentials = workdir / "credentials.json"
        credentials.write_text(json.dumps({"auths": {}}))
        self.service.write_text(json.dumps({
            "ioEngine": 0,
            "workpoolSize": 1,
            "logConfig": {"logPath": str(workdir / "service.log"), "logLevel": 2},
            "enableAudit": False,
            "cacheConfig": {"cacheType": "download", "cacheDir": str(workdir / "cache")},
            "credentialConfig": {"mode": "file", "path": str(credentials)},
            "gzipCacheConfig": {"enable": False},
            "download": {"enable": False},
            "p2pConfig": {"enable": False},
            "exporterConfig": {"enable": False},
            "serviceConfig": {"enable": False},
        }))
        self.scratch = workdir / "scratch"
        self.scratch.mkdir()
        self.environment = dict(os.environ, TMPDIR=str(self.scratch))
        self.no_scratch = dict(self.environment, TMPDIR=str(workdir / "missing-scratch"))

    def run(self, tool, arguments, *, succeeds=True, environment=None):
        command = [self.tools[tool], *map(str, arguments)]
        self.commands += 1
        result = subprocess.run(command, cwd=self.workdir,
                                env=environment or self.environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=60)
        if result.returncode < 0 or (result.returncode == 0) != succeeds:
            raise AssertionError(
                "Unexpected exit status {} for {}\n{}".format(
                    result.returncode, shlex.join(command), result.stdout))
        return result

    def paths(self, name, *, target=None):
        directory = self.workdir / name
        directory.mkdir()
        data = directory / "data"
        index = directory / "index"
        config = directory / "image.json"
        upper = {"data": str(data), "index": str(index)}
        if target is not None:
            upper["target"] = str(target)
        config.write_text(json.dumps({"lowers": [], "upper": upper}))
        return data, index, config

    def apply(self, config, flags=(), **kwargs):
        return self.run("apply", ["--service_config_path", self.service,
                                  *flags, self.archive, config], **kwargs)

    def seal(self, data, index):
        self.run("commit", ["--seal", data, index])

    @staticmethod
    def snapshot(path):
        """Hash allocated ranges so a 1 GiB sparse layer stays a cheap check."""
        size = path.stat().st_size
        digest = hashlib.sha256()
        with path.open("rb", buffering=0) as file:
            position = 0
            while position < size:
                try:
                    start = os.lseek(file.fileno(), position, os.SEEK_DATA)
                    end = min(os.lseek(file.fileno(), start, os.SEEK_HOLE), size)
                except OSError as error:
                    if error.errno == errno.ENXIO:
                        break
                    if error.errno not in (errno.EINVAL, errno.ENOTSUP):
                        raise
                    start, end = position, size
                digest.update(start.to_bytes(8, "little"))
                digest.update(end.to_bytes(8, "little"))
                file.seek(start)
                while file.tell() < end:
                    chunk = file.read(min(1 << 20, end - file.tell()))
                    if not chunk:
                        raise AssertionError("Unexpected EOF while hashing " + str(path))
                    digest.update(chunk)
                position = end
        return size, digest.digest()

    def expect_unchanged(self, paths, action):
        before = [self.snapshot(path) for path in paths]
        action()
        after = [self.snapshot(path) for path in paths]
        if before != after:
            raise AssertionError("Rejected command changed layer files: " + str(paths))

    def legacy_defaults(self):
        # An unavailable TMPDIR proves neither command enters the staging path.
        for name, flags in (("default", []), ("false", ["--compact-layout=false"])):
            data, index, config = self.paths("legacy-" + name)
            self.run("create", ["--mkfs", *flags, data, index, "1"],
                     environment=self.no_scratch)
            self.apply(config, [*flags, "--checksum", self.checksum],
                       environment=self.no_scratch)
            with data.open("rb") as file:
                file.seek(4096)
                if file.read(len(self.payload)) == self.payload:
                    raise AssertionError("Legacy apply unexpectedly staged the payload first")
            self.seal(data, index)

    def compact_apply(self):
        for name, flags in (("bare", ["--compact-layout"]),
                            ("true", ["--compact-layout=true"])):
            data, index, config = self.paths("compact-apply-" + name)
            self.run("create", [data, index, "1"])
            self.apply(config, ["--mkfs", *flags, "--checksum", self.checksum])
            with data.open("rb") as file:
                file.seek(4096)
                if file.read(len(self.payload)) != self.payload:
                    raise AssertionError("Compact apply did not put payload before metadata")
            self.seal(data, index)

    def checksum_mismatch(self):
        data, index, config = self.paths("checksum-mismatch")
        self.run("create", [data, index, "1"])

        def reject_checksum():
            result = self.apply(config, ["--mkfs", "--compact-layout", "--checksum",
                                         "sha256:" + "0" * 64], succeeds=False)
            if "sha256 checksum mismatch" not in result.stdout:
                raise AssertionError("Expected checksum rejection:\n" + result.stdout)

        # Staging may append unmapped payload bytes, but finalization must reject
        # the input before formatting or publishing any image mappings.
        self.expect_unchanged([index, config], reject_checksum)
        with data.open("rb") as file:
            file.seek(4096)
            if file.read(len(self.payload)) != self.payload:
                raise AssertionError("Checksum validation ran before staging the payload")

    def compact_mkfs(self):
        for name, create_flags, apply_flags in (
                ("bare", ["--compact-layout"], ["--compact-layout=true"]),
                ("true", ["--compact-layout=true"], ["--compact-layout"])):
            data, index, config = self.paths("compact-mkfs-" + name)
            self.run("create", ["--mkfs", *create_flags, data, index, "1"])
            payload_offset = data.stat().st_size
            if payload_offset <= 4096 or payload_offset % 4096:
                raise AssertionError("Compact mkfs metadata was not block aligned")
            # Import without --mkfs must reopen the formatted image correctly.
            self.apply(config, apply_flags)
            with data.open("rb") as file:
                file.seek(payload_offset)
                if file.read(len(self.payload)) != self.payload:
                    raise AssertionError("Compact apply did not preserve the mkfs prefix")
            self.seal(data, index)

    def rejected_create_options(self):
        for number, flag in enumerate(("-s", "--hybrid", "--raw", "--turboOCI", "--fastoci")):
            data, index, _ = self.paths("rejected-create-" + str(number))
            self.run("create", ["--mkfs", "--compact-layout=true", flag, data, index, "1"],
                     succeeds=False)
            if data.exists() or index.exists():
                raise AssertionError("Invalid create options created output files")

            # The same layer types remain available when the new flag is false.
            self.run("create", ["--compact-layout=false", flag, data, index, "1"])

        for name, flags in (("no-mkfs", ["--compact-layout"]),
                            ("bad-bool", ["--mkfs", "--compact-layout=invalid"])):
            data, index, _ = self.paths("rejected-create-" + name)
            self.run("create", [*flags, data, index, "1"], succeeds=False)
            if data.exists() or index.exists():
                raise AssertionError("Invalid create options created output files")

    def rejected_apply_options(self):
        data, index, config = self.paths("rejected-apply-static")
        self.run("create", [data, index, "1"])
        for flags in (("--raw",), ("--from_qcow2",),
                      ("--gz_index_path", self.workdir / "should-not-exist.gzip-index")):
            self.expect_unchanged([data, index, config], lambda: self.apply(
                config, ["--compact-layout", *flags], succeeds=False))
        if (self.workdir / "should-not-exist.gzip-index").exists():
            raise AssertionError("Invalid apply options created a gzip index")
        self.expect_unchanged([data, index], lambda: self.apply(
            config, ["--compact-layout=invalid"], succeeds=False))

        for name, flag in (("sparse", "-s"), ("hybrid", "--hybrid"),
                           ("warp", "--turboOCI")):
            data, index, config = self.paths(
                "rejected-apply-" + name, target=self.archive if name == "warp" else None)
            self.run("create", [flag, data, index, "1"])
            self.expect_unchanged([data, index, self.archive], lambda: self.apply(
                config, ["--mkfs", "--compact-layout=true"], succeeds=False))
            if name != "warp":
                # The legacy path can still format and apply to these layer types.
                self.apply(config, ["--mkfs", "--compact-layout=false"],
                           environment=self.no_scratch)

    def test(self):
        self.legacy_defaults()
        self.compact_apply()
        self.checksum_mismatch()
        self.compact_mkfs()
        self.rejected_create_options()
        self.rejected_apply_options()
        print("compact-layout CLI smoke passed ({} commands)".format(self.commands))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("create", "apply", "commit"):
        parser.add_argument("--" + name, required=True, type=Path)
    arguments = parser.parse_args()
    tools = {name: str(getattr(arguments, name).resolve())
             for name in ("create", "apply", "commit")}
    with tempfile.TemporaryDirectory(prefix="overlaybd-compact-layout-") as directory:
        SmokeTest(tools, Path(directory)).test()


if __name__ == "__main__":
    main()
