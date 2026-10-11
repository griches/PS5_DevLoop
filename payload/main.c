/*
 * PS5 DevLoop - resident input and screen-capture payload.
 * Copyright (C) 2026 Gary Riches
 * The command server's frame follows ps5vkctl (PS5_Vulkan), Copyright (C) 2026 Mihawk-99.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ps5vkctl (PS5_Vulkan) lets a run launch and close a title without a hand on
 * the console. What it cannot do is press a button or say what the screen
 * shows, so every run that needs either still needs a person. This payload is
 * the other half: it stays resident and answers one command per connection on
 * port 9112:
 *
 *   ping                      is the agent there, and what state the pad is in
 *   pad open [USERID]         create a virtual DualSense and give it to a user
 *   pad close                 remove it
 *   pad status                handle, device id, user, last insert result
 *   pad set B LX LY RX RY L2 R2   hold this state (B is a hex button mask)
 *   pad tap B MS              press B for MS milliseconds, then release
 *   pad handle N              bring-up: use N as the insert handle
 *   pad bind DEVICE [USERID]  bring-up: bind a device id to a user by hand
 *   pad evict DEVICE          disconnect another controller, so that the
 *                             virtual pad is its user's first, the one titles read
 *   shots                     the newest files under the console's captures
 *   latest                    send the newest capture: a header line, then bytes
 *   shot [HOLD_MS]            hold Create (the console's "take screenshot"
 *                             shortcut), wait for the new file, send it, and
 *                             delete it from the console's gallery
 *   forget PATH               delete a capture as the gallery does
 *   capture [TYPE]            bring-up: ask the screenshot service directly
 *                             (it never answers a payload on firmware 13.60)
 *   grab REQUEST FOLDER [MS]  ask a running title for its frame: create the
 *                             file REQUEST, which the title takes as "write a
 *                             picture into FOLDER", send the picture, delete it
 *   quit                      remove the pad and leave the console
 *
 * The pad is the system's own virtual device (libScePad's
 * scePadVirtualDevice*, the calls Remote Play goes through), so the home
 * screen and every title see it as a controller. The call order, the 120-byte
 * sample and the way the device id is recovered from the kernel log are what
 * SplashDown, Control4Free, OmniPad-PS5 and Ghostpad established on hardware:
 *
 *   - libSceMbus has no SDK stub and libScePad imports from it: dlopen it first;
 *   - scePadInit, then scePadSetProcessPrivilege(1), in that order;
 *   - AddDevice answers 0x803b0006 ("assignment pending") on a PS5 even when
 *     the device exists. Its id is only in the kernel log, on the login
 *     manager's DEVICE_ADDED line, subType 22;
 *   - sceMbusBindDeviceWithUserId gives the device to a user, which is what
 *     choosing a profile on the "Who's using this controller?" screen does.
 *
 * Which handle InsertData takes differs by firmware (the device id, its low
 * word, or a small index), so "pad open" tries each candidate with a neutral
 * sample and reports what every step returned. On firmware 13.60 it is the
 * device id (README.md, "What the console showed").
 *
 * The screen comes from the console's own screenshot, which is what a person
 * holding Create gets: the whole composited picture, system UI included, in
 * the format the console is set to (JXR when HDR is on). It costs the 1.2 s the
 * console wants Create held. The picture is then deleted through
 * libSceContentDelete, which removes it as the gallery's own Delete does.
 *
 * A title that can write its own frame is asked instead ("grab"): it is quicker
 * by an order of magnitude and leaves nothing behind, but shows only what the
 * title drew. The title's side is a file it looks for and removes
 * (AGENTS.md, "Giving a title grab").
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#define PS5LOOP_PORT 9112
#define PS5LOOP_LINE 1024
#define PS5LOOP_LOG "/data/ps5loop.log"
#define PS5LOOP_KLOG_PORT 3232

/* The virtual device type every known caller uses, on both consoles. */
#define PS5LOOP_DEVICE_TYPE 3
/* The user id that creates a device nobody owns yet. */
#define PS5LOOP_UNASSIGNED_USER 1
/* AddDevice's answer when the device exists and waits for a user. */
#define PS5LOOP_ASSIGNMENT_PENDING 0x803b0006u
/* The credentials the pad service and libSceMbus accept from a payload. */
#define PS5LOOP_AUTHID 0x4800000000010003ull
/* How often the held state is inserted: a real pad reports every 4 ms. */
#define PS5LOOP_INSERT_INTERVAL_US 8000
/* The Create button, and how long the console wants it held for a screenshot. */
#define PS5LOOP_BUTTON_CREATE 0x00000001u
#define PS5LOOP_BUTTON_L2 0x00000100u
#define PS5LOOP_BUTTON_R2 0x00000200u
#define PS5LOOP_CREATE_HOLD_MS 1200
#define PS5LOOP_SHOT_WAIT_MS 10000

/* Where the console keeps what Create captures. */
static const char *const capture_roots[] = {"/user/av_contents/photo", "/user/av_contents"};

/* One pad sample, the 120 bytes scePadRead fills and InsertData takes. */
typedef struct pad_touch
{
    uint16_t x;
    uint16_t y;
    uint8_t id;
    uint8_t reserved[3];
} pad_touch_t;

typedef struct pad_data
{
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t l2, r2;
    uint8_t reserved0[2];
    float orientation[4];
    float acceleration[3];
    float angular_velocity[3];
    uint8_t touch_count;
    uint8_t reserved1[7];
    pad_touch_t touch[2];
    uint8_t connected;
    uint8_t reserved2[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t reserved3[15];
} pad_data_t;
_Static_assert(sizeof(pad_data_t) == 120, "the pad sample is 120 bytes");

/* AddDevice's parameter block: its own size, the user, six unknown words. */
typedef struct add_device_param
{
    int32_t size;
    int32_t user_id;
    int32_t unknown[6];
} add_device_param_t;

/* libScePad and libSceUserService, through the payload SDK's stub libraries. */
extern int32_t scePadInit(void);
extern int32_t scePadSetProcessPrivilege(int32_t privilege);
extern int32_t scePadVirtualDeviceAddDevice(void *param, int32_t device_type);
extern int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *data);
extern int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
extern int sceUserServiceInitialize(void *param);
extern int sceUserServiceGetForegroundUser(int *user_id);

/* libSceMbus, resolved at run time. */
static int32_t (*mbus_bind)(uint64_t device_id, int32_t user_id);
static int32_t (*mbus_disconnect)(uint64_t device_id);

/* libSceScreenShot and libSceContentDelete, resolved at run time. Their
 * arguments are read from the libraries' own code (firmware 13.60):
 *   - sceScreenShotCapture asks the screenshot service for a picture of the
 *     screen and waits, 16 ms at a time, until the service names the file it
 *     saved, in a buffer of 0x400 bytes; a callback that returns non-zero ends
 *     the wait. Types 0 and 1 are accepted.
 *   - sceContentDeleteInitialize refuses any parameter block whose second
 *     word is not 0x4000; sceContentDeleteByPath then removes a capture as the
 *     gallery's own Delete does: the file, its thumbnail and its database row. */
typedef struct content_delete_param
{
    uint64_t unknown0;
    uint64_t buffer_size;
    uint64_t unknown[6];
} content_delete_param_t;
#define PS5LOOP_CAPTURE_PATH 0x400
#define PS5LOOP_CONTENT_DELETE_BUFFER 0x4000
static int32_t (*screenshot_capture)(int32_t type, char *path, int (*cancel)(void *), void *user);
static int32_t (*content_delete_by_path)(const char *path);
static bool g_capture_libraries;

/* The virtual pad. */
static struct
{
    pthread_mutex_t lock;
    bool libraries; /* libSceMbus loaded and the pad service initialised */
    bool open;
    int32_t handle;
    uint64_t device_id;
    int32_t user_id;
    int32_t last_insert;
    uint32_t inserts;
    pad_data_t state;
} g_pad = {.lock = PTHREAD_MUTEX_INITIALIZER, .handle = -1, .user_id = -1};

// Log one line to stdout (klog carries a payload's output) and to the log file
// the FTP server can hand back.
static void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    vprintf(format, arguments);
    printf("\n");
    fflush(stdout);
    FILE *const file = fopen(PS5LOOP_LOG, "a");
    if (file != NULL)
    {
        vfprintf(file, format, copy);
        fputc('\n', file);
        fclose(file);
    }
    va_end(copy);
    va_end(arguments);
}

// Answer one line, newline terminated.
static void reply(int client, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void reply(int client, const char *format, ...)
{
    char line[PS5LOOP_LINE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    log_line("ps5loop: -> %s", line);
    if (send(client, line, strlen(line), 0) < 0 || send(client, "\n", 1, 0) < 0)
        log_line("ps5loop: reply failed: %s", strerror(errno));
}

// Add to a report that a reply carries back, so one "pad open" tells the whole
// story of what each call returned.
static void note(char *report, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
static void note(char *report, size_t size, const char *format, ...)
{
    const size_t used = strlen(report);
    if (used + 2 >= size)
        return;
    if (used > 0)
        report[used] = ' ';
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(report + used + (used > 0), size - used - (used > 0), format, arguments);
    va_end(arguments);
}

static void elevate(char *report, size_t size);

static uint64_t monotonic_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

// A neutral sample: sticks centred, identity orientation, connected. The
// service discards a sample whose orientation is not a rotation.
static void pad_neutral(pad_data_t *data)
{
    memset(data, 0, sizeof(*data));
    data->lx = data->ly = data->rx = data->ry = 128;
    data->orientation[3] = 1.0f;
    data->acceleration[1] = 1.0f;
    data->connected = 1;
    data->connected_count = 1;
}

// One InsertData call, stamped as a real pad stamps its reports.
static int32_t pad_insert(int32_t handle, const pad_data_t *data)
{
    pad_data_t stamped = *data;
    stamped.timestamp = monotonic_us();
    return scePadVirtualDeviceInsertData(handle, &stamped);
}

// Keep the held state arriving, as a controller's reports do: a device that
// goes quiet reads as released, and then as gone.
static void *insert_thread(void *argument)
{
    (void)argument;
    for (;;)
    {
        pthread_mutex_lock(&g_pad.lock);
        if (g_pad.open)
        {
            g_pad.last_insert = pad_insert(g_pad.handle, &g_pad.state);
            g_pad.inserts++;
        }
        pthread_mutex_unlock(&g_pad.lock);
        usleep(PS5LOOP_INSERT_INTERVAL_US);
    }
    return NULL;
}

// Hold a state until the next one.
static void pad_hold(uint32_t buttons, int lx, int ly, int rx, int ry, int l2, int r2)
{
    pthread_mutex_lock(&g_pad.lock);
    pad_neutral(&g_pad.state);
    g_pad.state.buttons = buttons;
    g_pad.state.lx = (uint8_t)lx;
    g_pad.state.ly = (uint8_t)ly;
    g_pad.state.rx = (uint8_t)rx;
    g_pad.state.ry = (uint8_t)ry;
    // The console reads the triggers from their analog bytes: the bit alone
    // does not register.
    g_pad.state.l2 = (uint8_t)((buttons & PS5LOOP_BUTTON_L2) != 0 && l2 == 0 ? 255 : l2);
    g_pad.state.r2 = (uint8_t)((buttons & PS5LOOP_BUTTON_R2) != 0 && r2 == 0 ? 255 : r2);
    pthread_mutex_unlock(&g_pad.lock);
}

// The kernel log, for the one line that names a new device: the klog server
// when the console runs one (/dev/klog has a single reader, and the server is
// it), the device itself otherwise. -1 when neither opens.
static int klog_open(void)
{
    const int connection = socket(AF_INET, SOCK_STREAM, 0);
    if (connection >= 0)
    {
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(PS5LOOP_KLOG_PORT);
        if (connect(connection, (struct sockaddr *)&address, sizeof(address)) == 0)
            return connection;
        close(connection);
    }
    return open("/dev/klog", O_RDONLY | O_NONBLOCK);
}

// Read what the log has, waiting at most that long for more. 0 when it stayed
// quiet, -1 when the log closed.
static ssize_t klog_read(int log, char *buffer, size_t size, int wait_ms)
{
    struct pollfd wait = {.fd = log, .events = POLLIN};
    if (poll(&wait, 1, wait_ms) <= 0)
        return 0;
    const ssize_t bytes = read(log, buffer, size);
    if (bytes < 0 && (errno == EAGAIN || errno == EINTR))
        return 0;
    return bytes <= 0 ? -1 : bytes;
}

// The device id on a "device added" line, 0 for any other line. A line that
// names a subtype must name the virtual pad's (22): anything looser could
// return a real controller connected at the same moment.
static uint64_t klog_device_added(const char *line)
{
    if (strstr(line, "DEVICE_ADDED") == NULL)
        return 0;
    const char *subtype = strstr(line, "subType");
    if (subtype == NULL)
        subtype = strstr(line, "deviceType");
    if (subtype != NULL)
    {
        subtype += strcspn(subtype, ":=");
        if (strtoul(subtype + (*subtype != '\0'), NULL, 0) != 22)
            return 0;
    }
    const char *id = strstr(line, "DeviceId");
    if (id == NULL)
        id = strstr(line, "deviceId");
    if (id == NULL)
        return 0;
    id += strcspn(id, ":=");
    return strtoull(id + (*id != '\0'), NULL, 16);
}

// Follow the log for the new device's id, for at most that long.
static uint64_t klog_wait_device(int log, int wait_ms, char *report, size_t size)
{
    char line[1024];
    size_t used = 0;
    uint64_t device_id = 0;
    const uint64_t deadline = monotonic_us() + (uint64_t)wait_ms * 1000u;
    while (device_id == 0 && monotonic_us() < deadline)
    {
        char chunk[512];
        const ssize_t bytes = klog_read(log, chunk, sizeof(chunk), 100);
        if (bytes < 0)
            break;
        for (ssize_t index = 0; index < bytes && device_id == 0; index++)
        {
            if (chunk[index] != '\n' && used + 1 < sizeof(line))
            {
                line[used++] = chunk[index];
                continue;
            }
            line[used] = '\0';
            used = 0;
            if (strstr(line, "DEVICE_ADDED") != NULL)
                log_line("ps5loop: klog: %s", line);
            device_id = klog_device_added(line);
        }
    }
    if (device_id == 0)
        note(report, size, "klog=no-device-line");
    return device_id;
}

// Load libSceMbus, raise this process to the credentials the pad service
// accepts, and initialise the service. Once per process.
static int pad_libraries(char *report, size_t size)
{
    if (g_pad.libraries)
        return 0;
    elevate(report, size);

    // libScePad's imports from libSceMbus are unresolved in a payload: a pad
    // call before this load can end the process.
    void *mbus = dlopen("/system/common/lib/libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (mbus == NULL)
        mbus = dlopen("libSceMbus.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (mbus == NULL)
    {
        note(report, size, "libSceMbus=not-loaded");
        return -1;
    }
    mbus_bind = (int32_t (*)(uint64_t, int32_t))dlsym(mbus, "sceMbusBindDeviceWithUserId");
    mbus_disconnect = (int32_t (*)(uint64_t))dlsym(mbus, "sceMbusDisconnectDevice");
    note(report, size, "mbus_bind=%s", mbus_bind != NULL ? "found" : "missing");

    const int32_t init = scePadInit();
    const int32_t privilege = scePadSetProcessPrivilege(1);
    note(report, size, "padInit=0x%08x privilege=0x%08x", (uint32_t)init, (uint32_t)privilege);
    if (init != 0)
        return -1;
    g_pad.libraries = true;
    return 0;
}

// Find the handle InsertData accepts for a new device: whatever AddDevice
// returned or wrote back, then the device id and its low word.
static int32_t pad_find_handle(int32_t added, const add_device_param_t *param, uint64_t device_id,
                               char *report, size_t size)
{
    int32_t candidates[10];
    int count = 0;
    if (added > 0)
        candidates[count++] = added;
    for (int index = 0; index < 6; index++)
        if (param->unknown[index] > 0)
            candidates[count++] = param->unknown[index];
    if (device_id != 0)
    {
        candidates[count++] = (int32_t)(device_id & 0xffffffffu);
        if ((device_id >> 32) != 0)
            candidates[count++] = (int32_t)(device_id >> 32);
    }
    pad_data_t neutral;
    pad_neutral(&neutral);
    for (int index = 0; index < count; index++)
    {
        const int32_t result = pad_insert(candidates[index], &neutral);
        note(report, size, "insert(0x%x)=0x%08x", (uint32_t)candidates[index], (uint32_t)result);
        if (result == 0)
            return candidates[index];
    }
    return -1;
}

// Create the virtual pad and give it to a user. Returns 0 when samples reach
// it; the report says what every step returned either way.
static int pad_open(int32_t user_id, char *report, size_t size)
{
    if (g_pad.open)
    {
        note(report, size, "already open");
        return 0;
    }
    if (pad_libraries(report, size) != 0)
        return -1;
    if (user_id <= PS5LOOP_UNASSIGNED_USER && sceUserServiceGetForegroundUser(&user_id) != 0)
        user_id = -1;
    note(report, size, "user=0x%08x", (uint32_t)user_id);

    // The log replays what it holds to a new reader: let that pass, so the
    // device line that follows is this device's.
    const int log = klog_open();
    if (log < 0)
        note(report, size, "klog=unavailable");
    else
    {
        char drain[1024];
        while (klog_read(log, drain, sizeof(drain), 300) > 0)
            continue;
    }

    // Created for user 1 the device arrives with no owner, and is then bound.
    // Created for the real user, AddDevice answers 0x803b0001 and makes nothing
    // (firmware 13.60).
    add_device_param_t param;
    memset(&param, 0, sizeof(param));
    param.size = (int32_t)sizeof(param);
    param.user_id = PS5LOOP_UNASSIGNED_USER;
    const int32_t added = scePadVirtualDeviceAddDevice(&param, PS5LOOP_DEVICE_TYPE);
    note(report, size, "add=0x%08x", (uint32_t)added);

    uint64_t device_id = 0;
    if (log >= 0)
    {
        device_id = klog_wait_device(log, 3000, report, size);
        close(log);
    }
    note(report, size, "device=0x%llx", (unsigned long long)device_id);
    if (added != 0 && (uint32_t)added != PS5LOOP_ASSIGNMENT_PENDING && device_id == 0)
        return -1;

    // Without this the console shows "Who's using this controller?" and waits
    // for a person: binding is what choosing a profile there does.
    if (device_id != 0 && user_id > PS5LOOP_UNASSIGNED_USER && mbus_bind != NULL)
        note(report, size, "bind=0x%08x", (uint32_t)mbus_bind(device_id, user_id));
    else
        note(report, size, "bind=skipped");

    const int32_t handle = pad_find_handle(added, &param, device_id, report, size);
    if (handle < 0)
    {
        g_pad.device_id = device_id;
        return -1;
    }
    pthread_mutex_lock(&g_pad.lock);
    pad_neutral(&g_pad.state);
    g_pad.handle = handle;
    g_pad.device_id = device_id;
    g_pad.user_id = user_id;
    g_pad.inserts = 0;
    g_pad.open = true;
    pthread_mutex_unlock(&g_pad.lock);
    return 0;
}

static void pad_close(char *report, size_t size)
{
    pthread_mutex_lock(&g_pad.lock);
    const bool was_open = g_pad.open;
    const int32_t handle = g_pad.handle;
    g_pad.open = false;
    g_pad.handle = -1;
    pthread_mutex_unlock(&g_pad.lock);
    if (!was_open)
    {
        note(report, size, "not open");
        return;
    }
    pad_data_t neutral;
    pad_neutral(&neutral);
    (void)pad_insert(handle, &neutral);
    note(report, size, "delete=0x%08x", (uint32_t)scePadVirtualDeviceDeleteDevice(handle));
}

// Press and release, timed here and not across the network.
static void pad_tap(uint32_t buttons, int hold_ms)
{
    pad_hold(buttons, 128, 128, 128, 128, 0, 0);
    usleep((useconds_t)hold_ms * 1000u);
    pad_hold(0, 128, 128, 128, 128, 0, 0);
    usleep(50000);
}

/* The newest captures found by one walk of the capture folders. */
#define PS5LOOP_NEWEST 5
typedef struct capture
{
    char path[512];
    time_t modified;
    off_t size;
} capture_t;

static bool is_picture(const char *name)
{
    const char *const extension = strrchr(name, '.');
    return extension != NULL &&
           (strcasecmp(extension, ".jxr") == 0 || strcasecmp(extension, ".png") == 0 ||
            strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0);
}

// Walk a folder for pictures, keeping the newest, newest first.
static void captures_walk(const char *folder, int depth, capture_t *newest)
{
    DIR *const directory = opendir(folder);
    if (directory == NULL)
        return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;
        capture_t found;
        snprintf(found.path, sizeof(found.path), "%s/%s", folder, entry->d_name);
        struct stat status;
        if (stat(found.path, &status) != 0)
            continue;
        if (S_ISDIR(status.st_mode))
        {
            if (depth < 6)
                captures_walk(found.path, depth + 1, newest);
            continue;
        }
        if (!S_ISREG(status.st_mode) || !is_picture(entry->d_name))
            continue;
        found.modified = status.st_mtime;
        found.size = status.st_size;
        for (int index = 0; index < PS5LOOP_NEWEST; index++)
        {
            if (found.modified <= newest[index].modified)
                continue;
            memmove(&newest[index + 1], &newest[index],
                    sizeof(capture_t) * (size_t)(PS5LOOP_NEWEST - 1 - index));
            newest[index] = found;
            break;
        }
    }
    closedir(directory);
}

static void send_file(int client, const capture_t *capture);

// Ask a running title for its frame. The request is a file the title removes
// when it has seen it; the answer is the next new picture in the folder, which
// is sent and then deleted, so that nothing of a run stays in the title's folder.
static void grab(int client, const char *request, const char *folder, int wait_ms)
{
    capture_t before[PS5LOOP_NEWEST];
    memset(before, 0, sizeof(before));
    captures_walk(folder, 6, before);
    const int file = open(request, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (file < 0)
    {
        reply(client, "err grab %s: %s", request, strerror(errno));
        return;
    }
    close(file);
    (void)chmod(request, 0666);
    off_t last_size = -1;
    const uint64_t deadline = monotonic_us() + (uint64_t)wait_ms * 1000u;
    while (monotonic_us() < deadline)
    {
        usleep(15000);
        capture_t after[PS5LOOP_NEWEST];
        memset(after, 0, sizeof(after));
        captures_walk(folder, 6, after);
        if (after[0].path[0] == '\0' || strcmp(after[0].path, before[0].path) == 0)
            continue;
        if (after[0].size > 0 && after[0].size == last_size)
        {
            send_file(client, &after[0]);
            (void)unlink(after[0].path);
            return;
        }
        last_size = after[0].size;
    }
    // A request nobody took must not wait for the next launch by hand.
    const bool taken = unlink(request) != 0;
    reply(client, "err grab no picture in %s within %d ms (the title %s the request)", folder,
          wait_ms, taken ? "took" : "never took");
}

static void captures_newest(capture_t *newest)
{
    memset(newest, 0, sizeof(capture_t) * PS5LOOP_NEWEST);
    for (size_t root = 0; root < sizeof(capture_roots) / sizeof(capture_roots[0]); root++)
    {
        captures_walk(capture_roots[root], 0, newest);
        if (newest[0].path[0] != '\0')
            return;
    }
}

// Send a file: a header line with its size, then its bytes.
static void send_file(int client, const capture_t *capture)
{
    const int file = open(capture->path, O_RDONLY);
    if (file < 0)
    {
        reply(client, "err open %s: %s", capture->path, strerror(errno));
        return;
    }
    reply(client, "ok file size=%lld mtime=%lld path=%s", (long long)capture->size,
          (long long)capture->modified, capture->path);
    char buffer[65536];
    ssize_t bytes;
    while ((bytes = read(file, buffer, sizeof(buffer))) > 0)
    {
        for (ssize_t sent = 0; sent < bytes;)
        {
            const ssize_t step = send(client, buffer + sent, (size_t)(bytes - sent), 0);
            if (step <= 0)
            {
                close(file);
                return;
            }
            sent += step;
        }
    }
    close(file);
}

// The credentials the system's services accept from a payload.
static void elevate(char *report, size_t size)
{
    static const uint8_t all_caps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const int authid = kernel_set_ucred_authid(getpid(), PS5LOOP_AUTHID);
    const int caps = kernel_set_ucred_caps(getpid(), all_caps);
    note(report, size, "authid=%d caps=%d", authid, caps);
}

// Load the screenshot and content-delete libraries. Once per process.
static void capture_libraries(char *report, size_t size)
{
    if (g_capture_libraries)
        return;
    g_capture_libraries = true;
    elevate(report, size);
    void *const screenshot = dlopen("/system/common/lib/libSceScreenShot.sprx", RTLD_NOW);
    if (screenshot != NULL)
        screenshot_capture = (int32_t (*)(int32_t, char *, int (*)(void *), void *))dlsym(
            screenshot, "sceScreenShotCapture");
    void *const deleter = dlopen("/system/common/lib/libSceContentDelete.sprx", RTLD_NOW);
    if (deleter != NULL)
    {
        int32_t (*const initialize)(const content_delete_param_t *) =
            (int32_t (*)(const content_delete_param_t *))dlsym(deleter,
                                                               "sceContentDeleteInitialize");
        content_delete_param_t param;
        memset(&param, 0, sizeof(param));
        param.buffer_size = PS5LOOP_CONTENT_DELETE_BUFFER;
        const int32_t initialized = initialize != NULL ? initialize(&param) : -1;
        note(report, size, "deleteInit=0x%08x", (uint32_t)initialized);
        if (initialized == 0)
            content_delete_by_path =
                (int32_t (*)(const char *))dlsym(deleter, "sceContentDeleteByPath");
    }
    note(report, size, "capture=%s delete=%s", screenshot_capture != NULL ? "found" : "missing",
         content_delete_by_path != NULL ? "found" : "missing");
    log_line("ps5loop: capture libraries: %s", report);
}

// Ends a capture the service never finishes.
static int capture_expired(void *deadline)
{
    return monotonic_us() > *(const uint64_t *)deadline;
}

// Remove a capture the way the gallery does, so that a run leaves no picture,
// thumbnail or database row behind. The service knows captures by their path
// under /user, with or without that prefix: try both.
static int32_t capture_delete(const char *path)
{
    if (content_delete_by_path == NULL)
        return -1;
    int32_t result = content_delete_by_path(path);
    if (result != 0 && strncmp(path, "/user/", 6) == 0)
        result = content_delete_by_path(path + 5);
    log_line("ps5loop: delete %s -> 0x%08x", path, (uint32_t)result);
    return result;
}

// Take the screen from the screenshot service, send the picture and delete it
// from the console. Returns false, having sent nothing, when the service gave
// no picture.
static bool direct_shot(int client, int type, int wait_ms, char *report, size_t size)
{
    capture_libraries(report, size);
    if (screenshot_capture == NULL)
        return false;
    char named[PS5LOOP_CAPTURE_PATH] = {0};
    uint64_t deadline = monotonic_us() + (uint64_t)wait_ms * 1000u;
    const int32_t result = screenshot_capture(type, named, capture_expired, &deadline);
    named[sizeof(named) - 1] = '\0';
    note(report, size, "capture=0x%08x named=%s", (uint32_t)result, named);
    log_line("ps5loop: sceScreenShotCapture -> 0x%08x %s", (uint32_t)result, named);
    if (result != 0 || named[0] != '/')
        return false;
    // The service names the file as the media database does, under /user.
    char path[PS5LOOP_CAPTURE_PATH + 8];
    struct stat status;
    snprintf(path, sizeof(path), "%s", named);
    if (stat(path, &status) != 0)
    {
        snprintf(path, sizeof(path), "/user%s", named);
        if (stat(path, &status) != 0)
        {
            note(report, size, "file=missing");
            return false;
        }
    }
    char *const bytes = malloc((size_t)status.st_size);
    const int file = open(path, O_RDONLY);
    off_t have = 0;
    while (bytes != NULL && file >= 0 && have < status.st_size)
    {
        const ssize_t step = read(file, bytes + have, (size_t)(status.st_size - have));
        if (step <= 0)
            break;
        have += step;
    }
    if (file >= 0)
        close(file);
    if (have != status.st_size)
    {
        free(bytes);
        note(report, size, "file=unreadable");
        return false;
    }
    const int32_t deleted = capture_delete(path);
    reply(client, "ok file size=%lld mtime=%lld deleted=0x%08x path=%s", (long long)have,
          (long long)status.st_mtime, (uint32_t)deleted, path);
    for (off_t sent = 0; sent < have;)
    {
        const ssize_t step = send(client, bytes + sent, (size_t)(have - sent), 0);
        if (step <= 0)
            break;
        sent += step;
    }
    free(bytes);
    return true;
}

// Take the console's own screenshot: hold Create as a person would, wait for a
// picture newer than the press whose size has stopped growing, send it, and
// delete it from the console. (Asking the screenshot service instead, "capture",
// never answers a payload.)
static void take_shot(int client, int hold_ms)
{
    char report[PS5LOOP_LINE - 128] = {0};
    if (!g_pad.open)
    {
        reply(client, "err shot needs the pad: pad open first (latest sends the newest capture)");
        return;
    }
    capture_libraries(report, sizeof(report));
    capture_t before[PS5LOOP_NEWEST];
    captures_newest(before);
    pad_tap(PS5LOOP_BUTTON_CREATE, hold_ms);
    off_t last_size = -1;
    const uint64_t deadline = monotonic_us() + PS5LOOP_SHOT_WAIT_MS * 1000ull;
    while (monotonic_us() < deadline)
    {
        usleep(300000);
        capture_t after[PS5LOOP_NEWEST];
        captures_newest(after);
        if (after[0].path[0] == '\0' || strcmp(after[0].path, before[0].path) == 0)
            continue;
        if (after[0].size > 0 && after[0].size == last_size)
        {
            send_file(client, &after[0]);
            (void)capture_delete(after[0].path);
            return;
        }
        last_size = after[0].size;
    }
    reply(client, "err shot no new capture within %d ms (newest is still %s)", PS5LOOP_SHOT_WAIT_MS,
          before[0].path[0] != '\0' ? before[0].path : "none");
}

// Answer one command. Returns 0 to keep serving, -1 to leave the console.
static int handle_command(int client, char *line)
{
    char report[PS5LOOP_LINE - 64] = {0};
    char *const command = strtok(line, " \t");
    if (command == NULL)
    {
        reply(client, "err empty");
        return 0;
    }
    if (strcmp(command, "ping") == 0)
    {
        reply(client, "ok ps5loop 1 pid=%d fw=0x%08x pad=%s", (int)getpid(),
              kernel_get_fw_version(), g_pad.open ? "open" : "closed");
        return 0;
    }
    if (strcmp(command, "quit") == 0)
    {
        pad_close(report, sizeof(report));
        reply(client, "ok bye %s", report);
        return -1;
    }
    if (strcmp(command, "shots") == 0)
    {
        capture_t newest[PS5LOOP_NEWEST];
        captures_newest(newest);
        for (int index = 0; index < PS5LOOP_NEWEST && newest[index].path[0] != '\0'; index++)
            note(report, sizeof(report), "%s(%lld)", newest[index].path,
                 (long long)newest[index].size);
        reply(client, "ok shots %s", report[0] != '\0' ? report : "none");
        return 0;
    }
    if (strcmp(command, "latest") == 0)
    {
        capture_t newest[PS5LOOP_NEWEST];
        captures_newest(newest);
        if (newest[0].path[0] == '\0')
            reply(client, "err latest no captures under %s", capture_roots[0]);
        else
            send_file(client, &newest[0]);
        return 0;
    }
    if (strcmp(command, "shot") == 0)
    {
        const char *const hold = strtok(NULL, " \t");
        const int hold_ms = hold != NULL ? atoi(hold) : PS5LOOP_CREATE_HOLD_MS;
        take_shot(client, hold_ms > 0 && hold_ms <= 5000 ? hold_ms : PS5LOOP_CREATE_HOLD_MS);
        return 0;
    }
    if (strcmp(command, "forget") == 0)
    {
        // Delete a capture as the gallery does: file, thumbnail, database row.
        const char *const path = strtok(NULL, " \t");
        capture_libraries(report, sizeof(report));
        if (path == NULL || strncmp(path, "/user/av_contents/", 18) != 0 || strstr(path, "..") != NULL)
            reply(client, "err usage: forget /user/av_contents/...");
        else
            reply(client, "ok forget result=0x%08x %s", (uint32_t)capture_delete(path), path);
        return 0;
    }
    if (strcmp(command, "capture") == 0)
    {
        // bring-up: the screenshot service alone, with its type argument
        const char *const type = strtok(NULL, " \t");
        if (!direct_shot(client, type != NULL ? atoi(type) : 0, 5000, report, sizeof(report)))
            reply(client, "err capture %s", report);
        return 0;
    }
    if (strcmp(command, "grab") == 0)
    {
        const char *const request = strtok(NULL, " \t");
        const char *const folder = strtok(NULL, " \t");
        const char *const wait = strtok(NULL, " \t");
        const int wait_ms = wait != NULL ? atoi(wait) : 3000;
        if (request == NULL || folder == NULL || request[0] != '/' || folder[0] != '/')
            reply(client, "err usage: grab /REQUEST/FILE /PICTURE/FOLDER [MS]");
        else
            grab(client, request, folder, wait_ms > 0 && wait_ms <= 30000 ? wait_ms : 3000);
        return 0;
    }
    if (strcmp(command, "pad") != 0)
    {
        reply(client, "err unknown %s", command);
        return 0;
    }

    const char *const action = strtok(NULL, " \t");
    const char *arguments[7] = {0};
    int count = 0;
    while (count < 7 && (arguments[count] = strtok(NULL, " \t")) != NULL)
        count++;
    if (action == NULL)
    {
        reply(client, "err usage: pad open|close|status|set|tap|handle|bind");
        return 0;
    }
    if (strcmp(action, "open") == 0)
    {
        const int32_t user = count > 0 ? (int32_t)strtoul(arguments[0], NULL, 0) : -1;
        const int status = pad_open(user, report, sizeof(report));
        reply(client, "%s pad %s%s", status == 0 ? "ok" : "err", status == 0 ? "open " : "", report);
        return 0;
    }
    if (strcmp(action, "close") == 0)
    {
        pad_close(report, sizeof(report));
        reply(client, "ok pad closed %s", report);
        return 0;
    }
    if (strcmp(action, "status") == 0)
    {
        reply(client, "ok pad %s handle=0x%x device=0x%llx user=0x%08x inserts=%u last=0x%08x",
              g_pad.open ? "open" : "closed", (uint32_t)g_pad.handle,
              (unsigned long long)g_pad.device_id, (uint32_t)g_pad.user_id, g_pad.inserts,
              (uint32_t)g_pad.last_insert);
        return 0;
    }
    if (strcmp(action, "bind") == 0 && count >= 1)
    {
        if (pad_libraries(report, sizeof(report)) != 0 || mbus_bind == NULL)
        {
            reply(client, "err pad bind %s", report);
            return 0;
        }
        const uint64_t device_id = strtoull(arguments[0], NULL, 16);
        int user = count > 1 ? (int)strtoul(arguments[1], NULL, 0) : -1;
        if (user <= 0)
            (void)sceUserServiceGetForegroundUser(&user);
        reply(client, "ok pad bind device=0x%llx user=0x%08x result=0x%08x",
              (unsigned long long)device_id, (uint32_t)user, (uint32_t)mbus_bind(device_id, user));
        return 0;
    }
    if (strcmp(action, "evict") == 0 && count >= 1)
    {
        // A title reads its user's first controller. While a real one holds
        // that place the virtual pad is the second, which no title opens:
        // disconnecting the real one (PS on it brings it back) leaves ours first.
        const uint64_t device_id = strtoull(arguments[0], NULL, 16);
        if (pad_libraries(report, sizeof(report)) != 0 || mbus_disconnect == NULL)
            reply(client, "err pad evict %s", report);
        else if (device_id == 0 || device_id == g_pad.device_id)
            reply(client, "err pad evict: name another device's id");
        else
            reply(client, "ok pad evict device=0x%llx result=0x%08x",
                  (unsigned long long)device_id, (uint32_t)mbus_disconnect(device_id));
        return 0;
    }
    if (strcmp(action, "handle") == 0 && count >= 1)
    {
        if (pad_libraries(report, sizeof(report)) != 0)
        {
            reply(client, "err pad handle %s", report);
            return 0;
        }
        pthread_mutex_lock(&g_pad.lock);
        pad_neutral(&g_pad.state);
        g_pad.handle = (int32_t)strtoul(arguments[0], NULL, 0);
        g_pad.open = true;
        const int32_t result = pad_insert(g_pad.handle, &g_pad.state);
        pthread_mutex_unlock(&g_pad.lock);
        reply(client, "ok pad handle=0x%x insert=0x%08x", (uint32_t)g_pad.handle, (uint32_t)result);
        return 0;
    }
    if (!g_pad.open)
    {
        reply(client, "err pad %s: the pad is not open", action);
        return 0;
    }
    if (strcmp(action, "set") == 0 && count == 7)
    {
        int axes[6];
        for (int index = 0; index < 6; index++)
        {
            axes[index] = atoi(arguments[index + 1]);
            if (axes[index] < 0 || axes[index] > 255)
            {
                reply(client, "err pad set: sticks and triggers are 0 to 255");
                return 0;
            }
        }
        pad_hold((uint32_t)strtoul(arguments[0], NULL, 16), axes[0], axes[1], axes[2], axes[3],
                 axes[4], axes[5]);
        reply(client, "ok pad set");
        return 0;
    }
    if (strcmp(action, "tap") == 0 && count == 2)
    {
        const int hold_ms = atoi(arguments[1]);
        if (hold_ms <= 0 || hold_ms > 10000)
        {
            reply(client, "err pad tap: hold 1 to 10000 ms");
            return 0;
        }
        pad_tap((uint32_t)strtoul(arguments[0], NULL, 16), hold_ms);
        reply(client, "ok pad tap last=0x%08x", (uint32_t)g_pad.last_insert);
        return 0;
    }
    reply(client, "err usage: pad %s", action);
    return 0;
}

// Read one command line and answer it. Returns 0 to keep serving, -1 to leave.
static int serve_client(int client)
{
    char line[256] = {0};
    size_t used = 0;
    while (used + 1 < sizeof(line))
    {
        const ssize_t bytes = recv(client, line + used, sizeof(line) - 1 - used, 0);
        if (bytes <= 0)
            break;
        used += (size_t)bytes;
        line[used] = '\0';
        if (strchr(line, '\n') != NULL)
            break;
    }
    while (used > 0 && (line[used - 1] == '\n' || line[used - 1] == '\r'))
        line[--used] = '\0';
    if (used == 0)
        return 0;
    log_line("ps5loop: command %s", line);
    return handle_command(client, line);
}

int main(int argc, char *argv[])
{
    unsigned port = PS5LOOP_PORT;
    if (argc > 1)
    {
        const unsigned long requested = strtoul(argv[1], NULL, 10);
        if (requested > 0 && requested < 65536)
            port = (unsigned)requested;
    }
    (void)signal(SIGPIPE, SIG_IGN);
    const int user_init = sceUserServiceInitialize(NULL);
    log_line("ps5loop: starting, port %u, pid %d, user service 0x%08x", port, (int)getpid(),
             (uint32_t)user_init);

    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
    {
        log_line("ps5loop: socket: %s", strerror(errno));
        return 1;
    }
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(server, 4) != 0)
    {
        // A second copy finds the port taken and leaves the first one alone.
        log_line("ps5loop: bind or listen failed: %s", strerror(errno));
        close(server);
        return 1;
    }
    pthread_t inserter;
    if (pthread_create(&inserter, NULL, insert_thread, NULL) != 0)
    {
        log_line("ps5loop: no insert thread: %s", strerror(errno));
        close(server);
        return 1;
    }
    log_line("ps5loop: listening");
    for (;;)
    {
        const int client = accept(server, NULL, NULL);
        if (client < 0)
        {
            if (errno == EINTR)
                continue;
            log_line("ps5loop: accept failed: %s", strerror(errno));
            break;
        }
        const int leaving = serve_client(client);
        close(client);
        if (leaving != 0)
        {
            log_line("ps5loop: leaving");
            break;
        }
    }
    close(server);
    return 0;
}
