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
#pragma once

#include <vector>
#include <iostream>
#include <gtest/gtest.h>
#include <gflags/gflags.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define BENCHMARK true

extern uint32_t zfile_io_cnt;
extern uint64_t zfile_io_size;
extern uint64_t zfile_blk_cnt;
extern bool io_test;

#include <photon/common/utility.h>
#include <photon/fs/localfs.h>
#include "../index.cpp"
#include "../file.cpp"
#include "../../zfile/zfile.h"
#include <photon/thread/thread.h>
#include <photon/thread/thread11.h>
#include <photon/common/uuid.h>

using namespace std;
using namespace LSMT;
using namespace photon::fs;
using namespace ZFile;

const static char fndata[] = "fdata.lsmt";
const static char fnindex[] = "findex.lsmt";
const static char fnnew[] = "fnnew.lsmt";

#define PREAD_LEN (1 << 20)
ALIGNED_MEM4K(buf, PREAD_LEN);

#define LEN(x) (size_t)(sizeof(x) / sizeof(x[0]))

vector<string> args{};

DEFINE_string(io_engine, "psync", "set io engine, psync|libaio|posixaio");
DEFINE_int32(threads, 1, "number of photon threads (test in FileTest3.photon_verify)");
DEFINE_int32(nwrites, 4096, "write times in each layer.");
DEFINE_int32(layers, 3, "image layers.");
DEFINE_uint64(vsize, 64, "image virtual size. (MB)");
DEFINE_bool(verify, true, "create verify file.");
DEFINE_int32(log_level, 1, "alog level.");

class FileTest : public ::testing::Test {
public:
    IFileSystem *lfs = nullptr;
    vector<string> data_name, idx_name, layer_name;
    char layer_data[128]{}, layer_index[128]{}, layer_gc[128]{}, layer[128]{};
    uint64_t vsize = FLAGS_vsize << 20;
    uint32_t IMAGE_RO_LAYERS = FLAGS_layers;

    int ut_io_engine = 0;
    int next_layer_id = 0;
    int current_layer_id = 0;
    string parent_uuid;

    virtual void SetUp() override {

        // io_engine = ioengine_psync;
        if (FLAGS_io_engine == "libaio") {
            ut_io_engine = ioengine_libaio;
        }
        if (FLAGS_io_engine == "posixaio") {
            ut_io_engine = ioengine_posixaio;
        }
        LOG_INFO("create localfs_adaptor (io_engine = `).", ut_io_engine);

        lfs = new_localfs_adaptor("/tmp", ut_io_engine);

        memset(buf, 0, PREAD_LEN);
    }

    FileTest() {
    }
    ~FileTest() {
        delete lfs;
    }

    void snprintf_names(int i) {
        memset(layer_data, 0, sizeof(layer_data));
        snprintf(layer_data, sizeof(layer_data), "data%d.lsmt", i);

        memset(layer_index, 0, sizeof(layer_index));
        snprintf(layer_index, sizeof(layer_index), "index%d.lsmt", i);

        memset(layer, 0, sizeof(layer));
        snprintf(layer, sizeof(layer), "layer%d.lsmt", i);

        data_name.push_back(layer_data);
        idx_name.push_back(layer_index);
        layer_name.push_back(layer);
        current_layer_id = i;
    }

    void name_next_layer() {

        snprintf_names(next_layer_id++);
    }

    IFileRW *create_file_rw(bool sparse = false) {
        name_next_layer();
        auto fdata = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        // auto fdata = photon::fs::open_localfile_adaptor(data_name.back().c_str(), O_RDWR |
        // O_CREAT | O_TRUNC, S_IRWXU);
        auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        LOG_DEBUG("open_file: ` `", data_name.back().c_str(), idx_name.back().c_str());
        LayerInfo _;
        LOG_INFO("TEST: now create a rw layer with invalid args.. expected ret: nullptr");
        EXPECT_EQ(nullptr, ::create_file_rw(_, true));
        LOG_INFO("TEST OK");
        LayerInfo args(fdata, findex);
        args.sparse_rw = sparse;
        if (parent_uuid != "")
            args.parent_uuid.parse(parent_uuid.c_str(), parent_uuid.size());
        args.virtual_size = vsize;
        auto file = ::create_file_rw(args, true);
        return file;
    }
    IFileRW *open_file_rw() {
        auto fdata = lfs->open(data_name.back().c_str(), O_RDWR | O_APPEND, S_IRWXU);
        auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_APPEND, S_IRWXU);
        EXPECT_EQ(LSMT::open_file_rw(nullptr, findex), nullptr);
        EXPECT_EQ(LSMT::open_file_rw(nullptr, nullptr), nullptr);
        auto file = LSMT::open_file_rw(fdata, findex, true);
        return file;
    }
    IFileRO *open_file_ro(const char *fn = fndata) {
        LOG_INFO("TEST: now open a ro layer with invalid args.. expected ret: nullptr");
        EXPECT_EQ(LSMT::open_file_ro(nullptr), nullptr);
        LOG_INFO("TEST OK");
        auto fdata = lfs->open(fn, O_RDONLY);
        auto file = LSMT::open_file_ro(fdata, true);
        return file;
    }
    virtual void TearDown() override {

        LOG_DEBUG("next_layer_id: `", next_layer_id);
        if (lfs->access(fndata, 0) == 0)
            lfs->unlink(fndata);
        if (lfs->access(fnindex, 0) == 0)
            lfs->unlink(fnindex);
        if (lfs->access(fnnew, 0) == 0)
            lfs->unlink(fnnew);
        for (size_t i = 0; i < layer_name.size(); i++) {
            if (i < layer_name.size() && lfs->access(layer_name[i].c_str(), 0) == 0)
                lfs->unlink(layer_name[i].c_str());
            if (i < data_name.size() && lfs->access(data_name[i].c_str(), 0) == 0)
                lfs->unlink(data_name[i].c_str());
            if (i < idx_name.size() && lfs->access(idx_name[i].c_str(), 0) == 0)
                lfs->unlink(idx_name[i].c_str());
        }
    }
};

class FileTest2 : public FileTest {
public:
    const char *fn_verify = "verify.img";
    IFile *fcheck = nullptr;
    // char *data = nullptr;
    photon::mutex mtx;
    vector<size_t> clayer_size{};

#define DO_ALIGN(x) ((x) / ALIGNMENT * ALIGNMENT)

    size_t gen_write_data(char *buf, size_t vsize, size_t len, off_t &offset) {
        offset = DO_ALIGN(rand() % vsize);
        off_t length = DO_ALIGN(rand() % len);
        if ((uint64_t)(offset + length) > vsize)
            length = DO_ALIGN(vsize - offset);
        if (length == 0) {
            offset -= ALIGNMENT;
            length = ALIGNMENT;
            // return 0;
        }
        memset(buf, 0, length);
        for (auto j = 0; j < length; j++) {
            auto k = rand() % 256;
            buf[j] = k; // j  & 0xff;
        }
        return length;
    }

    void randwrite(LSMT::IFileRW *file, size_t nwrites) {
        timeval start_time, end_time;
        gettimeofday(&start_time, 0);
        printf("randwrite( %lu times ) ", nwrites);
        ALIGNED_MEM4K(buf, 1 << 20)
        struct iovec iov[8]{};
        for (size_t i = 0; i < nwrites; ++i) {
            off_t offset = 0;
            off_t length = gen_write_data(buf, vsize, (128 * 1024), offset);
            auto roll = rand() % 4;
            if (roll != 0) {
                LOG_DEBUG("offset: `, length: `", offset, length);
                if (FLAGS_verify) {
                    // memcpy(data + offset, buf, length);
                    fcheck->pwrite(buf, length, offset);
                }
                auto slice_count = rand() % 4 + 1;
                vector<int> seg_offset{0};
                for (int i = 0; i < slice_count - 1; i++) {
                    seg_offset.push_back(rand() % length);
                }
                seg_offset.push_back(length);
                for (auto i = 0; i < slice_count; i++) {
                    iov[i].iov_base = &buf[seg_offset[i]];
                    iov[i].iov_len = seg_offset[i + 1] - seg_offset[i];
                }
                ssize_t ret = file->pwritev(iov, slice_count, offset);
                if (ret != length) {
                    LOG_ERROR("`(`)", errno, strerror(errno));
                    exit(-1);
                }
                EXPECT_EQ(ret, length);
            } else {
                file->fallocate(3, offset, length);
                if (FLAGS_verify) {
                    // memcpy(data + offset, buf, length);
                    fcheck->fallocate(3, offset, length);
                }
            }
        }
        gettimeofday(&end_time, 0);
        auto elapsed_time = end_time.tv_sec * 1000000 + end_time.tv_usec -
                            start_time.tv_sec * 1000000 - start_time.tv_usec;
        printf("time cost: %ldms\n", elapsed_time / 1000);
    }

    void randwrite1(LSMT::IFileRW *file0, LSMT::IFileRW *file1, size_t nwrites) {
        timeval start_time, end_time;
        gettimeofday(&start_time, 0);
        printf("randwrite( %lu times ) in two files ", nwrites);
        // char buf[1<<20]{};
        ALIGNED_MEM4K(buf, 1 << 20)
        struct iovec iov[8]{};
        for (size_t i = 0; i < nwrites; ++i) {
            // off_t offset = 0;
            // off_t length =  gen_write_data(buf, vsize, (128 * 1024), offset);
            // LOG_DEBUG("offset: `, length: `", offset, length);
            off_t offset = DO_ALIGN(rand() % vsize);
            off_t length = DO_ALIGN(rand() % (128 * 1024));
            if ((uint64_t)(offset + length) > vsize)
                length = DO_ALIGN(vsize - offset);
            if (length == 0) {
                offset -= ALIGNMENT;
                length = ALIGNMENT;
                continue;
            }
            memset(buf, 0, length);
            for (auto j = 0; j < length; j++) {
                auto k = rand() % 256;
                buf[j] = k; // j  & 0xff;
            }
            LOG_DEBUG("offset: `, length: `", offset, length);
            if (FLAGS_verify) {
                fcheck->pwrite(buf, length, offset);
            }
            auto slice_count = rand() % 4 + 1;
            vector<int> seg_offset{0};
            for (int i = 0; i < slice_count - 1; i++) {
                seg_offset.push_back(rand() % length);
            }
            seg_offset.push_back(length);
            for (auto i = 0; i < slice_count; i++) {
                iov[i].iov_base = &buf[seg_offset[i]];
                iov[i].iov_len = seg_offset[i + 1] - seg_offset[i];
            }
            ssize_t ret = file0->pwritev(iov, slice_count, offset);
            // ssize_t ret = file->pwrite(buf, length, offset);
            if (ret != length) {
                LOG_ERROR("`(`)", errno, strerror(errno));
                exit(-1);
            }
            EXPECT_EQ(ret, length);

            ret = file1->pwritev(iov, slice_count, offset);
            if (ret != length) {
                LOG_ERROR("`(`)", errno, strerror(errno));
                exit(-1);
            }
            EXPECT_EQ(ret, length);
        }
        gettimeofday(&end_time, 0);
        auto elapsed_time = end_time.tv_sec * 1000000 + end_time.tv_usec -
                            start_time.tv_sec * 1000000 - start_time.tv_usec;
        printf("time cost: %ldms\n", elapsed_time / 1000);
    }

    void reset_verify_file() {
        if (fcheck) {
            delete fcheck;
        }
        LOG_INFO("create verify image.");
        auto fn_verify_path = string("/tmp/") + fn_verify;
        fcheck = open_localfile_adaptor(fn_verify_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
        if (fcheck == nullptr) {
            LOG_ERROR("create ` failed.", fn_verify_path.c_str());
            return;
        }
        LOG_INFO("fallocate ` MB size.", vsize >> 20);
        if (fcheck->fallocate(0, 0, vsize) != 0) {
            LOG_ERROR("fallocate verify file failed.");
            return;
        }

        struct stat st;
        fcheck->fstat(&st);
        EXPECT_EQ((uint64_t)st.st_size, vsize);
        // if (vsize > 512 << 20) {
        //     LOG_INFO("do mmap...");
        //     data = (char *)mmap(nullptr, vsize, PROT_WRITE | PROT_READ, MAP_SHARED, fd, 0);
        //     if (data == MAP_FAILED) {
        //         LOG_ERROR("err: `(`)", errno, strerror(errno));
        //         return;
        //     }
        // } else {
        //     data = new char[vsize]{};
        // }
        // ::close(fd);
        // memset(data, 0, vsize);
    }

    IFileRW *create_file(bool sparse = false) {
        cout << "creating a file, by randwrite()" << endl;
        // memset(data, 0, PREAD_LEN);
        cout << "create_file_rw" << endl;
        auto file = create_file_rw(sparse);
        cout << "randwrite" << endl;
        randwrite(file, FLAGS_nwrites);
        return file;
    }

    bool verify_file(LSMT::IFileRO *file) {
        if (FLAGS_verify) {
            cout << "read and verify file , vsize expected: " << (vsize >> 20) << "M" << endl;
        } else {
            cout << "seqread whole image , vsize expected: " << (vsize >> 20) << "M" << endl;
        }

        EXPECT_EQ(file->lseek(0, SEEK_END), (off_t)vsize);
        ALIGNED_MEM4K(buf, 1 << 20)
        auto buf_len = PREAD_LEN;
        for (off_t o = 0; o < (off_t)vsize; o += buf_len) {
            auto ret = file->pread(buf, buf_len, o);
            EXPECT_EQ(ret, (ssize_t)buf_len);
            if (ret != buf_len) {
                LOG_ERROR("pread error: ` < `, offset: `", ret, buf_len, o);
                exit(1);
            }
            if (!FLAGS_verify)
                continue;
            auto v = new char[buf_len];
            DEFER(delete[] v);
            auto ret_v = fcheck->pread(v, buf_len, o);
            EXPECT_EQ(ret, ret_v);
            for (auto i = 0; i < buf_len; i++) {
                EXPECT_EQ(buf[i], v[i]);
                if (buf[i] != v[i]) {
                    LOG_ERROR_RETURN(0, false, "verify failed (offset: `, inner: `), err: `(`)",
                                     o + i, i, errno, strerror(errno));
                }
            }
        }
        return true;
    }
    void verify_file(const char *fn) {
        auto file = open_file_ro(fn);
        verify_file(file);
        delete file;
    }

    void CleanUp() {
        reset_verify_file();
        FLAGS_verify = true;
        for (size_t i = 0; i < data_name.size(); i++) {
            if (lfs->access(layer_name[i].c_str(), 0) == 0)
                lfs->unlink(layer_name[i].c_str());
            if (lfs->access(data_name[i].c_str(), 0) == 0)
                lfs->unlink(data_name[i].c_str());
            if (lfs->access(idx_name[i].c_str(), 0) == 0)
                lfs->unlink(idx_name[i].c_str());
        }
        next_layer_id = 0;
        current_layer_id = 0;
        data_name.clear();
        idx_name.clear();
        layer_name.clear();
        parent_uuid = "";
    }
};

class FileTest3 : public FileTest2 {
public:
    IFile *files[255];
    char /* layer_data[128], layer_index[128], layer[128], layer_gc[128], */ fn_merged[128] =
        "merged.lsmt";

    IFileRW *create_a_layer(bool sparse = false) {
        name_next_layer();
        auto fdata = lfs->open(data_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        auto findex = lfs->open(idx_name.back().c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
        LOG_INFO("data: ` index: `", data_name.back().c_str(), idx_name.back().c_str());
        LayerInfo args(fdata, findex);
        if (sparse)
            args.sparse_rw = true;

        if (parent_uuid != "")
            args.parent_uuid.parse(parent_uuid.c_str(), parent_uuid.size());
        args.virtual_size = vsize;
        auto file = ::create_file_rw(args, true);
        char uu[37]{};
        args.uuid.to_string(uu, 37);
        cout << "create a layer. UUID: " << uu << endl;
        cout << "enable group commit of index for RW file" << endl;
        file->set_index_group_commit(4096);
        randwrite(file, FLAGS_nwrites);
        return file;
    }

    IFile *create_ro_layer() {
        auto file = create_a_layer();
        UUID uu;
        EXPECT_EQ(file->get_uuid(uu, 1000), -1); // invalid check
        file->get_uuid(uu);
        parent_uuid = UUID::String(uu).c_str();
        file->close_seal();

        delete file;

        return lfs->open(data_name.back().c_str(), O_RDONLY);
    }

    IFile *create_commit_layer(int i = 0, int io_engine = 0, bool compress = false,
                               bool verify = false, bool sparse = false) {
        auto file = create_a_layer(sparse);
        IFile *as = nullptr;
        IFile *dst = nullptr;
        auto dst_filename = layer_name.back();
        if (compress) {
            // auto temp_filename = dst_filename + ".tmp";
            // as = lfs->open(temp_filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
            dst = lfs->open(dst_filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);

        } else {
            dst = lfs->open(dst_filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRWXU);
            // dst = as;
        }
        CommitArgs args(nullptr);
        if (compress) {
            CompressOptions opt;
            opt.verify = 1;
            CompressArgs zfile_args(opt);
            args.as = new_zfile_builder(dst, &zfile_args);
        } else {
            args.as = dst;
        }
        char msg[1024]{};
        args.user_tag = msg;
        args.tag_len = 1024;
        if (file->commit(args) != 0) {
            memset(msg, 1, 256);
            args.tag_len = 256;
            file->commit(args);
        }
        args.as->close();
        lfs->unlink(data_name.back().c_str());
        lfs->unlink(idx_name.back().c_str());
        delete file;
        dst->close();

        dst = lfs->open(layer_name.back().c_str(), O_RDONLY);
        if (compress) {
            return ZFile::zfile_open_ro(dst, true);
        }
        return dst;
    }

    virtual IFileRO *create_image(int total_layers) {
        for (int i = 0; i < total_layers; ++i) {
            LOG_DEBUG("Creating image... (layer: `)", i);
            files[i] = create_commit_layer();
        }
        auto image_ro_layers = open_files_ro(files, total_layers, true);
        return image_ro_layers;
    }

    virtual IFileRO *load_image(IFileSystem *lfs, int image_ro_layer_size,
                                IFile **gc_layers = nullptr, size_t n = 0) {

        memset(files, 0, sizeof(files));
        for (int i = 0; i < image_ro_layer_size; i++) {
            LOG_INFO("layer `, `", i, layer_name[i].c_str());
            files[i] = lfs->open(layer_name[i].c_str(), O_RDONLY /*| O_DIRECT */);
            if (is_zfile(files[i]) == 0) {
                struct stat _st;
                files[i]->fstat(&_st);
                clayer_size.push_back(_st.st_size);
                files[i] = zfile_open_ro(files[i]);
            }
        }
        int total_layers = image_ro_layer_size + n;
        for (int i = 0; i < (int)n; i++)
            files[image_ro_layer_size + i] = gc_layers[i];
        auto image_ro_layers = open_files_ro(files, total_layers, true);
        return image_ro_layers;
    }

    void compare_commit(IFileRW *file_rw, IFileRO *file_ro) {
        LOG_INFO("compare RW file & commit file");
        struct stat st_rw, st_ro;
        file_rw->fstat(&st_rw);
        file_ro->fstat(&st_ro);
        EXPECT_EQ(st_rw.st_size, st_ro.st_size);
        auto vsize = st_rw.st_size;
        size_t len = vsize / ALIGNMENT;
        vector<Segment> reads{};
        for (int i = 0; i < 10000; i++) {
            uint64_t offset = (rand() % len);
            uint32_t length = 64; // 32K
            if (offset + length > len)
                offset = len - length;
            Segment s;
            s.offset = offset;
            s.length = length;
            reads.push_back(s);
        }
        char buffer_rw[32768], buffer_ro[32768];
        for (auto s : reads) {
            file_rw->pread(buffer_rw, s.length * ALIGNMENT, s.offset * ALIGNMENT);
            file_ro->pread(buffer_ro, s.length * ALIGNMENT, s.offset * ALIGNMENT);
            EXPECT_EQ(memcmp(buffer_rw, buffer_ro, s.length * ALIGNMENT), 0);
        }
    }

    virtual void TearDown() override {
        if (FLAGS_verify) {
            delete fcheck;
        }
        return;
        printf("Tear Down....\n");
        LOG_INFO("clean up tmp files.");
        FileTest2::TearDown();
        if (lfs->access(fn_merged, 0) == 0)
            lfs->unlink(fn_merged);
    }
};

class WarpFileTest : public FileTest3 {
public:
    void randwrite_warpfile(IFile *file, size_t nwrites);
    IFileRW *create_warpfile_rw(int io_engine = 0);
    IFileRO *create_commit_warpfile(int io_engine = 0, bool keepUUID = false);
    IFileRO *create_commit_warpfile(IFileRW* warpfile, bool keepUUID = false);
};
