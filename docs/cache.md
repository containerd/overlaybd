# Overlaybd File Cache

Cache is used to accelerate remote file access. In overlaybd's own terminology, we name the remote files to `source` as well as the local cached files to `media`, and the word `cache` refers to the whole caching library that provides a POSIX compliant filesystem API.

## Operations

Typically, a caching library should support these operations:

* Read
* Write
* Query (Check if data is already cached in media)
* Evict
* Refill / Prefetch (Cache data in advance)
* Flush
* ...

In the container image scenario, however, the layer blobs are immutable for the most of the time. So we may simplify our goal by trying to design a read-only file cache. Some of the operations will be no longer needed then.

## Cache implementations

Currently there are two supported caches in overlaybd:

* full file cache
* ocf cache

### full file cache

The full file cache implementation is based on a very simple idea: fill a duplicated file in disk and keep its size grow as long as we still can (not evicted). We managed to do this by leveraging many kernel features such as [`sparse files`](https://en.wikipedia.org/wiki/Sparse_file) and [`fiemap`](https://www.kernel.org/doc/html/latest/filesystems/fiemap.html). The first one is able to reduce cache usage, because containers would normally not require the whole image file content to start. The second one provides us a portable way to manage metadata (Query).

### ocf cache

The ocf cache is built on [Intel Open CAS Framework](https://open-cas.github.io/). This open-source framework is a high performance block storage caching meta-library written in C. We have implemented a read-only filesystem on top of this block driver with modern C++, and reshaped it to a new lib.

Ocf cache has solved many old issues that came along with full file cache, for instance, lacking good support to heterogeneous filesystems such as xfs and tmpfs, getting low performance if eviction happened, or the annoying bugs when src files is even larger than the entire cache media. Besides, the new flexible infrastructure makes it easier to adopt overlaybd's native coroutine-scheduling mechanism and perhaps some fresh I/O engines (io_uring) in the future, comparing to those heavy-weight caching systems. 

## Cache configurations

Edit `/etc/overlaybd/overlaybd.json`, add the following line. The default value of `cacheType` is "file".

```
"cacheType": "ocf",
```

## TODO

We are still working with the OCF community to improve ocf cache. Any contribution is welcome.