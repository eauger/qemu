/*
 * iommufd container backend
 *
 * Copyright (C) 2022 Intel Corporation.
 * Copyright Red Hat, Inc. 2022
 *
 * Authors: Yi Liu <yi.l.liu@intel.com>
 *          Eric Auger <eric.auger@redhat.com>
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
#include <sys/ioctl.h>
#include <linux/vfio.h>

#include "hw/vfio/vfio-common.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "qapi/error.h"
#include "hw/iommufd/iommufd.h"
#include "hw/qdev-core.h"
#include "sysemu/reset.h"
#include "qemu/cutils.h"

#define TYPE_VFIO_IOMMUFD_CONTAINER "qemu:vfio-iommufd-container"

static int iommufd_map(VFIOContainer *bcontainer, hwaddr iova,
                       ram_addr_t size, void *vaddr, bool readonly)
{
    VFIOIOMMUFDContainer *container = container_of(bcontainer,
                                                   VFIOIOMMUFDContainer, obj);

    return iommufd_map_dma(container->iommufd,
                           container->ioas_id,
                           iova, size, vaddr, readonly);
}

static int iommufd_copy(VFIOContainer *src, VFIOContainer *dst,
                        hwaddr iova, ram_addr_t size, bool readonly)
{
    VFIOIOMMUFDContainer *container_src = container_of(src,
                                                   VFIOIOMMUFDContainer, obj);
    VFIOIOMMUFDContainer *container_dst = container_of(dst,
                                                   VFIOIOMMUFDContainer, obj);

    assert(container_src->iommufd == container_dst->iommufd);

    return iommufd_copy_dma(container_src->iommufd, container_src->ioas_id,
                            container_dst->ioas_id, iova, size, readonly);
}

static int iommufd_unmap(VFIOContainer *bcontainer,
                         hwaddr iova, ram_addr_t size,
                         IOMMUTLBEntry *iotlb)
{
    VFIOIOMMUFDContainer *container = container_of(bcontainer,
                                                   VFIOIOMMUFDContainer, obj);

    /* properly handle the iotlb arg */
    return iommufd_unmap_dma(container->iommufd,
                             container->ioas_id, iova, size);
}

static int vfio_get_devicefd(const char *sysfs_path, Error **errp)
{
    long int vfio_id = -1, ret = -ENOTTY;
    char *path, *tmp = NULL;
    DIR *dir;
    struct dirent *dent;
    struct stat st;
    gchar *contents;
    gsize length;
    int major, minor;
    dev_t vfio_devt;

    path = g_strdup_printf("%s/vfio-device", sysfs_path);
    if (stat(path, &st) < 0) {
        error_setg_errno(errp, errno, "no such host device");
        goto out;
    }

    dir = opendir(path);
    if (!dir) {
        error_setg_errno(errp, errno, "couldn't open dirrectory %s", path);
        goto out;
    }

    while ((dent = readdir(dir))) {
        const char *end_name;

        if (!strncmp(dent->d_name, "vfio", 4)) {
            ret = qemu_strtol(dent->d_name + 4, &end_name, 10, &vfio_id);
            if (ret) {
                error_setg(errp, "suspicious vfio* file in %s", path);
                goto out;
            }
            break;
        }
    }

    /* check if the major:minor matches */
    tmp = g_strdup_printf("%s/%s/dev", path, dent->d_name);
    if (!g_file_get_contents(tmp, &contents, &length, NULL)) {
        error_setg(errp, "failed to load \"%s\"", tmp);
        goto out;
    }

    if (sscanf(contents, "%d:%d", &major, &minor) != 2) {
        error_setg(errp, "failed to load \"%s\"", tmp);
        goto out;
    }
    g_free(contents);
    g_free(tmp);

    tmp = g_strdup_printf("/dev/vfio/devices/vfio%ld", vfio_id);
    if (stat(tmp, &st) < 0) {
        error_setg_errno(errp, errno, "no such vfio device");
        goto out;
    }
    vfio_devt = makedev(major, minor);
    if (st.st_rdev != vfio_devt) {
        error_setg(errp, "minor do not match: %lu, %lu", vfio_devt, st.st_rdev);
        goto out;
    }

    ret = qemu_open_old(tmp, O_RDWR);
    if (ret < 0) {
        error_setg(errp, "Failed to open %s", tmp);
    }
    trace_vfio_iommufd_get_devicefd(tmp, ret);
out:
    g_free(tmp);
    g_free(path);

    if (*errp) {
        error_prepend(errp, VFIO_MSG_PREFIX, path);
    }
    return ret;
}

static VFIOIOASHwpt *vfio_container_get_hwpt(VFIOIOMMUFDContainer *container,
                                             uint32_t hwpt_id)
{
    VFIOIOASHwpt *hwpt;

    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        if (hwpt->hwpt_id == hwpt_id) {
            return hwpt;
        }
    }

    hwpt = g_malloc0(sizeof(*hwpt));

    hwpt->hwpt_id = hwpt_id;
    QLIST_INIT(&hwpt->device_list);
    QLIST_INSERT_HEAD(&container->hwpt_list, hwpt, next);

    return hwpt;
}

static VFIOIOASHwpt *vfio_find_hwpt_for_dev(VFIOIOMMUFDContainer *container,
                                            VFIODevice *vbasedev)
{
    VFIOIOASHwpt *hwpt;
    VFIODevice *vbasedev_iter;

    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        QLIST_FOREACH(vbasedev_iter, &hwpt->device_list, hwpt_next) {
            if (vbasedev_iter == vbasedev) {
                return hwpt;
            }
        }
    }
    return NULL;
}

static void __vfio_device_detach_container(VFIODevice *vbasedev,
                                           VFIOIOMMUFDContainer *container)
{
    struct vfio_device_detach_ioas detach_data;

    detach_data.argsz = sizeof(detach_data);
    detach_data.flags = 0;
    detach_data.iommufd = container->iommufd;
    detach_data.ioas_id = container->ioas_id;

    if (ioctl(vbasedev->fd, VFIO_DEVICE_DETACH_IOAS, &detach_data)) {
        printf("detach ioas: %d failed %m\n", container->ioas_id);
    }

    /* iommufd unbind is done per device fd close */
}

static void vfio_device_detach_container(VFIODevice *vbasedev,
                                         VFIOIOMMUFDContainer *container)
{
    VFIOIOASHwpt *hwpt;

    hwpt = vfio_find_hwpt_for_dev(container, vbasedev);
    if (hwpt) {
        QLIST_REMOVE(vbasedev, hwpt_next);
        if (QLIST_EMPTY(&hwpt->device_list)) {
            QLIST_REMOVE(hwpt, next);
            g_free(hwpt);
        }
    }

    __vfio_device_detach_container(vbasedev, container);
}

static int vfio_device_attach_container(VFIODevice *vbasedev,
                                        VFIOIOMMUFDContainer *container,
                                        Error **errp)
{
    struct vfio_device_bind_iommufd bind;
    struct vfio_device_attach_ioas attach_data;
    VFIOIOASHwpt *hwpt;
    int ret;

    bind.argsz = sizeof(bind);
    bind.flags = 0;
    bind.iommufd = container->iommufd;
    bind.dev_cookie = (uint64_t)vbasedev;

    /* Bind device to iommufd */
    ret = ioctl(vbasedev->fd, VFIO_DEVICE_BIND_IOMMUFD, &bind);
    if (ret) {
        error_setg_errno(errp, errno, "error bind device fd=%d to iommufd=%d",
                         vbasedev->fd, bind.iommufd);
        return ret;
    }

    vbasedev->devid = bind.out_devid;
    trace_vfio_iommufd_bind_device(bind.iommufd, vbasedev->name,
                                   vbasedev->fd, vbasedev->devid);

    /* Attach device to an ioas within iommufd */
    attach_data.argsz = sizeof(attach_data);
    attach_data.flags = 0;
    attach_data.iommufd = container->iommufd;
    attach_data.ioas_id = container->ioas_id;

    ret = ioctl(vbasedev->fd, VFIO_DEVICE_ATTACH_IOAS, &attach_data);
    if (ret) {
        error_setg_errno(errp, errno,
                         "[iommufd=%d] error attach %s (%d) to ioasid=%d",
                         container->iommufd, vbasedev->name, vbasedev->fd,
                         attach_data.ioas_id);
        return ret;

    }
    trace_vfio_iommufd_attach_device(bind.iommufd, vbasedev->name,
                                     vbasedev->fd, container->ioas_id,
                                     attach_data.out_hwpt_id);

    hwpt = vfio_container_get_hwpt(container, attach_data.out_hwpt_id);

    QLIST_INSERT_HEAD(&hwpt->device_list, vbasedev, hwpt_next);
    return 0;
}

static void
foreach_vfio_dev(void (*cb)(VFIODevice *vbasedev, void *opaque), void *opaque)
{
    VFIOIOMMUFDContainer *container;
    VFIOContainer *iommu;
    VFIOAddressSpace *space;
    VFIODevice *vbasedev;
    VFIOIOASHwpt *hwpt;

    QLIST_FOREACH(space, &vfio_address_spaces, list) {
        QLIST_FOREACH(iommu, &space->containers, next) {
            container = container_of(iommu, VFIOIOMMUFDContainer, obj);
            QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
                QLIST_FOREACH(vbasedev, &hwpt->device_list, hwpt_next) {
                    cb(vbasedev, opaque);
                }
            }
        }
    }
}

static void vfio_reset(VFIODevice *vbasedev, void *opaque)
{
    if (vbasedev->dev->realized) {
        vbasedev->ops->vfio_compute_needs_reset(vbasedev);
        if (vbasedev->needs_reset) {
            vbasedev->ops->vfio_hot_reset_multi(vbasedev);
        }
    }
}

static void iommufd_reset_handler(void *opaque)
{
    foreach_vfio_dev(vfio_reset, NULL);
}

static int iommufd_attach_device(VFIODevice *vbasedev, AddressSpace *as,
                                 Error **errp)
{
    VFIOContainer *bcontainer;
    VFIOIOMMUFDContainer *container;
    VFIOAddressSpace *space;
    struct vfio_device_info dev_info = { .argsz = sizeof(dev_info) };
    int ret, devfd, iommufd;
    uint32_t ioas_id;

    if (QLIST_EMPTY(&vfio_address_spaces)) {
        qemu_register_reset(iommufd_reset_handler, NULL);
    }

    devfd = vfio_get_devicefd(vbasedev->sysfsdev, errp);
    if (devfd < 0) {
        return devfd;
    }
    vbasedev->fd = devfd;

    space = vfio_get_address_space(as);

    /* try to attach to an existing container in this space */
    QLIST_FOREACH(bcontainer, &space->containers, next) {
        Error *err = NULL;

        if (!object_dynamic_cast(OBJECT(bcontainer), TYPE_VFIO_IOMMUFD_CONTAINER)) {
            continue;
        }
        container = container_of(bcontainer, VFIOIOMMUFDContainer, obj);
        if (vfio_device_attach_container(vbasedev, container, &err)) {
            const char *msg = error_get_pretty(err);

            trace_vfio_iommufd_fail_attach_existing_container(msg);
            error_free(err);
        } else {
#if 0 /* to check */
            ret = vfio_ram_block_discard_disable(container, true);
            if (ret) {
                vfio_device_detach_container(vbasedev, container);
                vfio_put_address_space(space);
                close(vbasedev->fd);
                error_setg_errno(errp, -ret,
                                 "Cannot set discarding of RAM broken");
                return ret;
            }
#endif
            goto out;
        }
    }

    /* Need to allocate a new dedicated container */
    ret = iommufd_get_ioas(&iommufd, &ioas_id);
    if (ret < 0) {
        vfio_put_address_space(space);
        close(vbasedev->fd);
        error_report("Failed to alloc ioas (%s)", strerror(errno));
        return ret;
    }

    trace_vfio_iommufd_alloc_ioas(iommufd, ioas_id);

    container = g_malloc0(sizeof(*container));
    container->iommufd = iommufd;
    container->ioas_id = ioas_id;
    bcontainer = &container->obj;
    vfio_container_init(bcontainer, sizeof(*bcontainer),
                        TYPE_VFIO_IOMMUFD_CONTAINER, space);

    /* TODO kvmgroup? */
    vfio_host_win_add(bcontainer, 0, (hwaddr)-1, 4096);

    ret = vfio_device_attach_container(vbasedev, container, errp);
    if (ret) {
        /* todo check if any other thing to do */
        vfio_container_destroy(bcontainer);
        g_free(container);
        iommufd_put_ioas(iommufd, ioas_id);
        vfio_put_address_space(space);
        close(vbasedev->fd);
        return ret;
    }
    QLIST_INSERT_HEAD(&space->containers, bcontainer, next);

    vfio_as_register_listener(bcontainer->space);

    bcontainer->initialized = true;
    vbasedev->container = bcontainer;

out:

    ret = ioctl(devfd, VFIO_DEVICE_GET_INFO, &dev_info);
    if (ret) {
        error_setg_errno(errp, errno, "error getting device info");
        vfio_device_detach_container(vbasedev, container);
        close(devfd);
        return ret;
    }
    /* TODO examine RAM_BLOCK_DISCARD stuff */
    vbasedev->group = 0;
    vbasedev->num_irqs = dev_info.num_irqs;
    vbasedev->num_regions = dev_info.num_regions;
    vbasedev->flags = dev_info.flags;
    vbasedev->reset_works = !!(dev_info.flags & VFIO_DEVICE_FLAGS_RESET);

    trace_vfio_iommufd_device_info(vbasedev->name, devfd, vbasedev->num_irqs,
                                   vbasedev->num_regions, vbasedev->flags);
    return 0;
}

#if 0
static int
iommufd_detach_device(VFIODevice *vbasedev, AddressSpace *as, Error **errp)
{
    VFIOContainer *bcontainer;
    VFIOIOMMUFDContainer *container;
    VFIOAddressSpace *space = vfio_get_address_space(as);
    VFIODevice *vbasedev_iter;
    VFIOIOASHwpt *hwpt;

    QLIST_FOREACH(container, &space->containers, next) {
        container = container_of(bcontainer, VFIOIOMMUFDContainer, obj);
        QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
            QLIST_FOREACH(vbasedev_iter, &hwpt->device_list, hwpt_next) {
                if (vbasedev_iter == vbasedev) {
                    vfio_device_detach_container(vbasedev, container);
                    return 0;
                }
            }
        }
    }
    return -1;
}
#endif

static void iommufd_detach_device(VFIODevice *vbasedev)
{
    VFIOContainer *bcontainer = vbasedev->container;
    VFIOIOMMUFDContainer *container;
    VFIODevice *vbasedev_iter;
    VFIOIOASHwpt *hwpt;
    bool found = false;

    if (!bcontainer) {
        goto out;
    }

    container = container_of(bcontainer, VFIOIOMMUFDContainer, obj);
    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        QLIST_FOREACH(vbasedev_iter, &hwpt->device_list, hwpt_next) {
            if (vbasedev_iter == vbasedev) {
                found = true;
                break;
            }
        }
    }

    if (found) {
        QLIST_REMOVE(vbasedev, hwpt_next);
        if (QLIST_EMPTY(&hwpt->device_list)) {
            QLIST_REMOVE(hwpt, next);
            g_free(hwpt);
        }

        if (QLIST_EMPTY(&container->hwpt_list)) {
            vfio_container_destroy(bcontainer);
        }
        __vfio_device_detach_container(vbasedev, container);
        if (QLIST_EMPTY(&container->hwpt_list)) {
            VFIOAddressSpace *space = bcontainer->space;

            iommufd_put_ioas(container->iommufd, container->ioas_id);
            g_free(container);
            vfio_put_address_space(space);
        }
    }
out:
    close(vbasedev->fd);
    g_free(vbasedev->name);
}

const VFIOIOMMUOps iommufd_ops = {
    .backend_type = VFIO_IOMMU_BACKEND_TYPE_IOMMUFD,
    .vfio_iommu_attach_device = iommufd_attach_device,
    .vfio_iommu_detach_device = iommufd_detach_device,
};

static void vfio_iommufd_class_init(ObjectClass *klass,
                                    void *data)
{
    VFIOContainerClass *vccs = VFIO_CONTAINER_OBJ_CLASS(klass);

    vccs->dma_map = iommufd_map;
    vccs->dma_copy = iommufd_copy;
    vccs->dma_unmap = iommufd_unmap;
}

static const TypeInfo vfio_iommufd_info = {
    .parent = TYPE_VFIO_CONTAINER_OBJ,
    .name = TYPE_VFIO_IOMMUFD_CONTAINER,
    .class_init = vfio_iommufd_class_init,
};

static void vfio_iommufd_register_types(void)
{
    type_register_static(&vfio_iommufd_info);
    vfio_register_iommu_ops(&iommufd_ops);
}

type_init(vfio_iommufd_register_types)
