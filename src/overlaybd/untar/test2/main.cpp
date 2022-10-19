#include "../libtar.h"
#include <fcntl.h>
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <photon/fs/filesystem.h>
#include <photon/fs/localfs.h>

void run() {
    auto tarf = photon::fs::open_localfile_adaptor("/home/admin/developments/ufs_test/test.tar", O_RDONLY, 0666, 0);
    auto target = photon::fs::new_localfs_adaptor("/home/admin/developments/ufs_test/rootfs");
    auto tar = new Tar(tarf, target, 0);
    if (tar->extract_all() < 0) {
        LOG_ERROR("extract all failed");
    } else {
        LOG_INFO("extract all done");
    }
}

int main(int argc, char **argv) {
    photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    set_log_output_level(ALOG_DEBUG);
    
    run();

    photon::fini();

    return 0;
}
