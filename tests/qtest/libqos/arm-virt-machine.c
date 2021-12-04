/*
 * libqos driver framework
 *
 * Copyright (c) 2018 Emanuele Giuseppe Esposito <e.emanuelegiuseppe@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/module.h"
#include "malloc.h"
#include "qgraph.h"
#include "virtio-mmio.h"
#include "pci-arm.h"

#define ARM_PAGE_SIZE               4096
#define VIRTIO_MMIO_BASE_ADDR       0x0A003E00
#define ARM_VIRT_RAM_ADDR           0x40000000
#define ARM_VIRT_RAM_SIZE           0x20000000
#define VIRTIO_MMIO_SIZE            0x00000200

typedef struct QVirtMachine QVirtMachine;
typedef struct generic_pcihost generic_pcihost;

struct generic_pcihost {
    QOSGraphObject obj;
    QPCIBusARM pci;
};

struct generic_pcie_root_port {
    QOSGraphObject obj;
    QPCIDevice dev;
};

struct QVirtMachine {
    QOSGraphObject obj;
    QGuestAllocator alloc;
    QVirtioMMIODevice virtio_mmio;
    generic_pcihost bridge;
};

/* generic_pcihost */

static QOSGraphObject *generic_pcihost_get_device(void *obj, const char *device)
{
    generic_pcihost *host = obj;
    if (!g_strcmp0(device, "pci-bus-arm")) {
        return &host->pci.obj;
    }
    fprintf(stderr, "%s not present in generic-pcihost\n", device);
    g_assert_not_reached();
}

static void qos_create_generic_pcihost(generic_pcihost *host,
                                       QTestState *qts,
                                       QGuestAllocator *alloc)
{
    host->obj.get_device = generic_pcihost_get_device;
    qpci_init_arm(&host->pci, qts, alloc, false);
}

static void virt_destructor(QOSGraphObject *obj)
{
    QVirtMachine *machine = (QVirtMachine *) obj;
    alloc_destroy(&machine->alloc);
}

static void *virt_get_driver(void *object, const char *interface)
{
    QVirtMachine *machine = object;
    if (!g_strcmp0(interface, "memory")) {
        return &machine->alloc;
    }

    g_assert_not_reached();
}

static QOSGraphObject *virt_get_device(void *obj, const char *device)
{
    QVirtMachine *machine = obj;
    if (!g_strcmp0(device, "generic-pcihost")) {
        return &machine->bridge.obj;
    }

    fprintf(stderr, "%s not present in arm/virt\n", device);
    g_assert_not_reached();
}

static void *qos_create_machine_arm_virt(QTestState *qts)
{
    QVirtMachine *machine = g_new0(QVirtMachine, 1);

    alloc_init(&machine->alloc, 0,
               ARM_VIRT_RAM_ADDR,
               ARM_VIRT_RAM_ADDR + ARM_VIRT_RAM_SIZE,
               ARM_PAGE_SIZE);
    if (strcmp(qtest_get_arch(), "arm")) {
        qvirtio_mmio_init_device(&machine->virtio_mmio, qts, VIRTIO_MMIO_BASE_ADDR,
                                 VIRTIO_MMIO_SIZE);
    }
    qos_create_generic_pcihost(&machine->bridge, qts, &machine->alloc);

    machine->obj.get_device = virt_get_device;
    machine->obj.get_driver = virt_get_driver;
    machine->obj.destructor = virt_destructor;
    return machine;
}

static void virt_machine_register_nodes(void)
{
    qos_node_create_machine("arm/virt", qos_create_machine_arm_virt);
//    qos_node_contains("arm/virt", "virtio-mmio", NULL);

    qos_node_create_machine("aarch64/virt", qos_create_machine_arm_virt);
    qos_node_contains("aarch64/virt", "generic-pcihost", NULL);

    qos_node_create_driver("generic-pcihost", NULL);
    qos_node_contains("generic-pcihost", "pci-bus-arm", NULL);
}

libqos_init(virt_machine_register_nodes);
