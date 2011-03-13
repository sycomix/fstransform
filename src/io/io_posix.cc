/*
 * io/io_posix.cc
 *
 *  Created on: Feb 28, 2011
 *      Author: max
 */

#include "../first.hh"

#include <cerrno>         // for errno
#include <cstdlib>        // for malloc(), free(), posix_fallocate()
#include <cstring>        // for memset()
#include <sys/types.h>    // for open()
#include <sys/stat.h>     //  "    "
#include <fcntl.h>        //  "    "
#include <unistd.h>       // for close(), unlink()
#include <sys/mman.h>     // for mmap(), munmap()

#include "../log.hh"      // for ff_log()
#include "../util.hh"     // for ff_max2(), ff_min2()

#include "extent_posix.hh" // for ft_extent_posixs()
#include "util_posix.hh"   // for ft_posix_*() misc functions
#include "io_posix.hh"     // for ft_io_posix


FT_IO_NAMESPACE_BEGIN

char const * const ft_io::label[] = {
		"device", "loop-file", "zero-file", "secondary-storage", "primary-storage", "storage"
};

/** default constructor */
ft_io_posix::ft_io_posix(ft_job & job)
: super_type(job), storage_mmap(MAP_FAILED), storage_mmap_size(0)
{
    /* mark fd[] as invalid: they are not open yet */
    for (ft_size i = 0; i < FC_ALL_FILE_COUNT; i++)
        fd[i] = -1;
}

/** destructor. calls close() */
ft_io_posix::~ft_io_posix()
{
    close();
}

/** return true if a single descriptor/stream is open */
bool ft_io_posix::is_open0(ft_size i) const
{
    return fd[i] >= 0;
}

/** close a single descriptor/stream */
void ft_io_posix::close0(ft_size i)
{
    if (fd[i] >= 0) {
        if (::close(fd[i]) != 0)
            ff_log(FC_WARN, errno, "closing %s file descriptor [%d] failed", label[i], fd[i]);
        fd[i] = -1;
    }
}

/** return true if this ft_io_posix is currently (and correctly) open */
bool ft_io_posix::is_open() const
{
    return dev_length() != 0 && is_open0(FC_DEVICE);
}

/** check for consistency and open DEVICE, LOOP-FILE and ZERO-FILE */
int ft_io_posix::open(char const* const path[FC_FILE_COUNT])
{
    if (is_open()) {
        // already open!
        ff_log(FC_ERROR, 0, "unexpected call, I/O is already open");
        return EISCONN;
    }

    ft_uoff dev_len;
    ft_size i;
    int err = 0;
    ft_dev dev[FC_FILE_COUNT];

    for (i = 0; i < FC_FILE_COUNT; i++)
        dev[i] = 0;

    do {
        for (i = 0; i < FC_FILE_COUNT; i++) {
            if ((fd[i] = ::open(path[i], i == FC_DEVICE ? O_RDWR : O_RDONLY)) < 0) {
                err = ff_log(FC_ERROR, errno, "error opening %s '%s'", label[i], path[i]);
                break;
            }

            if (i == FC_DEVICE)
                /* for DEVICE, we want to know its dev_t */
                err = ff_posix_blkdev_dev(fd[i], & dev[i]);
            else
                /* for LOOP-FILE and ZERO-FILE, we want to know the dev_t of the device they are stored into */
                err = ff_posix_dev(fd[i], & dev[i]);

            if (err != 0) {
                err = ff_log(FC_ERROR, err, "error in %s fstat('%s')", label[i], path[i]);
                break;
            }

            if (i == FC_DEVICE) {
                /* for DEVICE, we also want to know its length */
                if ((err = ff_posix_blkdev_size(fd[FC_DEVICE], & dev_len)) != 0) {
                    err = ff_log(FC_ERROR, err, "error in %s ioctl('%s', BLKGETSIZE64)", label[i], path[i]);
                    break;
                }
                /* device length is retrieved ONLY here. we must remember it */
                dev_length(dev_len);
                if (ff_log_is_enabled(FC_DEBUG)) {
                    double pretty_len;
                    const char * pretty_label = ff_pretty_size(dev_len, & pretty_len);
                    ff_log(FC_DEBUG, 0, "%s length is %.2f %sbytes", label[i], pretty_len, pretty_label);
                }

            } else {
                /* for LOOP-FILE and ZERO-FILE, we check they are actually contained in DEVICE */
                if (dev[FC_DEVICE] != dev[i]) {
                    err = ff_log(FC_ERROR, EINVAL, "'%s' is device 0x%04x, but %s '%s' is contained in device 0x%04x\n",
                                 path[FC_DEVICE], (unsigned)dev[FC_DEVICE], label[i], path[i], (unsigned)dev[i]);
                    break;
                }
            }
        }
    } while (0);

    if (err != 0)
        close();

    return err;
}



/**
 * close file descriptors.
 * return 0 for success, 1 for error (logs by itself error message)
 */
void ft_io_posix::close()
{
    close_storage();
    for (ft_size i = 0; i < FC_FILE_COUNT; i++)
        close0(i);
    super_type::close();
}



/** return true if this I/O has open descriptors/streams to LOOP-FILE and FREE-SPACE */
bool ft_io_posix::is_open_extents() const
{
    bool flag = false;
    if (dev_length() != 0) {
        ft_size which[] = { FC_LOOP_FILE, FC_ZERO_FILE };
        ft_size i, n = sizeof(which)/sizeof(which[0]);
        for (i = 0; i < n; i++)
            if (!is_open0(which[i]) < 0)
                break;
        flag = i == n;
    }
    return flag;
}

/**
 * retrieve LOOP-FILE extents and FREE-SPACE extents and insert them into
 * the vectors loop_file_extents and free_space_extents.
 * the vectors will be ordered by extent ->logical.
 *
 * return 0 for success, else error (and vectors contents will be UNDEFINED).
 *
 * if success, also returns in ret_effective_block_size_log2 the log2()
 * of device effective block size.
 * the device effective block size is defined as follows:
 * it is the largest power of 2 that exactly divides all physical,
 * logical and lengths in all returned extents (both for LOOP-FILE
 * and for FREE-SPACE) and that also exactly exactly divides device length.
 *
 * must be overridden by sub-classes.
 *
 * a common trick subclasses may use to implement this method
 * is to fill the device's free space with a ZERO-FILE,
 * and actually retrieve the extents used by ZERO-FILE.
 */
int ft_io_posix::read_extents(ft_vector<ft_uoff> & loop_file_extents,
                              ft_vector<ft_uoff> & free_space_extents,
                              ft_uoff & ret_block_size_bitmask)
{
    ft_uoff block_size_bitmask = ret_block_size_bitmask;
    int err = 0;
    do {
        if (!is_open_extents()) {
            err = ENOTCONN; // not open!
            break;
        }

        /* ff_read_extents_posix() appends into ft_vector<T>, does NOT overwrite it */
        if ((err = ff_read_extents_posix(fd[FC_LOOP_FILE], dev_length(), loop_file_extents, block_size_bitmask)) != 0)
            break;
        if ((err = ff_read_extents_posix(fd[FC_ZERO_FILE], dev_length(), free_space_extents, block_size_bitmask)) != 0)
            break;

    } while (0);

    if (err == 0)
        ret_block_size_bitmask = block_size_bitmask;

    return err;
}


/**
 * close the file descriptors for LOOP-FILE and ZERO-FILE
 */
void ft_io_posix::close_extents()
{
    ft_size which[] = { FC_LOOP_FILE, FC_ZERO_FILE };
    for (ft_size i = 0; i < sizeof(which)/sizeof(which[0]); i++)
        close0(which[i]);
}


/**
 * create and open SECONDARY-STORAGE job.job_dir() + '.storage',
 * fill it with 'secondary_len' bytes of zeros and mmap() it.
 *
 * then mmap() this->primary_storage extents.
 * finally setup a virtual storage composed by 'primary_storage' extents inside DEVICE, plus secondary-storage extents.
 *
 * return 0 if success, else error
 */
int ft_io_posix::create_storage(ft_uoff secondary_len)
{
    /*
     * how to get a contiguous mmapped() memory for all the extents in primary_storage and secondary_storage:
     *
     * mmap(MAP_ANON/MAP_ANONYMOUS) the total storage size (sum(primary_storage->length)+secondary_len)
     * then incrementally replace parts of it with munmap() followed by mmap(MAP_FIXED) of each storage extent
     */
    enum { i = FC_PRIMARY_STORAGE, j = FC_SECONDARY_STORAGE };

    if (storage_mmap != MAP_FAILED || is_open0(j)) {
        // already initialized!
        ff_log(FC_ERROR, 0, "unexpected call to create_storage(), %s is already initialized",
               storage_mmap != MAP_FAILED ? label[i] : label[j]);
        // return error as already reported
        return -EISCONN;
    }

    /**
     * recompute primary_len... we could receive it from caller, but it's redundant
     * and in any case we will still need to iterate on primary_storage to mmap() it
     */
    ft_uoff primary_len = 0;
    ft_vector<ft_uoff>::iterator begin = primary_storage().begin(), iter, end = primary_storage().end();
    for (iter = begin; iter != end; ++iter) {
        primary_len += iter->second.length;
    }

    int err = 0;
    do {
        ft_uoff total_len = primary_len + secondary_len;
        const ft_size mem_len = (ft_size) total_len;
        if (mem_len < 0 || total_len != (ft_uoff) mem_len) {
            err = ff_log(FC_ERROR, EOVERFLOW, "internal error, %s + %s total length = %"FS_ULL" is larger than addressable memory", label[i], label[j], (ft_ull) total_len);
            break;
        }

        /* mmap total length as PROT_NONE, MAP_ANONYMOUS - used to reserve a large enough contiguous memory area */
#if defined(MAP_ANONYMOUS)
        storage_mmap = mmap(NULL, mem_len, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
#elif defined(MAP_ANON)
        storage_mmap = mmap(NULL, mem_len, PROT_NONE, MAP_PRIVATE|MAP_ANON, -1, 0);
#else
#  error both MAP_ANONYMOUS and MAP_ANON are missing, cannot compile io_posix.cc
#endif
        if (storage_mmap == MAP_FAILED) {
        	err = ff_log(FC_ERROR, errno, "error in %s mmap(%"FS_ULL", PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1)",
        			label[FC_STORAGE], (ft_ull) mem_len);
            break;
        } else
            ff_log(FC_DEBUG, 0, "%s: preemptively reserved contiguous RAM,"
            		" mmap(length = %"FS_ULL", PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1) = ok",
            		label[FC_STORAGE], (ft_ull) mem_len);

        storage_mmap_size = mem_len;

        if (secondary_len != 0) {
            if ((err = create_secondary_storage(secondary_len)) != 0)
                break;
        } else
            ff_log(FC_INFO, 0, "not creating %s, %s is large enough", label[j], label[i]);

        /* now incrementally replace storage_mmap with actually mmapped() storage extents */
        begin = primary_storage().begin();
        end = primary_storage().end();
        ft_size mem_offset = 0;
        for (iter = begin; err == 0 && iter != end; ++iter)
            err = replace_storage_mmap(fd[FC_DEVICE], label[i], *iter, iter-begin, mem_offset);
        if (err != 0)
            break;

        if (secondary_len != 0) {
        	if ((err = replace_storage_mmap(fd[j], label[j], secondary_storage(), 0, mem_offset)) != 0)
        		break;
        }
        if (mem_offset != storage_mmap_size) {
            ff_log(FC_ERROR, 0, "internal error, mapped %s extents in RAM used %"FS_ULL" bytes instead of expected %"FS_ULL" bytes",
            		label[FC_STORAGE], (ft_ull) mem_offset, (ft_ull) storage_mmap_size);
            err = EINVAL;
        }
    } while (0);

    if (err == 0) {
        double pretty_len = 0.0;
        const char * pretty_label = ff_pretty_size(storage_mmap_size, & pretty_len);

        ff_log(FC_NOTICE, 0, "%s%s%s: initialized and mmapped() to %.2f %sbytes of contiguous RAM",
        		(primary_len != 0 ? label[i] : ""),
        		(primary_len != 0 && secondary_len != 0 ? " and " : ""),
        		(secondary_len != 0 ? label[j] : ""),
        		pretty_len, pretty_label);
    } else
        close_storage();

    return err;
}

/**
 * replace a part of the mmapped() storage_mmap area with specified storage_extent,
 * and store mmapped() address into storage_extent.user_data().
 * return 0 if success, else error
 *
 * note: fd shoud be this->fd[FC_DEVICE] for primary storage,
 * or this->fd[FC_SECONDARY_STORAGE] for secondary storage
 */
int ft_io_posix::replace_storage_mmap(int fd, const char * label_i,
		ft_extent<ft_uoff> & storage_extent, ft_size extent_index,
		ft_size & ret_mem_offset)
{
    ft_size len = (ft_size) storage_extent.length();
    ft_size mem_start = ret_mem_offset, mem_end = mem_start + len;
    int err = 0;
    do {
        if (mem_start >= storage_mmap_size || mem_end > storage_mmap_size) {
            ff_log(FC_FATAL, 0, "internal error mapping %s extent #%"FS_ULL" in RAM!"
            		" extent (%"FS_ULL", length = %"FS_ULL") overflows total %s length = %"FS_ULL,
            		label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len,
            		label[FC_STORAGE], (ft_ull) storage_mmap_size);
            /* mark error as reported */
            err = -EINVAL;
            break;
        }
        void * addr_old = (char *) storage_mmap + mem_start;
        if (munmap(addr_old, len) != 0) {
            err = ff_log(FC_ERROR, errno, "error mapping %s extent #%"FS_ULL" in RAM,"
            		" munmap(address + %"FS_ULL", length = %"FS_ULL") failed",
            		label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len);
            break;
        }
        void * addr_new = mmap(addr_old, len, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, storage_extent.physical());
        if (addr_new == MAP_FAILED) {
            err = ff_log(FC_ERROR, errno, "error mapping %s extent #%"FS_ULL" in RAM,"
            		" mmap(address + %"FS_ULL", length = %"FS_ULL", MAP_FIXED) failed",
            		label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len);
            break;
        }
        if (addr_new != addr_old) {
            ff_log(FC_ERROR, 0, "error mapping %s extent #%"FS_ULL" in RAM,"
            		" mmap(address + %"FS_ULL", length = %"FS_ULL", MAP_FIXED)"
                   " violated MAP_FIXED and returned a different address",
                   label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len);
            /* mark error as reported */
            err = -EFAULT;
            /** try at least to munmap() this problematic extent */
            if (munmap(addr_new, len) != 0) {
                ff_log(FC_WARN, errno, "weird OS! not only mmap() violated MAP_FIXED, but subsequent munmap() failed too");
            }
            break;
        }
        ff_log(FC_TRACE, 0, "%s extent #%"FS_ULL" mapped in RAM,"
        		" mmap(address + %"FS_ULL", length = %"FS_ULL", MAP_FIXED) = ok",
        		label_i, (ft_ull) extent_index, (ft_ull) mem_start, (ft_ull) len);
        /**
         * all ok, let's store mmapped() address into extent.user_data to remember it,
         * as it could be needed for munmap() inside close_storage()
         */
        storage_extent.user_data() = (ft_size) addr_new;
        /* and remember to update ret_mem_offset */
        ret_mem_offset += len;

    } while (0);
    return err;
}


/**
 * create and open SECONDARY-STORAGE in job.job_dir() + '.storage'
 * and fill it with 'secondary_len' bytes of zeros. do not mmap() it.
 * return 0 if success, else error
 */
int ft_io_posix::create_secondary_storage(ft_uoff secondary_len)
{
    enum { j = FC_SECONDARY_STORAGE };

    std::string filepath = job_dir();
    filepath += ".storage";
    const char * path = filepath.c_str();
    int err = 0;

    do {
        const ft_uoff len = secondary_len;

        const ft_off s_len = (ft_off) len;
        if (s_len < 0 || len != (ft_uoff) s_len) {
            err = ff_log(FC_ERROR, EOVERFLOW, "internal error, %s length = %"FS_ULL" overflows type (off_t)", label[j], (ft_ull) len);
            break;
        }
        
        const ft_size mem_len = (ft_size) len;
        if (mem_len < 0 || len != (ft_uoff) mem_len) {
            err = ff_log(FC_ERROR, EOVERFLOW, "internal error, %s length = %"FS_ULL" is larger than addressable memory", label[j], (ft_ull) len);
            break;
        }

        if ((fd[j] = ::open(path, O_RDWR|O_CREAT|O_TRUNC, 0600)) < 0) {
            err = ff_log(FC_ERROR, errno, "error in %s open('%s')", label[j], path);
            break;
        }

        double pretty_len = 0.0;
        const char * pretty_label = ff_pretty_size(len, & pretty_len);
        ff_log(FC_INFO, 0, "%s: writing %.2f %sbytes to '%s' ...", label[j], pretty_len, pretty_label, path);

#ifdef FT_HAVE_POSIX_FALLOCATE
        /* try with posix_fallocate() */
        if ((err = posix_fallocate(fd[j], 0, (ft_off) len)) != 0) {
#endif /* FT_HAVE_POSIX_FALLOCATE */

            /* else fall back on write() */
            enum { zero_len = 64*1024 };
            char zero[zero_len];
            ft_size pos = 0, chunk, written;

            while (pos < mem_len) {
                chunk = ff_min2<ft_size>(zero_len, mem_len - pos);
                while ((written = ::write(fd[j], zero, chunk)) == (ft_size)-1 && errno == EINTR)
                    ;
                if (written == (ft_size)-1 || written == 0) {
                    err = ff_log(FC_ERROR, errno, "error in %s write('%s')", label[j], path);
                    break;
                }
                pos += written;
            }
#ifdef FT_HAVE_POSIX_FALLOCATE
        }
#endif /* FT_HAVE_POSIX_FALLOCATE */
        
        /* remember secondary_storage details */
        ft_extent<ft_uoff> & extent = secondary_storage();
        extent.physical() = extent.logical() = 0;
        extent.length() = len;

        ff_log(FC_INFO, 0, "%s file created");

    } while (0);

    if (err != 0) {
        const bool need_unlink = is_open0(j);
        close0(fd[j]);
        if (need_unlink && unlink(path) != 0)
            ff_log(FC_WARN, errno, "removing %s file '%s' failed", label[j], path);
    }
    return err;
}


/** close and munmap() SECONDARY-STORAGE. called by close() */
void ft_io_posix::close_storage()
{
    enum { i = FC_PRIMARY_STORAGE, j = FC_SECONDARY_STORAGE };
    if (storage_mmap != MAP_FAILED) {
        if (munmap(storage_mmap, storage_mmap_size) != 0) {
            bool flag_j = secondary_storage().length() != 0;
            ff_log(FC_WARN, errno, "munmap() %s%s%s failed", label[i],
                   (flag_j ? " and" : ""),
                   (flag_j ? label[j] : "")
            );
        }
        storage_mmap = MAP_FAILED;
    }
    storage_mmap_size = 0;
    close0(i);
}


FT_IO_NAMESPACE_END