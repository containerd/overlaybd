/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
#include "../cli.h"
#include "../config_patch.h"
#include "../io_dispatch.h"

#include <gtest/gtest.h>
#include <ublk_cmd.h>

#include <cerrno>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// io_dispatch: in-memory UblkIOTarget covering the ImageFile seam
// ---------------------------------------------------------------------------

class MemTarget : public UblkIOTarget {
public:
    explicit MemTarget(size_t size) : data(size, 0) {
    }

    ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override {
        if (fail_read) {
            errno = EIO;
            return -1;
        }
        ssize_t done = 0;
        for (int i = 0; i < iovcnt; i++) {
            size_t n = iov[i].iov_len;
            if (short_read && n > 0)
                n -= 1; // simulate partial read
            memcpy(iov[i].iov_base, data.data() + offset + done, n);
            done += n;
            if (short_read)
                break;
        }
        return done;
    }

    ssize_t pwritev(const struct iovec *iov, int iovcnt, off_t offset) override {
        if (write_errno != 0) {
            errno = write_errno;
            return -1;
        }
        ssize_t done = 0;
        for (int i = 0; i < iovcnt; i++) {
            memcpy(data.data() + offset + done, iov[i].iov_base, iov[i].iov_len);
            done += iov[i].iov_len;
        }
        return done;
    }

    int fdatasync() override {
        sync_cnt++;
        return sync_ret;
    }

    int fallocate(int mode, off_t offset, off_t len) override {
        last_fallocate_mode = mode;
        if (fallocate_ret == 0)
            memset(data.data() + offset, 0, len); // punch hole => read zeros
        return fallocate_ret;
    }

    std::vector<char> data;
    bool fail_read = false;
    bool short_read = false;
    int write_errno = 0;
    int sync_ret = 0;
    int sync_cnt = 0;
    int fallocate_ret = 0;
    int last_fallocate_mode = -1;
};

TEST(io_dispatch, write_then_read_roundtrip) {
    MemTarget tgt(1 << 20);
    char wbuf[4096], rbuf[4096];
    memset(wbuf, 0xab, sizeof(wbuf));

    int ret = ublk_dispatch_io(&tgt, UBLK_IO_OP_WRITE, 8192, sizeof(wbuf), wbuf);
    EXPECT_EQ(ret, (int)sizeof(wbuf));

    ret = ublk_dispatch_io(&tgt, UBLK_IO_OP_READ, 8192, sizeof(rbuf), rbuf);
    EXPECT_EQ(ret, (int)sizeof(rbuf));
    EXPECT_EQ(memcmp(wbuf, rbuf, sizeof(rbuf)), 0);
}

TEST(io_dispatch, read_failure_maps_to_eio) {
    MemTarget tgt(1 << 20);
    char buf[512];
    tgt.fail_read = true;
    EXPECT_EQ(ublk_dispatch_io(&tgt, UBLK_IO_OP_READ, 0, sizeof(buf), buf), -EIO);
}

TEST(io_dispatch, partial_read_is_an_error) {
    MemTarget tgt(1 << 20);
    char buf[512];
    tgt.short_read = true;
    EXPECT_EQ(ublk_dispatch_io(&tgt, UBLK_IO_OP_READ, 0, sizeof(buf), buf), -EIO);
}

TEST(io_dispatch, write_erofs_is_propagated) {
    MemTarget tgt(1 << 20);
    char buf[512] = {};
    tgt.write_errno = EROFS;
    EXPECT_EQ(ublk_dispatch_io(&tgt, UBLK_IO_OP_WRITE, 0, sizeof(buf), buf), -EROFS);
    tgt.write_errno = ENOSPC; // any other write error becomes EIO
    EXPECT_EQ(ublk_dispatch_io(&tgt, UBLK_IO_OP_WRITE, 0, sizeof(buf), buf), -EIO);
}

TEST(io_dispatch, flush_maps_to_fdatasync) {
    MemTarget tgt(1 << 20);
    EXPECT_EQ(ublk_dispatch_io(&tgt, UBLK_IO_OP_FLUSH, 0, 0, nullptr), 0);
    EXPECT_EQ(tgt.sync_cnt, 1);
    tgt.sync_ret = -1;
    EXPECT_EQ(ublk_dispatch_io(&tgt, UBLK_IO_OP_FLUSH, 0, 0, nullptr), -EIO);
}

TEST(io_dispatch, discard_and_write_zeroes_punch_hole) {
    MemTarget tgt(1 << 20);
    char wbuf[4096], rbuf[4096], zero[4096] = {};
    memset(wbuf, 0xcd, sizeof(wbuf));
    ublk_dispatch_io(&tgt, UBLK_IO_OP_WRITE, 0, sizeof(wbuf), wbuf);

    EXPECT_EQ(ublk_dispatch_io(&tgt, UBLK_IO_OP_DISCARD, 0, sizeof(wbuf), nullptr), 0);
    // fallocate(3) = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, as in tcmu
    EXPECT_EQ(tgt.last_fallocate_mode, 3);
    ublk_dispatch_io(&tgt, UBLK_IO_OP_READ, 0, sizeof(rbuf), rbuf);
    EXPECT_EQ(memcmp(rbuf, zero, sizeof(rbuf)), 0);

    tgt.last_fallocate_mode = -1;
    EXPECT_EQ(ublk_dispatch_io(&tgt, UBLK_IO_OP_WRITE_ZEROES, 0, 4096, nullptr), 0);
    EXPECT_EQ(tgt.last_fallocate_mode, 3);

    tgt.fallocate_ret = -1;
    EXPECT_EQ(ublk_dispatch_io(&tgt, UBLK_IO_OP_DISCARD, 0, 4096, nullptr), -EIO);
}

TEST(io_dispatch, unknown_op_is_rejected) {
    MemTarget tgt(4096);
    EXPECT_EQ(ublk_dispatch_io(&tgt, UBLK_IO_OP_ZONE_APPEND, 0, 0, nullptr), -ENOTSUP);
}

// ---------------------------------------------------------------------------
// cli parsing
// ---------------------------------------------------------------------------

static int parse(std::vector<const char *> args, UblkCliCmd &cmd) {
    args.insert(args.begin(), "overlaybd-ublk");
    return ublk_parse_cli((int)args.size(), (char **)args.data(), cmd);
}

TEST(cli, add_requires_existing_config) {
    UblkCliCmd cmd;
    EXPECT_NE(parse({"add", "--config", "/nonexistent/config.v1.json"}, cmd), 0);
    EXPECT_EQ(cmd.kind, UblkCliCmd::Kind::NONE);
}

TEST(cli, add_parses_options) {
    UblkCliCmd cmd;
    // use this test binary itself as an always-existing file path
    char self[] = "/proc/self/exe";
    ASSERT_EQ(parse({"add", "--config", self, "-n", "3", "--depth", "64", "--foreground"},
                    cmd),
              0);
    EXPECT_EQ(cmd.kind, UblkCliCmd::Kind::ADD);
    EXPECT_EQ(cmd.opts.dev_id, 3);
    EXPECT_EQ(cmd.opts.queue_depth, 64);
    EXPECT_TRUE(cmd.foreground);
    EXPECT_EQ(cmd.opts.image_config_path, self);
}

TEST(cli, add_defaults) {
    UblkCliCmd cmd;
    char self[] = "/proc/self/exe";
    ASSERT_EQ(parse({"add", "--config", self}, cmd), 0);
    EXPECT_EQ(cmd.opts.dev_id, -1);       // kernel allocates
    EXPECT_EQ(cmd.opts.queue_depth, 128); // documented default
    EXPECT_FALSE(cmd.foreground);
    EXPECT_TRUE(cmd.opts.service_config_path.empty());
    EXPECT_TRUE(cmd.opts.log_path.empty()); // default: shared log
}

TEST(cli, add_log_path) {
    UblkCliCmd cmd;
    char self[] = "/proc/self/exe";
    ASSERT_EQ(parse({"add", "--config", self, "--log-path", "/var/log/obd-ublk0.log"},
                    cmd),
              0);
    EXPECT_EQ(cmd.opts.log_path, "/var/log/obd-ublk0.log");
}

TEST(cli, del_requires_dev_id) {
    UblkCliCmd cmd;
    EXPECT_NE(parse({"del"}, cmd), 0);
    EXPECT_EQ(cmd.kind, UblkCliCmd::Kind::NONE);

    UblkCliCmd cmd2;
    ASSERT_EQ(parse({"del", "-n", "5"}, cmd2), 0);
    EXPECT_EQ(cmd2.kind, UblkCliCmd::Kind::DEL);
    EXPECT_EQ(cmd2.del_dev_id, 5);
}

TEST(cli, list_and_missing_subcommand) {
    UblkCliCmd cmd;
    ASSERT_EQ(parse({"list"}, cmd), 0);
    EXPECT_EQ(cmd.kind, UblkCliCmd::Kind::LIST);

    UblkCliCmd cmd2;
    EXPECT_NE(parse({}, cmd2), 0); // subcommand is mandatory
    EXPECT_EQ(cmd2.kind, UblkCliCmd::Kind::NONE);
}

// ---------------------------------------------------------------------------
// config_patch: per-device cache dir rewriting (--cache-dir)
// ---------------------------------------------------------------------------

TEST(config_patch, rewrites_all_cache_dirs) {
    std::string base = R"({
        "logConfig": { "logLevel": 1 },
        "cacheConfig": { "cacheType": "file", "cacheDir": "/opt/overlaybd/registry_cache" },
        "gzipCacheConfig": { "enable": true, "cacheDir": "/opt/overlaybd/gzip_cache" }
    })";
    std::string out;
    ASSERT_EQ(ublk_patch_cache_dirs(base, "/var/cache/obd-dev0", out), 0);
    EXPECT_NE(out.find("/var/cache/obd-dev0/registry_cache"), std::string::npos);
    EXPECT_NE(out.find("/var/cache/obd-dev0/gzip_cache"), std::string::npos);
    EXPECT_EQ(out.find("/opt/overlaybd/registry_cache"), std::string::npos);
    EXPECT_EQ(out.find("/opt/overlaybd/gzip_cache"), std::string::npos);
    // unrelated fields survive the rewrite
    EXPECT_NE(out.find("\"cacheType\":\"file\""), std::string::npos);
    EXPECT_NE(out.find("\"logLevel\":1"), std::string::npos);
}

TEST(config_patch, legacy_config_without_cache_sections) {
    std::string out;
    ASSERT_EQ(ublk_patch_cache_dirs("{}", "/tmp/c", out), 0);
    // both the legacy field and the new-style section are covered
    EXPECT_NE(out.find("\"registryCacheDir\":\"/tmp/c/registry_cache\""),
              std::string::npos);
    EXPECT_NE(out.find("\"cacheConfig\""), std::string::npos);
    EXPECT_EQ(out.find("gzipCacheConfig"), std::string::npos); // section not invented
}

TEST(config_patch, rejects_invalid_json) {
    std::string out;
    EXPECT_NE(ublk_patch_cache_dirs("not json", "/tmp/c", out), 0);
    EXPECT_NE(ublk_patch_cache_dirs("[1,2]", "/tmp/c", out), 0); // array, not object
}

