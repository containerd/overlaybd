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

#include "zstdfile.h"

#include <vector>

#include <fcntl.h>
#include <zstd.h>
#include <photon/common/alog.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/virtual-file.h>

class ZStdAdaptorFile : public photon::fs::VirtualReadOnlyFile {
public:
    ZStdAdaptorFile(photon::fs::IFile* file)
        : m_file(file)
        , m_stream(ZSTD_createDStream())
        , m_buffer(ZSTD_DStreamInSize())
        , m_input({m_buffer.data(), 0, 0}) {
    }

    ~ZStdAdaptorFile() {
        ZSTD_freeDStream(m_stream);
        delete m_file;
    }

    ssize_t read(void *buf, size_t count) override {
        ssize_t bytes_read = 0;
        ZSTD_outBuffer output = { buf, count, 0 };
        while (output.pos < output.size) {
            // Output buffer is not filled, read more.
            if (m_input.pos == m_input.size) {
                // Reached the end of the input buffer, read more compressed data.
                ssize_t read_compressed_bytes = m_file->read(m_buffer.data(), m_buffer.size());
                if (read_compressed_bytes == 0) {
                    // EOF reached.
                    return bytes_read;
                }
                if (read_compressed_bytes < 0 || read_compressed_bytes > (ssize_t) m_buffer.size()) {
                    // Error reading file.
                    LOG_ERRNO_RETURN(EIO, -1, "read buffer error");
                }
                m_input.size = read_compressed_bytes;
                m_input.pos = 0;
            }
            const size_t prev_pos = output.pos;
            const size_t ret = ZSTD_decompressStream(m_stream, &output, &m_input);
            if (ZSTD_isError(ret)) {
                LOG_ERRNO_RETURN(EIO, -1, "failed to decompress zstd frame");
            }
            bytes_read += output.pos - prev_pos;
            if (ret == 0) {
                // End of this ZSTD frame, set up for the next one (if there is one).
                ZSTD_initDStream(m_stream);
            }
        }
        return bytes_read;
    }

    int fstat(struct stat *buf) override {
        return m_file->fstat(buf);
    }

    UNIMPLEMENTED_POINTER(photon::fs::IFileSystem *filesystem() override);
    UNIMPLEMENTED(off_t lseek(off_t offset, int whence) override);
    UNIMPLEMENTED(ssize_t readv(const struct iovec *iov, int iovcnt) override);
    UNIMPLEMENTED(ssize_t preadv(const struct iovec *iov, int iovcnt, off_t offset) override);
private:
    photon::fs::IFile* m_file;
    ZSTD_DStream* m_stream;

    std::vector<uint8_t> m_buffer;
    ZSTD_inBuffer m_input;
};

photon::fs::IFile *open_zstdfile_adaptor(photon::fs::IFile* file) {
    return new ZStdAdaptorFile(file);
}

const uint8_t ZSTD_MAGIC_HEADER[4] = {0x28, 0xB5, 0x2F, 0xFD};

bool is_zstdfile(photon::fs::IFile* file) {
    char buf[4] = {0};
    ssize_t readn = file->read(buf, 4);
    file->lseek(0, 0);
    return readn == 4 && memcmp(buf, ZSTD_MAGIC_HEADER, 4) == 0;
}