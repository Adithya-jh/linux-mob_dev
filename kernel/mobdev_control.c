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

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/string.h>

//
// Supported Commands
//
enum mobdev_cmd {
    MOBDEV_DETECT = 0,
    MOBDEV_FILE_TRANSFER,
    MOBDEV_TETHERING,
    MOBDEV_NOTIFICATIONS,
    MOBDEV_CALL_CONTROL,
    MOBDEV_MEDIA_CONTROL
};

//
// Struct for User Arguments
//
struct mobdev_args {
    int  enable;     // For file transfer/tethering/notifications (1=push/on, 0=pull/off)
    char path[128];  // File path for transfer
    char ifname[32]; // Interface name for tethering
    int action;      // For call control/media control: 1=answer/vol up, 0=reject/vol down
};

/* ------------------------------------------------------------------------------------
 *
 * 1) FILE TRANSFER VIA ADB (NO DETECTION)
 *
 * ------------------------------------------------------------------------------------
 */
static long mobdev_file_transfer(struct mobdev_args *args)
{
    // usermodehelper environment
    char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL };
    char *argv[5];
    int ret;

    pr_info("mobdev_control: Initiating ADB file transfer (detection bypassed).\n");

    // Use the wrapper script instead of /usr/bin/adb:
    argv[0] = "/tmp/adb_wrapper.sh";
    argv[1] = args->enable ? "push" : "pull";
    argv[2] = args->path;
    argv[3] = args->enable ? "/sdcard/" : "/home/user/";
    argv[4] = NULL;

    ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
    if (ret < 0) {
        pr_err("mobdev_control: ADB transfer failed (ret=%d).\n", ret);
        return -EIO;
    }

    pr_info("mobdev_control: ADB transfer successful.\n");
    return 0;
}

/* ------------------------------------------------------------------------------------
 *
 * 2) CALL CONTROL VIA ADB SHELL (NO DETECTION)
 *
 * ------------------------------------------------------------------------------------
 */
static long mobdev_call_control(struct mobdev_args *args)
{
    // We'll assume the phone is recognized by "adb devices"
    char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL };
    char *argv[6];
    int ret;

    argv[0] = "/tmp/adb_wrapper.sh";
    argv[1] = "shell";
    argv[2] = "input";
    argv[3] = "keyevent";

    if (args->action == 1) {
        // answer call
        argv[4] = "KEYCODE_CALL";
        pr_info("mobdev_control: Attempting to answer call via ADB.\n");
    } else {
        // reject call
        argv[4] = "KEYCODE_ENDCALL";
        pr_info("mobdev_control: Attempting to reject/end call via ADB.\n");
    }
    argv[5] = NULL;

    ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
    pr_info("mobdev_control: call_usermodehelper returned %d\n", ret);

    if (ret < 0) {
        pr_err("mobdev_control: ADB call control failed (ret=%d)\n", ret);
        return -EIO;
    }

    pr_info("mobdev_control: ADB call command completed successfully.\n");
    return 0;
}

/* ------------------------------------------------------------------------------------
 *
 * 3) TETHERING: Enable/Disable USB Tethering
 *
 * ------------------------------------------------------------------------------------
 */
static long mobdev_tethering(struct mobdev_args *args)
{
    struct net_device *ndev;
    char ifname[32];

    strncpy(ifname, args->ifname, sizeof(ifname) - 1);
    ifname[sizeof(ifname) - 1] = '\0';

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

/* ------------------------------------------------------------------------------------
 *
 * 4) NOTIFICATIONS: Enable/Disable
 *
 * ------------------------------------------------------------------------------------
 */
static long mobdev_notifications(struct mobdev_args *args)
{
    if (args->enable) {
        pr_info("mobdev_control: Notifications enabled\n");
    } else {
        pr_info("mobdev_control: Notifications disabled\n");
    }
    return 0;
}

/* ------------------------------------------------------------------------------------
 *
 * 5) MEDIA CONTROL: Volume Up/Down via ADB (NO DETECTION)
 *
 * ------------------------------------------------------------------------------------
 */
static long mobdev_media_control(struct mobdev_args *args)
{
    char *envp[] = { "HOME=/", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", NULL };
    char *argv[6];
    int ret;

    argv[0] = "/tmp/adb_wrapper.sh";
    argv[1] = "shell";
    argv[2] = "input";
    argv[3] = "keyevent";

    if (args->action == 1) {
        argv[4] = "KEYCODE_VOLUME_UP";
        pr_info("mobdev_control: Attempting to raise volume via ADB.\n");
    } else {
        argv[4] = "KEYCODE_VOLUME_DOWN";
        pr_info("mobdev_control: Attempting to lower volume via ADB.\n");
    }
    argv[5] = NULL;

    ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
    pr_info("mobdev_control: call_usermodehelper returned %d\n", ret);

    if (ret < 0) {
        pr_err("mobdev_control: ADB media control failed (ret=%d)\n", ret);
        return -EIO;
    }

    pr_info("mobdev_control: ADB media volume command completed successfully.\n");
    return 0;
}

/* ------------------------------------------------------------------------------------
 *
 * 6) SYSTEM CALL HANDLER
 *
 * ------------------------------------------------------------------------------------
 */
SYSCALL_DEFINE2(mobdev_control,
                unsigned int, cmd,
                unsigned long, arg)
{
    struct mobdev_args kargs;
    long ret = 0;

    // Copy arguments from user space if needed
    if ((cmd == MOBDEV_FILE_TRANSFER ||
         cmd == MOBDEV_TETHERING    ||
         cmd == MOBDEV_NOTIFICATIONS ||
         cmd == MOBDEV_CALL_CONTROL ||
         cmd == MOBDEV_MEDIA_CONTROL) && arg != 0)
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
        pr_info("mobdev_control: CALL_CONTROL command (ADB-based, detection bypassed)\n");
        ret = mobdev_call_control(&kargs);
        break;

    case MOBDEV_MEDIA_CONTROL:
        pr_info("mobdev_control: MEDIA_CONTROL command (Volume, detection bypassed)\n");
        ret = mobdev_media_control(&kargs);
        break;

    default:
        pr_err("mobdev_control: Unknown command %u\n", cmd);
        ret = -EINVAL;
        break;
    }

    return ret;
}
