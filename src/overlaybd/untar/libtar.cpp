/*
**  Copyright 2022 overlaybd authors
**  Copyright 1998-2003 University of Illinois Board of Trustees
**  Copyright 1998-2003 Mark D. Roth
**  All rights reserved.
**
**  libtar.c - demo driver program for libtar
**
**  Mark D. Roth <roth@uiuc.edu>
**  Campus Information Technologies and Educational Services
**  University of Illinois at Urbana-Champaign
*/

#include "libtar.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <utime.h>
#include <set>
#include <string>
#include <photon/fs/path.h>
#include <photon/common/string_view.h>
#include <photon/fs/filesystem.h>
#include <photon/common/alog.h>
#include <photon/common/enumerable.h>
#include <photon/fs/path.h>

#define BIT_ISSET(bitmask, bit) ((bitmask) & (bit))

int mkdir_hier(photon::fs::IFileSystem *fs, const std::string_view &dir) {
	struct stat s;
	std::string path(dir);
    if (fs->lstat(path.c_str(), &s) == 0) {
		if (S_ISDIR(s.st_mode)) {
			// LOG_DEBUG("skip mkdir `", path.c_str());
			return 0;
		} else {
			errno = ENOTDIR;
			// LOG_ERROR("mkdir ` failed, `", path.c_str(), strerror(errno));
			return -1;
		}
    }

	return photon::fs::mkdir_recursive(dir, fs, 0755);
}

int Tar::read_header_internal() {
	int i;
	int num_zero_blocks = 0;

	while ((i = file->read(&header, T_BLOCKSIZE)) == T_BLOCKSIZE) {
		/* two all-zero blocks mark EOF */
		if (header.name[0] == '\0') {
			num_zero_blocks++;
			if (!BIT_ISSET(options, TAR_IGNORE_EOT)
				&& num_zero_blocks >= 2)
				return 0;	/* EOF */
			else
				continue;
		}

		/* verify magic and version */
		if (BIT_ISSET(options, TAR_CHECK_MAGIC)
			&& strncmp(header.magic, TMAGIC, TMAGLEN - 1) != 0) {
			return -2;
		}

		if (BIT_ISSET(options, TAR_CHECK_VERSION)
		    && strncmp(header.version, TVERSION, TVERSLEN) != 0) {
			return -2;
		}

		/* check chksum */
		if (!BIT_ISSET(options, TAR_IGNORE_CRC) && !header.crc_ok()) {
			return -2;
		}

		break;
	}

	return i;
}

int Tar::read_header() {
	size_t sz, j, blocks;
	char *ptr;

	if (header.gnu_longname != nullptr)
		free(header.gnu_longname);
	if (header.gnu_longlink != nullptr)
		free(header.gnu_longlink);
	memset(&(header), 0, sizeof(TarHeader));

	int i = read_header_internal();
	if (i == 0)
		return 1;
	else if (i != T_BLOCKSIZE) {
		if (i != -1)
			errno = EINVAL;
		return -1;
	}

	/* check for GNU long link extention */
	if (header.typeflag == GNU_LONGLINK_TYPE) {
		sz = header.get_size();
		blocks = (sz / T_BLOCKSIZE) + (sz % T_BLOCKSIZE ? 1 : 0);
		if (blocks > ((size_t)-1 / T_BLOCKSIZE)) {
			errno = E2BIG;
			return -1;
		}

		header.gnu_longlink = (char *)malloc(blocks * T_BLOCKSIZE);
		if (header.gnu_longlink == nullptr)
			return -1;

		for (j = 0, ptr = header.gnu_longlink; j < blocks; j++, ptr += T_BLOCKSIZE) {
			i = file->read(ptr, T_BLOCKSIZE);
			if (i != T_BLOCKSIZE) {
				if (i != -1)
					errno = EINVAL;
				return -1;
			}
		}

		i = read_header_internal();
		if (i != T_BLOCKSIZE) {
			if (i != -1)
				errno = EINVAL;
			return -1;
		}
	}

	/* check for GNU long name extention */
	if (header.typeflag == GNU_LONGNAME_TYPE) {
		sz = header.get_size();
		blocks = (sz / T_BLOCKSIZE) + (sz % T_BLOCKSIZE ? 1 : 0);
		if (blocks > ((size_t)-1 / T_BLOCKSIZE)) {
			errno = E2BIG;
			return -1;
		}
		header.gnu_longname = (char *)malloc(blocks * T_BLOCKSIZE);
		if (header.gnu_longname == nullptr)
			return -1;

		for (j = 0, ptr = header.gnu_longname; j < blocks; j++, ptr += T_BLOCKSIZE) {
			file->read(ptr, T_BLOCKSIZE);
			if (i != T_BLOCKSIZE) {
				if (i != -1)
					errno = EINVAL;
				return -1;
			}
		}

		i = read_header_internal();
		if (i != T_BLOCKSIZE) {
			if (i != -1)
				errno = EINVAL;
			return -1;
		}
	}

	return 0;
}

int Tar::set_file_perms(const char *filename) {
	mode_t mode = header.get_mode();
	uid_t uid = header.get_uid();
	gid_t gid = header.get_gid();
	struct timeval tv[2];
	tv[0].tv_sec = tv[1].tv_sec = header.get_mtime();
	tv[0].tv_usec = tv[1].tv_usec = 0;

	/* change owner/group */
	if (geteuid() == 0) {
		if (fs->lchown(filename, uid, gid) == -1) {
			LOG_ERROR("lchown failed, filename `, `", filename, strerror(errno));
			return -1;
		}
	}

	/* change access/modification time */
	if (fs->lutimes(filename, tv) == -1) {
		LOG_ERROR("lutimes failed, filename `, `", filename, strerror(errno));
		return -1;
	}

	/* change permissions */
	// skip symlink
	// NOTE: Allow hardlink to the softlink, not the real one. For example,
	//
	//	touch /tmp/zzz
	//	ln -s /tmp/zzz /tmp/xxx
	//	ln /tmp/xxx /tmp/yyy
	//
	// /tmp/yyy should be softlink which be same of /tmp/xxx, not /tmp/zzz.
	struct stat s;
	if (fs->lstat(filename, &s) == 0 && S_ISLNK(s.st_mode)) {
		return 0;
	}
	if (fs->chmod(filename, mode) == -1) {
		LOG_ERROR("chmod failed `", strerror(errno));
		return -1;
	}

	return 0;
}

int Tar::extract_all() {
	int i, count = 0;
	unpackedPaths.clear();
	dirs.clear();

	while ((i = read_header()) == 0) {
		if (extract_file() != 0) {
			LOG_ERROR("extract failed, filename `, `", get_pathname(), strerror(errno));
			return -1;
		}
		if (TH_ISDIR(header)) {
			dirs.emplace_back(std::make_pair(std::string(get_pathname()), header.get_mtime()));
		}
		count++;
	}

	// change time for all dir
	for (auto dir : dirs) {
		std::string path = dir.first;
		struct utimbuf ut;
		ut.modtime = ut.actime = dir.second;
		if (fs->utime(path.c_str(), &ut) == -1) {
			LOG_ERROR("utime failed, filename `, `", dir.first.c_str(), strerror(errno));
			return -1;
		}
	}

	LOG_DEBUG("extract ` file", count);

	return (i == 1 ? 0 : -1);
}

int Tar::extract_file() {
	int i;

	// TODO: normalize name, resove symlinks for root + filename
	std::string npath(get_pathname());
	if (npath.back() == '/') {
		npath = npath.substr(0, npath.size() - 1);
	}
	const char *filename = npath.c_str();

	// ensure parent directory exists or is created.
	photon::fs::Path p(filename);
	if (mkdir_hier(fs, p.dirname()) < 0) {
		return -1;
	}

	// whiteout files by removing the target files.
	auto cwres = convert_whiteout(filename);
	if (cwres < 0) {
		return -1;
	}
	if (cwres == 1) {
		return 0;
	}

	// check file exist
	struct stat s;
	if (fs->lstat(npath.c_str(), &s) == 0 || errno != ENOENT) {
		if (options & TAR_NOOVERWRITE) {
			errno = EEXIST;
			return -1;
		} else {
			if (!(S_ISDIR(s.st_mode) && TH_ISDIR(header))) {
				// LOG_DEBUG("remove exist file `", npath.c_str());
				if (fs->unlink(npath.c_str()) == -1 && errno != ENOENT) {
					errno = EEXIST;
					return -1;
				}
			}
		}
	}

	if (TH_ISDIR(header)) {
		i = extract_dir(filename);
		if (i == 1)
			i = 0;
	}
	else if (TH_ISREG(header))
		i = extract_regfile(filename);
	else if (TH_ISLNK(header))
		i = extract_hardlink(filename);
	else if (TH_ISSYM(header))
		i = extract_symlink(filename);
	else if (TH_ISCHR(header) || TH_ISBLK(header) || TH_ISFIFO(header))
		i = extract_block_char_fifo(filename);
	else {
		LOG_WARN("ignore unimplemented type `, filename `", header.typeflag, filename);
		i = 0;
	}

	if (i != 0) {
		return i;
	}

	i = set_file_perms(filename);
	if (i != 0) {
		return i;
	}

	unpackedPaths.insert(filename);
	return 0;
}

int Tar::extract_regfile(const char *filename) {
	mode_t mode = header.get_mode();
	size_t size = header.get_size();
	uid_t uid = header.get_uid();
	gid_t gid = header.get_gid();

	LOG_DEBUG("  ==> extracting: ` (mode `, uid `, gid `, ` bytes)\n", filename, mode, uid, gid, size);
	photon::fs::IFile *fout = fs->open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fout == nullptr) {
		return -1;
	}
	DEFER({delete fout;});

#if 0
	/* change the owner.  (will only work if run as root) */
	if (fout->fchown(fdout, uid, gid) == -1 && errno != EPERM)
	{
		return -1;
	}

	/* make sure the mode isn't inheritted from a file we're overwriting */
	if (fout->fchmod(fdout, mode & 07777) == -1)
	{
		return -1;
	}
#endif
	char buf[1024*1024];
	off_t pos = 0;
	size_t left = size;
	while (left > 0) {
		size_t rsz = T_BLOCKSIZE;
		if (left > 1024 * 1024)
			rsz = 1024 * 1024;
		else if (left > T_BLOCKSIZE)
			rsz = left / T_BLOCKSIZE * T_BLOCKSIZE;
		if (file->read(buf, rsz) != rsz) {
			LOG_ERRNO_RETURN(0, -1, "failed to read block");
		}
		size_t wsz = (left < T_BLOCKSIZE) ? left : rsz;
		if (fout->pwrite(buf, wsz, pos) != wsz) {
			LOG_ERRNO_RETURN(0, -1, "failed to write file");
		}
		pos += wsz;
		left -= wsz;
		LOG_DEBUG(VALUE(rsz), VALUE(wsz), VALUE(pos), VALUE(left));
	}
	return 0;
}

int Tar::extract_hardlink(const char *filename) {
	auto mode = header.get_mode();
	// TODO: hardlinkRootPath

	// char *linktgt = safer_name_suffix(header.get_linkname());
	char *linktgt = header.get_linkname();
	LOG_DEBUG("  ==> extracting: ` (link to `)\n", filename, linktgt);
	if (fs->link(linktgt, filename) == -1) {
		LOG_ERROR("link failed, `", strerror(errno));
		return -1;
	}
	return 0;
}

int Tar::extract_symlink(const char *filename) {
	auto mode = header.get_mode();
	// char *linktgt = safer_name_suffix(header.get_linkname());
	char *linktgt = header.get_linkname();
	LOG_DEBUG("  ==> extracting: ` (symlink to `)\n", filename, linktgt);
	if (fs->symlink(linktgt, filename) == -1) {
		LOG_ERROR("symlink failed, `", strerror(errno));
		return -1;
	}
	return 0;
}

int Tar::extract_dir(const char *filename) {
	mode_t mode = header.get_mode();

	LOG_DEBUG("  ==> extracting: ` (mode `, directory)\n", filename, mode);
	if (fs->mkdir(filename, mode) < 0) {
		if (errno == EEXIST) {
			return 1;
		} else {
			return -1;
		}
	}
	return 0;
}

int Tar::extract_block_char_fifo(const char *filename) {
	auto mode = header.get_mode();
	auto devmaj = header.get_devmajor();
	auto devmin = header.get_devminor();

	LOG_DEBUG("  ==> extracting: ` (block/char/fifo `,`)\n", filename, devmaj, devmin);
	if (fs->mknod(filename, mode, makedev(devmaj, devmin)) == -1) {
		LOG_ERROR("block/char/fifo failed, `", strerror(errno));
		return -1;
	}

	return 0;
}
