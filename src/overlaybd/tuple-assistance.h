/*
 * tuple-assistance.h
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

struct tuple_assistance {
    template <int...>
    struct seq {};

    template <int N, int... S>
    struct gens : gens<N - 1, N - 1, S...> {};

    template <int... S>
    struct gens<0, S...> {
        typedef seq<S...> type;
    };

    template <class F>
    struct callable;

    template <class R, class... A>
    struct callable<R (*)(A...)> {
        typedef R (*F)(A...);

        typedef R return_type;

        typedef std::tuple<A...> arguments;

        template <typename... Ts>
        static return_type apply(F f, std::tuple<Ts...> &args) {
            typedef typename gens<sizeof...(Ts)>::type S;
            return do_apply(S(), f, args);
        }

    protected:
        template <int... S, typename... Ts>
        static return_type do_apply(seq<S...>, F f, std::tuple<Ts...> &args) {
            return f(std::forward<Ts>(std::get<S>(args))...);
        }
    };

    template <typename P, size_t I, typename... Ts>
    struct do_enumerate;

    template <typename P, size_t I, typename... Ts>
    struct do_enumerate<P, I, std::tuple<Ts...>> {
        static_assert(0 < I && I < sizeof...(Ts), "");
        static void proc(const P &p, std::tuple<Ts...> &t) {
            do_enumerate<P, I - 1, std::tuple<Ts...>>::proc(p, t);
            p.proc(std::get<I>(t));
        }
    };

    template <typename P, typename... Ts>
    struct do_enumerate<P, 0, std::tuple<Ts...>> {
        static void proc(const P &p, std::tuple<Ts...> &t) {
            p.proc(std::get<0>(t));
        }
    };

    template <typename P, typename... Ts>
    static void enumerate(const P &p, std::tuple<Ts...> &t) {
        do_enumerate<P, sizeof...(Ts) - 1, std::tuple<Ts...>>::proc(p, t);
    }
};
