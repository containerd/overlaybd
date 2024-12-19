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
#include "erofs_fs.h"
#include "erofs/inode.h"
#include "erofs/dir.h"
#include <dirent.h>
#include <vector>

struct ErofsFileSystem::ErofsFileSystemInt {
	struct erofs_sb_info sbi;
	struct liberofs_file target_file;
};
struct ErofsFile::ErofsFileInt {
	struct erofs_inode inode;
};

// ErofsFile
ErofsFile::ErofsFile(ErofsFileSystem *fs): fs(fs)
{
	ErofsFileSystem::ErofsFileSystemInt *fs_private = fs->fs_private;

	file_private = new ErofsFileInt;
	if (!file_private)
		LOG_ERROR("[erofs_fs] fail to prepare for file_private");

	memset(&file_private->inode, 0, sizeof(struct erofs_inode));
	file_private->inode.sbi = &fs_private->sbi;
}

ErofsFile::~ErofsFile() {
	delete file_private;
	file_private = NULL;
}

photon::fs::IFileSystem *ErofsFile::filesystem() { return fs; }

struct liberofs_nameidata {
	struct erofs_sb_info *sbi;
	erofs_nid_t	nid;
};

static int liberofs_link_path_walk(const char *name,
				   struct liberofs_nameidata *nd);

static struct erofs_dirent *liberofs_find_dirent(void *data, const char *name,
						 unsigned int len,
						 unsigned int nameoff,
						 unsigned int maxsize)
{
	struct erofs_dirent *de = (struct erofs_dirent *)data;
	const struct erofs_dirent *end = (struct erofs_dirent *)((char *)data + nameoff);

	while (de < end) {
		const char *de_name;
		unsigned int de_namelen;

		nameoff = le16_to_cpu(de->nameoff);
		de_name = (char *)((char *)data + nameoff);
		if (de + 1 >= end)
			de_namelen = strnlen(de_name, maxsize - nameoff);
		else
			de_namelen = le16_to_cpu(de[1].nameoff - nameoff);

		if (nameoff + de_namelen > maxsize) {
			LOG_ERROR("[erofs] bogus dirent");
			return (struct erofs_dirent *)ERR_PTR(-EINVAL);
		}

		if (len == de_namelen && !memcmp(de_name, name, de_namelen))
			return de;
		++de;
	}
	return NULL;
}

static int liberofs_namei(struct liberofs_nameidata *nd, const char *name,
			  unsigned int len)
{
	erofs_nid_t nid = nd->nid;
	int ret;
	char buf[EROFS_MAX_BLOCK_SIZE];
	struct erofs_sb_info *sbi = nd->sbi;
	struct erofs_inode vi = {};
	erofs_off_t offset;

	vi.sbi = sbi;
	vi.nid = nid;
	ret = erofs_read_inode_from_disk(&vi);
	if (ret)
		return ret;

	offset = 0;
	while (offset < vi.i_size) {
		erofs_off_t maxsize = min_t(erofs_off_t,
				vi.i_size - offset, erofs_blksiz(sbi));
		struct erofs_dirent *de = (struct erofs_dirent *)buf;
		unsigned int nameoff;

		ret = erofs_pread(&vi, buf, maxsize, offset);
		if (ret)
			return ret;

		nameoff = le16_to_cpu(de->nameoff);
		if (nameoff < sizeof(struct erofs_dirent) ||
			nameoff >= erofs_blksiz(sbi))
				LOG_ERRNO_RETURN(-EINVAL, -EINVAL, "[erofs] invalid nameoff");

		de = liberofs_find_dirent(buf, name, len, nameoff, maxsize);
		if (IS_ERR(de))
			return PTR_ERR(de);

		if (de) {
			nd->nid = le64_to_cpu(de->nid);
			return 0;
		}
		offset += maxsize;
	}
	return -ENOENT;
}


static int liberofs_step_into_link(struct liberofs_nameidata *nd,
				   struct erofs_inode *vi)
{
	char buf[PATH_MAX];
	int err;

	if (vi->i_size > PATH_MAX)
		return -EINVAL;
	memset(buf, 0, sizeof(buf));
	err = erofs_pread(vi, buf, vi->i_size, 0);
	if (err)
		return err;
	return liberofs_link_path_walk(buf, nd);
}

static int liberofs_link_path_walk(const char *name,
				   struct liberofs_nameidata *nd)
{
	struct erofs_inode vi;
	erofs_nid_t nid;
	const char *p;
	int ret;

	if (*name == '/')
		nd->nid = nd->sbi->root_nid;

	while (*name == '/')
		name ++;

	while (*name != '\0') {
		p = name;
		do {
			++p;
		} while (*p != '\0' && *p != '/');

		nid = nd->nid;
		ret = liberofs_namei(nd, name, p - name);
		if (ret)
			return ret;
		vi.sbi = nd->sbi;
		vi.nid = nd->nid;
		ret = erofs_read_inode_from_disk(&vi);
		if (ret)
			return ret;
		if (S_ISLNK(vi.i_mode)) {
			nd->nid = nid;
			ret = liberofs_step_into_link(nd, &vi);
			if (ret)
				return ret;
		}
		for (name = p; *name == '/'; ++name)
			;
	}
	return 0;
}


static int do_erofs_ilookup(const char *path, struct erofs_inode *vi)
{
	int ret;
	struct liberofs_nameidata nd = {.sbi = vi->sbi};

	nd.nid = vi->sbi->root_nid;
	ret = liberofs_link_path_walk(path, &nd);
	if (ret)
		return ret;
	vi->nid = nd.nid;
	return erofs_read_inode_from_disk(vi);
}

int ErofsFile::fstat(struct stat *buf)
{
	buf->st_mode = file_private->inode.i_mode;
	buf->st_nlink = file_private->inode.i_nlink;
	buf->st_size = file_private->inode.i_size;
	buf->st_blocks = roundup(file_private->inode.i_size,
				 erofs_blksiz(file_private->inode.sbi)) >> 9;
	buf->st_uid = file_private->inode.i_uid;
	buf->st_gid = file_private->inode.i_gid;
	buf->st_ctime = file_private->inode.i_mtime;
	buf->st_mtime = file_private->inode.i_mtime;
	buf->st_atime = file_private->inode.i_mtime;
	return 0;
}

int ErofsFile::fiemap(struct photon::fs::fiemap *map)
{
	photon::fs::fiemap_extent *ext_buf = &map->fm_extents[0];
	struct erofs_map_blocks erofs_map;
	int err;

	map->fm_mapped_extents = 0;
	erofs_map.index = UINT_MAX;
	erofs_map.m_la = 0;

	while (erofs_map.m_la < file_private->inode.i_size) {
		err = erofs_map_blocks(&file_private->inode, &erofs_map, 0);
		if (err)
			LOG_ERROR_RETURN(err, err, "[erofs] Fail to map erofs blocks");
		ext_buf[map->fm_mapped_extents].fe_physical = erofs_map.m_pa;
		ext_buf[map->fm_mapped_extents].fe_length = erofs_map.m_plen;
		map->fm_mapped_extents += 1;
		erofs_map.m_la += erofs_map.m_llen;
	}
	return 0;
}

ssize_t ErofsFile::pread(void *buf, size_t count, off_t offset)
{
	struct erofs_inode *inode = &file_private->inode;
	struct erofs_map_blocks map;
	erofs_off_t ptr = offset;
	ssize_t read = 0;
	int ret;

	map.index = UINT_MAX;
	while (ptr < offset + count) {
		char *estart = (char*)buf + ptr - offset;
		erofs_off_t eend, moff = 0;
		map.m_la = ptr;
		ret = erofs_map_blocks(inode, &map, 0);
		if (ret || map.m_plen != map.m_llen)
			LOG_ERROR_RETURN(0, -1, "[erofs_fs] fail to map blocks");
		eend = std::min(offset + count, map.m_la + map.m_llen);
		if (ptr < map.m_la)
			LOG_ERROR_RETURN(0, -1, "[erofs_fs] invalid read offset");
		if (!(map.m_flags & EROFS_MAP_MAPPED)) {
			if (!map.m_llen) {
				/* reached EOF */
				memset((void*)estart, 0, offset + count - ptr);
				ptr = offset + count;
				continue;
			}
			memset((void*)estart, 0, eend - ptr);
			ptr = eend;
			continue;;
		}
		if (ptr > map.m_la) {
			moff = ptr - map.m_la;
			map.m_la = ptr;
		}
		ret = erofs_read_one_data(inode, &map, estart, moff,
					  eend - map.m_la);
		if (ret)
			return ret;
		read += eend - map.m_la;
		ptr = eend;
	}
	return read;
}

ssize_t ErofsFile::fgetxattr(const char *name, void *value, size_t size)
{
    ssize_t value_size = 0;

    value_size = erofs_getxattr(&file_private->inode, name, NULL, 0);
    if (value_size < 0)
        LOG_ERROR_RETURN(-1, size, "[erofs] fail to get xattr `", name);
    if ((size_t)value_size > size)
        LOG_ERROR_RETURN(-1, -1, "[erofs] buffer is too small to put xattr value of `", name);
    return erofs_getxattr(&file_private->inode, name, (char*)value, value_size);
}

ssize_t ErofsFile::flistxattr(char *list, size_t size)
{
    ssize_t kllen;

    kllen = erofs_listxattr(&file_private->inode, NULL, 0);
    if (kllen < 0)
        LOG_ERROR_RETURN(-1, kllen, "[erofs] fail to list xattr");
    if ((size_t)kllen > size)
        LOG_ERROR_RETURN(-1, -1, "[erofs buffer size is too small to put xattrs");
    if (erofs_listxattr(&file_private->inode, list, kllen) != kllen)
        LOG_ERROR_RETURN(-1, -1, "[erofs] fail to list xattr");
    return kllen;
}

EROFS_UNIMPLEMENTED_FUNC(int, ErofsFile, fsetxattr(const char *name, const void *value, size_t size, int flags), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFile, fremovexattr(const char *name), -EROFS_UNIMPLEMENTED)

// ErofsFileSystem
EROFS_UNIMPLEMENTED_FUNC(photon::fs::IFile*, ErofsFileSystem, open(const char *pathname, int flags, mode_t mode), NULL)
EROFS_UNIMPLEMENTED_FUNC(photon::fs::IFile*, ErofsFileSystem, creat(const char *pathname, mode_t mode), NULL)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, mkdir(const char *pathname, mode_t mode), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, rmdir(const char *pathname), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, symlink(const char *oldname, const char *newname), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(ssize_t, ErofsFileSystem, readlink(const char *path, char *buf, size_t bufsiz), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, link(const char *oldname, const char *newname), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, rename(const char *oldname, const char *newname), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, unlink(const char *filename), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, chmod(const char *pathname, mode_t mode), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, chown(const char *pathname, uid_t owner, gid_t group), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, lchown(const char *pathname, uid_t owner, gid_t group), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, statfs(const char *path, struct statfs *buf), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, statvfs(const char *path, struct statvfs *buf), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, lstat(const char *path, struct stat *buf), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, access(const char *pathname, int mode), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, truncate(const char *path, off_t length), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, utime(const char *path, const struct utimbuf *file_times), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, utimes(const char *path, const struct timeval times[2]), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, lutimes(const char *path, const struct timeval times[2]), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, mknod(const char *path, mode_t mode, dev_t dev), -EROFS_UNIMPLEMENTED)
EROFS_UNIMPLEMENTED_FUNC(int, ErofsFileSystem, syncfs(), -EROFS_UNIMPLEMENTED)

ErofsFileSystem::ErofsFileSystem(photon::fs::IFile *imgfile, uint64_t blksize)
{
	fs_private = new ErofsFileSystemInt;

	if (!fs_private)
		LOG_ERROR("[erofs] Fail to prepare fs_private: no mem.");

	// init target_file
	fs_private->target_file.ops.pread = erofs_target_pread;
	fs_private->target_file.ops.pwrite = erofs_target_pwrite;
	fs_private->target_file.ops.pread = erofs_target_pread;
	fs_private->target_file.ops.pwrite = erofs_target_pwrite;
	fs_private->target_file.ops.fsync = erofs_target_fsync;
	fs_private->target_file.ops.fallocate = erofs_target_fallocate;
	fs_private->target_file.ops.ftruncate = erofs_target_ftruncate;
	fs_private->target_file.ops.read = erofs_target_read;
	fs_private->target_file.ops.lseek = erofs_target_lseek;
	fs_private->target_file.file = imgfile;
	fs_private->target_file.cache = new ErofsCache(imgfile, 128);
	if (!fs_private->target_file.cache)
		LOG_ERROR("[erofs] Fail to prepare target_file.cache: no mem.");

	// init sbi
	memset(&fs_private->sbi, 0, sizeof(struct erofs_sb_info));
	fs_private->sbi.blkszbits = ilog2(blksize);
	fs_private->sbi.bdev.ops = &fs_private->target_file.ops;
	fs_private->target_file.file->lseek(0,0);
	fs_private->sbi.devsz = INT64_MAX;
	if (erofs_read_superblock(&fs_private->sbi))
		LOG_ERROR("[erofs] Fail to read_super_block");
}

ErofsFileSystem::~ErofsFileSystem()
{
	delete fs_private->target_file.cache;
	delete fs_private;
	fs_private = NULL;
}

int ErofsFileSystem::stat(const char *path, struct stat *buf)
{
	struct erofs_inode vi;
	int err;

	vi.sbi = &fs_private->sbi;
	err = do_erofs_ilookup(path, &vi);
	if (err)
		LOG_ERRNO_RETURN(err, err, "[erofs] Fail to lookup inode");
	buf->st_mode = vi.i_mode;
	buf->st_nlink = vi.i_nlink;
	buf->st_size = vi.i_size;
	buf->st_blocks = roundup(vi.i_size, erofs_blksiz(vi.sbi)) >> 9;
	buf->st_uid = vi.i_uid;
	buf->st_gid = vi.i_gid;
	buf->st_ctime = vi.i_mtime;
	buf->st_mtime = vi.i_mtime;
	buf->st_atime = vi.i_mtime;
	return 0;
}

photon::fs::IFile* ErofsFileSystem::open(const char *pathname, int flags)
{
	ErofsFile *file = new ErofsFile(this);
	int err;

	err = do_erofs_ilookup(pathname, &file->file_private->inode);
	if (err) {
		delete file;
		LOG_ERROR_RETURN(-err, nullptr, "[erofs] Fail to lookup inode by path");
	}
	return file;
}

struct liberofs_dir_context {
	struct erofs_dir_context ctx;
	std::vector<::dirent> *dirs;
};

static int liberofs_readdir(struct erofs_dir_context *ctx)
{
	struct liberofs_dir_context *libctx =
			reinterpret_cast<struct liberofs_dir_context *>(ctx);
	std::vector<::dirent> *dirs  = libctx->dirs;
	struct dirent tmpdir;

	if (ctx->dot_dotdot)
		return 0;

	tmpdir.d_ino = (ino_t) ctx->de_nid;
	tmpdir.d_off = 0;
	tmpdir.d_reclen = sizeof(struct erofs_dirent);
	if (ctx->de_namelen > sizeof(tmpdir.d_name))
		LOG_ERROR_RETURN(-EINVAL, -EINVAL, "[erofs] Invalid name length");
	memset(tmpdir.d_name, 0, sizeof(tmpdir.d_name));
	memcpy(tmpdir.d_name, ctx->dname, ctx->de_namelen);
	dirs->emplace_back(tmpdir);
	return 0;
}

static int do_erofs_readdir(struct erofs_sb_info *sbi, const char *path,
			    std::vector<::dirent> *dirs)
{
	struct liberofs_dir_context ctx;
	struct erofs_inode vi;
	int err;

	vi.sbi = sbi;
	err = do_erofs_ilookup(path, &vi);
	if (err)
		LOG_ERRNO_RETURN(err, err, "[erofs] Fail to lookup inode");
	ctx.ctx.dir = &vi;
	ctx.ctx.cb = liberofs_readdir;
	ctx.dirs = dirs;

	return erofs_iterate_dir(&ctx.ctx, false);
}

photon::fs::DIR* ErofsFileSystem::opendir(const char *name)
{
	std::vector<::dirent> dirs;

	auto ret = do_erofs_readdir(&fs_private->sbi, name, &dirs);
	if (ret) {
		errno = -ret;
		return nullptr;
	}
	return new ErofsDir(dirs);
}

// ErofsDir
ErofsDir::ErofsDir(std::vector<::dirent> &dirs) : loc(0) {
	m_dirs = std::move(dirs);
	next();
}

ErofsDir::~ErofsDir() {
	closedir();
}

int ErofsDir::closedir() {
	if (!m_dirs.empty()) {
		m_dirs.clear();
	}
	return 0;
}

dirent *ErofsDir::get() {
	return direntp;
}

int ErofsDir::next() {
	if (!m_dirs.empty()) {
		if (loc < (long) m_dirs.size()) {
			direntp = &m_dirs[loc++];
		} else {
			direntp = nullptr;
		}
	}
	return direntp != nullptr ? 1 : 0;
}

void ErofsDir::rewinddir() {
	loc = 0;
	next();
}

void ErofsDir::seekdir(long loc){
	this->loc = loc;
	next();
}

long ErofsDir::telldir() {
	return loc;
}

bool erofs_check_fs(const photon::fs::IFile *imgfile)
{
	u8 data[EROFS_MAX_BLOCK_SIZE];
	struct erofs_super_block *dsb;
	photon::fs::IFile *file = const_cast<photon::fs::IFile *>(imgfile);
	int ret;

	ret = file->pread(data, EROFS_MAX_BLOCK_SIZE, 0);
	if (ret != EROFS_MAX_BLOCK_SIZE)
		LOG_ERROR_RETURN(-EIO, false, "[erofs] Fail to read superblock");
	dsb = reinterpret_cast<struct erofs_super_block *>(data + EROFS_SUPER_OFFSET);
	return le32_to_cpu(dsb->magic) == EROFS_SUPER_MAGIC_V1;
}

photon::fs::IFileSystem *erofs_create_fs(photon::fs::IFile *imgfile, uint64_t blksz)
{
	return new ErofsFileSystem(imgfile, blksz);
}
