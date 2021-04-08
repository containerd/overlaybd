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
#include <errno.h>
#include <memory>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include "object.h"
#include "photon/list.h"
#include "photon/thread.h"
#include "photon/timer.h"
#include "string-keyed.h"
#include "utility.h"

template <typename KeyType, typename... Ts>
class ExpireContainer {
protected:
    using MapKey = typename std::conditional<std::is_base_of<std::string, KeyType>::value,
                                             std::string_view, KeyType>::type;
    struct ItemRef : public intrusive_list_node<ItemRef> {
        template <typename... Gs>
        ItemRef(const MapKey &key, Gs &&...xs) : payload(key, std::forward<Gs>(xs)...) {
        }

        uint64_t timestamp = photon::now;

        // TODO: deduplication of the key (both in ItemRef and Map)
        std::tuple<MapKey, Ts...> payload;

        MapKey key() {
            return std::get<0>(payload);
        }
    };
    using Map = typename std::conditional<std::is_base_of<std::string, KeyType>::value,
                                          unordered_map_string_key<ItemRef *>,
                                          std::unordered_map<KeyType, ItemRef *>>::type;

    uint64_t m_expiration;
    intrusive_list<ItemRef> m_list;
    Map m_map;
    photon::Timer m_timer;

    // update timestamp of a reference to item.
    void refresh(ItemRef *ref) {
        this->m_list.pop(ref);
        ref->timestamp = photon::now;
        this->m_list.push_back(ref);
    }

    template <typename... Gs>
    typename Map::iterator insert_into_map(const MapKey &key, Gs &&...xs) {
        // when key is string view for outside,
        // it should change to an string view object that
        // refers key_string inside m_map
        // so emplace first, then create ItemRef by m_map iter
        auto res = m_map.emplace(key, nullptr);
        if (!res.second)
            LOG_ERROR_RETURN(EEXIST, m_map.end(), "` Already in the map");
        auto it = res.first;
        auto node = new ItemRef(it->first, std::forward<Gs>(xs)...);
        it->second = node;
        return it;
    }

public:
    explicit ExpireContainer(uint64_t expiration)
        : m_expiration(expiration), m_timer(expiration, {this, &ExpireContainer::expire}) {
    }

    ~ExpireContainer() {
        while (!m_list.empty())
            m_list.pop_front();
        for (auto &x : m_map) {
            delete x.second;
        }
    }

    template <typename... Gs>
    typename Map::iterator insert(const MapKey &key, Gs &&...xs) {
        // when key is string view for outside,
        // it should change to an string view object that
        // refers key_string inside m_map
        // so emplace first, then create ItemRef by m_map iter
        auto it = insert_into_map(key, std::forward<Gs>(xs)...);
        if (it == m_map.end())
            return m_map.end();
        this->m_list.push_back(it->second);
        return it;
    }

    uint64_t expire() {
        auto expire_time = photon::now - this->m_expiration;
        while (!m_list.empty() && m_list.front()->timestamp < expire_time) {
            auto p = m_list.pop_front();
            LOG_DEBUG("expire ", VALUE(p), VALUE(p->key()));
            m_map.erase(p->key());
            delete p;
        }
        return 0;
    };

    // return the # of items currently in the list
    size_t size() {
        return m_map.size();
    }

    // time to expire, in us
    size_t expiration() {
        return m_expiration;
    }

    struct iterator : public Map::iterator {
        using base = typename Map::iterator;
        iterator() = default;
        iterator(base it) : base(it) {
        }
        MapKey operator*() {
            return base::operator*().second->key();
        }
    };

    iterator begin() {
        return m_map.begin();
    }

    iterator end() {
        return m_map.end();
    }

    iterator find(const MapKey &key) {
        return m_map.find(key);
    }
};

// a set / list like structure
// able to query whether an item not expired in it.
template <typename ItemType>
class ExpireList : public ExpireContainer<ItemType> {
public:
    using ExpireContainer<ItemType>::ExpireContainer;
    bool keep_alive(ItemType item, bool insert_if_not_exists) {
        DEFER(this->expire());
        auto iter = this->m_map.find(item);
        if (iter != this->m_map.end()) {
            this->refresh(iter->second);
        } else if (insert_if_not_exists) {
            this->insert(item);
        } else
            return false;
        return true;
    }
};

// Resource pool based on reference count
// when the pool is fulled, it will try to remove items which can be sure is not
// referenced the base m_list works as gc list when object acquired, construct
// or findout the object, add reference count; when object release, reduce
// refcount. if it is
template <typename T, typename std::enable_if<std::is_pointer<T>::value, int>::type = 0>
using UniqPtr = typename std::unique_ptr<typename std::remove_pointer<T>::type>;

template <typename KeyType, typename ValType>
class ObjectCache : public ExpireContainer<KeyType, UniqPtr<ValType>, uint64_t, bool> {
protected:
    photon::condition_variable release_cv, block_cv;

public:
    using base = ExpireContainer<KeyType, UniqPtr<ValType>, uint64_t, bool>;
    using base::base;

    static inline UniqPtr<ValType> &get_ptr(typename base::ItemRef *ref) {
        return std::get<1>(ref->payload);
    }
    static inline uint64_t &get_refcnt(typename base::ItemRef *ref) {
        return std::get<2>(ref->payload);
    }
    static inline bool &get_block_flag(typename base::ItemRef *ref) {
        return std::get<3>(ref->payload);
    }

    // acquire resource with identity key. It can be sure to get an resource
    // resources are reusable, managed by reference count and key.
    // when the pool is full and not able to release any resource, it will block
    // till resourece
    template <typename Constructor>
    ValType acquire(const typename base::MapKey &key, const Constructor &ctor) {
        DEFER(this->expire());
        auto iter = this->m_map.find(key);
        // check if it is in immediately release
        // if it is true, block till complete
        while (iter != this->m_map.end() && get_block_flag(iter->second)) {
            block_cv.wait_no_lock();
            iter = this->m_map.find(key);
        }
        if (iter == this->m_map.end()) {
            // create an empty reference for item, block before make sure
            iter = base::insert_into_map(key, nullptr, 1, true);
            auto ref = iter->second;
            if (iter == this->m_map.end())
                LOG_ERROR_RETURN(0, nullptr, "Failed to insert into map");
            ValType val = ctor();
            if (val == nullptr) {
                this->m_map.erase(key);
                delete ref;
            } else {
                // unblock because object create normally
                get_ptr(ref).reset(val);
                get_block_flag(ref) = false;
            }
            block_cv.notify_all();
            return val;
        } else {
            auto ref = iter->second;
            auto &refcnt = get_refcnt(ref);
            if (refcnt++ == 0)
                this->m_list.erase(ref);
            return get_ptr(ref).get();
        }
    }

    // once user finished using a resource, it should call release(key) to tell
    // the pool that the reference is over
    int release(const typename base::MapKey &key, bool recycle = false) {
        DEFER(this->expire());
        auto iter = this->m_map.find(key);
        assert(iter != this->m_map.end());
        auto ref = iter->second;
        auto &refcnt = --get_refcnt(ref);
        assert(refcnt != -1UL);
        auto &immark = get_block_flag(ref);
        if (immark) { // already in imrelease procedure
            release_cv.notify_one();
            return refcnt;
        }
        // not in immediately release
        if (!recycle) { // and currently is normal release
            if (refcnt == 0) {
                ref->timestamp = photon::now;
                this->m_list.push_back(ref);
            }
            return refcnt;
        }
        // marked as new release immediatly
        get_block_flag(ref) = true;
        // wait for all refs released
        while (refcnt > 0)
            release_cv.wait_no_lock();
        assert(refcnt == 0);
        // delete
        this->m_map.erase(key);
        delete ref;
        // tell blocking acquires
        block_cv.notify_all();
        return 0;
    }
};
