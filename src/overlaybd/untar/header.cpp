#include "libtar.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <set>
#include <string>
#include <photon/fs/path.h>
#include <photon/common/string_view.h>
#include <photon/fs/filesystem.h>
#include <photon/common/alog.h>
#include <photon/common/enumerable.h>
#include <photon/fs/path.h>

char* safer_name_suffix(char const *file_name) {
	char const *p, *t;
	p = t = file_name;
	while (*p == '/') t = ++p;
	while (*p) {
		while (p[0] == '.' && p[0] == p[1] && p[2] == '/') {
			p += 3;
			t = p;
		}
		/* advance pointer past the next slash */
		while (*p && (p++)[0] != '/');
	}

	if (!*t) {
		t = ".";
	}

	if (t != file_name) {
		/* TODO: warn somehow that the path was modified */
	}
	return (char*)t;
}

char* Tar::get_pathname() {
	if (pax && pax->path)
		return safer_name_suffix(pax->path);
	if (header.gnu_longname)
		return safer_name_suffix(header.gnu_longname);

	/* allocate the th_pathname buffer if not already */
	if (th_pathname == nullptr) {
		th_pathname = (char *)malloc(MAXPATHLEN * sizeof(char));
		if (th_pathname == nullptr)
			/* out of memory */
			return nullptr;
	}

	/*
	 * Old GNU headers (also used by newer GNU tar when doing incremental
	 * dumps) use the POSIX prefix field for many other things, such as
	 * mtime and ctime. New-style GNU headers don't, but also don't use the
	 * POSIX prefix field. Thus, only honor the prefix field if the archive
	 * is actually a POSIX archive. This is the same logic as GNU tar uses.
	 */
	if (strncmp(header.magic, TMAGIC, TMAGLEN - 1) != 0 || header.prefix[0] == '\0'){
		snprintf(th_pathname, MAXPATHLEN, "%.100s", header.name);
	} else {
		snprintf(th_pathname, MAXPATHLEN, "%.155s/%.100s", header.prefix, header.name);
	}

	/* will be deallocated in tar_close() */
	return safer_name_suffix(th_pathname);
}

char* Tar::get_linkname() {
	if (pax && pax->linkpath) {
		return pax->linkpath;
	} else {
		return header.get_linkname();
	}
}

long Tar::get_size() {
	if (pax && pax->size > 0) {
		return pax->size;
	} else {
		return header.get_size();
	}
}

mode_t TarHeader::get_mode() {
	mode_t m = (mode_t)oct_to_int(mode);
	if (! (m & S_IFMT)) {
		switch (typeflag) {
		case SYMTYPE:
			m |= S_IFLNK;
			break;
		case CHRTYPE:
			m |= S_IFCHR;
			break;
		case BLKTYPE:
			m |= S_IFBLK;
			break;
		case DIRTYPE:
			m |= S_IFDIR;
			break;
		case FIFOTYPE:
			m |= S_IFIFO;
			break;
		case AREGTYPE:
			if (name[strlen(name) - 1] == '/') {
				m |= S_IFDIR;
				break;
			}
			/* FALLTHROUGH */
		case LNKTYPE:
		case REGTYPE:
		default:
			m |= S_IFREG;
		}
	}

	return m;
}

gid_t TarHeader::get_gid() {
	struct group *gr = getgrnam(gname);
	if (gr != NULL)
		return gr->gr_gid;
	/* if the group entry doesn't exist */
	int ret;
	sscanf(gid, "%o", &ret);
	return ret;
}

uid_t TarHeader::get_uid() {
	struct passwd *pw = getpwnam(uname);
	if (pw != NULL)
		return pw->pw_uid;
	/* if the password entry doesn't exist */
	int ret;
	sscanf(uid, "%o", &ret);
	return ret;
}

/* calculate a signed header checksum */
int TarHeader::signed_crc_calc() {
	int i, sum = 0;
	for (i = 0; i < T_BLOCKSIZE; i++)
		sum += ((signed char *)(this))[i];
	for (i = 0; i < 8; i++)
		sum += (' ' - (signed char)chksum[i]);
	return sum;
}

/* calculate header checksum */
int TarHeader::crc_calc() {
	int i, sum = 0;

	for (i = 0; i < T_BLOCKSIZE; i++)
		sum += ((unsigned char *)(this))[i];
	for (i = 0; i < 8; i++)
		sum += (' ' - (unsigned char)chksum[i]);

	return sum;
}
