/*
 * VFIO CONTAINER BASE OBJECT
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
#include "qapi/error.h"
#include "qom/object.h"
#include "qapi/visitor.h"
#include "hw/vfio/vfio-common.h"

int vfio_container_dma_map(VFIOContainer *bcontainer,
                           hwaddr iova, ram_addr_t size,
                           void *vaddr, bool readonly)
{
    VFIOContainerClass *vccs = VFIO_BASE_CONTAINER_OBJ_GET_CLASS(bcontainer);

    if (!vccs->dma_map) {
        return -EINVAL;
    }

    return vccs->dma_map(bcontainer, iova, size, vaddr, readonly);
}

int vfio_container_dma_unmap(VFIOContainer *bcontainer,
                             hwaddr iova, ram_addr_t size,
                             IOMMUTLBEntry *iotlb)
{
    VFIOContainerClass *vccs = VFIO_BASE_CONTAINER_OBJ_GET_CLASS(bcontainer);

    vccs = VFIO_BASE_CONTAINER_OBJ_GET_CLASS(bcontainer);

    if (!vccs->dma_unmap) {
        return -EINVAL;
    }

    return vccs->dma_unmap(bcontainer, iova, size, iotlb);
}

void vfio_container_set_dirty_page_tracking(VFIOContainer *bcontainer, bool start)
{
    VFIOContainerClass *vccs = VFIO_BASE_CONTAINER_OBJ_GET_CLASS(bcontainer);

    if (!vccs->set_dirty_page_tracking) {
        return;
    }

    vccs->set_dirty_page_tracking(bcontainer, start);
}

bool vfio_container_devices_all_dirty_tracking(VFIOContainer *bcontainer)
{
    VFIOContainerClass *vccs = VFIO_BASE_CONTAINER_OBJ_GET_CLASS(bcontainer);

    if (!vccs->devices_all_dirty_tracking) {
        return false;
    }

    return vccs->devices_all_dirty_tracking(bcontainer);
}

int vfio_container_get_dirty_bitmap(VFIOContainer *bcontainer, uint64_t iova,
                                    uint64_t size, ram_addr_t ram_addr)
{
    VFIOContainerClass *vccs = VFIO_BASE_CONTAINER_OBJ_GET_CLASS(bcontainer);

    if (!vccs->get_dirty_bitmap) {
        return -EINVAL;
    }

    return vccs->get_dirty_bitmap(bcontainer, iova, size, ram_addr);
}

int vfio_container_add_section_window(VFIOContainer *bcontainer,
                                      MemoryRegionSection *section,
                                      Error **errp)
{
    VFIOContainerClass *vccs = VFIO_BASE_CONTAINER_OBJ_GET_CLASS(bcontainer);

    if (!vccs->add_window) {
        return 0;
    }

    return vccs->add_window(bcontainer, section, errp);
}

void vfio_container_del_section_window(VFIOContainer *bcontainer,
                                       MemoryRegionSection *section)
{
    VFIOContainerClass *vccs = VFIO_BASE_CONTAINER_OBJ_GET_CLASS(bcontainer);

    if (!vccs->del_window) {
        return;
    }

    return vccs->del_window(bcontainer, section);
}

void vfio_container_init(void *_bcontainer, size_t instance_size,
                         const char *mrtypename,
                         VFIOAddressSpace *space)
{
    VFIOContainer *bcontainer;

    object_initialize(_bcontainer, instance_size, mrtypename);
    bcontainer = VFIO_BASE_CONTAINER_OBJ(_bcontainer);

    bcontainer->space = space;
    bcontainer->error = NULL;
    bcontainer->dirty_pages_supported = false;
    bcontainer->dma_max_mappings = 0;
    QLIST_INIT(&bcontainer->giommu_list);
    QLIST_INIT(&bcontainer->hostwin_list);
    QLIST_INIT(&bcontainer->vrdl_list);
}

void vfio_container_destroy(VFIOContainer *bcontainer)
{
    VFIORamDiscardListener *vrdl, *vrdl_tmp;
    VFIOGuestIOMMU *giommu, *tmp;
    VFIOHostDMAWindow *hostwin, *next;

    QLIST_REMOVE(bcontainer, next);

    QLIST_FOREACH_SAFE(vrdl, &bcontainer->vrdl_list, next, vrdl_tmp) {
        RamDiscardManager *rdm = memory_region_get_ram_discard_manager(vrdl->mr);

        ram_discard_manager_unregister_listener(rdm, &vrdl->listener);
        QLIST_REMOVE(vrdl, next);
        g_free(vrdl);
    }

    QLIST_FOREACH_SAFE(giommu, &bcontainer->giommu_list, giommu_next, tmp) {
        memory_region_unregister_iommu_notifier(
                MEMORY_REGION(giommu->iommu_mr), &giommu->n);
        QLIST_REMOVE(giommu, giommu_next);
        g_free(giommu);
    }

    QLIST_FOREACH_SAFE(hostwin, &bcontainer->hostwin_list, hostwin_next,
                       next) {
        QLIST_REMOVE(hostwin, hostwin_next);
        g_free(hostwin);
    }
}

static void vfio_container_finalize_fn(Object *obj)
{
    VFIOContainer *bcontainer = VFIO_BASE_CONTAINER_OBJ(obj);

    printf("%s bcontainer: %p\n", __func__, bcontainer);
}

static const TypeInfo vfio_container_info = {
    .parent             = TYPE_OBJECT,
    .name               = TYPE_VFIO_BASE_CONTAINER_OBJ,
    .class_size         = sizeof(VFIOContainerClass),
    .instance_size      = sizeof(VFIOContainer),
    .instance_finalize  = vfio_container_finalize_fn,
    .abstract           = true,
};

static void vfio_container_register_types(void)
{
    type_register_static(&vfio_container_info);
}

type_init(vfio_container_register_types)
