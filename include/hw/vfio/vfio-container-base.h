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

#ifndef HW_VFIO_VFIO_BASE_CONTAINER_H
#define HW_VFIO_VFIO_BASE_CONTAINER_H

#include "exec/memory.h"
#ifndef CONFIG_USER_ONLY
#include "exec/hwaddr.h"
#endif

typedef struct VFIOAddressSpace VFIOAddressSpace;
typedef struct VFIOContainer VFIOContainer;
typedef struct VFIODevice VFIODevice;
typedef struct VFIOIOMMUBackendOpsClass VFIOIOMMUBackendOpsClass;

typedef struct {
    unsigned long *bitmap;
    hwaddr size;
    hwaddr pages;
} VFIOBitmap;

/*
 * This is the base object for vfio container backends
 */
struct VFIOContainer {
    VFIOIOMMUBackendOpsClass *ops;
};

#define TYPE_VFIO_IOMMU_BACKEND_LEGACY_OPS "vfio-iommu-backend-legacy-ops"
#define TYPE_VFIO_IOMMU_BACKEND_OPS "vfio-iommu-backend-ops"

DECLARE_CLASS_CHECKERS(VFIOIOMMUBackendOpsClass,
                       VFIO_IOMMU_BACKEND_OPS, TYPE_VFIO_IOMMU_BACKEND_OPS)

struct VFIOIOMMUBackendOpsClass {
    /*< private >*/
    ObjectClass parent_class;

    /*< public >*/
    /* required */
    VFIODevice *(*dev_iter_next)(VFIOContainer *container, VFIODevice *curr);
    int (*dma_map)(VFIOContainer *container,
                   hwaddr iova, ram_addr_t size,
                   void *vaddr, bool readonly);
    int (*dma_unmap)(VFIOContainer *container,
                     hwaddr iova, ram_addr_t size,
                     IOMMUTLBEntry *iotlb);
    int (*attach_device)(char *name, VFIODevice *vbasedev,
                         AddressSpace *as, Error **errp);
    void (*detach_device)(VFIODevice *vbasedev);
    /* migration feature */
    int (*set_dirty_page_tracking)(VFIOContainer *container, bool start);
    int (*query_dirty_bitmap)(VFIOContainer *bcontainer, VFIOBitmap *vbmap,
                              hwaddr iova, hwaddr size);

    /* SPAPR specific */
    int (*add_window)(VFIOContainer *container,
                      MemoryRegionSection *section,
                      Error **errp);
    void (*del_window)(VFIOContainer *container,
                       MemoryRegionSection *section);
};

#endif /* HW_VFIO_VFIO_BASE_CONTAINER_H */
