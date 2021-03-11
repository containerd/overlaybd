/*
 * alog-functionptr.h
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
#include <typeinfo>
#include <cxxabi.h>
#include "alog.h"

template <typename T>
inline LogBuffer &__printfp__(LogBuffer &log, T func) {
    size_t size = 0;
    int status = -4; // some arbitrary value to eliminate the compiler warning
    auto name = abi::__cxa_demangle(typeid(func).name(), nullptr, &size, &status);
    log << "function_pointer<" << ALogString(name, size) << "> at " << (void *&)func;
    free(name);
    return log;
}

template <typename Ret, typename... Args>
inline LogBuffer &operator<<(LogBuffer &log, Ret (*func)(Args...)) {
    return __printfp__(log, func);
}

template <typename Ret, typename Clazz, typename... Args>
inline LogBuffer &operator<<(LogBuffer &log, Ret (Clazz::*func)(Args...)) {
    return __printfp__(log, func);
}

template <typename Ret, typename Clazz, typename... Args>
inline LogBuffer &operator<<(LogBuffer &log, Ret (Clazz::*func)(Args...) const) {
    return __printfp__(log, func);
}
