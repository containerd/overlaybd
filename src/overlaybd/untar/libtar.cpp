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
static const char ZERO_BLOCK[T_BLOCKSIZE] = {0};

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
		if (header.name[0] == '\0' && std::memcmp(&header, ZERO_BLOCK, T_BLOCKSIZE) == 0) {
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
			LOG_ERROR("failed check magic");
			return -2;
		}

		if (BIT_ISSET(options, TAR_CHECK_VERSION)
		    && strncmp(header.version, TVERSION, TVERSLEN) != 0) {
			LOG_ERROR("failed check version");
			return -2;
		}

		/* check chksum */
		if (!BIT_ISSET(options, TAR_IGNORE_CRC) && !header.crc_ok()) {
			LOG_ERROR("failed check crc");
			return -2;
		}

		break;
	}

	return i;
}

int Tar::read_sepcial_file(char *&buf) {
	size_t j, blocks;
	char *ptr;
	int sz = header.get_size();
	blocks = (sz / T_BLOCKSIZE) + (sz % T_BLOCKSIZE ? 1 : 0);
	if (blocks > ((size_t)-1 / T_BLOCKSIZE)) {
		errno = E2BIG;
		return -1;
	}
	buf = (char *)malloc(blocks * T_BLOCKSIZE);
	if (buf == nullptr)
		return -1;

	for (j = 0, ptr = buf; j < blocks; j++, ptr += T_BLOCKSIZE) {
		auto i = file->read(ptr, T_BLOCKSIZE);
		if (i != T_BLOCKSIZE) {
			if (i != -1)
				errno = EINVAL;
			return -1;
		}
	}

	return sz;
}

int Tar::read_header() {

	if (header.gnu_longname != nullptr)
		free(header.gnu_longname);
	if (header.gnu_longlink != nullptr)
		free(header.gnu_longlink);
	memset(&(header), 0, sizeof(TarHeader));
	if (pax != nullptr) {
		delete pax;
		pax = nullptr;
	}

	int i = read_header_internal();
	if (i == 0)
		return 1;
	else if (i != T_BLOCKSIZE) {
		if (i != -1)
			errno = EINVAL;
		return -1;
	}

	while (header.typeflag == GNU_LONGLINK_TYPE || 
		   header.typeflag == GNU_LONGNAME_TYPE ||
		   header.typeflag == PAX_HEADER) {
		size_t sz;
		switch (header.typeflag) {
		/* check for GNU long link extention */
		case GNU_LONGLINK_TYPE:	
			sz = read_sepcial_file(header.gnu_longlink);
			LOG_DEBUG("found gnu longlink ", VALUE(sz));
			if (sz < 0) return -1;
			break;
		/* check for GNU long name extention */
		case GNU_LONGNAME_TYPE:
			sz = read_sepcial_file(header.gnu_longname);
			LOG_DEBUG("found gnu longname ", VALUE(sz));
			if (sz < 0) return -1;
			break;
		/* check for Pax Format Header */
		case PAX_HEADER:
			if (pax == nullptr)
				pax = new PaxHeader();
			sz = read_sepcial_file(pax->pax_buf);
			LOG_DEBUG("found pax header ", VALUE(sz));
			if (sz < 0) return -1;
			i = pax->read_pax(sz);
			if (i) {
				errno = EINVAL;
				return -1;
			}
			break;
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

/*
	Each line consists of a decimal number, a space, a key string, an equals sign, a
value string, and a new line.  The decimal number indicates the length of the entire
line, including the initial length field and the trailing newline. 
An example of such a field is:
    25 ctime=1084839148.1212\n
*/
int PaxHeader::read_pax(size_t size) {
	char *start = pax_buf;
	char *end = pax_buf + size;
	while (start < end) {
		char *p = start;
		long len = strtol(p, &p, 0);
		if (len < 5 || len >= LONG_MAX)
			return -1;
		size_t sz = len - (++p - start) - 1;
		if (sz <= 0)
			return -1;
		std::string record(p, sz);
		auto pos = record.find('=');
		if (pos == std::string::npos)
			return -1;
		std::string key = record.substr(0, pos);
		std::string value = record.substr(pos + 1);
		LOG_DEBUG(VALUE(key.c_str()), VALUE(value.c_str()));
		records[key] = value;
		start += len;
	}

	return parse_pax_records();
}

int PaxHeader::parse_pax_records() {
	// TODO: support more pax type
	for (auto rec : records) {
		LOG_DEBUG("`->`", rec.first.c_str(), rec.second.c_str());
		if (rec.first == PAX_SIZE) {
			size = std::stol(rec.second);
		} else if (rec.first == PAX_PATH) {
			path = strdup(rec.second.data());
		} else if (rec.first == PAX_LINKPATH) {
			linkpath = strdup(rec.second.data());
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
		struct timeval tv[2];
		tv[0].tv_sec = tv[1].tv_sec = dir.second;
		tv[0].tv_usec = tv[1].tv_usec = 0;
		if (fs->lutimes(path.c_str(), tv) == -1) {
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
	else if (TH_ISCHR(header) || TH_ISBLK(header)) {
		if (geteuid() == 0) {
			i = extract_block_char_fifo(filename);
		} else {
			LOG_WARN("file ` ignored: skip for user namespace", filename);
			return 0;
		}
	}
	else if (TH_ISFIFO(header))
		i = extract_block_char_fifo(filename);
	else if (TH_ISGLOBALHEADER(header)) {
		LOG_WARN("PAX Global Extended Headers found and ignored");
		return 0;
	} else {
		LOG_ERROR("unhandled tar header type `", header.typeflag);
		return 1;
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
	long size = get_size();

	LOG_DEBUG("  ==> extracting: ` (` bytes)\n", filename, size);
	photon::fs::IFile *fout = fs->open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0666);
	if (fout == nullptr) {
		return -1;
	}
	DEFER({delete fout;});

	char buf[1024*1024];
	off_t pos = 0;
	size_t left = size;
	while (left > 0) {
		size_t rsz;
		if (left > 1024 * 1024)
			rsz = 1024 * 1024;
		else if (left > fs_blocksize)
			rsz = left & fs_blockmask;
		else
			rsz = (left & ~T_BLOCKMASK) ? (left & T_BLOCKMASK) + T_BLOCKSIZE : (left & T_BLOCKMASK);
		if (file->read(buf, rsz) != rsz) {
			LOG_ERRNO_RETURN(0, -1, "failed to read block");
		}
		size_t wsz = (left < rsz) ? left : rsz;
		if (fout->pwrite(buf, wsz, pos) != wsz) {
			LOG_ERRNO_RETURN(0, -1, "failed to write file");
		}
		pos += wsz;
		left -= wsz;
		// LOG_DEBUG(VALUE(rsz), VALUE(wsz), VALUE(pos), VALUE(left));
	}
	return 0;
}

int Tar::extract_hardlink(const char *filename) {
	char *linktgt = get_linkname();
	LOG_DEBUG("  ==> extracting: ` (link to `)\n", filename, linktgt);
	if (fs->link(linktgt, filename) == -1) {
		LOG_ERROR("link failed, `", strerror(errno));
		return -1;
	}
	return 0;
}

int Tar::extract_symlink(const char *filename) {
	char *linktgt = get_linkname();
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
