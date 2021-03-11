/*
  * list.h
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
#include <assert.h>
#include "../utility.h"

class __intrusive_list_node
{
public:
    __intrusive_list_node* __prev_ptr;
    __intrusive_list_node* __next_ptr;
    __intrusive_list_node()
    {
        __prev_ptr = __next_ptr = this;
    }
    bool single()
    {
        return __prev_ptr == this || __next_ptr == this;
    }
    __intrusive_list_node* remove_from_list()
    {
        if (single())
            return nullptr;

        __next_ptr->__prev_ptr = __prev_ptr;
        __prev_ptr->__next_ptr = __next_ptr;
        auto next = __next_ptr;
        __prev_ptr = __next_ptr = this;
        return next;
    }
    void insert_before(__intrusive_list_node* ptr)
    {
        ptr->insert_between(__prev_ptr, this);
    }
    void insert_tail(__intrusive_list_node* ptr)
    {
        insert_before(ptr);
    }
    void insert_after(__intrusive_list_node* ptr)
    {
        ptr->insert_between(this, __next_ptr);
    }

private:
    void insert_between(__intrusive_list_node* prev, __intrusive_list_node* next)
    {
        if (__prev_ptr != __next_ptr) return;
        prev->__next_ptr = this;
        next->__prev_ptr = this;
        __prev_ptr = prev;
        __next_ptr = next;
    }
};

template<typename T>
class intrusive_list_node : public __intrusive_list_node
{
public:
    T* remove_from_list()
    {
        auto ret = __intrusive_list_node::remove_from_list();
        return static_cast<T*>(ret);
    }
    void insert_before(T* ptr)
    {
        __intrusive_list_node::insert_before(ptr);
    }
    void insert_tail(T* ptr)
    {
        __intrusive_list_node::insert_tail(ptr);
    }
    void insert_after(T* ptr)
    {
        __intrusive_list_node::insert_after(ptr);
    }
    void insert_before(T& ptr)
    {
        __intrusive_list_node::insert_before(&ptr);
    }
    void insert_tail(T& ptr)
    {
        __intrusive_list_node::insert_tail(&ptr);
    }
    void insert_after(T& ptr)
    {
        __intrusive_list_node::insert_after(&ptr);
    }
    T* next()
    {
        return static_cast<T*>(__intrusive_list_node::__next_ptr);
    }
    T* prev()
    {
        return static_cast<T*>(__intrusive_list_node::__prev_ptr);
    }

    struct iterator
    {
        __intrusive_list_node* ptr;
        __intrusive_list_node* end;
        T& operator*()
        {
            return *static_cast<T*>(ptr);
        }
        iterator& operator++()
        {
            ptr = ptr->__next_ptr;
            if (ptr == end)
                ptr = nullptr;
            return *this;
        }
        iterator operator++(int)
        {
            auto rst = *this;
            ptr = ptr->__next_ptr;
            if (ptr == end)
                ptr = nullptr;
            return rst;
        }
        bool operator == (const iterator& rhs)
        {
            return ptr == rhs.ptr;
        }
        bool operator != (const iterator& rhs)
        {
            return  !(*this == rhs);
        }
    };

    iterator begin()
    {
        return {this, this};
    }
    iterator end()
    {
        return {nullptr, nullptr};
    }
};


template<typename NodeType>
class intrusive_list
{
public:
    NodeType* node = nullptr;
    ~intrusive_list()
    {   // node (NodeType*) MUST be intrusive_list_node<T>, which
        // should be implicitly convertible to __intrusive_list_node*
        __intrusive_list_node* __node = node;
        assert(__node == nullptr);
        _unused(__node);
    }
    void push_back(NodeType* ptr)
    {
        if (!node) {
            node = ptr;
        } else {
            node->insert_tail(ptr);
        }
    }
    void push_front(NodeType* ptr)
    {
        push_back(ptr);
        node = ptr;
    }
    NodeType* pop_front()
    {
        if (!node)
            return nullptr;
        
        auto rst = node;
        node = node->remove_from_list();
        return rst;
    }
    NodeType* pop_back()
    {
        if (!node)
            return nullptr;

        auto rst = node->prev();
        if (rst == node) {
            node = nullptr;
        } else {
            rst->remove_from_list();
        }
        return rst;
    }
    NodeType* erase(NodeType* ptr)
    {
        auto nx = ptr->remove_from_list();
        if (ptr == node)
            node = nx;
        return nx;
    }
    void pop(NodeType* ptr)
    {
        erase(ptr);
    }
    NodeType* front()
    {
        return node;
    }
    NodeType* back()
    {
        return node ? node->prev() : nullptr;
    }
    operator bool()
    {
        return node;
    }
    bool empty()
    {
        return node == nullptr;
    }
    typedef typename NodeType::iterator iterator;
    iterator begin()
    {
        return node ? node->begin() : end();
    }
    iterator end()
    {
        return {nullptr, nullptr};
    }
};

