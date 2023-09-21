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
#ifdef CONFIG_KVM
#include <linux/kvm.h>
#endif
#include <linux/vfio.h>

#include "hw/vfio/vfio-common.h"
#include "hw/vfio/vfio.h"
#include "hw/vfio/pci.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "exec/ram_addr.h"
#include "hw/hw.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/range.h"
#include "sysemu/kvm.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "trace.h"
#include "qapi/error.h"
#include "migration/migration.h"
#include "migration/misc.h"
#include "migration/blocker.h"
#include "migration/qemu-file.h"
#include "sysemu/tpm.h"

static QLIST_HEAD(, VFIOAddressSpace) vfio_address_spaces =
    QLIST_HEAD_INITIALIZER(vfio_address_spaces);

#ifdef CONFIG_KVM
/*
 * We have a single VFIO pseudo device per KVM VM.  Once created it lives
 * for the life of the VM.  Closing the file descriptor only drops our
 * reference to it and the device's reference to kvm.  Therefore once
 * initialized, this file descriptor is only released on QEMU exit and
 * we'll re-use it should another vfio device be attached before then.
 */
int vfio_kvm_device_fd = -1;
#endif

/*
 * Device state interfaces
 */

int vfio_bitmap_alloc(VFIOBitmap *vbmap, hwaddr size)
{
    vbmap->pages = REAL_HOST_PAGE_ALIGN(size) / qemu_real_host_page_size();
    vbmap->size = ROUND_UP(vbmap->pages, sizeof(__u64) * BITS_PER_BYTE) /
                                         BITS_PER_BYTE;
    vbmap->bitmap = g_try_malloc0(vbmap->size);
    if (!vbmap->bitmap) {
        return -ENOMEM;
    }

    return 0;
}

bool vfio_mig_active(void)
{
    VFIOAddressSpace *space;
    VFIOContainer *container;
    VFIODevice *vbasedev;

    if (QLIST_EMPTY(&vfio_address_spaces)) {
        return false;
    }

    QLIST_FOREACH(space, &vfio_address_spaces, list) {
        QLIST_FOREACH(container, &space->containers, next) {
            vbasedev = NULL;
            while ((vbasedev = vfio_container_dev_iter_next(container,
                                                            vbasedev))) {
                if (vbasedev->migration_blocker) {
                    return false;
                }
            }
        }
    }
    return true;
}

static Error *multiple_devices_migration_blocker;

/*
 * Multiple devices migration is allowed only if all devices support P2P
 * migration. Single device migration is allowed regardless of P2P migration
 * support.
 */
static bool vfio_multiple_devices_migration_is_supported(void)
{
    VFIOAddressSpace *space;
    VFIOContainer *container;
    VFIODevice *vbasedev;
    unsigned int device_num = 0;
    bool all_support_p2p = true;

    QLIST_FOREACH(space, &vfio_address_spaces, list) {
        QLIST_FOREACH(container, &space->containers, next) {
            vbasedev = NULL;
            while ((vbasedev = vfio_container_dev_iter_next(container,
                                                            vbasedev))) {
                if (vbasedev->migration) {
                    device_num++;

                    if (!(vbasedev->migration->mig_flags &
                          VFIO_MIGRATION_P2P)) {
                        all_support_p2p = false;
                    }
                }
            }
        }
    }

    return all_support_p2p || device_num <= 1;
}

int vfio_block_multiple_devices_migration(VFIODevice *vbasedev, Error **errp)
{
    int ret;

    if (vfio_multiple_devices_migration_is_supported()) {
        return 0;
    }

    if (vbasedev->enable_migration == ON_OFF_AUTO_ON) {
        error_setg(errp, "Multiple VFIO devices migration is supported only if "
                         "all of them support P2P migration");
        return -EINVAL;
    }

    if (multiple_devices_migration_blocker) {
        return 0;
    }

    error_setg(&multiple_devices_migration_blocker,
               "Multiple VFIO devices migration is supported only if all of "
               "them support P2P migration");
    ret = migrate_add_blocker(multiple_devices_migration_blocker, errp);
    if (ret < 0) {
        error_free(multiple_devices_migration_blocker);
        multiple_devices_migration_blocker = NULL;
    }

    return ret;
}

void vfio_unblock_multiple_devices_migration(void)
{
    if (!multiple_devices_migration_blocker ||
        !vfio_multiple_devices_migration_is_supported()) {
        return;
    }

    migrate_del_blocker(multiple_devices_migration_blocker);
    error_free(multiple_devices_migration_blocker);
    multiple_devices_migration_blocker = NULL;
}

static void vfio_set_migration_error(int err)
{
    MigrationState *ms = migrate_get_current();

    if (migration_is_setup_or_active(ms->state)) {
        WITH_QEMU_LOCK_GUARD(&ms->qemu_file_lock) {
            if (ms->to_dst_file) {
                qemu_file_set_error(ms->to_dst_file, err);
            }
        }
    }
}

bool vfio_device_state_is_running(VFIODevice *vbasedev)
{
    VFIOMigration *migration = vbasedev->migration;

    return migration->device_state == VFIO_DEVICE_STATE_RUNNING ||
           migration->device_state == VFIO_DEVICE_STATE_RUNNING_P2P;
}

bool vfio_device_state_is_precopy(VFIODevice *vbasedev)
{
    VFIOMigration *migration = vbasedev->migration;

    return migration->device_state == VFIO_DEVICE_STATE_PRE_COPY ||
           migration->device_state == VFIO_DEVICE_STATE_PRE_COPY_P2P;
}

static bool vfio_devices_all_dirty_tracking(VFIOLegacyContainer *container)
{
    VFIODevice *vbasedev = NULL;
    MigrationState *ms = migrate_get_current();

    if (ms->state != MIGRATION_STATUS_ACTIVE &&
        ms->state != MIGRATION_STATUS_DEVICE) {
        return false;
    }

    while ((vbasedev = vfio_container_dev_iter_next(&container->bcontainer, vbasedev))) {
        VFIOMigration *migration = vbasedev->migration;

        if (!migration) {
            return false;
        }

        if (vbasedev->pre_copy_dirty_page_tracking == ON_OFF_AUTO_OFF &&
            (vfio_device_state_is_running(vbasedev) ||
             vfio_device_state_is_precopy(vbasedev))) {
                return false;
        }
    }
    return true;
}

bool vfio_devices_all_device_dirty_tracking(VFIOLegacyContainer *container)
{
    VFIODevice *vbasedev = NULL;

    while ((vbasedev = vfio_container_dev_iter_next(&container->bcontainer, vbasedev))) {
        if (!vbasedev->dirty_pages_supported) {
            return false;
        }
    }

    return true;
}

/*
 * Check if all VFIO devices are running and migration is active, which is
 * essentially equivalent to the migration being in pre-copy phase.
 */
bool vfio_devices_all_running_and_mig_active(VFIOLegacyContainer *container)
{
    VFIODevice *vbasedev = NULL;

    if (!migration_is_active(migrate_get_current())) {
        return false;
    }

    while ((vbasedev = vfio_container_dev_iter_next(&container->bcontainer, vbasedev))) {
        VFIOMigration *migration = vbasedev->migration;

        if (!migration) {
            return false;
        }

        if (vfio_device_state_is_running(vbasedev) ||
            vfio_device_state_is_precopy(vbasedev)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

void vfio_host_win_add(VFIOLegacyContainer *container, hwaddr min_iova,
                       hwaddr max_iova, uint64_t iova_pgsizes)
{
    VFIOHostDMAWindow *hostwin;

    QLIST_FOREACH(hostwin, &container->hostwin_list, hostwin_next) {
        if (ranges_overlap(hostwin->min_iova,
                           hostwin->max_iova - hostwin->min_iova + 1,
                           min_iova,
                           max_iova - min_iova + 1)) {
            hw_error("%s: Overlapped IOMMU are not enabled", __func__);
        }
    }

    hostwin = g_malloc0(sizeof(*hostwin));

    hostwin->min_iova = min_iova;
    hostwin->max_iova = max_iova;
    hostwin->iova_pgsizes = iova_pgsizes;
    QLIST_INSERT_HEAD(&container->hostwin_list, hostwin, hostwin_next);
}

int vfio_host_win_del(VFIOLegacyContainer *container,
                      hwaddr min_iova, hwaddr max_iova)
{
    VFIOHostDMAWindow *hostwin;

    QLIST_FOREACH(hostwin, &container->hostwin_list, hostwin_next) {
        if (hostwin->min_iova == min_iova && hostwin->max_iova == max_iova) {
            QLIST_REMOVE(hostwin, hostwin_next);
            g_free(hostwin);
            return 0;
        }
    }

    return -1;
}

static bool vfio_listener_skipped_section(MemoryRegionSection *section)
{
    return (!memory_region_is_ram(section->mr) &&
            !memory_region_is_iommu(section->mr)) ||
           memory_region_is_protected(section->mr) ||
           /*
            * Sizing an enabled 64-bit BAR can cause spurious mappings to
            * addresses in the upper part of the 64-bit address space.  These
            * are never accessed by the CPU and beyond the address width of
            * some IOMMU hardware.  TODO: VFIO should tell us the IOMMU width.
            */
           section->offset_within_address_space & (1ULL << 63);
}

/* Called with rcu_read_lock held.  */
static bool vfio_get_xlat_addr(IOMMUTLBEntry *iotlb, void **vaddr,
                               ram_addr_t *ram_addr, bool *read_only)
{
    bool ret, mr_has_discard_manager;

    ret = memory_get_xlat_addr(iotlb, vaddr, ram_addr, read_only,
                               &mr_has_discard_manager);
    if (ret && mr_has_discard_manager) {
        /*
         * Malicious VMs might trigger discarding of IOMMU-mapped memory. The
         * pages will remain pinned inside vfio until unmapped, resulting in a
         * higher memory consumption than expected. If memory would get
         * populated again later, there would be an inconsistency between pages
         * pinned by vfio and pages seen by QEMU. This is the case until
         * unmapped from the IOMMU (e.g., during device reset).
         *
         * With malicious guests, we really only care about pinning more memory
         * than expected. RLIMIT_MEMLOCK set for the user/process can never be
         * exceeded and can be used to mitigate this problem.
         */
        warn_report_once("Using vfio with vIOMMUs and coordinated discarding of"
                         " RAM (e.g., virtio-mem) works, however, malicious"
                         " guests can trigger pinning of more memory than"
                         " intended via an IOMMU. It's possible to mitigate "
                         " by setting/adjusting RLIMIT_MEMLOCK.");
    }
    return ret;
}

static void vfio_iommu_map_notify(IOMMUNotifier *n, IOMMUTLBEntry *iotlb)
{
    VFIOGuestIOMMU *giommu = container_of(n, VFIOGuestIOMMU, n);
    VFIOContainer *container = giommu->container;
    hwaddr iova = iotlb->iova + giommu->iommu_offset;
    void *vaddr;
    int ret;

    trace_vfio_iommu_map_notify(iotlb->perm == IOMMU_NONE ? "UNMAP" : "MAP",
                                iova, iova + iotlb->addr_mask);

    if (iotlb->target_as != &address_space_memory) {
        error_report("Wrong target AS \"%s\", only system memory is allowed",
                     iotlb->target_as->name ? iotlb->target_as->name : "none");
        vfio_set_migration_error(-EINVAL);
        return;
    }

    rcu_read_lock();

    if ((iotlb->perm & IOMMU_RW) != IOMMU_NONE) {
        bool read_only;

        if (!vfio_get_xlat_addr(iotlb, &vaddr, NULL, &read_only)) {
            goto out;
        }
        /*
         * vaddr is only valid until rcu_read_unlock(). But after
         * vfio_dma_map has set up the mapping the pages will be
         * pinned by the kernel. This makes sure that the RAM backend
         * of vaddr will always be there, even if the memory object is
         * destroyed and its backing memory munmap-ed.
         */
        ret = vfio_container_dma_map(container, iova,
                                     iotlb->addr_mask + 1, vaddr,
                                     read_only);
        if (ret) {
            error_report("vfio_container_dma_map(%p, 0x%"HWADDR_PRIx", "
                         "0x%"HWADDR_PRIx", %p) = %d (%s)",
                         container, iova,
                         iotlb->addr_mask + 1, vaddr, ret, strerror(-ret));
        }
    } else {
        ret = vfio_container_dma_unmap(container, iova,
                                       iotlb->addr_mask + 1, iotlb);
        if (ret) {
            error_report("vfio_container_dma_unmap(%p, 0x%"HWADDR_PRIx", "
                         "0x%"HWADDR_PRIx") = %d (%s)",
                         container, iova,
                         iotlb->addr_mask + 1, ret, strerror(-ret));
            vfio_set_migration_error(ret);
        }
    }
out:
    rcu_read_unlock();
}

static void vfio_ram_discard_notify_discard(RamDiscardListener *rdl,
                                            MemoryRegionSection *section)
{
    VFIORamDiscardListener *vrdl = container_of(rdl, VFIORamDiscardListener,
                                                listener);
    const hwaddr size = int128_get64(section->size);
    const hwaddr iova = section->offset_within_address_space;
    int ret;

    /* Unmap with a single call. */
    ret = vfio_container_dma_unmap(&vrdl->container->bcontainer, iova, size , NULL);
    if (ret) {
        error_report("%s: vfio_container_dma_unmap() failed: %s", __func__,
                     strerror(-ret));
    }
}

static int vfio_ram_discard_notify_populate(RamDiscardListener *rdl,
                                            MemoryRegionSection *section)
{
    VFIORamDiscardListener *vrdl = container_of(rdl, VFIORamDiscardListener,
                                                listener);
    const hwaddr end = section->offset_within_region +
                       int128_get64(section->size);
    hwaddr start, next, iova;
    void *vaddr;
    int ret;

    /*
     * Map in (aligned within memory region) minimum granularity, so we can
     * unmap in minimum granularity later.
     */
    for (start = section->offset_within_region; start < end; start = next) {
        next = ROUND_UP(start + 1, vrdl->granularity);
        next = MIN(next, end);

        iova = start - section->offset_within_region +
               section->offset_within_address_space;
        vaddr = memory_region_get_ram_ptr(section->mr) + start;

        ret = vfio_container_dma_map(&vrdl->container->bcontainer, iova, next - start,
                                     vaddr, section->readonly);
        if (ret) {
            /* Rollback */
            vfio_ram_discard_notify_discard(rdl, section);
            return ret;
        }
    }
    return 0;
}

static void vfio_register_ram_discard_listener(VFIOLegacyContainer *container,
                                               MemoryRegionSection *section)
{
    RamDiscardManager *rdm = memory_region_get_ram_discard_manager(section->mr);
    VFIORamDiscardListener *vrdl;

    /* Ignore some corner cases not relevant in practice. */
    g_assert(QEMU_IS_ALIGNED(section->offset_within_region, TARGET_PAGE_SIZE));
    g_assert(QEMU_IS_ALIGNED(section->offset_within_address_space,
                             TARGET_PAGE_SIZE));
    g_assert(QEMU_IS_ALIGNED(int128_get64(section->size), TARGET_PAGE_SIZE));

    vrdl = g_new0(VFIORamDiscardListener, 1);
    vrdl->container = container;
    vrdl->mr = section->mr;
    vrdl->offset_within_address_space = section->offset_within_address_space;
    vrdl->size = int128_get64(section->size);
    vrdl->granularity = ram_discard_manager_get_min_granularity(rdm,
                                                                section->mr);

    g_assert(vrdl->granularity && is_power_of_2(vrdl->granularity));
    g_assert(container->pgsizes &&
             vrdl->granularity >= 1ULL << ctz64(container->pgsizes));

    ram_discard_listener_init(&vrdl->listener,
                              vfio_ram_discard_notify_populate,
                              vfio_ram_discard_notify_discard, true);
    ram_discard_manager_register_listener(rdm, &vrdl->listener, section);
    QLIST_INSERT_HEAD(&container->vrdl_list, vrdl, next);

    /*
     * Sanity-check if we have a theoretically problematic setup where we could
     * exceed the maximum number of possible DMA mappings over time. We assume
     * that each mapped section in the same address space as a RamDiscardManager
     * section consumes exactly one DMA mapping, with the exception of
     * RamDiscardManager sections; i.e., we don't expect to have gIOMMU sections
     * in the same address space as RamDiscardManager sections.
     *
     * We assume that each section in the address space consumes one memslot.
     * We take the number of KVM memory slots as a best guess for the maximum
     * number of sections in the address space we could have over time,
     * also consuming DMA mappings.
     */
    if (container->dma_max_mappings) {
        unsigned int vrdl_count = 0, vrdl_mappings = 0, max_memslots = 512;

#ifdef CONFIG_KVM
        if (kvm_enabled()) {
            max_memslots = kvm_get_max_memslots();
        }
#endif

        QLIST_FOREACH(vrdl, &container->vrdl_list, next) {
            hwaddr start, end;

            start = QEMU_ALIGN_DOWN(vrdl->offset_within_address_space,
                                    vrdl->granularity);
            end = ROUND_UP(vrdl->offset_within_address_space + vrdl->size,
                           vrdl->granularity);
            vrdl_mappings += (end - start) / vrdl->granularity;
            vrdl_count++;
        }

        if (vrdl_mappings + max_memslots - vrdl_count >
            container->dma_max_mappings) {
            warn_report("%s: possibly running out of DMA mappings. E.g., try"
                        " increasing the 'block-size' of virtio-mem devies."
                        " Maximum possible DMA mappings: %d, Maximum possible"
                        " memslots: %d", __func__, container->dma_max_mappings,
                        max_memslots);
        }
    }
}

static void vfio_unregister_ram_discard_listener(VFIOLegacyContainer *container,
                                                 MemoryRegionSection *section)
{
    RamDiscardManager *rdm = memory_region_get_ram_discard_manager(section->mr);
    VFIORamDiscardListener *vrdl = NULL;

    QLIST_FOREACH(vrdl, &container->vrdl_list, next) {
        if (vrdl->mr == section->mr &&
            vrdl->offset_within_address_space ==
            section->offset_within_address_space) {
            break;
        }
    }

    if (!vrdl) {
        hw_error("vfio: Trying to unregister missing RAM discard listener");
    }

    ram_discard_manager_unregister_listener(rdm, &vrdl->listener);
    QLIST_REMOVE(vrdl, next);
    g_free(vrdl);
}

static VFIOHostDMAWindow *vfio_find_hostwin(VFIOLegacyContainer *container,
                                            hwaddr iova, hwaddr end)
{
    VFIOHostDMAWindow *hostwin;
    bool hostwin_found = false;

    QLIST_FOREACH(hostwin, &container->hostwin_list, hostwin_next) {
        if (hostwin->min_iova <= iova && end <= hostwin->max_iova) {
            hostwin_found = true;
            break;
        }
    }

    return hostwin_found ? hostwin : NULL;
}

static bool vfio_known_safe_misalignment(MemoryRegionSection *section)
{
    MemoryRegion *mr = section->mr;

    if (!TPM_IS_CRB(mr->owner)) {
        return false;
    }

    /* this is a known safe misaligned region, just trace for debug purpose */
    trace_vfio_known_safe_misalignment(memory_region_name(mr),
                                       section->offset_within_address_space,
                                       section->offset_within_region,
                                       qemu_real_host_page_size());
    return true;
}

static bool vfio_listener_valid_section(MemoryRegionSection *section,
                                        const char *name)
{
    if (vfio_listener_skipped_section(section)) {
        trace_vfio_listener_region_skip(name,
                section->offset_within_address_space,
                section->offset_within_address_space +
                int128_get64(int128_sub(section->size, int128_one())));
        return false;
    }

    if (unlikely((section->offset_within_address_space &
                  ~qemu_real_host_page_mask()) !=
                 (section->offset_within_region & ~qemu_real_host_page_mask()))) {
        if (!vfio_known_safe_misalignment(section)) {
            error_report("%s received unaligned region %s iova=0x%"PRIx64
                         " offset_within_region=0x%"PRIx64
                         " qemu_real_host_page_size=0x%"PRIxPTR,
                         __func__, memory_region_name(section->mr),
                         section->offset_within_address_space,
                         section->offset_within_region,
                         qemu_real_host_page_size());
        }
        return false;
    }

    return true;
}

static bool vfio_get_section_iova_range(VFIOLegacyContainer *container,
                                        MemoryRegionSection *section,
                                        hwaddr *out_iova, hwaddr *out_end,
                                        Int128 *out_llend)
{
    Int128 llend;
    hwaddr iova;

    iova = REAL_HOST_PAGE_ALIGN(section->offset_within_address_space);
    llend = int128_make64(section->offset_within_address_space);
    llend = int128_add(llend, section->size);
    llend = int128_and(llend, int128_exts64(qemu_real_host_page_mask()));

    if (int128_ge(int128_make64(iova), llend)) {
        return false;
    }

    *out_iova = iova;
    *out_end = int128_get64(int128_sub(llend, int128_one()));
    if (out_llend) {
        *out_llend = llend;
    }
    return true;
}

static void vfio_listener_region_add(MemoryListener *listener,
                                     MemoryRegionSection *section)
{
    VFIOLegacyContainer *container = container_of(listener, VFIOLegacyContainer, listener);
    VFIOContainer *bcontainer = &container->bcontainer;
    hwaddr iova, end;
    Int128 llend, llsize;
    void *vaddr;
    int ret;
    VFIOHostDMAWindow *hostwin;
    Error *err = NULL;

    if (!vfio_listener_valid_section(section, "region_add")) {
        return;
    }

    if (!vfio_get_section_iova_range(container, section, &iova, &end, &llend)) {
        if (memory_region_is_ram_device(section->mr)) {
            trace_vfio_listener_region_add_no_dma_map(
                memory_region_name(section->mr),
                section->offset_within_address_space,
                int128_getlo(section->size),
                qemu_real_host_page_size());
        }
        return;
    }

    if (vfio_container_add_section_window(container, section, &err)) {
        goto fail;
    }

    hostwin = vfio_find_hostwin(container, iova, end);
    if (!hostwin) {
        error_setg(&err, "Container %p can't map guest IOVA region"
                   " 0x%"HWADDR_PRIx"..0x%"HWADDR_PRIx, container, iova, end);
        goto fail;
    }

    memory_region_ref(section->mr);

    if (memory_region_is_iommu(section->mr)) {
        VFIOGuestIOMMU *giommu;
        IOMMUMemoryRegion *iommu_mr = IOMMU_MEMORY_REGION(section->mr);
        int iommu_idx;

        trace_vfio_listener_region_add_iommu(iova, end);
        /*
         * FIXME: For VFIO iommu types which have KVM acceleration to
         * avoid bouncing all map/unmaps through qemu this way, this
         * would be the right place to wire that up (tell the KVM
         * device emulation the VFIO iommu handles to use).
         */
        giommu = g_malloc0(sizeof(*giommu));
        giommu->iommu_mr = iommu_mr;
        giommu->iommu_offset = section->offset_within_address_space -
                               section->offset_within_region;
        giommu->container = bcontainer;
        llend = int128_add(int128_make64(section->offset_within_region),
                           section->size);
        llend = int128_sub(llend, int128_one());
        iommu_idx = memory_region_iommu_attrs_to_index(iommu_mr,
                                                       MEMTXATTRS_UNSPECIFIED);
        iommu_notifier_init(&giommu->n, vfio_iommu_map_notify,
                            IOMMU_NOTIFIER_IOTLB_EVENTS,
                            section->offset_within_region,
                            int128_get64(llend),
                            iommu_idx);

        ret = memory_region_iommu_set_page_size_mask(giommu->iommu_mr,
                                                     container->pgsizes,
                                                     &err);
        if (ret) {
            g_free(giommu);
            goto fail;
        }

        ret = memory_region_register_iommu_notifier(section->mr, &giommu->n,
                                                    &err);
        if (ret) {
            g_free(giommu);
            goto fail;
        }
        QLIST_INSERT_HEAD(&bcontainer->giommu_list, giommu, giommu_next);
        memory_region_iommu_replay(giommu->iommu_mr, &giommu->n);

        return;
    }

    /* Here we assume that memory_region_is_ram(section->mr)==true */

    /*
     * For RAM memory regions with a RamDiscardManager, we only want to map the
     * actually populated parts - and update the mapping whenever we're notified
     * about changes.
     */
    if (memory_region_has_ram_discard_manager(section->mr)) {
        vfio_register_ram_discard_listener(container, section);
        return;
    }

    vaddr = memory_region_get_ram_ptr(section->mr) +
            section->offset_within_region +
            (iova - section->offset_within_address_space);

    trace_vfio_listener_region_add_ram(iova, end, vaddr);

    llsize = int128_sub(llend, int128_make64(iova));

    if (memory_region_is_ram_device(section->mr)) {
        hwaddr pgmask = (1ULL << ctz64(hostwin->iova_pgsizes)) - 1;

        if ((iova & pgmask) || (int128_get64(llsize) & pgmask)) {
            trace_vfio_listener_region_add_no_dma_map(
                memory_region_name(section->mr),
                section->offset_within_address_space,
                int128_getlo(section->size),
                pgmask + 1);
            return;
        }
    }

    ret = vfio_container_dma_map(&container->bcontainer, iova, int128_get64(llsize),
                                 vaddr, section->readonly);
    if (ret) {
        error_setg(&err, "vfio_container_dma_map(%p, 0x%"HWADDR_PRIx", "
                   "0x%"HWADDR_PRIx", %p) = %d (%s)",
                   container, iova, int128_get64(llsize), vaddr, ret,
                   strerror(-ret));
        if (memory_region_is_ram_device(section->mr)) {
            /* Allow unexpected mappings not to be fatal for RAM devices */
            error_report_err(err);
            return;
        }
        goto fail;
    }

    return;

fail:
    if (memory_region_is_ram_device(section->mr)) {
        error_report("failed to vfio_container_dma_map. pci p2p may not work");
        return;
    }
    /*
     * On the initfn path, store the first error in the container so we
     * can gracefully fail.  Runtime, there's not much we can do other
     * than throw a hardware error.
     */
    if (!container->initialized) {
        if (!container->error) {
            error_propagate_prepend(&container->error, err,
                                    "Region %s: ",
                                    memory_region_name(section->mr));
        } else {
            error_free(err);
        }
    } else {
        error_report_err(err);
        hw_error("vfio: DMA mapping failed, unable to continue");
    }
}

static void vfio_listener_region_del(MemoryListener *listener,
                                     MemoryRegionSection *section)
{
    VFIOLegacyContainer *container = container_of(listener, VFIOLegacyContainer, listener);
    VFIOContainer *bcontainer = &container->bcontainer;
    hwaddr iova, end;
    Int128 llend, llsize;
    int ret;
    bool try_unmap = true;

    if (!vfio_listener_valid_section(section, "region_del")) {
        return;
    }

    if (memory_region_is_iommu(section->mr)) {
        VFIOGuestIOMMU *giommu;

        QLIST_FOREACH(giommu, &bcontainer->giommu_list, giommu_next) {
            if (MEMORY_REGION(giommu->iommu_mr) == section->mr &&
                giommu->n.start == section->offset_within_region) {
                memory_region_unregister_iommu_notifier(section->mr,
                                                        &giommu->n);
                QLIST_REMOVE(giommu, giommu_next);
                g_free(giommu);
                break;
            }
        }

        /*
         * FIXME: We assume the one big unmap below is adequate to
         * remove any individual page mappings in the IOMMU which
         * might have been copied into VFIO. This works for a page table
         * based IOMMU where a big unmap flattens a large range of IO-PTEs.
         * That may not be true for all IOMMU types.
         */
    }

    if (!vfio_get_section_iova_range(container, section, &iova, &end, &llend)) {
        return;
    }

    llsize = int128_sub(llend, int128_make64(iova));

    trace_vfio_listener_region_del(iova, end);

    if (memory_region_is_ram_device(section->mr)) {
        hwaddr pgmask;
        VFIOHostDMAWindow *hostwin;

        hostwin = vfio_find_hostwin(container, iova, end);
        assert(hostwin); /* or region_add() would have failed */

        pgmask = (1ULL << ctz64(hostwin->iova_pgsizes)) - 1;
        try_unmap = !((iova & pgmask) || (int128_get64(llsize) & pgmask));
    } else if (memory_region_has_ram_discard_manager(section->mr)) {
        vfio_unregister_ram_discard_listener(container, section);
        /* Unregistering will trigger an unmap. */
        try_unmap = false;
    }

    if (try_unmap) {
        if (int128_eq(llsize, int128_2_64())) {
            /* The unmap ioctl doesn't accept a full 64-bit span. */
            llsize = int128_rshift(llsize, 1);
            ret = vfio_container_dma_unmap(&container->bcontainer, iova,
                                           int128_get64(llsize), NULL);
            if (ret) {
                error_report("vfio_container_dma_unmap(%p, 0x%"HWADDR_PRIx", "
                             "0x%"HWADDR_PRIx") = %d (%s)",
                             container, iova, int128_get64(llsize), ret,
                             strerror(-ret));
            }
            iova += int128_get64(llsize);
        }
        ret = vfio_container_dma_unmap(&container->bcontainer, iova,
                                       int128_get64(llsize), NULL);
        if (ret) {
            error_report("vfio_container_dma_unmap(%p, 0x%"HWADDR_PRIx", "
                         "0x%"HWADDR_PRIx") = %d (%s)",
                         container, iova, int128_get64(llsize), ret,
                         strerror(-ret));
        }
    }

    memory_region_unref(section->mr);

    vfio_container_del_section_window(container, section);
}

typedef struct VFIODirtyRanges {
    hwaddr min32;
    hwaddr max32;
    hwaddr min64;
    hwaddr max64;
    hwaddr minpci64;
    hwaddr maxpci64;
} VFIODirtyRanges;

typedef struct VFIODirtyRangesListener {
    VFIOLegacyContainer *container;
    VFIODirtyRanges ranges;
    MemoryListener listener;
} VFIODirtyRangesListener;

static bool vfio_section_is_vfio_pci(MemoryRegionSection *section,
                                     VFIOLegacyContainer *container)
{
    VFIOPCIDevice *pcidev;
    VFIODevice *vbasedev = NULL;
    Object *owner;

    owner = memory_region_owner(section->mr);

    while ((vbasedev = vfio_container_dev_iter_next(&container->bcontainer, vbasedev))) {
        if (vbasedev->type != VFIO_DEVICE_TYPE_PCI) {
            continue;
        }
        pcidev = container_of(vbasedev, VFIOPCIDevice, vbasedev);
        if (OBJECT(pcidev) == owner) {
            return true;
        }
    }

    return false;
}

static void vfio_dirty_tracking_update(MemoryListener *listener,
                                       MemoryRegionSection *section)
{
    VFIODirtyRangesListener *dirty = container_of(listener,
                                                  VFIODirtyRangesListener,
                                                  listener);
    VFIODirtyRanges *range = &dirty->ranges;
    hwaddr iova, end, *min, *max;

    if (!vfio_listener_valid_section(section, "tracking_update") ||
        !vfio_get_section_iova_range(dirty->container, section,
                                     &iova, &end, NULL)) {
        return;
    }

    /*
     * The address space passed to the dirty tracker is reduced to three ranges:
     * one for 32-bit DMA ranges, one for 64-bit DMA ranges and one for the
     * PCI 64-bit hole.
     *
     * The underlying reports of dirty will query a sub-interval of each of
     * these ranges.
     *
     * The purpose of the three range handling is to handle known cases of big
     * holes in the address space, like the x86 AMD 1T hole, and firmware (like
     * OVMF) which may relocate the pci-hole64 to the end of the address space.
     * The latter would otherwise generate large ranges for tracking, stressing
     * the limits of supported hardware. The pci-hole32 will always be below 4G
     * (overlapping or not) so it doesn't need special handling and is part of
     * the 32-bit range.
     *
     * The alternative would be an IOVATree but that has a much bigger runtime
     * overhead and unnecessary complexity.
     */
    if (vfio_section_is_vfio_pci(section, dirty->container) &&
        iova >= UINT32_MAX) {
        min = &range->minpci64;
        max = &range->maxpci64;
    } else {
        min = (end <= UINT32_MAX) ? &range->min32 : &range->min64;
        max = (end <= UINT32_MAX) ? &range->max32 : &range->max64;
    }
    if (*min > iova) {
        *min = iova;
    }
    if (*max < end) {
        *max = end;
    }

    trace_vfio_device_dirty_tracking_update(iova, end, *min, *max);
    return;
}

static const MemoryListener vfio_dirty_tracking_listener = {
    .name = "vfio-tracking",
    .region_add = vfio_dirty_tracking_update,
};

static void vfio_dirty_tracking_init(VFIOLegacyContainer *container,
                                     VFIODirtyRanges *ranges)
{
    VFIODirtyRangesListener dirty;

    memset(&dirty, 0, sizeof(dirty));
    dirty.ranges.min32 = UINT32_MAX;
    dirty.ranges.min64 = UINT64_MAX;
    dirty.ranges.minpci64 = UINT64_MAX;
    dirty.listener = vfio_dirty_tracking_listener;
    dirty.container = container;

    memory_listener_register(&dirty.listener,
                             container->bcontainer.space->as);

    *ranges = dirty.ranges;

    /*
     * The memory listener is synchronous, and used to calculate the range
     * to dirty tracking. Unregister it after we are done as we are not
     * interested in any follow-up updates.
     */
    memory_listener_unregister(&dirty.listener);
}

static void vfio_devices_dma_logging_stop(VFIOLegacyContainer *container)
{
    uint64_t buf[DIV_ROUND_UP(sizeof(struct vfio_device_feature),
                              sizeof(uint64_t))] = {};
    struct vfio_device_feature *feature = (struct vfio_device_feature *)buf;
    VFIODevice *vbasedev = NULL;

    feature->argsz = sizeof(buf);
    feature->flags = VFIO_DEVICE_FEATURE_SET |
                     VFIO_DEVICE_FEATURE_DMA_LOGGING_STOP;

    while ((vbasedev = vfio_container_dev_iter_next(&container->bcontainer, vbasedev))) {
        if (!vbasedev->dirty_tracking) {
            continue;
        }

        if (ioctl(vbasedev->fd, VFIO_DEVICE_FEATURE, feature)) {
            warn_report("%s: Failed to stop DMA logging, err %d (%s)",
                        vbasedev->name, -errno, strerror(errno));
        }
        vbasedev->dirty_tracking = false;
    }
}

static struct vfio_device_feature *
vfio_device_feature_dma_logging_start_create(VFIOLegacyContainer *container,
                                             VFIODirtyRanges *tracking)
{
    struct vfio_device_feature *feature;
    size_t feature_size;
    struct vfio_device_feature_dma_logging_control *control;
    struct vfio_device_feature_dma_logging_range *ranges;

    feature_size = sizeof(struct vfio_device_feature) +
                   sizeof(struct vfio_device_feature_dma_logging_control);
    feature = g_try_malloc0(feature_size);
    if (!feature) {
        errno = ENOMEM;
        return NULL;
    }
    feature->argsz = feature_size;
    feature->flags = VFIO_DEVICE_FEATURE_SET |
                     VFIO_DEVICE_FEATURE_DMA_LOGGING_START;

    control = (struct vfio_device_feature_dma_logging_control *)feature->data;
    control->page_size = qemu_real_host_page_size();

    /*
     * DMA logging uAPI guarantees to support at least a number of ranges that
     * fits into a single host kernel base page.
     */
    control->num_ranges = !!tracking->max32 + !!tracking->max64 +
        !!tracking->maxpci64;
    ranges = g_try_new0(struct vfio_device_feature_dma_logging_range,
                        control->num_ranges);
    if (!ranges) {
        g_free(feature);
        errno = ENOMEM;

        return NULL;
    }

    control->ranges = (__u64)(uintptr_t)ranges;
    if (tracking->max32) {
        ranges->iova = tracking->min32;
        ranges->length = (tracking->max32 - tracking->min32) + 1;
        ranges++;
    }
    if (tracking->max64) {
        ranges->iova = tracking->min64;
        ranges->length = (tracking->max64 - tracking->min64) + 1;
        ranges++;
    }
    if (tracking->maxpci64) {
        ranges->iova = tracking->minpci64;
        ranges->length = (tracking->maxpci64 - tracking->minpci64) + 1;
    }

    trace_vfio_device_dirty_tracking_start(control->num_ranges,
                                           tracking->min32, tracking->max32,
                                           tracking->min64, tracking->max64,
                                           tracking->minpci64, tracking->maxpci64);

    return feature;
}

static void vfio_device_feature_dma_logging_start_destroy(
    struct vfio_device_feature *feature)
{
    struct vfio_device_feature_dma_logging_control *control =
        (struct vfio_device_feature_dma_logging_control *)feature->data;
    struct vfio_device_feature_dma_logging_range *ranges =
        (struct vfio_device_feature_dma_logging_range *)(uintptr_t)control->ranges;

    g_free(ranges);
    g_free(feature);
}

static int vfio_devices_dma_logging_start(VFIOLegacyContainer *container)
{
    struct vfio_device_feature *feature;
    VFIODirtyRanges ranges;
    VFIODevice *vbasedev = NULL;
    int ret = 0;

    vfio_dirty_tracking_init(container, &ranges);
    feature = vfio_device_feature_dma_logging_start_create(container,
                                                           &ranges);
    if (!feature) {
        return -errno;
    }

    while ((vbasedev = vfio_container_dev_iter_next(&container->bcontainer, vbasedev))) {
        if (vbasedev->dirty_tracking) {
            continue;
        }

        ret = ioctl(vbasedev->fd, VFIO_DEVICE_FEATURE, feature);
        if (ret) {
            ret = -errno;
            error_report("%s: Failed to start DMA logging, err %d (%s)",
                         vbasedev->name, ret, strerror(errno));
            goto out;
        }
        vbasedev->dirty_tracking = true;
    }

out:
    if (ret) {
        vfio_devices_dma_logging_stop(container);
    }

    vfio_device_feature_dma_logging_start_destroy(feature);

    return ret;
}

static void vfio_listener_log_global_start(MemoryListener *listener)
{
    VFIOLegacyContainer *container = container_of(listener, VFIOLegacyContainer, listener);
    int ret;

    if (vfio_devices_all_device_dirty_tracking(container)) {
        ret = vfio_devices_dma_logging_start(container);
    } else {
        ret = vfio_set_dirty_page_tracking(container, true);
    }

    if (ret) {
        error_report("vfio: Could not start dirty page tracking, err: %d (%s)",
                     ret, strerror(-ret));
        vfio_set_migration_error(ret);
    }
}

static void vfio_listener_log_global_stop(MemoryListener *listener)
{
    VFIOLegacyContainer *container = container_of(listener, VFIOLegacyContainer, listener);
    int ret = 0;

    if (vfio_devices_all_device_dirty_tracking(container)) {
        vfio_devices_dma_logging_stop(container);
    } else {
        ret = vfio_set_dirty_page_tracking(container, false);
    }

    if (ret) {
        error_report("vfio: Could not stop dirty page tracking, err: %d (%s)",
                     ret, strerror(-ret));
        vfio_set_migration_error(ret);
    }
}

static int vfio_device_dma_logging_report(VFIODevice *vbasedev, hwaddr iova,
                                          hwaddr size, void *bitmap)
{
    uint64_t buf[DIV_ROUND_UP(sizeof(struct vfio_device_feature) +
                        sizeof(struct vfio_device_feature_dma_logging_report),
                        sizeof(__u64))] = {};
    struct vfio_device_feature *feature = (struct vfio_device_feature *)buf;
    struct vfio_device_feature_dma_logging_report *report =
        (struct vfio_device_feature_dma_logging_report *)feature->data;

    report->iova = iova;
    report->length = size;
    report->page_size = qemu_real_host_page_size();
    report->bitmap = (__u64)(uintptr_t)bitmap;

    feature->argsz = sizeof(buf);
    feature->flags = VFIO_DEVICE_FEATURE_GET |
                     VFIO_DEVICE_FEATURE_DMA_LOGGING_REPORT;

    if (ioctl(vbasedev->fd, VFIO_DEVICE_FEATURE, feature)) {
        return -errno;
    }

    return 0;
}

int vfio_devices_query_dirty_bitmap(VFIOLegacyContainer *container,
                                    VFIOBitmap *vbmap, hwaddr iova,
                                    hwaddr size)
{
    VFIODevice *vbasedev = NULL;
    int ret;

    while ((vbasedev = vfio_container_dev_iter_next(&container->bcontainer, vbasedev))) {
        ret = vfio_device_dma_logging_report(vbasedev, iova, size,
                                             vbmap->bitmap);
        if (ret) {
            error_report("%s: Failed to get DMA logging report, iova: "
                         "0x%" HWADDR_PRIx ", size: 0x%" HWADDR_PRIx
                         ", err: %d (%s)",
                         vbasedev->name, iova, size, ret, strerror(-ret));

            return ret;
        }
    }

    return 0;
}

int vfio_get_dirty_bitmap(VFIOLegacyContainer *container, uint64_t iova,
                          uint64_t size, ram_addr_t ram_addr)
{
    bool all_device_dirty_tracking =
        vfio_devices_all_device_dirty_tracking(container);
    uint64_t dirty_pages;
    VFIOBitmap vbmap;
    int ret;

    if (!container->dirty_pages_supported && !all_device_dirty_tracking) {
        cpu_physical_memory_set_dirty_range(ram_addr, size,
                                            tcg_enabled() ? DIRTY_CLIENTS_ALL :
                                            DIRTY_CLIENTS_NOCODE);
        return 0;
    }

    ret = vfio_bitmap_alloc(&vbmap, size);
    if (ret) {
        return ret;
    }

    if (all_device_dirty_tracking) {
        ret = vfio_devices_query_dirty_bitmap(container, &vbmap, iova, size);
    } else {
        ret = vfio_query_dirty_bitmap(container, &vbmap, iova, size);
    }

    if (ret) {
        goto out;
    }

    dirty_pages = cpu_physical_memory_set_dirty_lebitmap(vbmap.bitmap, ram_addr,
                                                         vbmap.pages);

    trace_vfio_get_dirty_bitmap(container->fd, iova, size, vbmap.size,
                                ram_addr, dirty_pages);
out:
    g_free(vbmap.bitmap);

    return ret;
}

typedef struct {
    IOMMUNotifier n;
    VFIOGuestIOMMU *giommu;
} vfio_giommu_dirty_notifier;

static void vfio_iommu_map_dirty_notify(IOMMUNotifier *n, IOMMUTLBEntry *iotlb)
{
    vfio_giommu_dirty_notifier *gdn = container_of(n,
                                                vfio_giommu_dirty_notifier, n);
    VFIOGuestIOMMU *giommu = gdn->giommu;
    VFIOContainer *bcontainer = giommu->container;
    VFIOLegacyContainer *container = container_of(bcontainer,
                                                  VFIOLegacyContainer,
                                                  bcontainer);
    hwaddr iova = iotlb->iova + giommu->iommu_offset;
    ram_addr_t translated_addr;
    int ret = -EINVAL;

    trace_vfio_iommu_map_dirty_notify(iova, iova + iotlb->addr_mask);

    if (iotlb->target_as != &address_space_memory) {
        error_report("Wrong target AS \"%s\", only system memory is allowed",
                     iotlb->target_as->name ? iotlb->target_as->name : "none");
        goto out;
    }

    rcu_read_lock();
    if (vfio_get_xlat_addr(iotlb, NULL, &translated_addr, NULL)) {
        ret = vfio_get_dirty_bitmap(container, iova, iotlb->addr_mask + 1,
                                    translated_addr);
        if (ret) {
            error_report("vfio_iommu_map_dirty_notify(%p, 0x%"HWADDR_PRIx", "
                         "0x%"HWADDR_PRIx") = %d (%s)",
                         container, iova, iotlb->addr_mask + 1, ret,
                         strerror(-ret));
        }
    }
    rcu_read_unlock();

out:
    if (ret) {
        vfio_set_migration_error(ret);
    }
}

static int vfio_ram_discard_get_dirty_bitmap(MemoryRegionSection *section,
                                             void *opaque)
{
    const hwaddr size = int128_get64(section->size);
    const hwaddr iova = section->offset_within_address_space;
    const ram_addr_t ram_addr = memory_region_get_ram_addr(section->mr) +
                                section->offset_within_region;
    VFIORamDiscardListener *vrdl = opaque;

    /*
     * Sync the whole mapped region (spanning multiple individual mappings)
     * in one go.
     */
    return vfio_get_dirty_bitmap(vrdl->container, iova, size, ram_addr);
}

static int vfio_sync_ram_discard_listener_dirty_bitmap(VFIOLegacyContainer *container,
                                                   MemoryRegionSection *section)
{
    RamDiscardManager *rdm = memory_region_get_ram_discard_manager(section->mr);
    VFIORamDiscardListener *vrdl = NULL;

    QLIST_FOREACH(vrdl, &container->vrdl_list, next) {
        if (vrdl->mr == section->mr &&
            vrdl->offset_within_address_space ==
            section->offset_within_address_space) {
            break;
        }
    }

    if (!vrdl) {
        hw_error("vfio: Trying to sync missing RAM discard listener");
    }

    /*
     * We only want/can synchronize the bitmap for actually mapped parts -
     * which correspond to populated parts. Replay all populated parts.
     */
    return ram_discard_manager_replay_populated(rdm, section,
                                              vfio_ram_discard_get_dirty_bitmap,
                                                &vrdl);
}

static int vfio_sync_dirty_bitmap(VFIOLegacyContainer *container,
                                  MemoryRegionSection *section)
{
    VFIOContainer *bcontainer = &container->bcontainer;
    ram_addr_t ram_addr;

    if (memory_region_is_iommu(section->mr)) {
        VFIOGuestIOMMU *giommu;

        QLIST_FOREACH(giommu, &bcontainer->giommu_list, giommu_next) {
            if (MEMORY_REGION(giommu->iommu_mr) == section->mr &&
                giommu->n.start == section->offset_within_region) {
                Int128 llend;
                vfio_giommu_dirty_notifier gdn = { .giommu = giommu };
                int idx = memory_region_iommu_attrs_to_index(giommu->iommu_mr,
                                                       MEMTXATTRS_UNSPECIFIED);

                llend = int128_add(int128_make64(section->offset_within_region),
                                   section->size);
                llend = int128_sub(llend, int128_one());

                iommu_notifier_init(&gdn.n,
                                    vfio_iommu_map_dirty_notify,
                                    IOMMU_NOTIFIER_MAP,
                                    section->offset_within_region,
                                    int128_get64(llend),
                                    idx);
                memory_region_iommu_replay(giommu->iommu_mr, &gdn.n);
                break;
            }
        }
        return 0;
    } else if (memory_region_has_ram_discard_manager(section->mr)) {
        return vfio_sync_ram_discard_listener_dirty_bitmap(container, section);
    }

    ram_addr = memory_region_get_ram_addr(section->mr) +
               section->offset_within_region;

    return vfio_get_dirty_bitmap(container,
                   REAL_HOST_PAGE_ALIGN(section->offset_within_address_space),
                   int128_get64(section->size), ram_addr);
}

static void vfio_listener_log_sync(MemoryListener *listener,
        MemoryRegionSection *section)
{
    VFIOLegacyContainer *container = container_of(listener, VFIOLegacyContainer, listener);
    int ret;

    if (vfio_listener_skipped_section(section)) {
        return;
    }

    if (vfio_devices_all_dirty_tracking(container)) {
        ret = vfio_sync_dirty_bitmap(container, section);
        if (ret) {
            error_report("vfio: Failed to sync dirty bitmap, err: %d (%s)", ret,
                         strerror(-ret));
            vfio_set_migration_error(ret);
        }
    }
}

const MemoryListener vfio_memory_listener = {
    .name = "vfio",
    .region_add = vfio_listener_region_add,
    .region_del = vfio_listener_region_del,
    .log_global_start = vfio_listener_log_global_start,
    .log_global_stop = vfio_listener_log_global_stop,
    .log_sync = vfio_listener_log_sync,
};

void vfio_reset_handler(void *opaque)
{
    VFIOAddressSpace *space;
    VFIOContainer *container;
    VFIODevice *vbasedev;

    QLIST_FOREACH(space, &vfio_address_spaces, list) {
        QLIST_FOREACH(container, &space->containers, next) {
            vbasedev = NULL;
            while ((vbasedev = vfio_container_dev_iter_next(container,
                                                            vbasedev))) {
                if (vbasedev->dev->realized) {
                    vbasedev->ops->vfio_compute_needs_reset(vbasedev);
                }
            }
        }
    }

    QLIST_FOREACH(space, &vfio_address_spaces, list) {
        QLIST_FOREACH(container, &space->containers, next) {
            vbasedev = NULL;
            while ((vbasedev = vfio_container_dev_iter_next(container,
                                                            vbasedev))) {
                if (vbasedev->dev->realized && vbasedev->needs_reset) {
                    vbasedev->ops->vfio_hot_reset_multi(vbasedev);
                    }
            }
        }
    }
}

int vfio_kvm_device_add_fd(int fd)
{
#ifdef CONFIG_KVM
    struct kvm_device_attr attr = {
        .group = KVM_DEV_VFIO_FILE,
        .attr = KVM_DEV_VFIO_FILE_ADD,
        .addr = (uint64_t)(unsigned long)&fd,
    };

    if (!kvm_enabled()) {
        return 0;
    }

    if (vfio_kvm_device_fd < 0) {
        struct kvm_create_device cd = {
            .type = KVM_DEV_TYPE_VFIO,
        };

        if (kvm_vm_ioctl(kvm_state, KVM_CREATE_DEVICE, &cd)) {
            error_report("Failed to create KVM VFIO device: %m");
            return -ENODEV;
        }

        vfio_kvm_device_fd = cd.fd;
    }

    if (ioctl(vfio_kvm_device_fd, KVM_SET_DEVICE_ATTR, &attr)) {
        error_report("Failed to add fd %d to KVM VFIO device: %m",
                     fd);
        return -errno;
    }
#endif
    return 0;
}

int vfio_kvm_device_del_fd(int fd)
{
#ifdef CONFIG_KVM
    struct kvm_device_attr attr = {
        .group = KVM_DEV_VFIO_FILE,
        .attr = KVM_DEV_VFIO_FILE_DEL,
        .addr = (uint64_t)(unsigned long)&fd,
    };

    if (vfio_kvm_device_fd < 0) {
        return -EINVAL;
    }

    if (ioctl(vfio_kvm_device_fd, KVM_SET_DEVICE_ATTR, &attr)) {
        error_report("Failed to remove fd %d from KVM VFIO device: %m",
                     fd);
        return -EBADF;
    }
#endif
    return 0;
}

VFIOAddressSpace *vfio_get_address_space(AddressSpace *as)
{
    VFIOAddressSpace *space;

    QLIST_FOREACH(space, &vfio_address_spaces, list) {
        if (space->as == as) {
            return space;
        }
    }

    /* No suitable VFIOAddressSpace, create a new one */
    space = g_malloc0(sizeof(*space));
    space->as = as;
    QLIST_INIT(&space->containers);

    if (QLIST_EMPTY(&vfio_address_spaces)) {
        qemu_register_reset(vfio_reset_handler, NULL);
    }

    QLIST_INSERT_HEAD(&vfio_address_spaces, space, list);

    return space;
}

void vfio_put_address_space(VFIOAddressSpace *space)
{
    if (QLIST_EMPTY(&space->containers)) {
        QLIST_REMOVE(space, list);
        g_free(space);
    }
    if (QLIST_EMPTY(&vfio_address_spaces)) {
        qemu_unregister_reset(vfio_reset_handler, NULL);
    }
}

struct vfio_device_info *vfio_get_device_info(int fd)
{
    struct vfio_device_info *info;
    uint32_t argsz = sizeof(*info);

    info = g_malloc0(argsz);

retry:
    info->argsz = argsz;

    if (ioctl(fd, VFIO_DEVICE_GET_INFO, info)) {
        g_free(info);
        return NULL;
    }

    if (info->argsz > argsz) {
        argsz = info->argsz;
        info = g_realloc(info, argsz);
        goto retry;
    }

    return info;
}
