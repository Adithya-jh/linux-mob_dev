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
#include <linux/rtnetlink.h> // for rtnl_lock/rtnl_unlock
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/kmod.h>  // For call_usermodehelper()

//
// Example command IDs
//
enum mobdev_cmd {
    MOBDEV_DETECT = 0,
    MOBDEV_FILE_TRANSFER,
    MOBDEV_TETHERING,
    MOBDEV_NOTIFICATIONS,
};

// This struct can be passed in from user space for commands requiring extra args
struct mobdev_args {
    int  enable;       // 1 = push, 0 = pull
    char path[128];    // File path for transfer
    char ifname[32];   // Interface name for tethering
};

//
// 1) DETECT: Scan USB devices to check for phones
//
static int mobdev_detect_cb(struct usb_device *udev, void *data)
{
    unsigned short vid = le16_to_cpu(udev->descriptor.idVendor);
    unsigned short pid = le16_to_cpu(udev->descriptor.idProduct);
    int cfg_index, if_index;

    pr_info("mobdev_control: Checking device %04x:%04x\n", vid, pid);

    for (cfg_index = 0; cfg_index < udev->descriptor.bNumConfigurations; cfg_index++) {
        struct usb_host_config *cfg = &udev->config[cfg_index];

        for (if_index = 0; if_index < cfg->desc.bNumInterfaces; if_index++) {
            struct usb_interface *interface = cfg->interface[if_index];

            for (int alt_index = 0; alt_index < interface->num_altsetting; alt_index++) {
                struct usb_interface_descriptor *intf_desc = &interface->altsetting[alt_index].desc;

                switch (intf_desc->bInterfaceClass) {
                case USB_CLASS_STILL_IMAGE: // 0x06 (MTP/PTP)
                    pr_info("mobdev_control: Detected MTP/PTP device\n");
                    return 1;

                case USB_CLASS_WIRELESS_CONTROLLER: // 0xE0 (RNDIS tethering)
                    pr_info("mobdev_control: Detected RNDIS device\n");
                    return 1;

                case USB_CLASS_VENDOR_SPEC: // 0xFF (vendor-specific, common on Android)
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
// 2) FILE_TRANSFER: Uses ADB to transfer files
//
static long mobdev_file_transfer(struct mobdev_args *args)
{
    int ret = mobdev_detect_phone();
    char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL };
    char *argv[5];

    if (ret == 1) {
        pr_info("mobdev_control: Phone detected, initiating ADB file transfer.\n");

        argv[0] = "/usr/bin/adb";
        argv[1] = args->enable ? "push" : "pull"; // 1 = push, 0 = pull
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
// 3) TETHERING: Controls USB network interfaces
//
static long mobdev_tethering(struct mobdev_args *args)
{
    struct net_device *ndev;
    char ifname[32];

    ifname[0] = '\0';
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
// 4) NOTIFICATIONS: Simulates notifications from phone
//
static bool notifications_enabled = false;

static void send_fake_notification_to_userspace(void)
{
    pr_info("mobdev_control: Sending fake notification to user space\n");
}

static long mobdev_notifications(struct mobdev_args *args)
{
    if (args->enable && !notifications_enabled) {
        pr_info("mobdev_control: Enabling notifications\n");
        notifications_enabled = true;
        send_fake_notification_to_userspace();
    } else if (!args->enable && notifications_enabled) {
        pr_info("mobdev_control: Disabling notifications\n");
        notifications_enabled = false;
    } else {
        pr_info("mobdev_control: No change in notification state\n");
    }
    return 0;
}

// -------------------------------------------------------------------------------------------------

//
// System Call Implementation
//
SYSCALL_DEFINE2(mobdev_control,
                unsigned int, cmd,
                unsigned long, arg)
{
    struct mobdev_args kargs;
    long ret = 0;

    if ((cmd == MOBDEV_FILE_TRANSFER ||
         cmd == MOBDEV_TETHERING   ||
         cmd == MOBDEV_NOTIFICATIONS) && arg != 0) {
        if (copy_from_user(&kargs, (struct mobdev_args __user *)arg, sizeof(kargs))) {
            pr_err("mobdev_control: Failed to copy args from user\n");
            return -EFAULT;
        }
    }

    switch (cmd) {
    case MOBDEV_DETECT:
        pr_info("mobdev_control: DETECT command\n");
        ret = mobdev_detect_phone();
        break;

    case MOBDEV_FILE_TRANSFER:
        pr_info("mobdev_control: FILE_TRANSFER command\n");
        ret = mobdev_file_transfer(&kargs);
        break;

    case MOBDEV_TETHERING:
        pr_info("mobdev_control: TETHERING command\n");
        ret = mobdev_tethering(&kargs);
        break;

    case MOBDEV_NOTIFICATIONS:
        pr_info("mobdev_control: NOTIFICATIONS command\n");
        ret = mobdev_notifications(&kargs);
        break;

    default:
        pr_err("mobdev_control: Unknown command %u\n", cmd);
        ret = -EINVAL;
        break;
    }

    return ret;
}
