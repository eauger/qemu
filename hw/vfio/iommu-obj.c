/*
 * VFIO CONTAINER BASE OBJECT
 *
 * Copyright (C) 2022 Intel Corporation.
 *
 * Authors: Yi Liu <yi.l.liu@intel.com>
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
#include "hw/vfio/vfio-iommu-obj.h"

int vfio_iommu_dma_map(VFIOIOMMUObj *iommu,
                       hwaddr iova, ram_addr_t size,
                       void *vaddr, bool readonly)
{
    VFIOIOMMUObjClass *vibjcs;

    if (!iommu) {
        return -EINVAL;
    }

    vibjcs = VFIO_IOMMU_OBJ_GET_CLASS(iommu);
    if (!vibjcs) {
        return -EINVAL;
    }

    if (!vibjcs->dma_map) {
        return -EINVAL;
    }

    return vibjcs->dma_map(iommu, iova, size, vaddr, readonly);
}

int vfio_iommu_dma_unmap(VFIOIOMMUObj *iommu,
                         hwaddr iova, ram_addr_t size,
                         IOMMUTLBEntry *iotlb)
{
    VFIOIOMMUObjClass *vibjcs;

    if (!iommu) {
        return -EINVAL;
    }

    vibjcs = VFIO_IOMMU_OBJ_GET_CLASS(iommu);
    if (!vibjcs) {
        return -EINVAL;
    }

    if (!vibjcs->dma_unmap) {
        return -EINVAL;
    }

    return vibjcs->dma_unmap(iommu, iova, size, iotlb);
}

void vfio_iommu_set_dirty_page_tracking(VFIOIOMMUObj *iommu, bool start)
{
    VFIOIOMMUObjClass *vibjcs;

    if (!iommu) {
        return;
    }

    vibjcs = VFIO_IOMMU_OBJ_GET_CLASS(iommu);
    if (!vibjcs) {
        return;
    }

    if (!vibjcs->set_dirty_page_tracking) {
        return;
    }

    vibjcs->set_dirty_page_tracking(iommu, start);
}

bool vfio_iommu_devices_all_dirty_tracking(VFIOIOMMUObj *iommu)
{
    VFIOIOMMUObjClass *vibjcs;

    if (!iommu) {
        return false;
    }

    vibjcs = VFIO_IOMMU_OBJ_GET_CLASS(iommu);
    if (!vibjcs) {
        return false;
    }

    if (!vibjcs->devices_all_dirty_tracking) {
        return false;
    }

    return vibjcs->devices_all_dirty_tracking(iommu);
}

int vfio_iommu_get_dirty_bitmap(VFIOIOMMUObj *iommu, uint64_t iova,
                                    uint64_t size, ram_addr_t ram_addr)
{
    VFIOIOMMUObjClass *vibjcs;

    if (!iommu) {
        return -EINVAL;
    }

    vibjcs = VFIO_IOMMU_OBJ_GET_CLASS(iommu);
    if (!vibjcs) {
        return -EINVAL;
    }

    if (!vibjcs->get_dirty_bitmap) {
        return -EINVAL;
    }

    return vibjcs->get_dirty_bitmap(iommu, iova, size, ram_addr);
}

int vfio_iommu_add_section_window(VFIOIOMMUObj *iommu,
                                  MemoryRegionSection *section,
                                  Error **errp)
{
    VFIOIOMMUObjClass *vibjcs;

    if (!iommu) {
        return -EINVAL;
    }

    vibjcs = VFIO_IOMMU_OBJ_GET_CLASS(iommu);
    if (!vibjcs) {
        return -EINVAL;
    }

    if (!vibjcs->add_window) {
        return 0;
    }

    return vibjcs->add_window(iommu, section, errp);
}

void vfio_iommu_del_section_window(VFIOIOMMUObj *iommu,
                                   MemoryRegionSection *section)
{
    VFIOIOMMUObjClass *vibjcs;

    if (!iommu) {
        return;
    }

    vibjcs = VFIO_IOMMU_OBJ_GET_CLASS(iommu);
    if (!vibjcs) {
        return;
    }

    if (!vibjcs->del_window) {
        return;
    }

    return vibjcs->del_window(iommu, section);
}

void vfio_iommu_init(void *_iommu, size_t instance_size,
                     const char *mrtypename,
                     VFIOAddressSpace *space)
{
    VFIOIOMMUObj *iommu;

    object_initialize(_iommu, instance_size, mrtypename);
    iommu = VFIO_IOMMU_OBJ(_iommu);

    iommu->space = space;
    iommu->error = NULL;
    iommu->dirty_pages_supported = false;
    iommu->dma_max_mappings = 0;
    QLIST_INIT(&iommu->giommu_list);
    QLIST_INIT(&iommu->hostwin_list);
    QLIST_INIT(&iommu->vrdl_list);
}

void vfio_iommu_destroy(VFIOIOMMUObj *iommu)
{

}

static void vfio_iommu_finalize_fn(Object *obj)
{
    VFIOIOMMUObj *iommu = VFIO_IOMMU_OBJ(obj);

    printf("%s iommu: %p\n", __func__, iommu);
}

static const TypeInfo host_iommu_context_info = {
    .parent             = TYPE_OBJECT,
    .name               = TYPE_VFIO_IOMMU_OBJ,
    .class_size         = sizeof(VFIOIOMMUObjClass),
    .instance_size      = sizeof(VFIOIOMMUObj),
    .instance_finalize  = vfio_iommu_finalize_fn,
    .abstract           = true,
};

static void vfio_iommu_register_types(void)
{
    type_register_static(&host_iommu_context_info);
}

type_init(vfio_iommu_register_types)
