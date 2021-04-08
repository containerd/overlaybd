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
#include <stdlib.h>
#include <inttypes.h>
#include "callback.h"
#include <new>
#include "photon/list.h"

class IdentityPoolBase : public intrusive_list_node<IdentityPoolBase> {
protected:
    bool autoscale = false;

public:
    ~IdentityPoolBase() {
        if (autoscale)
            disable_autoscale();
    }
    virtual uint64_t do_scale() = 0;
    int enable_autoscale();
    int disable_autoscale();
};

template <typename T>
class IdentityPool0 : public IdentityPoolBase {
public:
    T *get() {
        DEFER(min_size_in_interval = m_size < min_size_in_interval ? m_size : min_size_in_interval);
        if (m_size > 0)
            return m_items[--m_size];

        T *obj = nullptr;
        m_ctor(&obj);
        return obj;
    }
    void put(T *obj) {
        DEFER(min_size_in_interval = m_size < min_size_in_interval ? m_size : min_size_in_interval);
        if (!obj)
            return;
        if (m_size < m_capacity) {
            m_items[m_size++] = obj;
        } else {
            m_dtor(obj);
        }
    }

    typedef Callback<T **> Constructor;
    typedef Callback<T *> Destructor;
    static IdentityPool0 *new_identity_pool(uint32_t capacity) {
        auto rst = do_malloc(capacity);
        return new (rst) IdentityPool0(capacity);
    }
    static IdentityPool0 *new_identity_pool(uint32_t capacity, Constructor ctor, Destructor dtor) {
        auto rst = do_malloc(capacity);
        return new (rst) IdentityPool0(capacity, ctor, dtor);
    }
    static void delete_identity_pool(IdentityPool0 *p) {
        p->~IdentityPool0();
        free(p);
    }
    uint64_t do_scale() override {
        auto des_n = (min_size_in_interval + 1) / 2;
        assert(des_n <= m_size);
        while (m_size > 0 && des_n-- > 0) {
            m_dtor(m_items[--m_size]);
        }
        min_size_in_interval = m_size;
        return 0;
    }

protected:
    uint32_t m_capacity, m_size = 0;
    Constructor m_ctor{nullptr, &default_constructor};
    Destructor m_dtor{nullptr, &default_desstructor};
    void *m_reserved;
    uint32_t min_size_in_interval = 0;
    T *m_items[0]; // THIS MUST BE THE LAST MEMBER DATA!!!

    static int default_constructor(void *, T **ptr) {
        *ptr = new T;
        return 0;
    }
    static int default_desstructor(void *, T *ptr) {
        delete ptr;
        return 0;
    }
    static void *do_malloc(uint32_t capacity) {
        return malloc(sizeof(IdentityPool0) + capacity * sizeof(m_items[0]));
    }
    IdentityPool0(uint32_t capacity) : m_capacity(capacity) {
    }

    IdentityPool0(uint32_t capacity, Constructor ctor, Destructor dtor)
        : m_capacity(capacity), m_ctor(ctor), m_dtor(dtor) {
    }

    ~IdentityPool0() {
        T **pitems = m_items;
        for (uint32_t i = 0; i < m_size; ++i)
            m_dtor(pitems[i]);
    }
};

template <typename T, uint32_t CAPACITY>
class IdentityPool : public IdentityPool0<T> {
public:
    IdentityPool() : IdentityPool0<T>(CAPACITY) {
    }

    using typename IdentityPool0<T>::Constructor;

    using typename IdentityPool0<T>::Destructor;

    IdentityPool(Constructor ctor, Destructor dtor) : IdentityPool0<T>(CAPACITY, ctor, dtor) {
    }

protected:
    T *m_items[CAPACITY];
};

template <typename T>
inline IdentityPool0<T> *new_identity_pool(uint32_t capacity) {
    return IdentityPool0<T>::new_identity_pool(capacity);
}

template <typename T>
inline IdentityPool0<T> *new_identity_pool(uint32_t capacity,
                                           typename IdentityPool0<T>::Constructor ctor,
                                           typename IdentityPool0<T>::Destructor dtor) {
    return IdentityPool0<T>::new_identity_pool(capacity, ctor, dtor);
}

template <typename T>
inline void delete_identity_pool(IdentityPool0<T> *p) {
    IdentityPool0<T>::delete_identity_pool(p);
}
