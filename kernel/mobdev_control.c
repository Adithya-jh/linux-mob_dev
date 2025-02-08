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
    char ifname[32];  // <-- Add a field for interface name
};

//
// 1) DETECT
//    Example: Scan USB devices to see if something that looks like a phone is connected.
//
static int mobdev_detect_cb(struct usb_device *udev, void *data)
{
    unsigned short vid = le16_to_cpu(udev->descriptor.idVendor);
    unsigned short pid = le16_to_cpu(udev->descriptor.idProduct);
    int cfg_index, if_index;

    // Print out each device for debugging
    pr_info("mobdev_control: Checking device %04x:%04x (bNumConfigurations=%u)\n",
            vid, pid, udev->descriptor.bNumConfigurations);

    // We'll scan each configuration and interface to see if anything "looks like a phone."
    // Many phones present MTP/PTP (class=0x06), RNDIS tethering (class=0xE0), or vendor-specific (class=0xFF).
    // This is a heuristic, not a guarantee.

    for (cfg_index = 0; cfg_index < udev->descriptor.bNumConfigurations; cfg_index++) {
        struct usb_host_config *cfg = &udev->config[cfg_index];
        int num_interfaces = cfg->desc.bNumInterfaces;

        for (if_index = 0; if_index < num_interfaces; if_index++) {
            struct usb_interface *interface = cfg->interface[if_index];
            int alt_index;

            // Each interface can have multiple altsettings.
            for (alt_index = 0; alt_index < interface->num_altsetting; alt_index++) {
                struct usb_interface_descriptor *intf_desc =
                    &interface->altsetting[alt_index].desc;

                pr_info("mobdev_control: Interface class=0x%02x, subclass=0x%02x, protocol=0x%02x\n",
                        intf_desc->bInterfaceClass,
                        intf_desc->bInterfaceSubClass,
                        intf_desc->bInterfaceProtocol);

                // Heuristic: check for MTP/PTP, RNDIS, or vendor-specific classes
                switch (intf_desc->bInterfaceClass) {
                case USB_CLASS_STILL_IMAGE: // 0x06 (PTP, often used by MTP)
                    // MTP is actually a subclass of PTP, so many Android phones show up here
                    pr_info("mobdev_control: Detected MTP/PTP interface => phone-like\n");
                    return 1; // Found a phone-like device

                case USB_CLASS_WIRELESS_CONTROLLER: // 0xE0 (RNDIS tethering, etc.)
                    // Some phones present RNDIS for USB tethering
                    pr_info("mobdev_control: Detected RNDIS (wireless controller) => phone-like\n");
                    return 1;

                case USB_CLASS_VENDOR_SPEC: // 0xFF (vendor-specific, common on many Android devices)
                    pr_info("mobdev_control: Detected vendor-specific interface => phone-like\n");
                    return 1;

                // You could add more cases if you want to catch other classes
                default:
                    // Not recognized as a phone-like interface, keep checking
                    break;
                }
            }
        }
    }

    // If we get here, no interface matched phone-like criteria; keep scanning next device
    return 0;
}

static long mobdev_detect_phone(void)
{
    // usb_for_each_dev stops when the callback returns non-zero.
    // We'll return 1 if we find a "phone," or 0 if none found.
    int found = usb_for_each_dev(NULL, mobdev_detect_cb);
    return found; // 1 => found phone, 0 => no phone
}

// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------




//
// 2) FILE_TRANSFER
//    For real usage, you'd rely on MTP or mass storage and user-space mounts.
//    Below is just a placeholder.
//






static int mobdev_detect_mtp_cb(struct usb_device *udev, void *data)
{
    int i, j;

    /*
     * In this callback, we examine each USB device's configs and interfaces
     * to see if any match MTP/Still Image class = 0x06 or vendor-specific 0xFF.
     * If we find it, return 1 => "stop iteration and report found."
     */

    // Iterate over each config
    for (i = 0; i < udev->descriptor.bNumConfigurations; i++) {
        struct usb_host_config *cfg = &udev->config[i];
        int num_ifs = cfg->desc.bNumInterfaces;

        // For each interface in this config
        for (j = 0; j < num_ifs; j++) {
            struct usb_interface *intf = cfg->interface[j];
            int alt;
            for (alt = 0; alt < intf->num_altsetting; alt++) {
                struct usb_interface_descriptor *idesc =
                    &intf->altsetting[alt].desc;

                /*
                 * MTP often shows up as USB_CLASS_STILL_IMAGE (0x06),
                 * though subClass/protocol can vary. We'll just check class.
                 */
                if (idesc->bInterfaceClass == USB_CLASS_STILL_IMAGE) {
                    pr_info("mobdev_detect_mtp_cb: Found standard MTP device %04x:%04x\n",
                        le16_to_cpu(udev->descriptor.idVendor),
                        le16_to_cpu(udev->descriptor.idProduct));

                    return 1; // Found standard MTP
                }
                /*
                 * Some devices (like OnePlus) use vendor-specific class 0xFF
                 * We check subClass = 0xFF, protocol = 0x00 to confirm.
                 */
                else if (idesc->bInterfaceClass == 0xFF && idesc->bInterfaceSubClass == 0xFF && idesc->bInterfaceProtocol == 0x00) {
                    pr_info("mobdev_detect_mtp_cb: Found vendor-specific MTP device %04x:%04x\n",
                        le16_to_cpu(udev->descriptor.idVendor),
                        le16_to_cpu(udev->descriptor.idProduct));

                    return 1; // Found vendor-specific MTP
                }
            }
        }
    }

    return 0; // "Didn't find MTP in this device"
}

static int mobdev_detect_mtp(void)
{
    /*
     * usb_for_each_dev(owner, callback):
     *  - Iterates over every USB device.
     *  - If the callback returns non-zero, iteration stops.
     *  - The non-zero value is returned from usb_for_each_dev().
     */
    int found;
    found = usb_for_each_dev(NULL, mobdev_detect_mtp_cb);

    /*
     * If found == 1, that means we found an MTP interface.
     * If 0, no MTP device was found.
     */
    return found;
}



static long mobdev_file_transfer(struct mobdev_args *args)
{
    int ret = mobdev_detect_mtp();
    if (ret == 1) {
        pr_info("mobdev_control: MTP device present, user-space should do actual file copy.\n");
        return 0; // success
    } else {
        pr_err("mobdev_control: No MTP device found.\n");
        return -ENODEV;
    }
}







// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------------------




//
// 3) TETHERING
//    Naive example: Turn a USB network interface on or off (assume "usb0").
//
static long mobdev_tethering(struct mobdev_args *args)
{
    struct net_device *ndev;
    char ifname[32];

    // Safety: ensure null-terminated
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
        // Bring interface up
        if (!(ndev->flags & IFF_UP)) {
            pr_info("mobdev_control: Bringing '%s' up\n", ifname);
            ndev->flags |= IFF_UP;
        }
    } else {
        // Bring interface down
        if (ndev->flags & IFF_UP) {
            pr_info("mobdev_control: Bringing '%s' down\n", ifname);
            ndev->flags &= ~IFF_UP;
        }
    }

    dev_put(ndev);
    rtnl_unlock();
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
