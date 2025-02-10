// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/usb.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/device.h>
#include <linux/rtnetlink.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/kmod.h>  // For call_usermodehelper()

//
// Supported Commands
//
enum mobdev_cmd {
    MOBDEV_DETECT = 0,
    MOBDEV_FILE_TRANSFER,
    MOBDEV_TETHERING,
    MOBDEV_NOTIFICATIONS,
    MOBDEV_CALL_CONTROL,  // Value 4
};

//
// Struct for User Arguments
//
struct mobdev_args {
    int  enable;       // For file transfer/tethering/notifications (1 = push/on, 0 = pull/off)
    char path[128];    // File path for transfer
    char ifname[32];   // Interface name for tethering
    int action;        // For call control: 1 = answer, 0 = reject
};

// -------------------------------------------------------------------------------------------------
//
// 1️⃣ DETECT: Check for connected phones via USB
//
static int mobdev_detect_cb(struct usb_device *udev, void *data)
{
    unsigned short vid = le16_to_cpu(udev->descriptor.idVendor);
    unsigned short pid = le16_to_cpu(udev->descriptor.idProduct);

    pr_info("mobdev_control: Checking device %04x:%04x\n", vid, pid);

    for (int cfg_index = 0; cfg_index < udev->descriptor.bNumConfigurations; cfg_index++) {
        struct usb_host_config *cfg = &udev->config[cfg_index];

        for (int if_index = 0; if_index < cfg->desc.bNumInterfaces; if_index++) {
            struct usb_interface *interface = cfg->interface[if_index];

            for (int alt_index = 0; alt_index < interface->num_altsetting; alt_index++) {
                struct usb_interface_descriptor *intf_desc = &interface->altsetting[alt_index].desc;

                switch (intf_desc->bInterfaceClass) {
                case USB_CLASS_STILL_IMAGE:
                    pr_info("mobdev_control: Detected MTP/PTP device\n");
                    return 1;
                case USB_CLASS_WIRELESS_CONTROLLER:
                    pr_info("mobdev_control: Detected RNDIS device\n");
                    return 1;
                case USB_CLASS_VENDOR_SPEC:
                    pr_info("mobdev_control: Detected vendor-specific device\n");
                    return 1;
                default:
                    break;
                }
            }
        }
    }
    return 0;
}

static long mobdev_detect_phone(void)
{
    return usb_for_each_dev(NULL, mobdev_detect_cb);
}

// -------------------------------------------------------------------------------------------------
//
// 2️⃣ FILE_TRANSFER: Uses ADB for file transfers
//
static long mobdev_file_transfer(struct mobdev_args *args)
{
    int ret = mobdev_detect_phone();
    char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL };
    char *argv[5];

    if (ret == 1) {
        pr_info("mobdev_control: Phone detected, initiating ADB file transfer.\n");

        argv[0] = "/usr/bin/adb";
        argv[1] = args->enable ? "push" : "pull";
        argv[2] = args->path;
        argv[3] = args->enable ? "/sdcard/" : "/home/user/";
        argv[4] = NULL;

        ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
        if (ret < 0) {
            pr_err("mobdev_control: ADB transfer failed.\n");
            return -EIO;
        }

        pr_info("mobdev_control: ADB transfer successful.\n");
        return 0;
    } else {
        pr_err("mobdev_control: No MTP device found.\n");
        return -ENODEV;
    }
}

// -------------------------------------------------------------------------------------------------
//
// 3️⃣ CALL CONTROL: Detect & answer/reject calls via ADB
//
static long mobdev_call_control(struct mobdev_args *args)
{
    char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL };
    char *adb_check_call[] = { "/usr/bin/adb", "shell", "dumpsys", "telephony.registry", NULL };
    char *adb_answer_call[] = { "/usr/bin/adb", "shell", "input", "keyevent", "KEYCODE_CALL", NULL };
    char *adb_reject_call[] = { "/usr/bin/adb", "shell", "input", "keyevent", "KEYCODE_ENDCALL", NULL };

    pr_info("mobdev_control: Checking for incoming calls...\n");

    int ret = call_usermodehelper(adb_check_call[0], adb_check_call, envp, UMH_WAIT_PROC);
    if (ret < 0) {
        pr_err("mobdev_control: Failed to check call state.\n");
        return -EIO;
    }

    pr_info("mobdev_control: Incoming call detected! Processing action...\n");

    /* Use the action field to determine the ADB command:
       action == 1 means answer the call,
       action == 0 means reject the call.
    */
    char **adb_action = args->action ? adb_answer_call : adb_reject_call;
    ret = call_usermodehelper(adb_action[0], adb_action, envp, UMH_WAIT_PROC);
    if (ret < 0) {
        pr_err("mobdev_control: Failed to process call action.\n");
        return -EIO;
    }

    pr_info("mobdev_control: Call processed successfully.\n");
    return 0;
}

// -------------------------------------------------------------------------------------------------
//
// 4️⃣ TETHERING: Enable/Disable USB tethering
//
static long mobdev_tethering(struct mobdev_args *args)
{
    struct net_device *ndev;
    char ifname[32];

    strncpy(ifname, args->ifname, sizeof(ifname) - 1);

    rtnl_lock();
    ndev = dev_get_by_name(&init_net, ifname);
    if (!ndev) {
        pr_err("mobdev_control: interface '%s' not found\n", ifname);
        rtnl_unlock();
        return -ENODEV;
    }

    if (args->enable) {
        if (!(ndev->flags & IFF_UP)) {
            pr_info("mobdev_control: Bringing '%s' up\n", ifname);
            ndev->flags |= IFF_UP;
        }
    } else {
        if (ndev->flags & IFF_UP) {
            pr_info("mobdev_control: Bringing '%s' down\n", ifname);
            ndev->flags &= ~IFF_UP;
        }
    }

    dev_put(ndev);
    rtnl_unlock();
    return 0;
}

// -------------------------------------------------------------------------------------------------
//
// 5️⃣ NOTIFICATIONS: Enable/Disable notification mirroring
//
static long mobdev_notifications(struct mobdev_args *args)
{
    if (args->enable) {
        pr_info("mobdev_control: Notifications enabled\n");
    } else {
        pr_info("mobdev_control: Notifications disabled\n");
    }
    return 0;
}

// -------------------------------------------------------------------------------------------------
//
// 6️⃣ SYSTEM CALL HANDLER
//
SYSCALL_DEFINE2(mobdev_control,
                unsigned int, cmd,
                unsigned long, arg)
{
    struct mobdev_args kargs;
    long ret = 0;

    /* For commands that require user arguments, copy the data from user space */
    if ((cmd == MOBDEV_FILE_TRANSFER ||
         cmd == MOBDEV_TETHERING   ||
         cmd == MOBDEV_NOTIFICATIONS ||
         cmd == MOBDEV_CALL_CONTROL) && arg != 0) {
        if (copy_from_user(&kargs, (struct mobdev_args __user *)arg, sizeof(kargs))) {
            pr_err("mobdev_control: Failed to copy args from user\n");
            return -EFAULT;
        }
    }

    switch (cmd) {
    case MOBDEV_DETECT:
        ret = mobdev_detect_phone();
        break;
    case MOBDEV_FILE_TRANSFER:
        ret = mobdev_file_transfer(&kargs);
        break;
    case MOBDEV_TETHERING:
        ret = mobdev_tethering(&kargs);
        break;
    case MOBDEV_NOTIFICATIONS:
        ret = mobdev_notifications(&kargs);
        break;
    case MOBDEV_CALL_CONTROL:
        pr_info("mobdev_control: CALL_CONTROL command\n");
        ret = mobdev_call_control(&kargs);
        break;
    default:
        pr_err("mobdev_control: Unknown command %u\n", cmd);
        ret = -EINVAL;
        break;
    }

    return ret;
}
