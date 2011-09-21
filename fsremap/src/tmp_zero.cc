/*
 * tmp_zero.cc
 *
 *  Created on: Mar 27, 2011
 *      Author: max
 */

#include "first.hh"

#include <cstring>              // for memset()
#include <sys/types.h>          // for open()
#include <sys/stat.h>           //  "    "
#include <fcntl.h>              //  "    "


#include "log.hh"               // for ff_log()
#include "util.hh"              // for ff_str2un(), ff_min2()
#include "map.hh"               // for fr_map<T>
#include "vector.hh"            // for fr_vector<T>
#include "work.hh"              // for fr_work<T>::show()

#include "io/extent_file.hh"    // for ff_load_extents_file()
#include "io/util_posix.hh"     // for ff_posix_write()

FT_IO_NAMESPACE_BEGIN

/**
 * argv[0] = program_name
 * argv[1] = device length
 * argv[2] = save-file containing loop-file extents
 */
int ff_zero_loop_file_holes(int argc, char ** argv)
{
    ft_uoff dev_len;
    FILE * f = NULL;
    char const* const* const args = argv + 1;
    int dev_fd = -1, err;
    do {
        if ((dev_fd = ::open(args[0], O_RDWR)) < 0) {
            err = ff_log(FC_ERROR, errno, "error opening device '%s'", args[0]);
            break;
        }
        if ((err = ff_posix_blkdev_size(dev_fd, & dev_len)) != 0) {
            ff_log(FC_WARN, errno, "warning: device ioctl('%s', BLKGETSIZE64) failed, trying fstat() to get device length", args[0]);
            if ((err = ff_posix_size(dev_fd, & dev_len)) != 0) {
                err = ff_log(FC_ERROR, errno, "error in device fstat('%s')", args[0]);
                break;
            }
        }
        if ((f = fopen(args[1], "r")) == NULL) {
            err = ff_log(FC_ERROR, errno, "error opening persistence file '%s'", args[1]);
            break;
        }
        fr_vector<ft_uoff> loop_extents;
        ft_uoff block_size_bitmask = 0;
        if ((err = ff_load_extents_file(f, loop_extents, block_size_bitmask)) != 0) {
            err = ff_log(FC_ERROR, err, "error reading persistence file '%s'");
            break;
        }
        ft_uoff eff_block_size_log2 = 0;
        if (block_size_bitmask != 0) {
            while ((block_size_bitmask & 1) == 0) {
                eff_block_size_log2++;
                block_size_bitmask >>= 1;
            }
        }


        fr_map<ft_uoff> loop_holes_map;
        loop_holes_map.complement0_logical_shift(loop_extents, eff_block_size_log2, dev_len);
        fr_work<ft_uoff>::show("loop-holes", "", eff_block_size_log2, loop_holes_map, FC_INFO);

        fr_map<ft_uoff>::const_iterator iter = loop_holes_map.begin(), end = loop_holes_map.end();
        enum { ZERO_BUF_LEN = 1024*1024 };
        char zero_buf[ZERO_BUF_LEN];
        memset(zero_buf, '\0', ZERO_BUF_LEN);

        ft_uoff offset, left, chunk;
        for (; iter != end; ++iter) {
            const fr_map<ft_uoff>::value_type & extent = *iter;
            offset = extent.first.physical << eff_block_size_log2;
            left = extent.second.length << eff_block_size_log2;
            if ((err = ff_posix_lseek(dev_fd, offset)) != 0) {
                err = ff_log(FC_ERROR, err, "error in device lseek('%s', offset = %"FS_ULL")", args[0], (ft_ull) offset);
                break;
            }
            while (left != 0) {
                chunk = ff_min2<ft_uoff>(left, ZERO_BUF_LEN);
                if ((err = ff_posix_write(dev_fd, zero_buf, chunk)) != 0) {
                    err = ff_log(FC_ERROR, err, "error in device write({'%s', offset = %"FS_ULL"}, zero_buffer, length = %"FS_ULL")",
                                 args[0], (ft_ull) offset, (ft_ull) chunk);
                    break;
                }
                left -= chunk;
            }
        }
    } while (0);
    if (f != NULL)
        fclose(f);
    if (dev_fd >= 0)
        close(dev_fd);
    return err;
}

FT_IO_NAMESPACE_END