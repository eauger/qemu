/*
 * generic functions used by VFIO devices
 *
 * Copyright Red Hat, Inc. 2012
 *
 * Authors:
 *  Alex Williamson <alex.williamson@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Based on qemu-kvm device-assignment:
 *  Adapted for KVM by Qumranet.
 *  Copyright (c) 2007, Neocleus, Alex Novik (alex@neocleus.com)
 *  Copyright (c) 2007, Neocleus, Guy Zana (guy@neocleus.com)
 *  Copyright (C) 2008, Qumranet, Amit Shah (amit.shah@qumranet.com)
 *  Copyright (C) 2008, Red Hat, Amit Shah (amit.shah@redhat.com)
 *  Copyright (C) 2008, IBM, Muli Ben-Yehuda (muli@il.ibm.com)
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <linux/vfio.h>

#include "hw/vfio/vfio-common.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "qapi/error.h"
#include "hw/iommufd/iommufd.h"

static int iommufd_map(VFIOContainer *container, hwaddr iova,
                       ram_addr_t size, void *vaddr, bool readonly)
{
    return iommufd_map_dma(container->iommufd, container->ioas_id,
                           iova, size, vaddr, readonly);
}

static int iommufd_unmap(VFIOContainer *container,
                         hwaddr iova, ram_addr_t size,
                         IOMMUTLBEntry *iotlb)
{
     return iommufd_unmap_dma(container->iommufd,
                              container->ioas_id, iova, size); 
}

static int vfio_get_devicefd(const char *sysfs_path, Error **errp)
{
    int vfio_id = -1, ret = 0;
    char *path, *tmp;
    DIR *dir;
    struct dirent *dent;
    struct stat st;
    gchar *contents;
    gsize length;
    int major, minor;
    dev_t vfio_devt;

    path = g_strdup_printf("%s/vfio-device", sysfs_path);
    printf("path: %s, \n", path);
    if (stat(path, &st) < 0) {
        error_setg_errno(errp, errno, "no such host device");
        error_prepend(errp, VFIO_MSG_PREFIX, path);
        return -ENOTTY;
    }

    dir = opendir(path);
    if (!dir) {
        ret = -ENOTTY;
        goto out;
    }

    while ((dent = readdir(dir))) {
        char *end_name;

        if (!strncmp(dent->d_name, "vfio", 4)) {
            vfio_id = strtol(dent->d_name + 4, &end_name, 10);
            break;
        }
    }

    printf("vfio_id: %d\n", vfio_id);
    if (vfio_id == -1) {
        ret = -ENOTTY;
        goto out;
    }

    /* check if the major:minor matches */
    tmp = g_strdup_printf("%s/%s/dev", path, dent->d_name);
    if (!g_file_get_contents(tmp, &contents, &length, NULL)) {
        error_report("failed to load \"%s\"", tmp);
        exit(1);
    }
    printf("tmp: %s, content: %s, len: %ld\n", tmp, contents, length);
    if (sscanf(contents, "%d:%d", &major, &minor) != 2) {
        error_report("failed to load \"%s\"", tmp);
        exit(1);
    }
    printf("%d, %d\n", major, minor);
    g_free(contents);
    g_free(tmp);

    tmp = g_strdup_printf("/dev/vfio/devices/vfio%d", vfio_id);
    if (stat(tmp, &st) < 0) {
        error_setg_errno(errp, errno, "no such vfio device");
        error_prepend(errp, VFIO_MSG_PREFIX, tmp);
        ret = -ENOTTY;
        goto out;
    }
    vfio_devt = makedev(major, minor);
    printf("vfio_devt: %lu, %lu\n", vfio_devt, st.st_rdev);
    if (st.st_rdev != vfio_devt) {
        ret = -EINVAL;
    } else {
        ret = qemu_open_old(tmp, O_RDWR);
    }
    g_free(tmp);

out:
    g_free(path);
    return ret;
}

static VFIOIOASHwpt *vfio_container_get_hwpt(VFIOContainer *container,
                                             uint32_t hwpt_id)
{
    VFIOIOASHwpt *hwpt;

    QLIST_FOREACH(hwpt, &container->hwpt_list, next) {
        if (hwpt->hwpt_id == hwpt_id) {
            return hwpt;
        }
    }

    hwpt = g_malloc0(sizeof(*hwpt));
    if (!hwpt) {
        return NULL;
    }

    hwpt->hwpt_id = hwpt_id;
    QLIST_INIT(&hwpt->device_list);

    return hwpt;
}

static VFIOIOASHwpt *vfio_find_hwpt_for_dev(VFIOContainer *container,
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

static void vfio_device_detach_container(VFIODevice *vbasedev,
                                         VFIOContainer *container)
{
    struct vfio_device_detach_ioas detach_data;
    VFIOIOASHwpt *hwpt;

    hwpt = vfio_find_hwpt_for_dev(container, vbasedev);
    if (hwpt) {
        QLIST_REMOVE(hwpt, next);
        QLIST_REMOVE(vbasedev, hwpt_next);
        g_free(hwpt);
    }

    detach_data.argsz = sizeof(detach_data);
    detach_data.flags = 0;
    detach_data.iommufd = container->iommufd;
    detach_data.ioas_id = container->ioas_id;

    if (ioctl(vbasedev->fd, VFIO_DEVICE_DETACH_IOAS, &detach_data)) {
        printf("detach ioas: %d failed %m\n", container->ioas_id);
    }

    /* iommufd unbind is done per device fd close */
}

static int vfio_device_attach_container(VFIODevice *vbasedev,
                                       VFIOContainer *container, Error **errp)
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
        error_setg_errno(errp, errno, "error bind iommufd");
        return ret;
    }

    vbasedev->devid = bind.out_devid;

    /* Attach device to an ioas within iommufd */
    attach_data.argsz = sizeof(attach_data);
    attach_data.flags = 0;
    attach_data.iommufd = container->iommufd;
    attach_data.ioas_id = container->ioas_id;

    ret = ioctl(vbasedev->fd, VFIO_DEVICE_ATTACH_IOAS, &attach_data);
    if (ret) {
        error_setg_errno(errp, errno, "error attach ioas");
        return ret;
    }

    /* Record the hwpt returned per attach */
    hwpt = vfio_container_get_hwpt(container, attach_data.out_hwpt_id);
    if (!hwpt) {
        error_setg_errno(errp, errno, "error to get hwpt");
        vfio_device_detach_container(vbasedev, container);
        return -EINVAL;
    }

    QLIST_INSERT_HEAD(&hwpt->device_list, vbasedev, hwpt_next);
    QLIST_INSERT_HEAD(&container->hwpt_list, hwpt, next);

    /* todo register reset handler */

    return 0;
}

static int iommufd_attach_device(VFIODevice *vbasedev, AddressSpace *as,
                                 Error **errp)
{
    VFIOContainer *container;
    VFIOAddressSpace *space = vfio_get_address_space(as);
    struct vfio_device_info dev_info = { .argsz = sizeof(dev_info) };
    int ret, fd, iommufd;
    struct iommu_ioas_alloc ioas_alloc = {
                .size = sizeof(ioas_alloc),
                .flags = 0,
    };

    fd = vfio_get_devicefd(vbasedev->sysfsdev, errp);
    if (fd < 0) {
        return fd;
    }
    vbasedev->fd = fd;

    /* try to attach on existing container */
    QLIST_FOREACH(container, &space->containers, next) {
        if (!vfio_device_attach_container(vbasedev, container, errp)) {
#if 0 /* to check */
            ret = vfio_ram_block_discard_disable(container, true);
            if (ret) {
                vfio_device_detach_container(vbasedev, container);
                error_setg_errno(errp, -ret,
                                 "Cannot set discarding of RAM broken");
                return ret;
            }
#endif
	    goto out;
	}
    }

    iommufd = iommufd_get();

    ret = ioctl(iommufd, IOMMU_IOAS_ALLOC, &ioas_alloc);
    if (ret < 0) {
        error_report("Failed to alloc ioas (%s)", strerror(errno));
        return ret;
    } else {
        error_report("Allocated ioas=%d", ioas_alloc.out_ioas_id);
    }

    container = g_malloc0(sizeof(*container));
    container->iommu_ops = vbasedev->iommu_ops;
    container->space = space;
    container->iommufd = iommufd;
    container->ioas_id = ioas_alloc.out_ioas_id;
    container->fd = 0;
    container->iommu_type = 0xbeef; /* TODO */
    container->error = NULL;
    container->dirty_pages_supported = false;
    container->dma_max_mappings = 0;
    QLIST_INIT(&container->giommu_list);
    QLIST_INIT(&container->hostwin_list);
    QLIST_INIT(&container->vrdl_list);
    error_report("%s new container with ioas%d is finalized",
                 __func__, container->ioas_id);

    ret = vfio_device_attach_container(vbasedev, container, errp);
    if (ret) {
	    /* todo */
	    return ret;
    }
    vfio_host_win_add(container, 0, (hwaddr)-1, 4096);

out:

    ret = ioctl(fd, VFIO_DEVICE_GET_INFO, &dev_info);
    if (ret) {
        error_setg_errno(errp, errno, "error getting device info");
        vfio_device_detach_container(vbasedev, container);
        close(fd);
        return ret;
    }
    /* TODO examine RAM_BLOCK_DISCARD stuff */
    vbasedev->group = 0;
    vbasedev->num_irqs = dev_info.num_irqs;
    vbasedev->num_regions = dev_info.num_regions;
    vbasedev->flags = dev_info.flags;
    vbasedev->reset_works = !!(dev_info.flags & VFIO_DEVICE_FLAGS_RESET);
    return 0;
}

static int iommufd_detach_device(VFIODevice *vbasedev, AddressSpace *as, Error **errp)
{
    VFIOContainer *container;
    VFIOAddressSpace *space = vfio_get_address_space(as);
    VFIODevice *vbasedev_iter;
    VFIOIOASHwpt *hwpt;

    QLIST_FOREACH(container, &space->containers, next) {
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

static void iommufd_put_device(VFIODevice *vbasedev)
{
    QLIST_REMOVE(vbasedev, hwpt_next);
    trace_vfio_put_base_device(vbasedev->fd);
    close(vbasedev->fd);
    g_free(vbasedev->name);
}

const VFIOIOMMUOps iommufd_ops = {
    .backend_type = VFIO_IOMMU_BACKEND_TYPE_IOMMUFD,
    .vfio_iommu_dma_map = iommufd_map,
    .vfio_iommu_dma_unmap = iommufd_unmap,
    .vfio_iommu_attach_device = iommufd_attach_device,
    .vfio_iommu_detach_device = iommufd_detach_device,
    .vfio_iommu_put_device = iommufd_put_device,
};

