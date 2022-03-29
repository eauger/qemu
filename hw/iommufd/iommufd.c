/*
 * QEMU abstract of IOMMU
 *
 * Copyright (C) 2022 Intel Corporation.
 *
 * Authors: Liu Yi L <yi.l.liu@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qom/object.h"
#include <sys/ioctl.h>
#include "qemu/error-report.h"
#include "hw/iommufd/iommufd.h"

static int iommufd_users;
static int iommufd = -1;

static int iommufd_get(void)
{
    if (iommufd == -1) {
        iommufd = qemu_open_old("/dev/iommu", O_RDWR);
        if (iommufd < 0) {
            error_report("Failed to open /dev/iommu!");
        } else {
            iommufd_users = 1;
        }
        printf("open iommufd: %d\n", iommufd);
    } else {
        iommufd_users++;
    }
    return iommufd;
}

static void iommufd_put(int fd)
{
    if (--iommufd_users) {
        return;
    }
    iommufd = -1;
    printf("close iommufd: %d\n", fd);
    close(fd);
}

static int iommufd_alloc_ioas(int fd, uint32_t *ioas)
{
    struct iommu_ioas_alloc alloc_data;
    int ret;

    if (fd < 0) {
        return -EINVAL;
    }

    alloc_data.size = sizeof(alloc_data);
    alloc_data.flags = 0;

    ret = ioctl(fd, IOMMU_IOAS_ALLOC, &alloc_data);
    if (ret) {
        error_report("Failed to allocate ioas %m");
    }

    *ioas = alloc_data.out_ioas_id;

    return ret;
}

static void iommufd_free_ioas(int fd, uint32_t ioas)
{
    struct iommu_destroy des;

    if (fd < 0) {
        return;
    }

    des.size = sizeof(des);
    des.id = ioas;

    if (ioctl(fd, IOMMU_DESTROY, &des)) {
        error_report("Failed to free ioas: %u %m", ioas);
    }
}

int iommu_get_ioas(int *fd, uint32_t *ioas_id)
{
    int ret;

    *fd = iommufd_get();
    if (*fd < 0) {
        return -ENODEV;
    }

    ret = iommufd_alloc_ioas(*fd, ioas_id);
    if (ret) {
        iommufd_put(*fd);
    }
    return ret;
}

void iommu_put_ioas(int fd, uint32_t ioas_id)
{
    iommufd_free_ioas(fd, ioas_id);
    iommufd_put(fd);
}

int iommufd_unmap_dma(int iommufd, uint32_t ioas, hwaddr iova, ram_addr_t size)
{
    struct iommu_ioas_unmap unmap;
    int ret;

    memset(&unmap, 0x0, sizeof(unmap));
    unmap.size = sizeof(unmap);
    unmap.ioas_id = ioas;
    unmap.iova = iova;
    unmap.length = size;

    ret = ioctl(iommufd, IOMMU_IOAS_UNMAP, &unmap);
    if (ret) {
        error_report("IOMMU_IOAS_UNMAP failed: %s", strerror(errno));
    }
    return ret;
}

int iommufd_map_dma(int iommufd, uint32_t ioas, hwaddr iova,
                    ram_addr_t size, void *vaddr, bool readonly)
{
    struct iommu_ioas_map map;
    int ret;

    memset(&map, 0x0, sizeof(map));
    map.size = sizeof(map);
    map.flags = IOMMU_IOAS_MAP_READABLE |
                IOMMU_IOAS_MAP_FIXED_IOVA;
    map.ioas_id = ioas;
    map.user_va = (int64_t)vaddr;
    map.iova = iova;
    map.length = size;
    if (!readonly) {
        map.flags |= IOMMU_IOAS_MAP_WRITEABLE;
    }

    ret = ioctl(iommufd, IOMMU_IOAS_MAP, &map);
    if (ret) {
        error_report("IOMMU_IOAS_MAP failed: %s", strerror(errno));
    }
    return ret;
}
