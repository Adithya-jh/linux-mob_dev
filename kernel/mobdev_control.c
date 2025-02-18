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
#include <linux/rtnetlink.h>  // for rtnl_lock/rtnl_unlock
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/kmod.h>       // For call_usermodehelper()
#include <linux/module.h>
#include <net/netlink.h>
#include <net/sock.h>

/* Define our Netlink protocol number */
#define NETLINK_MOBDEV 31

/* Generic Netlink message structure used for IPC */
struct mobdev_netlink_msg {
    int type;   // 1 = volume, 2 = screen mirror, 3 = screenshot
    int value;  // For type 1: volume (1 = up, 0 = down);
                // For type 2: mirror (1 = start, 0 = stop);
                // For type 3: flag (always 1)
};

/* Global Netlink socket */
static struct sock *mobdev_nl_sk = NULL;

/* Prototype for our helper function */
static int send_netlink_msg(struct mobdev_netlink_msg *msg);

/*
 * Example command IDs (must match with user space)
 */
enum mobdev_cmd {
    MOBDEV_DETECT = 0,
    MOBDEV_FILE_TRANSFER,
    MOBDEV_TETHERING,
    MOBDEV_NOTIFICATIONS,
    MOBDEV_CALL_CONTROL,
    MOBDEV_MEDIA_CONTROL,
    MOBDEV_SCREENSHOT,
    MOBDEV_SCREEN_MIRROR
};

/*
 * Structure for arguments passed from user space.
 */
struct mobdev_args {
    int  enable;       // For transfer: 1 = push, 0 = pull.
                       // For mirror: 1 = start, 0 = stop.
                       // For volume: 1 = up, 0 = down.
    char path[128];    // For file transfer.
    char ifname[32];   // For tethering.
    int action;        // For volume.
};

/*
 * 1) DETECT: Scan USB devices to check for phones.
 */
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

/*
 * 2) FILE_TRANSFER: Uses ADB to transfer files.
 */
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

/*
 * 3) TETHERING: Controls USB network interfaces.
 */
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

/*
 * 4) NOTIFICATIONS: Simulates notifications from phone.
 */
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

/*
 * 5) CALL CONTROL: Detect & answer/reject calls via ADB.
 */
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
    pr_info("mobdev_control: Incoming call detected!\n");
    char **adb_action = args->action ? adb_answer_call : adb_reject_call;
    ret = call_usermodehelper(adb_action[0], adb_action, envp, UMH_WAIT_PROC);
    if (ret < 0) {
        pr_err("mobdev_control: Failed to process call action.\n");
        return -EIO;
    }
    pr_info("mobdev_control: Call processed successfully.\n");
    return 0;
}

/*
 * 6) MEDIA CONTROL: Volume up/down using Netlink IPC.
 * (Type 1 for volume: value 1 for up, 0 for down)
 */
static long mobdev_media_control(struct mobdev_args *args)
{
    struct mobdev_netlink_msg msg;
    if (!mobdev_detect_phone()) {
        pr_err("mobdev_control: No phone detected for media volume control.\n");
        return -ENODEV;
    }
    msg.type = 1;
    msg.value = (args->action == 1) ? 1 : 0;
    pr_info("mobdev_control: Received volume command: %s\n", (msg.value == 1) ? "up" : "down");
    if (send_netlink_msg(&msg) < 0) {
         pr_err("mobdev_control: Failed to send netlink message for volume control.\n");
         return -EIO;
    }
    pr_info("mobdev_control: Volume command sent to user space via netlink.\n");
    return 0;
}

/*
 * 7) SCREENSHOT: Trigger screenshot via Netlink IPC.
 * (Type 3 for screenshot; value is a flag.)
 */
static long mobdev_screenshot(void)
{
    struct mobdev_netlink_msg msg;
    int ret = mobdev_detect_phone();
    if (ret != 1) {
        pr_err("mobdev_control: No phone detected.\n");
        return -ENODEV;
    }
    msg.type = 3;
    msg.value = 1; // Flag value
    pr_info("mobdev_control: Sending screenshot command via netlink IPC.\n");
    if (send_netlink_msg(&msg) < 0) {
         pr_err("mobdev_control: Failed to send netlink message for screenshot.\n");
         return -EIO;
    }
    pr_info("mobdev_control: Screenshot command sent to user space via netlink.\n");
    return 0;
}

/*
 * 8) SCREEN MIRRORING: Trigger screen mirroring via Netlink IPC.
 * (Type 2 for screen mirroring; value: 1 = start, 0 = stop)
 */
static long mobdev_screen_mirror(struct mobdev_args *args)
{
    struct mobdev_netlink_msg msg;
    if (!mobdev_detect_phone()) {
        pr_err("mobdev_control: No phone detected for screen mirroring.\n");
        return -ENODEV;
    }
    msg.type = 2;
    msg.value = args->action; // 1 = start, 0 = stop
    pr_info("mobdev_control: Received screen mirror command: %s\n", (msg.value == 1) ? "start" : "stop");
    if (send_netlink_msg(&msg) < 0) {
         pr_err("mobdev_control: Failed to send netlink message for screen mirroring.\n");
         return -EIO;
    }
    pr_info("mobdev_control: Screen mirror command sent to user space via netlink.\n");
    return 0;
}

/*
 * Generic function to send a Netlink message.
 */
static int send_netlink_msg(struct mobdev_netlink_msg *msg)
{
    struct sk_buff *skb_out;
    struct nlmsghdr *nlh;
    int msg_size = sizeof(struct mobdev_netlink_msg);
    int res;

    skb_out = nlmsg_new(msg_size, GFP_KERNEL);
    if (!skb_out) {
         pr_err("mobdev_control: Failed to allocate new skb\n");
         return -ENOMEM;
    }
    nlh = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, msg_size, 0);
    if (!nlh) {
         kfree_skb(skb_out);
         return -ENOMEM;
    }
    memcpy(nlmsg_data(nlh), msg, msg_size);
    NETLINK_CB(skb_out).dst_group = 1; /* Multicast group 1 */
    res = nlmsg_multicast(mobdev_nl_sk, skb_out, 0, 1, GFP_KERNEL);
    if (res < 0) {
         if (res == -ESRCH) {
             pr_info("mobdev_control: No netlink listeners present, treating as success.\n");
             return 0;
         }
         pr_err("mobdev_control: Error sending netlink message: %d\n", res);
         return res;
    }
    return 0;
}

/*
 * Netlink initialization and exit functions.
 */
static int mobdev_nl_init(void)
{
    struct netlink_kernel_cfg cfg = {
        .input = NULL,  // Not used in this module.
    };

    mobdev_nl_sk = netlink_kernel_create(&init_net, NETLINK_MOBDEV, &cfg);
    if (!mobdev_nl_sk) {
         pr_err("mobdev_control: Error creating netlink socket.\n");
         return -ENOMEM;
    }
    pr_info("mobdev_control: Netlink socket created.\n");
    return 0;
}

static void mobdev_nl_exit(void)
{
    if (mobdev_nl_sk)
        netlink_kernel_release(mobdev_nl_sk);
}

/*
 * System Call Handler.
 */
SYSCALL_DEFINE2(mobdev_control,
                unsigned int, cmd,
                unsigned long, arg)
{
    struct mobdev_args kargs;
    long ret = 0;

    if ((cmd == MOBDEV_FILE_TRANSFER ||
         cmd == MOBDEV_TETHERING    ||
         cmd == MOBDEV_NOTIFICATIONS ||
         cmd == MOBDEV_CALL_CONTROL ||
         cmd == MOBDEV_MEDIA_CONTROL ||
         cmd == MOBDEV_SCREENSHOT    ||
         cmd == MOBDEV_SCREEN_MIRROR) && arg != 0)
    {
        if (copy_from_user(&kargs, (struct mobdev_args __user *)arg, sizeof(kargs))) {
            pr_err("mobdev_control: Failed to copy args from user\n");
            return -EFAULT;
        }
    } else {
        memset(&kargs, 0, sizeof(kargs));
    }

    switch (cmd) {
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
        pr_info("mobdev_control: CALL_CONTROL command (ADB-based).\n");
        ret = mobdev_call_control(&kargs);
        break;
    case MOBDEV_MEDIA_CONTROL:
        pr_info("mobdev_control: MEDIA_CONTROL command (Volume, Netlink-based).\n");
        ret = mobdev_media_control(&kargs);
        break;
    case MOBDEV_SCREENSHOT:
        pr_info("mobdev_control: SCREENSHOT command (Netlink-based).\n");
        ret = mobdev_screenshot();
        break;
    case MOBDEV_SCREEN_MIRROR:
        pr_info("mobdev_control: SCREEN_MIRROR command (Netlink-based).\n");
        ret = mobdev_screen_mirror(&kargs);
        break;
    default:
        pr_err("mobdev_control: Unknown command %u\n", cmd);
        ret = -EINVAL;
        break;
    }
    return ret;
}

MODULE_LICENSE("GPL");
module_init(mobdev_nl_init);
module_exit(mobdev_nl_exit);
