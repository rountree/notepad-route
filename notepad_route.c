// notepad_route.c — USB capture routing for Soundcraft Notepad mixers on macOS.
// Protocol per socratools/socradoc + socranop. No dependencies beyond macOS frameworks.
//
//   clang -O2 -Wall -o notepad-route notepad_route.c -framework IOKit -framework CoreFoundation
//
//   notepad-route <0..3>   set routing now and remember it
//                          8FX: 0=MIC 1+2  1=LINE 3/4  2=LINE 5/6  3=MASTER L+R (power-on default)
//   notepad-route --watch  run forever; whenever a Notepad appears on the bus, re-apply the
//                          remembered routing (this is what the launchd agent runs)
//   notepad-route --status print the remembered routing
//
// The mixer resets to MASTER L+R whenever its 18V supply is removed, so the remembered
// value lives in ~/Library/Application Support/notepad-route/routing and the watcher
// re-sends it on every (re)appearance.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HARMAN_VID 0x05fc
#define CMD_AUDIO_ROUTING 0x04

#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 120000
#define IOKIT_MAIN_PORT kIOMainPortDefault
#else
#define IOKIT_MAIN_PORT kIOMasterPortDefault
#endif

static const char *kSources8FX[] = {"MIC 1+2", "LINE 3/4", "LINE 5/6", "MASTER L+R"};

/* ---------- logging ---------- */

static void logmsg(const char *fmt, ...) {
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(stderr, "%s notepad-route: ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---------- remembered routing ---------- */

static int state_dir(char *buf, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !*home) return -1;
    snprintf(buf, n, "%s/Library/Application Support/notepad-route", home);
    return 0;
}

static int read_state(void) {
    char path[1024];
    if (state_dir(path, sizeof path) < 0) return -1;
    strlcat(path, "/routing", sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int idx = -1;
    if (fscanf(f, "%d", &idx) != 1 || idx < 0 || idx > 3) idx = -1;
    fclose(f);
    return idx;
}

static int write_state(int idx) {
    char dir[1024], path[1024];
    if (state_dir(dir, sizeof dir) < 0) return -1;
    mkdir(dir, 0755); // ignore EEXIST
    snprintf(path, sizeof path, "%s/routing", dir);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", idx);
    fclose(f);
    return 0;
}

/* ---------- IOKit ---------- */

static int vendor_of(io_service_t svc) {
    int vid = -1;
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, CFSTR(kUSBVendorID), kCFAllocatorDefault, 0);
    if (v) {
        if (CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberIntType, &vid);
        CFRelease(v);
    }
    return vid;
}

// Enumerate all USB devices (IOUSBHostDevice since macOS 10.12) and return the first
// Harman one. A bare idVendor key in the matching dictionary is not a legal USB match
// pattern, so we filter by property instead, as libusb does. Caller releases.
static io_service_t find_notepad(void) {
    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    if (!match) return IO_OBJECT_NULL;
    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(IOKIT_MAIN_PORT, match, &iter) != KERN_SUCCESS) return IO_OBJECT_NULL;
    io_service_t svc, found = IO_OBJECT_NULL;
    while ((svc = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        if (vendor_of(svc) == HARMAN_VID) { found = svc; break; }
        IOObjectRelease(svc);
    }
    IOObjectRelease(iter);
    return found;
}

// The whole "driver": one 8-byte vendor control OUT request to endpoint 0.
// Byte layout (verified against socranop): [2]=command 0x04, [4]=source index 0..3.
static kern_return_t send_routing(io_service_t svc, int idx, UInt16 *pid_out) {
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                                                         kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr != KERN_SUCCESS || !plugin) return kr ? kr : kIOReturnError;

    IOUSBDeviceInterface **dev = NULL;
    HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
    (*plugin)->Release(plugin);
    if (hr || !dev) return kIOReturnNotFound;

    if (pid_out) (*dev)->GetDeviceProduct(dev, pid_out);

    UInt8 data[8] = {0x00, 0x00, CMD_AUDIO_ROUTING, 0x00, (UInt8)idx, 0x00, 0x00, 0x00};
    IOUSBDevRequest req = {0};
    req.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBVendor, kUSBDevice); // 0x40
    req.bRequest      = 16;
    req.wValue        = 0;
    req.wIndex        = 0;
    req.wLength       = sizeof data;
    req.pData         = data;

    kr = (*dev)->DeviceRequest(dev, &req);
    (*dev)->Release(dev);
    return kr;
}

/* ---------- one-shot mode ---------- */

static int do_set(int idx) {
    if (write_state(idx) < 0) logmsg("warning: could not save routing to Application Support");

    io_service_t svc = find_notepad();
    if (!svc) {
        logmsg("no Notepad on the bus; saved %d (%s) for when it appears", idx, kSources8FX[idx]);
        return 1;
    }
    UInt16 pid = 0;
    kern_return_t kr = send_routing(svc, idx, &pid);
    IOObjectRelease(svc);
    if (kr != kIOReturnSuccess) {
        logmsg("DeviceRequest failed: 0x%x (pid 0x%04x)", kr, pid);
        return 1;
    }
    printf("pid 0x%04x: USB capture <- %s (remembered)\n", pid, kSources8FX[idx]);
    return 0;
}

/* ---------- watch mode ---------- */

static void device_appeared(void *refcon, io_iterator_t iter) {
    (void)refcon;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        if (vendor_of(svc) == HARMAN_VID) {
            int idx = read_state();
            if (idx < 0) {
                logmsg("Notepad appeared but no routing is remembered; run `notepad-route <0..3>` once");
            } else {
                // Give the device a moment to finish configuration before poking endpoint 0.
                usleep(750 * 1000);
                kern_return_t kr = kIOReturnError;
                UInt16 pid = 0;
                for (int attempt = 0; attempt < 3; attempt++) {
                    kr = send_routing(svc, idx, &pid);
                    if (kr == kIOReturnSuccess) break;
                    usleep(500 * 1000);
                }
                if (kr == kIOReturnSuccess)
                    logmsg("Notepad (pid 0x%04x) appeared; USB capture <- %s", pid, kSources8FX[idx]);
                else
                    logmsg("Notepad appeared but DeviceRequest failed: 0x%x", kr);
            }
        }
        IOObjectRelease(svc);
    }
}

static int do_watch(void) {
    IONotificationPortRef port = IONotificationPortCreate(IOKIT_MAIN_PORT);
    if (!port) { logmsg("IONotificationPortCreate failed"); return 1; }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);

    CFMutableDictionaryRef match = IOServiceMatching("IOUSBHostDevice");
    io_iterator_t iter = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, match,
                                                        device_appeared, NULL, &iter); // consumes match
    if (kr != KERN_SUCCESS) { logmsg("IOServiceAddMatchingNotification: 0x%x", kr); return 1; }

    logmsg("watching for Soundcraft Notepad (vid 0x%04x)", HARMAN_VID);
    // Draining the iterator arms the notification; it also delivers any device already present.
    device_appeared(NULL, iter);

    CFRunLoopRun(); // never returns; launchd stops us with SIGTERM
    return 0;
}

/* ---------- main ---------- */

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s <0..3> | --watch | --status\n"
            "  0  MIC 1+2      1  LINE 3/4      2  LINE 5/6      3  MASTER L+R\n", argv0);
}

int main(int argc, char **argv) {
    if (argc != 2) { usage(argv[0]); return 2; }

    if (strcmp(argv[1], "--watch") == 0) return do_watch();

    if (strcmp(argv[1], "--status") == 0) {
        int idx = read_state();
        if (idx < 0) { printf("no routing remembered\n"); return 1; }
        printf("remembered: %d (%s)\n", idx, kSources8FX[idx]);
        return 0;
    }

    char *end = NULL;
    long idx = strtol(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || idx < 0 || idx > 3) { usage(argv[0]); return 2; }
    return do_set((int)idx);
}
