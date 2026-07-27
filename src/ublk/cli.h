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

#include "ublk_device.h"

// Parsed CLI command, kept apart from execution for unit testing.
struct UblkCliCmd {
    enum class Kind { NONE, ADD, DEL, LIST };
    Kind kind = Kind::NONE;

    UblkDeviceOpts opts; // ADD
    bool foreground = false;

    int del_dev_id = -1; // DEL
};

// Parse argv into cmd. On success returns 0 with cmd.kind set; on
// error/help prints the message (CLI11 behavior), leaves cmd.kind as NONE
// and returns the intended process exit code (0 for help).
int ublk_parse_cli(int argc, char **argv, UblkCliCmd &cmd);
