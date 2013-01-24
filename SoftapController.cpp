/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/wireless.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#define LOG_TAG "SoftapController"
#include <cutils/log.h>
#include <cutils/properties.h>
#include <netutils/ifc.h>
#include <private/android_filesystem_config.h>
#include "wifi.h"

#include "SoftapController.h"

#ifndef HOSTAPD_DRIVER_NAME
#define HOSTAPD_DRIVER_NAME "nl80211"
#endif

#define AP_DEFAULT_CHANNEL 5
#define AP_SOCKET_PATH "/data/misc/wifi/hostapd"

static const char HOSTAPD_CONF_FILE[]    = "/data/misc/wifi/hostapd.conf";

SoftapController::SoftapController() {
    mPid = 0;
    mSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (mSock < 0)
        ALOGE("Failed to open socket");
}

SoftapController::~SoftapController() {
    if (mSock >= 0)
        close(mSock);
}

int SoftapController::startDriver(char *iface) {
    int ret = 0;

    ifc_init();
    ret = ifc_up(iface);
    ifc_close();

    if (mSock < 0) {
        ALOGE("Softap driver start - failed to open socket");
        return -1;
    }
    if (!iface || (iface[0] == '\0')) {
        ALOGD("Softap driver start - wrong interface");
        iface = mIface;
    }

    *mBuf = 0;

    usleep(AP_DRIVER_START_DELAY);
    ALOGD("Softap driver start");
    return ret;
}

int SoftapController::stopDriver(char *iface) {
    int ret = 0;

    if (mSock < 0) {
        ALOGE("Softap driver stop - failed to open socket");
        return -1;
    }
    if (!iface || (iface[0] == '\0')) {
        ALOGD("Softap driver stop - wrong interface");
        iface = mIface;
    }
    *mBuf = 0;

    ifc_init();
    ret = ifc_down(iface);
    ifc_close();

    ALOGD("Softap driver stop");
    return ret;
}

int SoftapController::startSoftap() {
    pid_t pid = 1;

    if (mPid) {
        ALOGE("Softap already started");
        return 0;
    }
    if (mSock < 0) {
        ALOGE("Softap startap - failed to open socket");
        return -1;
    }

    if ((pid = fork()) < 0) {
        ALOGE("fork failed (%s)", strerror(errno));
        return -1;
    }

    if (!pid) {
#ifndef HOSTAPD_NO_ENTROPY
        ensure_entropy_file_exists();
#endif
        if (execl("/system/bin/hostapd", "/system/bin/hostapd",
#ifndef HOSTAPD_NO_ENTROPY
                  "-e", WIFI_ENTROPY_FILE,
#endif
                  HOSTAPD_CONF_FILE, (char *) NULL)) {
            ALOGE("execl failed (%s)", strerror(errno));
        }
        ALOGE("Should never get here!");
        return -1;
    } else {
        mPid = pid;
        ALOGD("Softap startap - Ok");
        usleep(AP_BSS_START_DELAY);
    }
    return 0;
}

int SoftapController::stopSoftap() {
    if (mPid == 0) {
        ALOGE("Softap already stopped");
        return 0;
    }

    ALOGD("Stopping Softap service");
    kill(mPid, SIGTERM);
    waitpid(mPid, NULL, 0);

    if (mSock < 0) {
        ALOGE("Softap stopap - failed to open socket");
        return -1;
    }
    mPid = 0;
    ALOGD("Softap service stopped");
    usleep(AP_BSS_STOP_DELAY);
    return 0;
}

bool SoftapController::isSoftapStarted() {
    return (mPid != 0 ? true : false);
}

/*
 * Arguments:
 *      argv[2] - wlan interface
 *      argv[3] - SSID
 *	argv[4] - Security
 *	argv[5] - Key
 *	argv[6] - Channel
 *	argv[7] - Preamble
 *	argv[8] - Max SCB
 */
int SoftapController::setSoftap(int argc, char *argv[]) {
    char psk_str[2*SHA256_DIGEST_LENGTH+1];
    int ret = 0, i = 0, fd;
    char *ssid;

    if (mSock < 0) {
        ALOGE("Softap set - failed to open socket");
        return -1;
    }
    if (argc < 4) {
        ALOGE("Softap set - missing arguments");
        return -1;
    }

    strncpy(mIface, argv[2], sizeof(mIface));

    char *wbuf = NULL;
    char *fbuf = NULL;
    int channel = 0;

    if (argc > 3) {
        ssid = argv[3];
    } else {
        ssid = (char *)"AndroidAP";
    }

    if (argc > 6) {
        channel = atoi(argv[6]);
    } else {
        char value[PROPERTY_VALUE_MAX];
        property_get("wifi.ap.channel", value, "0");
        channel = atoi(value);
    }
    if (channel == 0) {
        channel = AP_DEFAULT_CHANNEL;
        ALOGV("No valid wifi channel specified, using default");
    }

#ifdef HAVE_LEGACY_HOSTAPD
    asprintf(&wbuf, "interface=%s\nctrl_interface=" AP_SOCKET_PATH "\n"
#else
    asprintf(&wbuf, "interface=%s\ndriver=" HOSTAPD_DRIVER_NAME "\n"
                    "ctrl_interface=" AP_SOCKET_PATH "\n"
#endif
                    "ssid=%s\nchannel=%d\n", mIface, ssid, channel);

    ALOGV("%s", wbuf);

    if (argc > 4) {
        if (!strcmp(argv[4], "wpa-psk")) {
            generatePsk(ssid, argv[5], psk_str);
            asprintf(&fbuf, "%swpa=1\nwpa_pairwise=TKIP CCMP\nwpa_psk=%s\n", wbuf, psk_str);
        } else if (!strcmp(argv[4], "wpa2-psk")) {
            generatePsk(ssid, argv[5], psk_str);
            asprintf(&fbuf, "%swpa=2\nwpa_pairwise=CCMP\nwpa_psk=%s\n", wbuf, psk_str);
        } else if (!strcmp(argv[4], "open")) {
            asprintf(&fbuf, "%s", wbuf);
        } else {
            ALOGE("Invalid softap security type '%s'!\n", argv[4]);
	}
    } else {
        asprintf(&fbuf, "%s", wbuf);
    }

    fd = open(HOSTAPD_CONF_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0660);
    if (fd < 0) {
        ALOGE("Cannot update \"%s\": %s", HOSTAPD_CONF_FILE, strerror(errno));
        free(wbuf);
        free(fbuf);
        return -1;
    }
    if (write(fd, fbuf, strlen(fbuf)) < 0) {
        ALOGE("Cannot write to \"%s\": %s", HOSTAPD_CONF_FILE, strerror(errno));
        ret = -1;
    }
    close(fd);
    free(wbuf);
    free(fbuf);

    /* Note: apparently open can fail to set permissions correctly at times */
    if (chmod(HOSTAPD_CONF_FILE, 0660) < 0) {
        ALOGE("Error changing permissions of %s to 0660: %s",
                HOSTAPD_CONF_FILE, strerror(errno));
        unlink(HOSTAPD_CONF_FILE);
        return -1;
    }

    if (chown(HOSTAPD_CONF_FILE, AID_SYSTEM, AID_WIFI) < 0) {
        ALOGE("Error changing group ownership of %s to %d: %s",
                HOSTAPD_CONF_FILE, AID_WIFI, strerror(errno));
        unlink(HOSTAPD_CONF_FILE);
        return -1;
    }
    return ret;
}

void SoftapController::generatePsk(char *ssid, char *passphrase, char *psk_str) {
    unsigned char psk[SHA256_DIGEST_LENGTH];
    int j;
    // Use the PKCS#5 PBKDF2 with 4096 iterations
    PKCS5_PBKDF2_HMAC_SHA1(passphrase, strlen(passphrase),
            reinterpret_cast<const unsigned char *>(ssid), strlen(ssid),
            4096, SHA256_DIGEST_LENGTH, psk);
    for (j=0; j < SHA256_DIGEST_LENGTH; j++) {
        sprintf(&psk_str[j<<1], "%02x", psk[j]);
    }
    psk_str[j<<1] = '\0';
}


/*
 * Arguments:
 *	argv[2] - interface name
 *	argv[3] - AP or STA
 */
int SoftapController::fwReloadSoftap(int argc, char *argv[])
{
#ifdef WLAN_NO_FWRELOAD
    return 0;
#endif
    int ret, i = 0;
    char *iface;
    char *fwpath;

    if (mSock < 0) {
        ALOGE("Softap fwrealod - failed to open socket");
        return -1;
    }
    if (argc < 4) {
        ALOGE("Softap fwreload - missing arguments");
        return -1;
    }

    iface = argv[2];
    if (strcmp(argv[3], "AP") == 0) {
        fwpath = (char *)wifi_get_fw_path(WIFI_GET_FW_PATH_AP);
    } else if (strcmp(argv[3], "P2P") == 0) {
        fwpath = (char *)wifi_get_fw_path(WIFI_GET_FW_PATH_P2P);
    } else {
        fwpath = (char *)wifi_get_fw_path(WIFI_GET_FW_PATH_STA);
    }

    if (!fwpath) {
        ALOGD("Softap fwReload: no fwpath found");
	return -1;
    }
    ret = wifi_change_fw_path((const char *)fwpath);
    if (ret) {
        ALOGE("Softap fwReload - failed: %d", ret);
    } else {
        ALOGD("Softap fwReload - Ok");
    }
    return ret;
}

int SoftapController::clientsSoftap(char **retbuf)
{
    return 0;
}
