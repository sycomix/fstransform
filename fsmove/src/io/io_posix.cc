/*
 * io/io_posix.cc
 *
 *  Created on: Sep 22, 2011
 *      Author: max
 */

#include "../first.hh"

#include <cerrno>          // for errno
#include <climits>         // for PATH_MAX
#include <cstdio>          // for rename()
#include <cstring>         // for strcmp(), memset(), memcpy()
#include <dirent.h>        // for opendir(), readdir(), closedir()
#include <fcntl.h>         // for open(), mknod()
#include <sys/types.h>     // for   "        "    , lstat(), mkdir(), mkfifo(), umask()
#include <sys/stat.h>      //  "    "        "        "        "        "         "
#include <unistd.h>        //  "    "        "        "    , rmdir(), lchown(), close(), readlink(), symlink(), unlink(), read(), write()
#include <sys/time.h>      // for utimes()
#include <utime.h>         //  "    "



#include "../log.hh"       // for ff_log()

#include "io_posix.hh"     // for fm_io_posix
#include "io_posix_dir.hh" // for fm_io_posix_dir

#ifndef PATH_MAX
# define PATH_MAX 4096
#endif /* PATH_MAX */

FT_IO_NAMESPACE_BEGIN

/** default constructor */
fm_io_posix::fm_io_posix()
: super_type()
{ }

/** destructor. calls close() */
fm_io_posix::~fm_io_posix()
{
    close();
}

/** return true if this fr_io_posix is currently (and correctly) open */
bool fm_io_posix::is_open() const
{
    return !source_root().empty() && !target_root().empty();
}

/** check for consistency and open SOURCE_ROOT, TARGET_ROOT */
int fm_io_posix::open(const fm_args & args)
{
    return super_type::open(args);
}

/** close this I/O, including file descriptors to DEVICE, LOOP-FILE, ZERO-FILE and SECONDARY-STORAGE */
void fm_io_posix::close()
{
    super_type::close();
}

/**
 * return true if 'stat' information is about a directory
 */
FT_INLINE static bool fm_io_posix_is_dir(const ft_stat & stat)
{
    return S_ISDIR(stat.st_mode);
}

/**
 * return true if 'stat' information is about a regular file
 */
FT_INLINE static bool fm_io_posix_is_file(const ft_stat & stat)
{
    return S_ISREG(stat.st_mode);
}


/** core of recursive move algorithm, actually moves the whole source tree into target */
int fm_io_posix::move()
{
    if (move_rename(source_root().c_str(), target_root().c_str()) == 0)
        return 0;

    /* avoid messing up permissions of created files/directories/special-devices */
    umask(0);

    return move(source_root(), target_root());
}


/**
 * move a single file/socket/special-device or a whole directory tree
 */
int fm_io_posix::move(const ft_string & source_path, const ft_string & target_path)
{
    ft_stat stat;
    int err = 0;

    ff_log(FC_DEBUG, 0, "move()         `%s'\t-> `%s'", source_path.c_str(), target_path.c_str());

    do {
        if ((err = this->stat(source_path, stat)) != 0)
            break;
        
        if (fm_io_posix_is_file(stat)) {
            err = this->move_file(source_path, stat, target_path);
            break;
        } else if (!fm_io_posix_is_dir(stat)) {
            err = this->move_special(source_path, stat, target_path);
            break;
        }
        fm_io_posix_dir source_dir;
        if ((err = source_dir.open(source_path)))
            break;
        if ((err = this->create_dir(target_path, stat)) != 0)
            break;

        ft_string child_source = source_path, child_target = target_path;
        child_source += '/';
        child_target += '/';

        fm_io_posix_dirent * dirent;

        /* recurse on directory contents */
        while ((err = source_dir.next(dirent)) == 0 && dirent != NULL) {
            /* skip "." and ".." */
            if (!strcmp(".", dirent->d_name) || !strcmp("..", dirent->d_name))
                continue;

            child_source.resize(1 + source_path.size()); // faster than child_source = source_path + '/'
            child_source += dirent->d_name;

            child_target.resize(1 + target_path.size()); // faster than child_target = target_path + '/'
            child_target += dirent->d_name;

            if ((err = this->move(child_source, child_target)) != 0)
                break;
        }
        if (err != 0)
            break;
        if ((err = this->copy_stat(target_path, stat)) != 0)
            break;
        if ((err = this->remove_dir(source_path)) != 0)
            break;

    } while (0);
    return err;
}

/**
 * fill 'stat' with information about the file/directory/special-device 'path'
 */
int fm_io_posix::stat(const ft_string & path, ft_stat & stat)
{
    const char * str = path.c_str();
    int err = 0;
    if (lstat(str, & stat) != 0)
        err = ff_log(FC_ERROR, errno, "failed to lstat() `%s'", str);
    return err;
}

/**
 * move the special-device 'source_path' to 'target_path'.
 */
int fm_io_posix::move_special(const ft_string & source_path, const ft_stat & stat, const ft_string & target_path)
{
    const char * source = source_path.c_str(), * target = target_path.c_str();
    int err = 0;
    ff_log(FC_TRACE, 0, "move_special() `%s'\t-> `%s'", source, target);

    if (simulate_run())
        return err; 
    
    do {
        // TODO use inode_cache

        /* found a special device */
        if (S_ISCHR(stat.st_mode) || S_ISBLK(stat.st_mode) || S_ISSOCK(stat.st_mode)) {
            if (mknod(target, (stat.st_mode | 0600) & ~0077, stat.st_rdev) != 0) {
                if (!S_ISSOCK(stat.st_mode)) {
                    err = ff_log(FC_ERROR, errno, "failed to create special device `%s'", target);
                    break;
                }
                ff_log(FC_WARN, errno, "failed to create UNIX socket `%s'", target);
            }
        } else if (S_ISFIFO(stat.st_mode)) {
            if (mkfifo(target, 0600) != 0) {
                err = ff_log(FC_ERROR, errno, "failed to create named pipe `%s'", target);
                break;
            }
        } else if (S_ISLNK(stat.st_mode)) {
            char link_to[PATH_MAX+1];
            ssize_t link_len = readlink(source, link_to, PATH_MAX);
            if (link_len == -1) {
                err = ff_log(FC_ERROR, errno, "failed to read symbolic link `%s'", source);
                break;
            }
            link_to[link_len] = '\0';
            if (symlink(target, link_to) != 0) {
                err = ff_log(FC_ERROR, errno, "failed to create creating symbolic link `%s'\t-> `%s'", target, link_to);
                break;
            }

        } else {
            ff_log(FC_ERROR, 0, "special device %s has unknown type 0%"FS_OLL", cannot create it",
                         source, (ft_ull)(stat.st_mode & ~07777));
            err = -EOPNOTSUPP;
            break;
        }

        if ((err = copy_stat(target, stat)) != 0)
            break;

    } while (0);
   
    if (err == 0 && unlink(source) != 0)
        err = ff_log(FC_ERROR, errno, "failed to remove special device `%s'", source);

    return err;
}

/**
 * move the regular file 'source_path' to 'target_path'.
 */
int fm_io_posix::move_file(const ft_string & source_path, const ft_stat & stat, const ft_string & target_path)
{
    const char * source = source_path.c_str(), * target = target_path.c_str();
    int err = 0;
    ff_log(FC_TRACE, 0, "move_file()    `%s'\t-> `%s'", source, target);

    if (simulate_run())
        return err;

    // TODO use inode_cache

    int in_fd = ::open(source, O_RDONLY);
    if (in_fd < 0)
        err = ff_log(FC_ERROR, errno, "failed to open file `%s'", source);

    int out_fd = ::open(target, O_CREAT|O_WRONLY, stat.st_mode);
    if (out_fd < 0)
        err = ff_log(FC_ERROR, errno, "failed to create file `%s'", target);

    if (err == 0)
        err = copy_stream(in_fd, out_fd, source, target);

    if (in_fd >= 0)
        (void) ::close(in_fd);
    if (out_fd >= 0)
        (void) ::close(out_fd);

    if (err == 0) do {
        if ((err = copy_stat(target, stat)))
            break;

        if (unlink(source) != 0) {
            err = ff_log(FC_ERROR, errno, "failed to remove file `%s'", source);
            break;
        }

    } while (0);
    return err;
}

/**
 * try to rename a file, directory or special-device from 'source_path' to 'target_path'.
 */
int fm_io_posix::move_rename(const char * source, const char * target)
{
    int err = 0;
    do {
        if (simulate_run()) {
            err = EXDEV;
            break;
        }
        if (rename(source, target) != 0) {
            err = errno;
            break;
        }
        ff_log(FC_TRACE, 0, "move_rename()  `%s'\t-> `%s': success", source, target);

    } while (0);
    return err;
}

/**
 * copy file/stream contents from in_fd to out_fd
 */
int fm_io_posix::copy_stream(int in_fd, int out_fd, const char * source, const char * target)
{
    char buf[65536];
    ssize_t got, sent, chunk;
    int err = 0;
    while (err == 0) {
        got = read(in_fd, buf, sizeof(buf));
        if (got < 0)
            err = ff_log(FC_ERROR, errno, "error reading from %s", source);
        if (got <= 0)
            break;

        // TODO: detect and create holes
        sent = 0;
        while (sent < got) {
            while ((chunk = write(out_fd, buf + sent, got - sent)) == -1 && errno == EINTR)
                ;
            if (chunk <= 0) {
                err = ff_log(FC_ERROR, errno, "error writing to %s", target);
                break;
            }
            sent += chunk;
        }
    }
    return err;
}

/**
 * copy the permission bits, owner/group and timestamps from 'stat' to 'target'
 */
int fm_io_posix::copy_stat(const char * target, const ft_stat & stat)
{
    int err = 0;
    do {
        struct timeval time_buf[2];
        time_buf[0].tv_sec = stat.st_atime;
        time_buf[1].tv_sec = stat.st_mtime;
        time_buf[0].tv_usec = time_buf[1].tv_usec = 0;
       
        if (utimes(target, time_buf) != 0)
	    ff_log(FC_WARN, errno, "warning: cannot change file/directory `%s' timestamps", target);
    } while (0);
   
    do {
        if (lchown(target, stat.st_uid, stat.st_gid) != 0) {
            err = ff_log(FC_ERROR, errno, "failed to change file/directory `%s' owner/group to %"FS_ULL"/%"FS_ULL,
                         target, (ft_ull)stat.st_uid, (ft_ull)stat.st_gid);
            break;
        }
        /*
         * 1. chmod() on a symbolic link has no sense, don't do it
         * 2. chmod() must be performed AFTER lchown(), because lchown() will reset SUID bits
         */
        if (!S_ISLNK(stat.st_mode) && chmod(target, stat.st_mode) != 0) {
            err = ff_log(FC_ERROR, errno, "failed to change file/directory `%s' mode to 0%"FS_OLL,
                         target, (ft_ull)stat.st_mode);
            break;
        }
    } while (0);
    return err;
}

/** create a target directory, copying its mode and other meta-data from 'stat' */
int fm_io_posix::create_dir(const ft_string & path, const ft_stat & stat)
{
    const char * dir = path.c_str();
    int err = 0;
    ff_log(FC_TRACE, 0, "create_dir()   `%s'", dir);

    do {
        if (simulate_run())
            break;

        if (mkdir(dir, 0700) != 0) {
            err = ff_log(FC_ERROR, errno, "failed to create directory `%s'", dir);
            break;
        }
    } while (0);
    return err;
}

/** remove a source directory */
int fm_io_posix::remove_dir(const ft_string & path)
{
    const char * dir = path.c_str();
    int err = 0;
    ff_log(FC_TRACE, 0, "remove_dir()   `%s'", dir);
    do {
        if (simulate_run())
            break;

        if (rmdir(dir) != 0) {
            err = ff_log(FC_ERROR, errno, "failed to remove directory `%s'", dir);
            break;
        }
    } while (0);
    return err;
}

FT_IO_NAMESPACE_END
