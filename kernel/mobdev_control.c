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
    int  enable;
    char path[128];
};

//
// 1) DETECT
//    Example: Scan USB devices to see if something that looks like a phone is connected.
//
static int mobdev_detect_cb(struct usb_device *udev, void *data)
{
    unsigned short vid = le16_to_cpu(udev->descriptor.idVendor);
    unsigned short pid = le16_to_cpu(udev->descriptor.idProduct);

    /*
     * Based on lsusb output:
     *   Bus 001 Device 010: ID 18d1:4ee8 Google Inc. Nexus/Pixel Device (MIDI)
     * We'll specifically match that vendor/product pair.
     */
    if (vid == 0x18d1 && pid == 0x4ee8) {
        pr_info("mobdev_control: Found phone (Google) at %04x:%04x\n", vid, pid);
        return 1; // signal “found phone” to usb_for_each_dev
    }

    // Otherwise, keep scanning
    return 0;
}

static long mobdev_detect_phone(void)
{
    int found = usb_for_each_dev(NULL, mobdev_detect_cb);
    /* 
     * If found == 1, we matched our Google device. If 0, none found.
     * Return 1 means “phone detected,” 0 means “not detected.”
     */
    return found;
}

//
// 2) FILE_TRANSFER
//    For real usage, you'd rely on MTP or mass storage and user-space mounts.
//    Below is just a placeholder.
//
static long mobdev_file_transfer(struct mobdev_args *args)
{
    if (!args->path[0]) {
        pr_err("mobdev_control: No path specified for file transfer\n");
        return -EINVAL;
    }
    pr_info("mobdev_control: (Stub) file transfer to/from: %s\n", args->path);
    // Real logic for MTP or block device mount would be far more complex.
    return 0;
}

//
// 3) TETHERING
//    Naive example: Turn a USB network interface on or off (assume "usb0").
//
static long mobdev_tethering(struct mobdev_args *args)
{
    struct net_device *ndev;

    rtnl_lock();
    ndev = dev_get_by_name(&init_net, "usb0");
    if (!ndev) {
        pr_err("mobdev_control: usb0 interface not found\n");
        rtnl_unlock();
        return -ENODEV;
    }

    if (args->enable) {
        // Bring interface up
        if (!(ndev->flags & IFF_UP)) {
            pr_info("mobdev_control: Bringing usb0 up\n");
            ndev->flags |= IFF_UP;
        }
    } else {
        // Bring interface down
        if (ndev->flags & IFF_UP) {
            pr_info("mobdev_control: Bringing usb0 down\n");
            ndev->flags &= ~IFF_UP;
        }
    }

    dev_put(ndev);
    rtnl_unlock();

    // Additional steps (enabling IP forwarding, NAT, DHCP, etc.) typically happen in user space.
    return 0;
}

//
// 4) NOTIFICATIONS
//    For demonstration: enable or disable a "fake" notification mechanism.
//
static bool notifications_enabled = false;

static void send_fake_notification_to_userspace(void)
{
    // Real code might open a netlink socket or similar.
    pr_info("mobdev_control: Sending a fake notification to user space\n");
}

static long mobdev_notifications(struct mobdev_args *args)
{
    if (args->enable && !notifications_enabled) {
        pr_info("mobdev_control: Enabling phone notifications\n");
        notifications_enabled = true;
        send_fake_notification_to_userspace();
    } else if (!args->enable && notifications_enabled) {
        pr_info("mobdev_control: Disabling phone notifications\n");
        notifications_enabled = false;
        // teardown subscription if applicable
    } else {
        pr_info("mobdev_control: No change in notification subscription\n");
    }
    return 0;
}

//
// The main syscall. We define it via SYSCALL_DEFINE2(...) so that the kernel
// auto-generates the correct 'sys_mobdev_control' symbol and prototype.
//
SYSCALL_DEFINE2(mobdev_control,
                unsigned int, cmd,
                unsigned long, arg)
{
    struct mobdev_args kargs;
    long ret = 0;

    // If command uses arguments, copy from user
    if ((cmd == MOBDEV_FILE_TRANSFER ||
         cmd == MOBDEV_TETHERING   ||
         cmd == MOBDEV_NOTIFICATIONS) && arg != 0) {
        if (copy_from_user(&kargs, (struct mobdev_args __user *)arg, sizeof(kargs))) {
            pr_err("mobdev_control: Failed to copy args from user\n");
            return -EFAULT;
        }
    }

    // Dispatch by command
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
