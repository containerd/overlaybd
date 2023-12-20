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
#include <photon/common/alog-stdstring.h>
#include <photon/common/enumerable.h>
#include <photon/fs/path.h>
#include <photon/common/estring.h>

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
#define SEP(x) ((x) == '/' || (x) == 0)
char *clean_name(char *name) {
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
    p = q = dotdot = name + rooted;
    while (*p) {
        if (p[0] == '/') /* null element */
            p++;
        else if (p[0] == '.' && SEP(p[1]))
            p += 1; /* don't count the separator in case it is nul */
        else if (p[0] == '.' && p[1] == '.' && SEP(p[2])) {
            p += 2;
            if (q > dotdot) { /* can backtrack */
                while (--q > dotdot && *q != '/')
                    ;
            } else if (!rooted) { /* /.. is / but ./../ is .. */
                if (q != name)
                    *q++ = '/';
                *q++ = '.';
                *q++ = '.';
                dotdot = q;
            }
        } else { /* real path element */
            if (q != name + rooted)
                *q++ = '/';
            while ((*q = *p) != '/' && *q != 0)
                p++, q++;
        }
    }
    if (q == name) /* empty string is really ``.'' */
        *q++ = '.';
    *q = '\0';
    return name;
}

std::string remove_last_slash(const std::string_view &path) {
    if (path.back() == '/')
        return std::string(path.substr(0, path.size() - 1));
    else
        return std::string(path);
}

char *TarCore::get_pathname() {
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
    if (strncmp(header.magic, TMAGIC, TMAGLEN - 1) != 0 || header.prefix[0] == '\0') {
        snprintf(th_pathname, MAXPATHLEN, "%.100s", header.name);
    } else {
        snprintf(th_pathname, MAXPATHLEN, "%.155s/%.100s", header.prefix, header.name);
    }

    /* will be deallocated in tar_close() */
    return clean_name(th_pathname);
}

char *TarCore::get_linkname() {
    if (pax && pax->linkpath)
        return clean_name(pax->linkpath);
    if (header.gnu_longlink)
        return clean_name(header.gnu_longlink);

    if (th_linkname == nullptr) {
        th_linkname = (char *)malloc(MAXPATHLEN * sizeof(char));
        if (th_linkname == nullptr)
            return nullptr;
    }

    snprintf(th_linkname, MAXPATHLEN, "%.100s", header.linkname);
    return clean_name(th_linkname);
}

size_t TarCore::get_size() {
    if (pax && pax->size > 0) {
        return pax->size;
    } else {
        return header.get_size();
    }
}

static const char ZERO_BLOCK[T_BLOCKSIZE] = {0};

int TarCore::read_header_internal(photon::fs::IFile *dump) {
    int i;
    int num_zero_blocks = 0;

    while ((i = file->read(&header, T_BLOCKSIZE)) == T_BLOCKSIZE) {
        /* two all-zero blocks mark EOF */
        if (header.name[0] == '\0' && std::memcmp(&header, ZERO_BLOCK, T_BLOCKSIZE) == 0) {
            num_zero_blocks++;
            if (!BIT_ISSET(options, TAR_IGNORE_EOT) && num_zero_blocks >= 2)
                return 0; /* EOF */
            else
                continue;
        }

        /* verify magic and version */
        if (BIT_ISSET(options, TAR_CHECK_MAGIC) &&
            strncmp(header.magic, TMAGIC, TMAGLEN - 1) != 0) {
            LOG_ERROR("failed check magic");
            return -2;
        }

        if (BIT_ISSET(options, TAR_CHECK_VERSION) &&
            strncmp(header.version, TVERSION, TVERSLEN) != 0) {
            LOG_ERROR("failed check version");
            return -2;
        }

        /* check chksum */
        if (!BIT_ISSET(options, TAR_IGNORE_CRC) && !header.crc_ok()) {
            LOG_ERROR("failed check crc");
            return -2;
        }
        if (dump) {
            if (TH_ISREG(header)) {
                off_t file_offset = file->lseek(0, SEEK_CUR);
                *(off_t*)(&header.devmajor) = file_offset; // tmp save
                LOG_DEBUG("regfile: `, inner_offset: `(expected: `)",
                    get_pathname(), *(off_t*)(&header.devmajor), file_offset);
            }
            if (dump->write(&header, T_BLOCKSIZE) != T_BLOCKSIZE) {
                LOG_ERRNO_RETURN(0, -1, "dump tarheader failed");
            }
        };
        break;
    }

    return i;
}

int TarCore::read_sepcial_file(char *&buf, photon::fs::IFile *dump) {
    size_t j, blocks;
    char *ptr;
    size_t sz = header.get_size();
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
        if (dump && dump->write(ptr, T_BLOCKSIZE) != T_BLOCKSIZE) {
            LOG_ERRNO_RETURN(0, -1, "dump tarheader failed");
        }
    }
    return sz;
}

int TarCore::read_header(photon::fs::IFile *dump) {

    if (header.gnu_longname != nullptr)
        free(header.gnu_longname);
    if (header.gnu_longlink != nullptr)
        free(header.gnu_longlink);
    memset(&(header), 0, sizeof(TarHeader));
    if (pax != nullptr) {
        delete pax;
        pax = nullptr;
    }

    int i = read_header_internal(dump);
    if (i == 0)
        return 1;
    else if (i != T_BLOCKSIZE) {
        if (i != -1)
            errno = EINVAL;
        return -1;
    }

    while (header.typeflag == GNU_LONGLINK_TYPE ||
           header.typeflag == GNU_LONGNAME_TYPE ||
           header.typeflag == PAX_HEADER ||
           header.typeflag == PAX_GLOBAL_HEADER) {
        size_t sz;
        switch (header.typeflag) {
        /* check for GNU long link extention */
        case GNU_LONGLINK_TYPE:
            sz = read_sepcial_file(header.gnu_longlink, dump);
            LOG_DEBUG("found gnu longlink ", VALUE(sz));
            if (sz < 0) return -1;
            break;
        /* check for GNU long name extention */
        case GNU_LONGNAME_TYPE:
            sz = read_sepcial_file(header.gnu_longname, dump);
            LOG_DEBUG("found gnu longname ", VALUE(sz));
            if (sz < 0) return -1;
            break;
        /* check for Pax Format Header */
        case PAX_HEADER:
            if (pax == nullptr)
                pax = new PaxHeader();
            sz = read_sepcial_file(pax->pax_buf, dump);
            LOG_DEBUG("found pax header ", VALUE(sz));
            if (sz < 0) return -1;
            i = pax->read_pax(sz);
            if (i) {
                errno = EINVAL;
                return -1;
            }
            break;
        /* check for Pax Global Header */
        case PAX_GLOBAL_HEADER:
            if (pax == nullptr)
                pax = new PaxHeader();
            sz = read_sepcial_file(pax->pax_buf, dump);
            LOG_WARN("found and ignored pax global header ", VALUE(sz));
            break;
        }

        i = read_header_internal(dump);
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
        LOG_DEBUG(VALUE(key), VALUE(value));
        records[key] = value;
        start += len;
    }

    return parse_pax_records();
}

int PaxHeader::parse_pax_records() {
    // TODO: support more pax type
    for (auto rec : records) {
        LOG_DEBUG("`->`", rec.first, rec.second);
        if (rec.first == PAX_SIZE) {
            size = std::stol(rec.second);
        } else if (rec.first == PAX_PATH) {
            path = strdup(rec.second.data());
        } else if (rec.first == PAX_LINKPATH) {
            linkpath = strdup(rec.second.data());
        } else if (estring_view(rec.first).starts_with(PAX_SCHILY_XATTR_PREFIX)) {
            LOG_DEBUG("found pax record with 'SCHILY.xattr.' prefix: `", rec.first);
        } else if (estring_view(rec.first).starts_with(PAX_GNU_SPARSE_PREFIX)) {
            LOG_WARN("found and ignored pax record with 'GNU.sparse.' prefix: `", rec.first);
        } else {
            LOG_WARN("found and ignored unknown pax record: `", rec.first);
        }
    }
    return 0;
}

mode_t TarHeader::get_mode() {
    mode_t m = (mode_t)oct_to_int(mode);
    if (!(m & S_IFMT)) {
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
