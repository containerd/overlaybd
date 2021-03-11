/*
 * alog-audit.h
 *
 * Copyright (C) 2021 Alibaba Group.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */
#pragma once
#include <utility>

#include "alog.h"
#include "utility.h"

#define AU_FILEOP(pathname, offset, size)                                                          \
    make_named_value("pathname", pathname), make_named_value("offset", offset),                    \
        make_named_value("size", size)

#define AU_SOCKETOP(ep) make_named_value("endpoint", ep)

#ifndef DISABLE_AUDIT
#define SCOPE_AUDIT(...)                                                                           \
    auto _CONCAT(__audit_start_time__, __LINE__) = photon::now;                                    \
    DEFER(default_audit_logger << LOG_AUDIT(                                                       \
              __VA_ARGS__, make_named_value("latency", photon::now - _CONCAT(__audit_start_time__, \
                                                                             __LINE__))));

#define SCOPE_AUDIT_THRESHOLD(threshold, ...)                                                      \
    auto _CONCAT(__audit_start_time__, __LINE__) = photon::now;                                    \
    DEFER({                                                                                        \
        auto latency = photon::now - _CONCAT(__audit_start_time__, __LINE__);                      \
        if (latency >= (threshold)) {                                                              \
            default_audit_logger << LOG_AUDIT(__VA_ARGS__, make_named_value("latency", latency));  \
        }                                                                                          \
    });

#else
#define SCOPE_AUDIT(...)
#define SCOPE_AUDIT_THRESHOLD(...)
#endif