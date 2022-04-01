/*
 * QEMU abstraction of Host IOMMU
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

#ifndef HW_VFIO_VFIO_IOMMU_OBJ_H
#define HW_VFIO_VFIO_IOMMU_OBJ_H

#include "qom/object.h"
#include "exec/memory.h"
#include "qemu/queue.h"
#include "qemu/thread.h"
#ifndef CONFIG_USER_ONLY
#include "exec/hwaddr.h"
#endif

#define TYPE_VFIO_IOMMU_OBJ "qemu:vfio-iommu-obj"
#define VFIO_IOMMU_OBJ(obj) \
        OBJECT_CHECK(VFIOIOMMUObj, (obj), TYPE_VFIO_IOMMU_OBJ)
#define VFIO_IOMMU_OBJ_CLASS(klass) \
        OBJECT_CLASS_CHECK(VFIOIOMMUObjClass, (klass), \
                         TYPE_VFIO_IOMMU_OBJ)
#define VFIO_IOMMU_OBJ_GET_CLASS(obj) \
        OBJECT_GET_CLASS(VFIOIOMMUObjClass, (obj), \
                         TYPE_VFIO_IOMMU_OBJ)

typedef struct VFIOIOMMUObj VFIOIOMMUObj;

typedef struct VFIOAddressSpace {
    AddressSpace *as;
    QLIST_HEAD(, VFIOIOMMUObj) iommus;
    QLIST_ENTRY(VFIOAddressSpace) list;
} VFIOAddressSpace;

typedef struct VFIOGuestIOMMU {
    VFIOIOMMUObj *iommu;
    IOMMUMemoryRegion *iommu_mr;
    hwaddr iommu_offset;
    IOMMUNotifier n;
    QLIST_ENTRY(VFIOGuestIOMMU) giommu_next;
} VFIOGuestIOMMU;

typedef struct VFIORamDiscardListener {
    VFIOIOMMUObj *iommu;
    MemoryRegion *mr;
    hwaddr offset_within_address_space;
    hwaddr size;
    uint64_t granularity;
    RamDiscardListener listener;
    QLIST_ENTRY(VFIORamDiscardListener) next;
} VFIORamDiscardListener;

typedef struct VFIOHostDMAWindow {
    hwaddr min_iova;
    hwaddr max_iova;
    uint64_t iova_pgsizes;
    QLIST_ENTRY(VFIOHostDMAWindow) hostwin_next;
} VFIOHostDMAWindow;

/*
 * This is an abstraction of host IOMMU with dual-stage capability
 */
struct VFIOIOMMUObj {
    Object parent_obj;

    VFIOAddressSpace *space;
    MemoryListener listener;
    Error *error;
    bool initialized;
    bool dirty_pages_supported;
    uint64_t dirty_pgsizes;
    uint64_t max_dirty_bitmap_size;
    unsigned long pgsizes;
    unsigned int dma_max_mappings;
    QLIST_HEAD(, VFIOGuestIOMMU) giommu_list;
    QLIST_HEAD(, VFIOHostDMAWindow) hostwin_list;
    QLIST_HEAD(, VFIORamDiscardListener) vrdl_list;
    QLIST_ENTRY(VFIOIOMMUObj) next;
};

typedef struct VFIOIOMMUObjClass {
    /* private */
    ObjectClass parent_class;

    int (*dma_map)(VFIOIOMMUObj *iommu,
                   hwaddr iova, ram_addr_t size,
                   void *vaddr, bool readonly);
    int (*dma_unmap)(VFIOIOMMUObj *iommu,
                     hwaddr iova, ram_addr_t size,
                     IOMMUTLBEntry *iotlb);
    bool (*devices_all_dirty_tracking)(VFIOIOMMUObj *iommu);
    void (*set_dirty_page_tracking)(VFIOIOMMUObj *iommu, bool start);
    int (*get_dirty_bitmap)(VFIOIOMMUObj *iommu, uint64_t iova,
                            uint64_t size, ram_addr_t ram_addr);
    int (*add_window)(VFIOIOMMUObj *iommu,
                      MemoryRegionSection *section,
                      Error **errp);
    void (*del_window)(VFIOIOMMUObj *iommu,
                       MemoryRegionSection *section);
} VFIOIOMMUObjClass;

int vfio_iommu_dma_map(VFIOIOMMUObj *iommu,
                           hwaddr iova, ram_addr_t size,
                           void *vaddr, bool readonly);
int vfio_iommu_dma_unmap(VFIOIOMMUObj *iommu,
                             hwaddr iova, ram_addr_t size,
                             IOMMUTLBEntry *iotlb);
bool vfio_iommu_devices_all_dirty_tracking(VFIOIOMMUObj *iommu);
void vfio_iommu_set_dirty_page_tracking(VFIOIOMMUObj *iommu, bool start);
int vfio_iommu_get_dirty_bitmap(VFIOIOMMUObj *iommu, uint64_t iova,
                                    uint64_t size, ram_addr_t ram_addr);
int vfio_iommu_add_section_window(VFIOIOMMUObj *iommu,
                                  MemoryRegionSection *section,
                                  Error **errp);
void vfio_iommu_del_section_window(VFIOIOMMUObj *iommu,
                                   MemoryRegionSection *section);

void vfio_iommu_init(void *_container, size_t instance_size,
                     const char *mrtypename,
                     VFIOAddressSpace *space);
void vfio_iommu_destroy(VFIOIOMMUObj *iommu);
#endif /* HW_VFIO_VFIO_IOMMU_OBJ_H */
