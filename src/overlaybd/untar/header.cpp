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


/*
 * In place, rewrite name to compress multiple /, eliminate ., and process ..
 *
 * clean_name iteratively does the following until no further processing can be done:
 *		1. Reduce multiple slashes to a single slash.
 *		2. Eliminate . path name elements (the current directory).
 *		3. Eliminate .. path name elements (the parent directory) and the non-. non-.., element that precedes them.
 *		4. Eliminate .. elements that begin a rooted path, that is, replace /.. by / at the beginning of a path.
 *		5. Leave intact .. elements that begin a non-rooted path.
 * If the result of this process is a null string, cleanname returns the string ".", representing the current directory.
 *
 * See also Rob Pike, “Lexical File Names in Plan 9 or Getting Dot-Dot Right,”
 * https://9p.io/sys/doc/lexnames.html
 */
#define SEP(x)	((x) == '/' || (x) == 0)
char* clean_name(char *name) {
    char *p, *q, *dotdot;
    int rooted;

    rooted = name[0] == '/';

    /*
     * invariants:
     *	p points at beginning of path element we're considering.
     *	q points just past the last path element we wrote (no slash).
     *	dotdot points just past the point where .. cannot backtrack
     *		any further (no slash).
     */
    p = q = dotdot = name+rooted;
    while(*p) {
        if(p[0] == '/')	/* null element */
            p++;
        else if(p[0] == '.' && SEP(p[1]))
            p += 1;	/* don't count the separator in case it is nul */
        else if(p[0] == '.' && p[1] == '.' && SEP(p[2])) {
            p += 2;
            if(q > dotdot) {	/* can backtrack */
                while(--q > dotdot && *q != '/')
                    ;
            } else if(!rooted) {	/* /.. is / but ./../ is .. */
                if(q != name)
                    *q++ = '/';
                *q++ = '.';
                *q++ = '.';
                dotdot = q;
            }
        } else {	/* real path element */
            if(q != name+rooted)
                *q++ = '/';
            while((*q = *p) != '/' && *q != 0)
                p++, q++;
        }
    }
    if(q == name)	/* empty string is really ``.'' */
        *q++ = '.';
    *q = '\0';
    return name;
}

char* Tar::get_pathname() {
    if (pax && pax->path)
        return clean_name(pax->path);
    if (header.gnu_longname)
        return clean_name(header.gnu_longname);

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
    return clean_name(th_pathname);
}

char* Tar::get_linkname() {
    if (pax && pax->linkpath) {
        return clean_name(pax->linkpath);
    } else {
        return clean_name(header.get_linkname());
    }
}

size_t Tar::get_size() {
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
    int ret;
    sscanf(gid, "%o", &ret);
    return ret;
}

uid_t TarHeader::get_uid() {
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
