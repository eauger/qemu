/*
 * hw/arm/linux-boot-if.h : interface for devices which need to behave
 * specially for direct boot of an ARM Linux kernel
 */

#ifndef HW_ARM_LINUX_BOOT_IF_H
#define HW_ARM_LINUX_BOOT_IF_H

#include "qom/object.h"

#define TYPE_ARM_LINUX_BOOT_IF "arm-linux-boot-if"
#define ARM_LINUX_BOOT_IF_CLASS(klass) \
    OBJECT_CLASS_CHECK(ARMLinuxBootIfClass, (klass), TYPE_ARM_LINUX_BOOT_IF)
#define ARM_LINUX_BOOT_IF_GET_CLASS(obj) \
    OBJECT_GET_CLASS(ARMLinuxBootIfClass, (obj), TYPE_ARM_LINUX_BOOT_IF)
#define ARM_LINUX_BOOT_IF(obj) \
    INTERFACE_CHECK(ARMLinuxBootIf, (obj), TYPE_ARM_LINUX_BOOT_IF)

typedef struct ARMLinuxBootIf {
    /*< private >*/
    Object parent_obj;
} ARMLinuxBootIf;

typedef struct ARMLinuxBootIfClass {
    /*< private >*/
    InterfaceClass parent_class;

    /*< public >*/
    /** arm_linux_init: configure the device for a direct boot
     * of an ARM Linux kernel (so that device reset puts it into
     * the state the kernel expects after firmware initialization,
     * rather than the true hardware reset state). This callback is
     * called once after machine construction is complete (before the
     * first system reset).
     *
     * @obj: the object implementing this interface
     * @secure_boot: true if we are booting Secure, false for NonSecure
     * (or for a CPU which doesn't support TrustZone)
     */
    void (*arm_linux_init)(ARMLinuxBootIf *obj, bool secure_boot);
} ARMLinuxBootIfClass;

#define TYPE_ARM_DEVICE_RESET_IF "arm-device-reset-if"
#define ARM_DEVICE_RESET_IF_CLASS(klass) \
    OBJECT_CLASS_CHECK(ARMDeviceResetIfClass, (klass), TYPE_ARM_DEVICE_RESET_IF)
#define ARM_DEVICE_RESET_IF_GET_CLASS(obj) \
    OBJECT_GET_CLASS(ARMDeviceResetIfClass, (obj), TYPE_ARM_DEVICE_RESET_IF)
#define ARM_DEVICE_RESET_IF(obj) \
    INTERFACE_CHECK(ARMDeviceResetIf, (obj), TYPE_ARM_DEVICE_RESET_IF)

typedef struct ARMDeviceResetIf {
    /*< private >*/
    Object parent_obj;
} ARMDeviceResetIf;

typedef struct ARMDeviceResetIfClass {
    /*< private >*/
    InterfaceClass parent_class;

    /*< public >*/
    /** arm_device_reset: Reset the device when cpu is reset is
     * called. Some device registers like GICv3 cpu interface registers
     * required to be reset when CPU is reset instead of GICv3 device
     * reset. This callback is called when arm_cpu_reset is called.
     *
     * @obj: the object implementing this interface
     * @cpu_num: CPU number being reset
     */
    void (*arm_device_reset)(ARMDeviceResetIf *obj, unsigned int cpu_num);
} ARMDeviceResetIfClass;
#endif
