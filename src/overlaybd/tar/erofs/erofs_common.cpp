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

#include "erofs_common.h"
#include <photon/common/alog.h>

ssize_t ErofsCache::write_sector(u64 addr, char *buf)
{
	struct liberofs_inmem_sector *sector;

	if (addr & (SECTOR_SIZE-1)) {
		LOG_ERROR("Invalid addr, should be aligned to SECTOR_SIZE.");
		return -1;
	}
	if (caches.find(addr) != caches.end()) {
		sector = caches[addr];
		memcpy(sector->data, buf, SECTOR_SIZE);
		dirty.insert(addr);
	} else {
		if (caches.size() == capacity) {
			auto it = caches.begin();
			if (dirty.find(it->first) != dirty.end()) {
				if (file->pwrite(it->second->data, SECTOR_SIZE, it->first)
				    != SECTOR_SIZE)
				{
					LOG_ERROR("Fail to write sector %lld.", it->first);
					return -EIO;
				}
				dirty.erase(it->first);
			}
			sector = it->second;
			caches.erase(it);
		} else  {
			sector = (struct liberofs_inmem_sector*)malloc(
					sizeof(struct liberofs_inmem_sector));
			if (!sector)
				return -ENOMEM;
		}
		memcpy(sector->data, buf, SECTOR_SIZE);
		caches[addr] = sector;
		dirty.insert(addr);
	}
	return SECTOR_SIZE;
}

ssize_t ErofsCache::read_sector(u64 addr, char *buf)
{
	struct liberofs_inmem_sector *sector;

	if (addr & (SECTOR_SIZE - 1)) {
		LOG_ERROR("Invalid addr, should be aligned to SECTOR_SIZE.");
		return -1;
	}

	if (caches.find(addr) != caches.end()) {
		sector = caches[addr];
		memcpy(buf, sector->data, SECTOR_SIZE);
	} else {
		if (caches.size() == capacity) {
			auto it = caches.begin();
			if (dirty.find(it->first) != dirty.end()) {
				sector = it->second;
				if (file->pwrite(sector->data, SECTOR_SIZE, it->first)
					!= SECTOR_SIZE)
				{
					LOG_ERROR("Fail to write sector %lld.", it->first);
					return -EIO;
				}
				dirty.erase(it->first);
			}
			sector = it->second;
			caches.erase(it);
		} else {
			sector = (struct liberofs_inmem_sector*)malloc(
					sizeof(struct liberofs_inmem_sector));
			if (!sector)
				return -ENOMEM;
		}
		if (file->pread(sector->data, SECTOR_SIZE, addr) != SECTOR_SIZE) {
			LOG_ERROR("Fail to read sector %lld", addr);
			return -EIO;
		}
		caches[addr] = sector;
		memcpy(buf, sector->data, SECTOR_SIZE);
	}
	return SECTOR_SIZE;
}

int ErofsCache::flush()
{
	for (auto it = caches.begin(); it != caches.end();) {
		if (dirty.find(it->first) != dirty.end()) {
			if (file->pwrite(it->second->data, SECTOR_SIZE, it->first)
			    != SECTOR_SIZE)
			{
				LOG_ERROR("Fail to flush sector %lld.\n", it->first);
				return -1;
			}
			dirty.erase(it->first);
		}
		free(it->second);
		it = caches.erase(it);
	}
	return dirty.size() != 0 || caches.size() != 0;
}

/*
 * Helper function for reading from the photon file, since
 * the photon file requires reads to be 512-byte aligned.
 */
ssize_t erofs_read_photon_file(void *buf, u64 offset, size_t len,
			       ErofsCache *cache)
{

	ssize_t read;
	u64 start, end;
	char extra_buf[SECTOR_SIZE];
	u64 i, j;

	start = round_down_blk(offset);
	end = round_up_blk(offset + len);
	read = 0;

	/* we use the extra_buf to store the first sector or last sector when:
	 * - start != offset
	 * or
	 * - end != offset + len
	 */
	if (start != offset || end != offset + len) {
		i = start == offset ? start : start + SECTOR_SIZE;
		j = end == offset + len ? end : end - SECTOR_SIZE;

		/* read the first sector */
		if (i != start) {
			if (cache->read_sector(start, extra_buf) != SECTOR_SIZE) {
				LOG_ERROR("Fail to read start sector.");
				return -1;
			}
			memcpy((char*)buf, extra_buf + offset - start,
						erofs_min(start + SECTOR_SIZE - offset,len));
			read += erofs_min(start + SECTOR_SIZE - offset,len);
		}

		/* read the last sector and avoid re-reading the same sector as above */
		if (j != end && (i == start || end - start > SECTOR_SIZE)) {
			if (cache->read_sector(end - SECTOR_SIZE,
				extra_buf) != SECTOR_SIZE)
			{
				LOG_ERROR("Fail to read start sector.");
				return -1;
			}
			memcpy((char*)buf + end - SECTOR_SIZE - offset, extra_buf,
					len + offset + SECTOR_SIZE - end);
			read += len + offset + SECTOR_SIZE - end;
		}

		for (u64 addr = i; addr < j; addr += SECTOR_SIZE) {
			if (cache->read_sector(addr,
					       addr - offset + (char*)buf) != SECTOR_SIZE)
			{
				LOG_ERROR("Fail to read sector %lld in read_photo_file.\n", i);
				return -1;
			}
			read += SECTOR_SIZE;
		}
	} else {
		/* if read request is sector-aligned, we use the original buffer */
		for (u64 i = start; i < end; i += SECTOR_SIZE) {
			if (cache->read_sector(i, (char*)buf + i - start) != SECTOR_SIZE) {
				LOG_ERROR("Fail to read start sector.");
				return -1;
			}
			read += SECTOR_SIZE;
		}
	}

	return read;
}

/*
 * Helper function for writing to a photon file.
 */
ssize_t erofs_write_photon_file(const void *buf, u64 offset,
				size_t len, ErofsCache *cache)
 {
	ssize_t write;
	u64 start, end;
	char extra_buf[SECTOR_SIZE];
	u64 i, j;

	start = round_down_blk(offset);
	end = round_up_blk(offset + len);
	write = 0;

	if (start != offset || end != offset + len) {
		i = start == offset ? start : start + SECTOR_SIZE;
		j = end == offset + len ? end : end - SECTOR_SIZE;

		if (i != start) {
			if (cache->read_sector(start, extra_buf) != SECTOR_SIZE) {
				LOG_ERROR("Fail to read sector %lld.\n", start);
				return -1;
			}
			memcpy(extra_buf + offset - start, buf,
			       erofs_min(start +  SECTOR_SIZE - offset, len));
			if (cache->write_sector(start, extra_buf) != SECTOR_SIZE) {
				LOG_ERROR("Fail to write sector %lld\n", start);
				return -1;
			}
			write += erofs_min(start +  SECTOR_SIZE - offset, len);
		}

		if (j != end && (i == start || end - start > SECTOR_SIZE)) {
			if (cache->read_sector(end - SECTOR_SIZE, extra_buf)
				!= SECTOR_SIZE)
			{
				LOG_ERROR("Fail to read sector %lld.", end - SECTOR_SIZE);
				return -1;
			}
			memcpy(extra_buf, (char*)buf + end - SECTOR_SIZE - offset,
			       offset + len + SECTOR_SIZE - end);
			if (cache->write_sector(end - SECTOR_SIZE, extra_buf)
				!= SECTOR_SIZE)
			{
				LOG_ERROR("Fail to write sector %lld.", end - SECTOR_SIZE);
				return -1;
			}
			write += offset + len + SECTOR_SIZE - end;
		}

		for (u64 addr = i; addr < j; addr += SECTOR_SIZE) {
			if (cache->write_sector(addr, (char*)buf + addr - offset)
				!= SECTOR_SIZE)
			{
				LOG_ERROR("Fail to write sector %lld.", addr);
				return -1;
			}
			write += SECTOR_SIZE;
		}
	} else {
		for (u64 addr = start; addr < end; addr += SECTOR_SIZE) {
			if (cache->write_sector(addr, (char *)buf + addr - start)
			    != SECTOR_SIZE)
			{
				LOG_ERROR("Fail to write sector %lld.", addr);
				return -1;
			}
			write += SECTOR_SIZE;
		}
	}

	return write;
}

/* I/O control for target */
ssize_t erofs_target_pread(struct erofs_vfile *vf, void *buf, u64 offset,
			   size_t len)
{
	struct liberofs_file *target_file =
			reinterpret_cast<struct liberofs_file *>(vf->ops);

	if (!target_file)
		return -EINVAL;
	if (erofs_read_photon_file(buf, offset, len, target_file->cache)
							   != (ssize_t)len)
		return -1;

	return len;
}

ssize_t erofs_target_pwrite(struct erofs_vfile *vf, const void *buf,
			    u64 offset, size_t len)
{
	struct liberofs_file *target_file =
			reinterpret_cast<struct liberofs_file *>(vf->ops);

	if (!target_file)
		return -EINVAL;
	if (!buf)
		return -EINVAL;

	return erofs_write_photon_file(buf, offset, len, target_file->cache);
}

int erofs_target_fsync(struct erofs_vfile *vf)
{
	struct liberofs_file *target_file =
			reinterpret_cast<struct liberofs_file *>(vf->ops);

	if (!target_file)
		return -EINVAL;
	return target_file->cache->flush();
}

int erofs_target_fallocate(struct erofs_vfile *vf, u64 offset,
			   size_t len, bool pad)
{
	static const char zero[4096] = {0};
	ssize_t ret;

	while (len > 4096) {
		ret = erofs_target_pwrite(vf, zero, offset, 4096);
		if (ret)
			return ret;
		len -= 4096;
		offset += 4096;
	}
	ret = erofs_target_pwrite(vf, zero, offset, len);
	if (ret != (ssize_t)len) {
		return -1;
	}
	return 0;
}

int erofs_target_ftruncate(struct erofs_vfile *vf, u64 length)
{
	return 0;
}

ssize_t erofs_target_read(struct erofs_vfile *vf, void *buf, size_t len)
{
	return -EROFS_UNIMPLEMENTED;
}

off_t erofs_target_lseek(struct erofs_vfile *vf, u64 offset, int whence)
{
	return -EROFS_UNIMPLEMENTED;
}

/* I/O control for source */
ssize_t erofs_source_pread(struct erofs_vfile *vf, void *buf, u64 offset,
			   size_t len)
{
	return -EROFS_UNIMPLEMENTED;
}

ssize_t erofs_source_pwrite(struct erofs_vfile *vf, const void *buf,
			    u64 offset, size_t len)
{
	return -EROFS_UNIMPLEMENTED;
}

int erofs_source_fsync(struct erofs_vfile *vf)
{
	return -EROFS_UNIMPLEMENTED;
}

int erofs_source_fallocate(struct erofs_vfile *vf,
			   u64 offset, size_t len, bool pad)
{
	return -EROFS_UNIMPLEMENTED;
}

int erofs_source_ftruncate(struct erofs_vfile *vf, u64 length)
{
	return -EROFS_UNIMPLEMENTED;
}

ssize_t erofs_source_read(struct erofs_vfile *vf, void *buf,
			  size_t bytes)
{
	struct liberofs_file *source_file =
			reinterpret_cast<struct liberofs_file *>(vf->ops);

	if (!source_file)
		return -EINVAL;
	return source_file->file->read(buf, bytes);
}

off_t erofs_source_lseek(struct erofs_vfile *vf, u64 offset, int whence)
{
	struct liberofs_file *source_file =
			reinterpret_cast<struct liberofs_file *>(vf->ops);
	if (!source_file)
		return -EINVAL;
	return source_file->file->lseek(offset, whence);
}
