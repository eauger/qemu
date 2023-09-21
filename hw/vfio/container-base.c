/*
 * VFIO BASE CONTAINER
 *
 * Copyright (C) 2023 Intel Corporation.
 * Copyright Red Hat, Inc. 2023
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
#include "qemu/error-report.h"
#include "hw/vfio/vfio-container-base.h"

int vfio_container_dma_map(VFIOContainer *container,
                           hwaddr iova, ram_addr_t size,
                           void *vaddr, bool readonly)
{
    if (!container->ops->dma_map) {
        return -EINVAL;
    }

    return container->ops->dma_map(container, iova, size, vaddr, readonly);
}

int vfio_container_dma_unmap(VFIOContainer *container,
                             hwaddr iova, ram_addr_t size,
                             IOMMUTLBEntry *iotlb)
{
    if (!container->ops->dma_unmap) {
        return -EINVAL;
    }

    return container->ops->dma_unmap(container, iova, size, iotlb);
}

void vfio_container_init(VFIOContainer *container,
                         struct VFIOIOMMUBackendOpsClass *ops)
{
    container->ops = ops;
    QLIST_INIT(&container->giommu_list);
}

void vfio_container_destroy(VFIOContainer *container)
{
    VFIOGuestIOMMU *giommu, *tmp;

    QLIST_FOREACH_SAFE(giommu, &container->giommu_list, giommu_next, tmp) {
        memory_region_unregister_iommu_notifier(
                MEMORY_REGION(giommu->iommu_mr), &giommu->n);
        QLIST_REMOVE(giommu, giommu_next);
        g_free(giommu);
    }
}

static const TypeInfo vfio_iommu_backend_ops_type_info = {
    .name = TYPE_VFIO_IOMMU_BACKEND_OPS,
    .parent = TYPE_OBJECT,
    .abstract = true,
    .class_size = sizeof(VFIOIOMMUBackendOpsClass),
};

static void vfio_iommu_backend_ops_register_types(void)
{
    type_register_static(&vfio_iommu_backend_ops_type_info);
}
type_init(vfio_iommu_backend_ops_register_types);
