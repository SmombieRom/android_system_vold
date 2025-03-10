/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "PrivateVolume.h"
#include "EmulatedVolume.h"
#include "Utils.h"
#include "VolumeManager.h"
#include "cryptfs.h"
#include "fs/Ext4.h"
#include "fs/F2fs.h"
#include "MetadataCrypt.h"
#include "FsCrypt.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <cutils/fs.h>
#include <private/android_filesystem_config.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>

#define RETRY_MOUNT_ATTEMPTS 10
#define RETRY_MOUNT_DELAY_SECONDS 1

constexpr int FS_AES_256_XTS_KEY_SIZE = 64;

using android::base::StringPrintf;

namespace android {
namespace vold {

static const unsigned int kMajorBlockMmc = 179;

PrivateVolume::PrivateVolume(dev_t device, const std::string& keyRaw, int flags)
    : VolumeBase(Type::kPrivate), mRawDevice(device), mKeyRaw(keyRaw),
    mDiskFlags(flags) {
    setId(StringPrintf("private:%u,%u", major(device), minor(device)));
    mRawDevPath = StringPrintf("/dev/block/vold/%s", getId().c_str());
}

PrivateVolume::~PrivateVolume() {}

status_t PrivateVolume::readMetadata() {
    status_t res = ReadMetadata(mDmDevPath, &mFsType, &mFsUuid, &mFsLabel);

    auto listener = getListener();
    if (listener) listener->onVolumeMetadataChanged(getId(), mFsType, mFsUuid, mFsLabel);

    return res;
}

status_t PrivateVolume::doCreate() {
    if (CreateDeviceNode(mRawDevPath, mRawDevice)) {
        return -EIO;
    }

    if (is_ice_supported_external(mDiskFlags)) {
        if (mKeyRaw.size() != FS_AES_256_XTS_KEY_SIZE) {
            PLOG(ERROR) << getId() << " Keysize " << mKeyRaw.size()
                        << " does not match AES XTS keysize " << FS_AES_256_XTS_KEY_SIZE;
            return -EIO;
        }
    } else {
        if (mKeyRaw.size() != cryptfs_get_keysize()) {
            PLOG(ERROR) << getId() << " Raw keysize " << mKeyRaw.size()
                        << " does not match crypt keysize " << cryptfs_get_keysize();
            return -EIO;
        }
    }

    // Recover from stale vold by tearing down any old mappings
    cryptfs_revert_ext_volume(getId().c_str());

    // TODO: figure out better SELinux labels for private volumes

    unsigned char* key = (unsigned char*)mKeyRaw.data();
    char crypto_blkdev[MAXPATHLEN];
    int res = 0;

    if (is_ice_supported_external(mDiskFlags))
        res = fscrypt_setup_ufscard_volume (getId(), mRawDevPath, mKeyRaw, mDmDevPath);
    else {
        res = cryptfs_setup_ext_volume(getId().c_str(), mRawDevPath.c_str(),
                              key, crypto_blkdev);
        mDmDevPath = crypto_blkdev;
    }

    if (res != 0) {
        PLOG(ERROR) << getId() << " failed to setup cryptfs";
        return -EIO;
    }

    int fd = 0;
    int retries = RETRY_MOUNT_ATTEMPTS;
    while ((fd = open(mDmDevPath.c_str(), O_WRONLY|O_CLOEXEC)) < 0) {
        if (retries > 0) {
            retries--;
            PLOG(ERROR) << "Error opening crypto_blkdev " << mDmDevPath.c_str()
                            << " for private volume. err=" << errno
                            << "(" << strerror(errno) << "), retrying for the "
                            << RETRY_MOUNT_ATTEMPTS - retries << " time";
            sleep(RETRY_MOUNT_DELAY_SECONDS);
        } else {
            PLOG(ERROR) << "Error opening crypto_blkdev " << mDmDevPath.c_str()
                            << " for private volume. err=" << errno
                            << "(" << strerror(errno) << "), retried "
                            << RETRY_MOUNT_ATTEMPTS << " times";
            close(fd);
            return -EIO;
        }
    }
    close(fd);
    return OK;
}

status_t PrivateVolume::doDestroy() {
    if (cryptfs_revert_ext_volume(getId().c_str())) {
        LOG(ERROR) << getId() << " failed to revert cryptfs";
    }
    return DestroyDeviceNode(mRawDevPath);
}

status_t PrivateVolume::doMount() {
    if (readMetadata()) {
        LOG(ERROR) << getId() << " failed to read metadata";
        return -EIO;
    }

    mPath = StringPrintf("/mnt/expand/%s", mFsUuid.c_str());
    setPath(mPath);

    if (PrepareDir(mPath, 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount point " << mPath;
        return -EIO;
    }

    if (mFsType == "ext4") {
        int res = ext4::Check(mDmDevPath, mPath);
        if (res == 0 || res == 1) {
            LOG(DEBUG) << getId() << " passed filesystem check";
        } else {
            PLOG(ERROR) << getId() << " failed filesystem check";
            return -EIO;
        }

        if (ext4::Mount(mDmDevPath, mPath, false, false, true, "", true)) {
            PLOG(ERROR) << getId() << " failed to mount";
            return -EIO;
        }

    } else if (mFsType == "f2fs") {
        int res = f2fs::Check(mDmDevPath);
        if (res == 0) {
            LOG(DEBUG) << getId() << " passed filesystem check";
        } else {
            PLOG(ERROR) << getId() << " failed filesystem check";
            return -EIO;
        }

        if (f2fs::Mount(mDmDevPath, mPath, "", true)) {
            PLOG(ERROR) << getId() << " failed to mount";
            return -EIO;
        }

    } else {
        LOG(ERROR) << getId() << " unsupported filesystem " << mFsType;
        return -EIO;
    }

    RestoreconRecursive(mPath);

    // Verify that common directories are ready to roll
    if (PrepareDir(mPath + "/app", 0771, AID_SYSTEM, AID_SYSTEM) ||
        PrepareDir(mPath + "/user", 0711, AID_SYSTEM, AID_SYSTEM) ||
        PrepareDir(mPath + "/user_de", 0711, AID_SYSTEM, AID_SYSTEM) ||
        PrepareDir(mPath + "/media", 0770, AID_MEDIA_RW, AID_MEDIA_RW) ||
        PrepareDir(mPath + "/media/0", 0770, AID_MEDIA_RW, AID_MEDIA_RW) ||
        PrepareDir(mPath + "/local", 0751, AID_ROOT, AID_ROOT) ||
        PrepareDir(mPath + "/local/tmp", 0771, AID_SHELL, AID_SHELL)) {
        PLOG(ERROR) << getId() << " failed to prepare";
        return -EIO;
    }

    // Create a new emulated volume stacked above us, it will automatically
    // be destroyed during unmount
    std::string mediaPath(mPath + "/media");
    auto vol = std::shared_ptr<VolumeBase>(new EmulatedVolume(mediaPath, mRawDevice, mFsUuid));
    addVolume(vol);
    vol->create();

    return OK;
}

status_t PrivateVolume::doUnmount() {
    ForceUnmount(mPath);

    if (TEMP_FAILURE_RETRY(rmdir(mPath.c_str()))) {
        PLOG(ERROR) << getId() << " failed to rmdir mount point " << mPath;
    }

    return OK;
}

status_t PrivateVolume::doFormat(const std::string& fsType) {
    std::string resolvedFsType = fsType;
    if (fsType == "auto") {
        // For now, assume that all MMC devices are flash-based SD cards, and
        // give everyone else ext4 because sysfs rotational isn't reliable.
        if (((major(mRawDevice) == kMajorBlockMmc) ||
                  ((mDiskFlags & android::vold::Disk::Flags::kUfsCard) ==
                           android::vold::Disk::Flags::kUfsCard)) &&
                               f2fs::IsSupported()) {
            resolvedFsType = "f2fs";
        } else {
            resolvedFsType = "ext4";
        }
        LOG(DEBUG) << "Resolved auto to " << resolvedFsType;
    }

    if (resolvedFsType == "ext4") {
        // TODO: change reported mountpoint once we have better selinux support
        if (ext4::Format(mDmDevPath, 0, "/data")) {
            PLOG(ERROR) << getId() << " failed to format";
            return -EIO;
        }
    } else if (resolvedFsType == "f2fs") {
        if (f2fs::Format(mDmDevPath)) {
            PLOG(ERROR) << getId() << " failed to format";
            return -EIO;
        }
    } else {
        LOG(ERROR) << getId() << " unsupported filesystem " << fsType;
        return -EINVAL;
    }

    return OK;
}

}  // namespace vold
}  // namespace android
