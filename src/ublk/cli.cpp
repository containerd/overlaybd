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
#include "cli.h"

#include "../tools/CLI11.hpp"

int ublk_parse_cli(int argc, char **argv, UblkCliCmd &cmd) {
    CLI::App app{"overlaybd-ublk: expose an overlaybd image as /dev/ublkbN "
                 "(one process serves one device)"};
    app.require_subcommand(1);

    auto *add = app.add_subcommand("add", "create a ublk device from an image config");
    add->add_option("--config", cmd.opts.image_config_path,
                    "overlaybd image config path (config.v1.json)")
        ->required()
        ->check(CLI::ExistingFile);
    add->add_option("-n,--dev-id", cmd.opts.dev_id,
                    "device id (default -1: allocated by kernel)");
    add->add_option("--depth", cmd.opts.queue_depth, "queue depth")
        ->default_val(128)
        ->check(CLI::Range(1, 4096));
    add->add_option("--service-config", cmd.opts.service_config_path,
                    "global service config path (default /etc/overlaybd/overlaybd.json)");
    add->add_option("--log-path", cmd.opts.log_path,
                    "per-device log file (default: shared log from service config; "
                    "recommended when running multiple devices)");
    add->add_option("--cache-dir", cmd.opts.cache_dir,
                    "dedicated cache directory for this device (default: shared cache "
                    "dirs from service config; required when running multiple devices, "
                    "cache locking is in-process only)");
    add->add_flag("--foreground", cmd.foreground, "run in foreground (no daemonize)");

    auto *del = app.add_subcommand("del", "stop the daemon serving /dev/ublkbN");
    del->add_option("-n,--dev-id", cmd.del_dev_id, "device id")->required();

    app.add_subcommand("list", "list overlaybd-ublk devices on this host");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        // help returns 0, errors return non-zero; cmd.kind stays NONE in
        // both cases so the caller just exits with this code
        cmd.kind = UblkCliCmd::Kind::NONE;
        return app.exit(e);
    }

    if (app.got_subcommand("add"))
        cmd.kind = UblkCliCmd::Kind::ADD;
    else if (app.got_subcommand("del"))
        cmd.kind = UblkCliCmd::Kind::DEL;
    else
        cmd.kind = UblkCliCmd::Kind::LIST;
    return 0;
}
