/*
 * QTest testcase for VirtIO IOMMU
 *
 * Copyright (c) 2014 SUSE LINUX Products GmbH
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "qemu/module.h"
#include "libqos/qgraph.h"
#include "libqos/virtio-iommu.h"
#include "hw/virtio/virtio-iommu.h"

#define PCI_SLOT_HP             0x06
#define QVIRTIO_IOMMU_TIMEOUT_US (30 * 1000 * 1000)

static QGuestAllocator *alloc;

static void iommu_hotplug(void *obj, void *data, QGuestAllocator *alloc)
{
    QVirtioPCIDevice *dev = obj;
    QTestState *qts = dev->pdev->bus->qts;

    //const char *arch = qtest_get_arch();

    qtest_qmp_device_add(qts, "virtio-iommu-pci", "rng1",
                         "{'addr': %s}", stringify(PCI_SLOT_HP));

#if 0
    if (strcmp(arch, "i386") == 0 || strcmp(arch, "x86_64") == 0) {
        qpci_unplug_acpi_device_test(qts, "rng1", PCI_SLOT_HP);
    }
#endif
}

static void pci_config(void *obj, void *data, QGuestAllocator *t_alloc)
{
    QVirtioIOMMU *v_iommu = obj;
    QVirtioDevice *dev = v_iommu->vdev;
    uint64_t page_size_mask = qvirtio_config_readl(dev, 0);
    uint64_t input_range_start = qvirtio_config_readq(dev, 8);
    uint64_t input_range_end = qvirtio_config_readq(dev, 16);
    uint32_t domain_range_start = qvirtio_config_readl(dev, 24);
    uint32_t domain_range_end = qvirtio_config_readl(dev, 28);
    uint32_t probe_size = qvirtio_config_readl(dev, 32);

    g_printerr("page_size_mask = 0x%lx\n", page_size_mask);
    g_printerr("input range start= 0x%lx end=0x%lx\n", input_range_start, input_range_end);
    g_printerr("domain range start= 0x%x end=0x%x\n", domain_range_start, domain_range_end);
    g_printerr("probe_size=0x%x\n", probe_size);

    g_assert_cmpint(input_range_start, ==, 0);
    g_assert_cmphex(input_range_end, ==, 0xFFFFFFFFFFFFFFFF);
    g_assert_cmpint(domain_range_start, ==, 0);
    g_assert_cmpint(domain_range_end, ==, 32);
    g_assert_cmphex(probe_size, ==, 0x200);

#if 0
    tag = g_malloc(tag_len);
    for (i = 0; i < tag_len; i++) {
        tag[i] = qvirtio_config_readb(v9p->vdev, i + 2);
    }
    g_assert_cmpmem(tag, tag_len, MOUNT_TAG, tag_len);
    g_free(tag);
#endif
}

static int send_attach_detach(QTestState *qts, QVirtioIOMMU *v_iommu,
                              uint8_t type, uint32_t domain, uint32_t ep)
{
    QVirtioDevice *dev = v_iommu->vdev;
    QVirtQueue *vq = v_iommu->vq;
    uint64_t ro_addr, wr_addr;
    uint32_t free_head;
    struct virtio_iommu_req_attach req; /* same layout as detach */
    size_t ro_size = sizeof(req) - sizeof(struct virtio_iommu_req_tail);
    size_t wr_size = sizeof(struct virtio_iommu_req_tail);
    char buffer[64];
    int ret;

    req.head.type = type;
    req.domain = domain;
    req.endpoint = ep;

    ro_addr = guest_alloc(alloc, ro_size);
    wr_addr = guest_alloc(alloc, wr_size);

    qtest_memwrite(qts, ro_addr, &req, ro_size);
    free_head = qvirtqueue_add(qts, vq, ro_addr, ro_size, false, true);
    qvirtqueue_add(qts, vq, wr_addr, wr_size, true, false);
    qvirtqueue_kick(qts, dev, vq, free_head);
    qvirtio_wait_used_elem(qts, dev, vq, free_head, NULL,
                           QVIRTIO_IOMMU_TIMEOUT_US);
    qtest_memread(qts, wr_addr, buffer, wr_size);
    ret = ((struct virtio_iommu_req_tail *)buffer)->status;
    g_printerr("attach domain=%d, ep=%d ret=%d\n", domain, ep, ret);
    guest_free(alloc, ro_addr);
    guest_free(alloc, wr_addr);
    return ret;
}

static int send_map(QTestState *qts, QVirtioIOMMU *v_iommu,
                    uint32_t domain, uint64_t virt_start, uint64_t virt_end,
                    uint64_t phys_start, uint32_t flags)
{
    QVirtioDevice *dev = v_iommu->vdev;
    QVirtQueue *vq = v_iommu->vq;
    uint64_t ro_addr, wr_addr;
    uint32_t free_head;
    struct virtio_iommu_req_map req;
    size_t ro_size = sizeof(req) - sizeof(struct virtio_iommu_req_tail);
    size_t wr_size = sizeof(struct virtio_iommu_req_tail);
    char buffer[64];
    int ret;

    req.head.type = VIRTIO_IOMMU_T_MAP;
    req.domain = domain;
    req.virt_start = virt_start;
    req.virt_end = virt_end;
    req.phys_start = phys_start;
    req.flags = flags;

    ro_addr = guest_alloc(alloc, ro_size);
    wr_addr = guest_alloc(alloc, wr_size);

    qtest_memwrite(qts, ro_addr, &req, ro_size);
    free_head = qvirtqueue_add(qts, vq, ro_addr, ro_size, false, true);
    qvirtqueue_add(qts, vq, wr_addr, wr_size, true, false);
    qvirtqueue_kick(qts, dev, vq, free_head);
    qvirtio_wait_used_elem(qts, dev, vq, free_head, NULL,
                           QVIRTIO_IOMMU_TIMEOUT_US);
    memread(wr_addr, buffer, wr_size);
    ret = ((struct virtio_iommu_req_tail *)buffer)->status;
    g_printerr("map domain=%d, virt start = 0x%lx end=0x%lx phys =0x%lx flags=%d ret=%d\n",
               domain, virt_start, virt_end, phys_start, flags, ret);
    guest_free(alloc, ro_addr);
    guest_free(alloc, wr_addr);
    return ret;
}

static int send_unmap(QTestState *qts, QVirtioIOMMU *v_iommu,
                      uint32_t domain, uint64_t virt_start, uint64_t virt_end)
{
    QVirtioDevice *dev = v_iommu->vdev;
    QVirtQueue *vq = v_iommu->vq;
    uint64_t ro_addr, wr_addr;
    uint32_t free_head;
    struct virtio_iommu_req_unmap req;
    size_t ro_size = sizeof(req) - sizeof(struct virtio_iommu_req_tail);
    size_t wr_size = sizeof(struct virtio_iommu_req_tail);
    char buffer[64];
    int ret;

    req.head.type = VIRTIO_IOMMU_T_UNMAP;
    req.domain = domain;
    req.virt_start = virt_start;
    req.virt_end = virt_end;

    ro_addr = guest_alloc(alloc, ro_size);
    wr_addr = guest_alloc(alloc, wr_size);

    qtest_memwrite(qts, ro_addr, &req, ro_size);
    free_head = qvirtqueue_add(qts, vq, ro_addr, ro_size, false, true);
    qvirtqueue_add(qts, vq, wr_addr, wr_size, true, false);
    qvirtqueue_kick(qts, dev, vq, free_head);
    qvirtio_wait_used_elem(qts, dev, vq, free_head, NULL,
                           QVIRTIO_IOMMU_TIMEOUT_US);
    memread(wr_addr, buffer, wr_size);
    ret = ((struct virtio_iommu_req_tail *)buffer)->status;
    g_printerr("unmap domain=%d, virt start = 0x%lx end=0x%lx ret=%d\n",
               domain, virt_start, virt_end, ret);
    guest_free(alloc, ro_addr);
    guest_free(alloc, wr_addr);
    return ret;
}

static void test_attach_detach(void *obj, void *data, QGuestAllocator *t_alloc)
{
    QVirtioIOMMU *v_iommu = obj;
    QTestState *qts = global_qtest;
    int ret;

    alloc = t_alloc;

    /* type, domain, ep */
    ret = send_attach_detach(qts, v_iommu, VIRTIO_IOMMU_T_ATTACH, 0, 0);
    g_assert_cmpint(ret, ==, 0);
    ret = send_attach_detach(qts, v_iommu, VIRTIO_IOMMU_T_ATTACH, 1, 2);
    g_assert_cmpint(ret, ==, 0);
    ret = send_attach_detach(qts, v_iommu, VIRTIO_IOMMU_T_ATTACH, 1, 2);
    g_assert_cmpint(ret, ==, 0);
    ret = send_attach_detach(qts, v_iommu, VIRTIO_IOMMU_T_ATTACH, 0, 2);
    g_assert_cmpint(ret, ==, 0);

    /* domain, virt start, virt end, phys start, flags */
    ret = send_map(qts, v_iommu, 0, 0, 0xFFF, 0xa1000, VIRTIO_IOMMU_MAP_F_READ);
    g_assert_cmpint(ret, ==, 0);

    ret = send_unmap(qts, v_iommu, 4, 0x10, 0xFFF);
    g_assert_cmpint(ret, ==, VIRTIO_IOMMU_S_NOENT);

    ret = send_unmap(qts, v_iommu, 0, 0x10, 0xFFF);
    g_assert_cmpint(ret, ==, VIRTIO_IOMMU_S_RANGE);

    /* Spec example sequence */

    /* 1 */
    g_printerr("*** unmap 1 ***\n");
    ret = send_unmap(qts, v_iommu, 1, 0, 4);
    g_assert_cmpint(ret, ==, 0); /* doesn't unmap anything */

    /* 2 */
    g_printerr("*** unmap 2 ***\n");
    send_map(qts, v_iommu, 1, 0, 9, 0xa1000, VIRTIO_IOMMU_MAP_F_READ);
    ret = send_unmap(qts, v_iommu, 1, 0, 9);
    g_assert_cmpint(ret, ==, 0); /* unmaps [0,9] */

    /* 3 */
    g_printerr("*** unmap 3 ***\n");
    send_map(qts, v_iommu, 1, 0, 4, 0xb1000, VIRTIO_IOMMU_MAP_F_READ);
    send_map(qts, v_iommu, 1, 5, 9, 0xb2000, VIRTIO_IOMMU_MAP_F_READ);
    ret = send_unmap(qts, v_iommu, 1, 0, 9);
    g_assert_cmpint(ret, ==, 0); /* unmaps [0,4] and [5,9] */

    /* 4 */
    g_printerr("*** unmap 4 ***\n");
    send_map(qts, v_iommu, 1, 0, 9, 0xc1000, VIRTIO_IOMMU_MAP_F_READ);
    ret = send_unmap(qts, v_iommu, 1, 0, 4);
    g_assert_cmpint(ret, ==, VIRTIO_IOMMU_S_RANGE); /* doesn't unmap anything */

    ret = send_unmap(qts, v_iommu, 1, 0, 10);
    g_assert_cmpint(ret, ==, 0);

    /* 5 */
    g_printerr("*** unmap 5 ***\n");
    send_map(qts, v_iommu, 1, 0, 4, 0xd1000, VIRTIO_IOMMU_MAP_F_READ);
    send_map(qts, v_iommu, 1, 5, 9, 0xd2000, VIRTIO_IOMMU_MAP_F_READ);
    ret = send_unmap(qts, v_iommu, 1, 0, 4);
    g_assert_cmpint(ret, ==, 0); /* unmaps [0,4] */

    ret = send_unmap(qts, v_iommu, 1, 5, 9);
    g_assert_cmpint(ret, ==, 0);

    /* 6 */
    g_printerr("*** unmap 6 ***\n");
    send_map(qts, v_iommu, 1, 0, 4, 0xe2000, VIRTIO_IOMMU_MAP_F_READ);
    ret = send_unmap(qts, v_iommu, 1, 0, 9);
    g_assert_cmpint(ret, ==, 0); /* unmaps [0,4] */

    /* 7 */
    g_printerr("*** unmap 7 ***\n");
    send_map(qts, v_iommu, 1, 0, 4, 0xf2000, VIRTIO_IOMMU_MAP_F_READ);
    send_map(qts, v_iommu, 1, 10, 14, 0xf3000, VIRTIO_IOMMU_MAP_F_READ);
    ret = send_unmap(qts, v_iommu, 1, 0, 14);
    g_assert_cmpint(ret, ==, 0); /* unmaps [0,4] and [10,14] */

    g_printerr("*** unmap 8 ***\n");
    send_unmap(qts, v_iommu, 1, 0, 100);
    send_map(qts, v_iommu, 1, 10, 14, 0xf3000, VIRTIO_IOMMU_MAP_F_READ);
    send_map(qts, v_iommu, 1, 0, 4, 0xf2000, VIRTIO_IOMMU_MAP_F_READ);
    ret = send_unmap(qts, v_iommu, 1, 0, 4);
    g_assert_cmpint(ret, ==, 0); /* unmaps [0,4] and [10,14] */
}

static void register_virtio_iommu_test(void)
{
    qos_add_test("hotplug", "virtio-iommu-pci", iommu_hotplug, NULL);
    qos_add_test("config", "virtio-iommu", pci_config, NULL);
    qos_add_test("attach_detach", "virtio-iommu", test_attach_detach, NULL);
}

libqos_init(register_virtio_iommu_test);
