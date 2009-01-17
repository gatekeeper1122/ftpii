/*

Copyright (C) 2008 Joseph Jordan <joe.ftpii@psychlaw.com.au>

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from
the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1.The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software in a
product, an acknowledgment in the product documentation would be
appreciated but is not required.

2.Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3.This notice may not be removed or altered from any source distribution.

*/
#include <di/di.h>
#include <fat.h>
#include <fst/fst.h>
#include <isfs/isfs.h>
#include <iso/iso.h>
#include <nandimg/nandimg.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <ogc/usbstorage.h>
#include <sdcard/gcsd.h>
#include <sdcard/wiisd_io.h>
#include <stdio.h>
#include <string.h>
#include <sys/dir.h>
#include <unistd.h>
#include <wiiuse/wpad.h>
#include <wod/wod.h>

#include "dvd.h"
#include "fs.h"

VIRTUAL_PARTITION VIRTUAL_PARTITIONS[] = {
    { "SD Gecko A", "/gcsda", "gcsda", "gcsda:/", false, false, &__io_gcsda },
    { "SD Gecko B", "/gcsdb", "gcsdb", "gcsdb:/", false, false, &__io_gcsdb },
    { "Front SD", "/sd", "sd", "sd:/", false, false, &__io_wiisd },
    { "USB storage device", "/usb", "usb", "usb:/", false, false, &__io_usbstorage },
    { "ISO9660 filesystem", "/dvd", "dvd", "dvd:/", false, false, NULL },
    { "Wii disc image", "/wod", "wod", "wod:/", false, false, NULL },
    { "Wii disc filesystem", "/fst", "fst", "fst:/", false, false, NULL },
    { "NAND images", "/nand", "nand", "nand:/", false, false, NULL },
    { "NAND filesystem", "/isfs", "isfs", "isfs:/", false, false, NULL }
};
const u32 MAX_VIRTUAL_PARTITIONS = (sizeof(VIRTUAL_PARTITIONS) / sizeof(VIRTUAL_PARTITION));

VIRTUAL_PARTITION *PA_GCSDA = VIRTUAL_PARTITIONS + 0;
VIRTUAL_PARTITION *PA_GCSDB = VIRTUAL_PARTITIONS + 1;
VIRTUAL_PARTITION *PA_SD    = VIRTUAL_PARTITIONS + 2;
VIRTUAL_PARTITION *PA_USB   = VIRTUAL_PARTITIONS + 3;
VIRTUAL_PARTITION *PA_DVD   = VIRTUAL_PARTITIONS + 4;
VIRTUAL_PARTITION *PA_WOD   = VIRTUAL_PARTITIONS + 5;
VIRTUAL_PARTITION *PA_FST   = VIRTUAL_PARTITIONS + 6;
VIRTUAL_PARTITION *PA_NAND  = VIRTUAL_PARTITIONS + 7;
VIRTUAL_PARTITION *PA_ISFS  = VIRTUAL_PARTITIONS + 8;

static const u32 CACHE_PAGES = 8;

static bool fatInitState = false;
static u32 device_check_iteration = 1;

const char *to_real_prefix(VIRTUAL_PARTITION *partition) {
    return partition->prefix;
}

static VIRTUAL_PARTITION *to_virtual_partition(const char *virtual_prefix) {
    u32 i;
    for (i = 0; i < MAX_VIRTUAL_PARTITIONS; i++)
        if (!strcasecmp(VIRTUAL_PARTITIONS[i].alias, virtual_prefix))
            return &VIRTUAL_PARTITIONS[i];
    return NULL;
}

static bool is_gecko(VIRTUAL_PARTITION *partition) {
    return partition == PA_GCSDA || partition == PA_GCSDB;
}

static bool is_fat(VIRTUAL_PARTITION *partition) {
    return partition == PA_SD || partition == PA_USB || is_gecko(partition);
}

static bool is_dvd(VIRTUAL_PARTITION *partition) {
    return partition == PA_DVD || partition == PA_WOD || partition == PA_FST;
}

bool mounted(VIRTUAL_PARTITION *partition) {
    DIR_ITER *dir = diropen(to_real_prefix(partition));
    if (dir) {
        dirclose(dir);
        return true;
    }
    return false;
}

static bool was_inserted_or_removed(VIRTUAL_PARTITION *partition) {
    if ((!partition->disc || partition->geckofail) && !is_dvd(partition)) return false;
    bool already_inserted = partition->inserted || mounted(partition);
    if (!already_inserted && partition == PA_SD) partition->disc->startup();
    if (is_dvd(partition)) {
        if (partition == PA_DVD) {
            if (!dvd_mountWait()) {
                u32 status;
                if (!DI_GetCoverRegister(&status)) partition->inserted = (status & 2) == 2;
            }
        } else {
            partition->inserted = PA_DVD->inserted;
        }
    } else {
        partition->inserted = partition->disc->isInserted();        
    }
    return already_inserted != partition->inserted;
}

static void fat_enable_readahead(VIRTUAL_PARTITION *partition) {
    // if (!fatEnableReadAhead(to_real_prefix(partition), 64, 128))
    //     printf("Could not enable FAT read-ahead caching on %s, speed will suffer...\n", partition->name);
}

static void fat_enable_readahead_all() {
    u32 i;
    for (i = 0; i < MAX_VIRTUAL_PARTITIONS; i++) {
        VIRTUAL_PARTITION *partition = VIRTUAL_PARTITIONS + i;
        if (is_fat(partition) && mounted(partition)) fat_enable_readahead(partition);
    }
}

static bool initialise_fat() {
    if (fatInitState) return true;
    if (fatInit(CACHE_PAGES, false)) { 
        fatInitState = 1;
        fat_enable_readahead_all();
    }
    return fatInitState;
}

typedef enum { MOUNTSTATE_START, MOUNTSTATE_SELECTDEVICE, MOUNTSTATE_WAITFORDEVICE } mountstate_t;
static mountstate_t mountstate = MOUNTSTATE_START;
static VIRTUAL_PARTITION *mount_partition = NULL;
static u64 mount_timer = 0;

bool mount(VIRTUAL_PARTITION *partition) {
    if (!partition || mounted(partition) || (is_dvd(partition) && dvd_mountWait())) return false;
    
    bool success = false;
    printf("Mounting %s...", partition->name);
    if (is_dvd(partition)) {
        set_dvd_mountWait(true);
        DI_Mount();
        u64 timeout = gettime() + secs_to_ticks(10);
        while (!(DI_GetStatus() & DVD_READY) && gettime() < timeout) usleep(2000);
        if (DI_GetStatus() & DVD_READY) {
            set_dvd_mountWait(false);
            if (partition == PA_DVD) success = ISO9660_Mount();
            else if (partition == PA_WOD) success = WOD_Mount();
            else if (partition == PA_FST) success = FST_Mount();
        }
        if (!dvd_mountWait() && !dvd_last_access()) dvd_stop();
    } else if (is_fat(partition)) {
        bool retry_gecko = true;
        gecko_retry:
        if (partition->disc->shutdown() & partition->disc->startup()) {
            if (!fatInitState) {
                if (initialise_fat()) success = mounted(partition);
            } else if (fatMount(partition->mount_point, partition->disc, 0, CACHE_PAGES)) {
                fat_enable_readahead(partition);
                success = true;
            }
        } else if (is_gecko(partition) && retry_gecko) {
            retry_gecko = false;
            sleep(1);
            goto gecko_retry;
        }
    } else if (partition == PA_NAND) {
        success = NANDIMG_Mount();
    } else if (partition == PA_ISFS) {
        success = ISFS_Mount();
    }
    printf(success ? "succeeded.\n" : "failed.\n");
    if (success && is_gecko(partition)) partition->geckofail = false;

    return success;
}

bool mount_virtual(const char *dir) {
    return mount(to_virtual_partition(dir));
}

bool unmount(VIRTUAL_PARTITION *partition) {
    if (!partition || !mounted(partition) || (is_dvd(partition) && dvd_mountWait())) return false;

    printf("Unmounting %s...", partition->name);
    bool success = false;
    if (is_dvd(partition)) {
        if (partition == PA_DVD) success = ISO9660_Unmount();
        else if (partition == PA_WOD) success = WOD_Unmount();
        else if (partition == PA_FST) success = FST_Unmount();
        if (!dvd_mountWait() && !dvd_last_access()) dvd_stop();
    } else if (is_fat(partition)) {
        fatUnmount(to_real_prefix(partition));
        success = true;
    } else if (partition == PA_NAND) {
        success = NANDIMG_Unmount();
    } else if (partition == PA_ISFS) {
        success = ISFS_Unmount();
    }
    printf(success ? "succeeded.\n" : "failed.\n");

    return success;
}

bool unmount_virtual(const char *dir) {
    return unmount(to_virtual_partition(dir));
}

void check_removable_devices() {
    if (device_check_iteration++ % 400) return;

    u32 i;
    for (i = 0; i < MAX_VIRTUAL_PARTITIONS; i++) {
        VIRTUAL_PARTITION *partition = VIRTUAL_PARTITIONS + i;
        if (mount_timer && partition == mount_partition) continue;
        if (was_inserted_or_removed(partition)) {
            if (partition->inserted && (partition == PA_DVD || (!is_dvd(partition) && !mounted(partition)))) {
                printf("Device inserted; ");
                if (partition == PA_DVD) {
                    set_dvd_mountWait(true);
                    DI_Mount();
                    printf("Mounting DVD...\n");
                } else if (!mount(partition) && is_gecko(partition)) {
                    printf("%s failed to automount.  Insertion or removal will not be detected until it is mounted manually.\n", partition->name);
                    printf("Note that inserting an SD Gecko without an SD card in it can be problematic.\n");
                    partition->geckofail = true;
                }
            } else if (!partition->inserted && mounted(partition)) {
                printf("Device removed; ");
                unmount(partition);
            }
        }
    }
}

void process_remount_event() {
    if (mountstate == MOUNTSTATE_START || mountstate == MOUNTSTATE_SELECTDEVICE) {
        mountstate = MOUNTSTATE_SELECTDEVICE;
        mount_partition = NULL;
        printf("\nWhich device would you like to remount? (hold button on controller #1)\n\n");
        printf("           SD Gecko A (Up)\n");
        printf("                  | \n");
        printf("Front SD (Left) --+-- USB Storage Device (Right)\n");
        printf("                  | \n");
        printf("           SD Gecko B (Down)\n");
        printf("                  | \n");
        printf("              DVD (1/X)\n");
    } else if (mountstate == MOUNTSTATE_WAITFORDEVICE) {
        mount_timer = 0;
        mountstate = MOUNTSTATE_START;
        if (is_dvd(mount_partition)) {
            set_dvd_mountWait(true);
            DI_Mount();
            printf("Mounting DVD...\n");
        } else {
            mount(mount_partition);
        }
        mount_partition = NULL;
    }
}

void process_device_select_event(u32 pressed) {
    if (mountstate == MOUNTSTATE_SELECTDEVICE) {
        if (pressed & WPAD_BUTTON_LEFT) mount_partition = PA_SD;
        else if (pressed & WPAD_BUTTON_RIGHT) mount_partition = PA_USB;
        else if (pressed & WPAD_BUTTON_UP) mount_partition = PA_GCSDA;
        else if (pressed & WPAD_BUTTON_DOWN) mount_partition = PA_GCSDB;
        else if (pressed & WPAD_BUTTON_1) mount_partition = PA_DVD;
        if (mount_partition) {
            mountstate = MOUNTSTATE_WAITFORDEVICE;
            if (is_dvd(mount_partition)) {
                if (dvd_mountWait()) {
                    printf("The DVD is in the process of being mounted, it is not a good idea to mess with it.\n");
                    mountstate = MOUNTSTATE_START;
                    return;
                }
                dvd_unmount();
            }
            else if (is_fat(mount_partition)) unmount(mount_partition);
            printf("To continue after changing the device hold B on controller #1 or wait 30 seconds.\n");
            mount_timer = gettime() + secs_to_ticks(30);
        }
    }
}

void check_mount_timer(u64 now) {
    if (mount_timer && now > mount_timer) process_remount_event();
}

/*
    Returns a copy of path up to the last '/' character,
    If path does not contain '/', returns "".
    Returns a pointer to internal static storage space that will be overwritten by subsequent calls.
    This function is not thread-safe.
*/

void initialise_fs() {
    NANDIMG_Mount();
    ISFS_SU();
    if (ISFS_Initialize() == IPC_OK) ISFS_Mount();
    initialise_fat();
}

char *dirname(char *path) {
    static char result[MAXPATHLEN];
    strncpy(result, path, MAXPATHLEN - 1);
    result[MAXPATHLEN - 1] = '\0';
    s32 i;
    for (i = strlen(result) - 1; i >= 0; i--) {
        if (result[i] == '/') {
            result[i] = '\0';
            return result;
        }
    }
    return "";
}

/*
    Returns a pointer into path, starting after the right-most '/' character.
    If path does not contain '/', returns path.
*/
char *basename(char *path) {
    s32 i;
    for (i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            return path + i + 1;
        }
    }
    return path;
}