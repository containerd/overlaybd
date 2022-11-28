/*
**  Copyright 2022 overlaybd authors
**  Copyright 1998-2003 University of Illinois Board of Trustees
**  Copyright 1998-2003 Mark D. Roth
**  All rights reserved.
**
**  libtar.h - header file for libtar library
**
**  Mark D. Roth <roth@uiuc.edu>
**  Campus Information Technologies and Educational Services
**  University of Illinois at Urbana-Champaign
*/

#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <tar.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <photon/fs/filesystem.h>
#include <set>
#include <string>
#include <list>
#include <map>

#define T_BLOCKSIZE		512
#define T_NAMELEN		100
#define T_PREFIXLEN		155
#define T_MAXPATHLEN		(T_NAMELEN + T_PREFIXLEN)
#define T_BLOCKMASK		(~(uint64_t)(T_BLOCKSIZE - 1))
#define FS_BLOCKSIZE	4096

/* GNU extensions for typeflag */
#define GNU_LONGNAME_TYPE	'L'
#define GNU_LONGLINK_TYPE	'K'


static int oct_to_int(char *oct) {
	int i;
	return sscanf(oct, "%o", &i) == 1 ? i : 0;
}
static size_t oct_to_size(char *oct) {
	size_t i;
	return sscanf(oct, "%zo", &i) == 1 ? i : 0;
}
#define int_to_oct(num, oct, octlen) \
	snprintf((oct), (octlen), "%*lo ", (octlen) - 2, (unsigned long)(num))

static void int_to_oct_nonull(int num, char *oct, size_t octlen) {
    snprintf(oct, octlen, "%*lo", (int)(octlen - 1), (unsigned long)num);
    oct[octlen - 1] = ' ';
}

char* clean_name(char *name);


class TarHeader {
public:
	char name[100];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char chksum[8];
	char typeflag;
	char linkname[100];
	char magic[6];
	char version[2];
	char uname[32];
	char gname[32];
	char devmajor[8];
	char devminor[8];
	char prefix[155];
	char padding[12];
	char *gnu_longname = nullptr;
	char *gnu_longlink = nullptr;

	mode_t get_mode();
	gid_t get_gid();
	uid_t get_uid();
	int get_mtime() { return oct_to_int(mtime); }
	int get_crc() { return oct_to_int(chksum); }
	int get_size() { return oct_to_size(size); }
	int get_devmajor() { return oct_to_int(devmajor); }
	int get_devminor() { return oct_to_int(devminor); }
	char *get_linkname() { return gnu_longlink ? gnu_longlink : linkname; }
	bool crc_ok() {
		return (get_crc() == crc_calc() || get_crc() == signed_crc_calc());
	}
	int crc_calc();
	int signed_crc_calc();
};

/* PAX format */
#define PAX_HEADER			'x'
#define PAX_GLOBAL_HEADER	'g'
#define PAX_PATH			"path"
#define PAX_LINKPATH		"linkpath"
#define PAX_SIZE			"size"
#define PAX_UID				"uid"
#define PAX_GID				"gid"
#define PAX_UNAME			"uname"
#define PAX_GNAME			"gname"
#define PAX_MTIME			"mtime"
#define PAX_ATIME			"atime"
#define PAX_CTIME			"ctime"
class PaxHeader {
public:
	char *path = nullptr;
	char *linkpath = nullptr;
	long size = -1;
	uid_t uid = -1;
	gid_t gid = -1;
	char *uname = nullptr;
	char *gname = nullptr;
	long mtime = -1;
	long atime = -1;
	long ctime = -1;

	char *pax_buf = nullptr;
	std::map<std::string, std::string> records;

	~PaxHeader() {
		if (pax_buf) free(pax_buf);
		if (path) free(path);
		if (linkpath) free(linkpath);
	}

	int read_pax(size_t size);
private:
	int parse_pax_records();
};

class Tar {
public:
	photon::fs::IFileSystem *fs = nullptr; // target
	photon::fs::IFile *file = nullptr; // source
	int options;
	uint64_t fs_blocksize;
	uint64_t fs_blockmask;
	TarHeader header;
	char *th_pathname = nullptr;
	std::set<std::string> unpackedPaths;
	std::list<std::pair<std::string, int>> dirs;	// <path, utime>
	PaxHeader *pax = nullptr;

	Tar(photon::fs::IFile *file, photon::fs::IFileSystem *fs, int options,
		uint64_t fs_blocksize = FS_BLOCKSIZE)
		: file(file), fs(fs), options(options), fs_blocksize(fs_blocksize) {
		fs_blockmask = ~(fs_blocksize - 1);
	}
	~Tar() {
		if (th_pathname != nullptr)
			free(th_pathname);
		if (pax != nullptr)
			delete pax;
	}
	char* get_pathname();
	char* get_linkname();
	long get_size();
	int extract_all();

private:
	int read_header();
	int read_header_internal();
	int read_sepcial_file(char *&buf);

	int extract_file();
	int extract_regfile(const char *filename);
	int extract_hardlink(const char *filename);
	int extract_symlink(const char *filename);
	int extract_dir(const char *filename);
	int extract_block_char_fifo(const char *filename);

	int set_file_perms(const char *filename);
	int convert_whiteout(const char *filename);
};

/* constant values for the TAR options field */
#define TAR_GNU			 1	/* use GNU extensions */
#define TAR_VERBOSE		 2	/* output file info to stdout */
#define TAR_NOOVERWRITE		 4	/* don't overwrite existing files */
#define TAR_IGNORE_EOT		 8	/* ignore double zero blocks as EOF */
#define TAR_CHECK_MAGIC		16	/* check magic in file header */
#define TAR_CHECK_VERSION	32	/* check version in file header */
#define TAR_IGNORE_CRC		64	/* ignore CRC in file header */

/* this is obsolete - it's here for backwards-compatibility only */
#define TAR_IGNORE_MAGIC	0

const char libtar_version[] = "1";

/* determine file type */
#define TH_ISREG(h)	(h.typeflag == REGTYPE \
			 || h.typeflag == AREGTYPE \
			 || h.typeflag == CONTTYPE \
			 || (S_ISREG((mode_t)oct_to_int(h.mode)) \
			     && h.typeflag != LNKTYPE))
#define TH_ISLNK(h)	(h.typeflag == LNKTYPE)
#define TH_ISSYM(h)	(h.typeflag == SYMTYPE \
			 || S_ISLNK((mode_t)oct_to_int(h.mode)))
#define TH_ISCHR(h)	(h.typeflag == CHRTYPE \
			 || S_ISCHR((mode_t)oct_to_int(h.mode)))
#define TH_ISBLK(h)	(h.typeflag == BLKTYPE \
			 || S_ISBLK((mode_t)oct_to_int(h.mode)))
#define TH_ISDIR(h)	(h.typeflag == DIRTYPE \
			 || S_ISDIR((mode_t)oct_to_int(h.mode)) \
			 || (h.typeflag == AREGTYPE \
			     && strlen(h.name) \
			     && (h.name[strlen(h.name) - 1] == '/')))
#define TH_ISFIFO(h)	(h.typeflag == FIFOTYPE \
			 || S_ISFIFO((mode_t)oct_to_int(h.mode)))
#define TH_ISGLOBALHEADER(h) (h.typeflag == PAX_GLOBAL_HEADER)


