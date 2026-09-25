#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pwd.h>
#include <sys/stat.h>
#include <getopt.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#define VERSION "1.0.0"
#define SERVICE_LABEL "com.stevenrobinson.midi-router"

typedef struct {
    char sourcePattern[128];
    char destPattern[128];
    bool bidirectional;
    int verbose; // 0 = off, 1 = notes/cc/transport, 2 = all including clock
} RouterConfig;

static RouterConfig gConfig = {
    .sourcePattern = "Digitakt",
    .destPattern = "Digitone",
    .bidirectional = false,
    .verbose = 0
};

static CFRunLoopRef gRunLoop = NULL;
static MIDIClientRef gClient = 0;
static MIDIPortRef gInPortForward = 0;
static MIDIPortRef gOutPortForward = 0;
static MIDIPortRef gInPortReverse = 0;
static MIDIPortRef gOutPortReverse = 0;

static MIDIEndpointRef gSrcEndpoint = 0;
static MIDIEndpointRef gDstEndpoint = 0;
static bool gConnected = false;

static const char *NOTE_NAMES[] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static void printTimestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_info);
    printf("[%s.%03ld] ", buf, ts.tv_nsec / 1000000);
}

static void getEndpointName(MIDIEndpointRef endpoint, char *buf, size_t maxLen) {
    buf[0] = '\0';
    if (!endpoint) return;
    CFStringRef name = NULL;
    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) == noErr && name) {
        CFStringGetCString(name, buf, (CFIndex)maxLen, kCFStringEncodingUTF8);
        CFRelease(name);
    } else if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &name) == noErr && name) {
        CFStringGetCString(name, buf, (CFIndex)maxLen, kCFStringEncodingUTF8);
        CFRelease(name);
    }
}

static bool isEndpointOnline(MIDIEndpointRef endpoint) {
    if (!endpoint) return false;
    SInt32 offline = 0;
    if (MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyOffline, &offline) == noErr) {
        return (offline == 0);
    }
    return true;
}

static MIDIEndpointRef findEndpoint(bool isSource, const char *pattern, char *outName, size_t maxLen) {
    ItemCount count = isSource ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < count; i++) {
        MIDIEndpointRef ep = isSource ? MIDIGetSource(i) : MIDIGetDestination(i);
        if (!ep || !isEndpointOnline(ep)) continue;
        char name[256];
        getEndpointName(ep, name, sizeof(name));
        if (strcasestr(name, pattern) != NULL) {
            if (outName && maxLen > 0) {
                strncpy(outName, name, maxLen - 1);
                outName[maxLen - 1] = '\0';
            }
            return ep;
        }
    }
    return 0;
}

static void logMidiMessage(const char *tag, const UInt8 *data, UInt16 length) {
    if (length == 0 || gConfig.verbose == 0) return;

    UInt8 status = data[0];
    if (status == 0xF8) {
        // Timing clock
        if (gConfig.verbose >= 2) {
            printTimestamp();
            printf("%s [Clock]\n", tag);
        }
        return;
    }
    if (status == 0xFE) return; // Active sensing

    printTimestamp();
    printf("%s ", tag);

    if (status >= 0x80 && status <= 0xEF) {
        UInt8 type = status & 0xF0;
        UInt8 ch = (status & 0x0F) + 1;
        UInt8 d1 = (length > 1) ? data[1] : 0;
        UInt8 d2 = (length > 2) ? data[2] : 0;

        switch (type) {
            case 0x80: {
                int oct = ((int)d1 / 12) - 1;
                printf("[CH %2d] Note Off: %3d (%-2s%d), Vel: %3d\n", ch, d1, NOTE_NAMES[d1 % 12], oct, d2);
                break;
            }
            case 0x90: {
                int oct = ((int)d1 / 12) - 1;
                if (d2 == 0) {
                    printf("[CH %2d] Note Off: %3d (%-2s%d)\n", ch, d1, NOTE_NAMES[d1 % 12], oct);
                } else {
                    printf("[CH %2d] Note On:  %3d (%-2s%d), Vel: %3d\n", ch, d1, NOTE_NAMES[d1 % 12], oct, d2);
                }
                break;
            }
            case 0xA0:
                printf("[CH %2d] Poly Aftertouch: Note %d, Pressure %d\n", ch, d1, d2);
                break;
            case 0xB0:
                printf("[CH %2d] Control Change: CC# %3d = %3d\n", ch, d1, d2);
                break;
            case 0xC0:
                printf("[CH %2d] Program Change: %3d\n", ch, d1);
                break;
            case 0xD0:
                printf("[CH %2d] Channel Aftertouch: %3d\n", ch, d1);
                break;
            case 0xE0: {
                int bend = ((int)d2 << 7 | d1) - 8192;
                printf("[CH %2d] Pitch Bend: %+5d\n", ch, bend);
                break;
            }
        }
    } else {
        switch (status) {
            case 0xFA: printf("[Transport] START\n"); break;
            case 0xFB: printf("[Transport] CONTINUE\n"); break;
            case 0xFC: printf("[Transport] STOP\n"); break;
            case 0xF2: printf("[Song Position Pointer] %d\n", (length > 2) ? ((data[2] << 7) | data[1]) : 0); break;
            case 0xF0: printf("[SysEx] length %u bytes\n", (unsigned)length); break;
            default:
                printf("[Raw %02X] len %u: ", status, (unsigned)length);
                for (UInt16 i = 0; i < length && i < 8; i++) {
                    printf("%02X ", data[i]);
                }
                if (length > 8) printf("...");
                printf("\n");
                break;
        }
    }
    fflush(stdout);
}

static void updateRouting(void) {
    char srcName[256] = {0};
    char dstName[256] = {0};

    MIDIEndpointRef newSrc = findEndpoint(true, gConfig.sourcePattern, srcName, sizeof(srcName));
    MIDIEndpointRef newDst = findEndpoint(false, gConfig.destPattern, dstName, sizeof(dstName));

    if (newSrc != 0 && newDst != 0) {
        if (!gConnected || newSrc != gSrcEndpoint || newDst != gDstEndpoint) {
            // Disconnect old if any
            if (gConnected && gSrcEndpoint && gInPortForward) {
                MIDIPortDisconnectSource(gInPortForward, gSrcEndpoint);
            }
            if (gConfig.bidirectional && gConnected && gDstEndpoint && gInPortReverse) {
                MIDIPortDisconnectSource(gInPortReverse, gDstEndpoint);
            }

            gSrcEndpoint = newSrc;
            gDstEndpoint = newDst;

            OSStatus st = MIDIPortConnectSource(gInPortForward, gSrcEndpoint, NULL);
            if (st == noErr) {
                printTimestamp();
                printf("[CONNECTED] '%s' -> '%s'\n", srcName, dstName);

                if (gConfig.bidirectional && gInPortReverse) {
                    // In reverse, the destination becomes the source
                    char revSrcName[256] = {0};
                    MIDIEndpointRef revSrc = findEndpoint(true, gConfig.destPattern, revSrcName, sizeof(revSrcName));
                    char revDstName[256] = {0};
                    MIDIEndpointRef revDst = findEndpoint(false, gConfig.sourcePattern, revDstName, sizeof(revDstName));

                    if (revSrc && revDst) {
                        MIDIPortConnectSource(gInPortReverse, revSrc, (void *)(uintptr_t)revDst);
                        printTimestamp();
                        printf("[CONNECTED] '%s' -> '%s' (Reverse)\n", revSrcName, revDstName);
                    }
                }

                gConnected = true;
                fflush(stdout);
            } else {
                printTimestamp();
                printf("[ERROR] Failed to connect source: %d\n", (int)st);
            }
        }
    } else {
        if (gConnected) {
            printTimestamp();
            printf("[DISCONNECTED] Device offline. Waiting for connection...\n");
            if (gSrcEndpoint && gInPortForward) {
                MIDIPortDisconnectSource(gInPortForward, gSrcEndpoint);
            }
            gSrcEndpoint = 0;
            gDstEndpoint = 0;
            gConnected = false;
            fflush(stdout);
        }
    }
}

static void timerCallback(CFRunLoopTimerRef timer, void *info) {
    (void)timer;
    (void)info;
    updateRouting();
}

static void notifyCallback(const MIDINotification *message, void *refCon) {
    (void)refCon;
    switch (message->messageID) {
        case kMIDIMsgObjectAdded:
        case kMIDIMsgObjectRemoved:
        case kMIDIMsgSetupChanged:
        case kMIDIMsgPropertyChanged:
            updateRouting();
            break;
        default:
            break;
    }
}

static void signalHandler(int sig) {
    (void)sig;
    printTimestamp();
    printf("Stopping MIDI Router...\n");
    if (gRunLoop) {
        CFRunLoopStop(gRunLoop);
    }
}

static void listDevices(void) {
    printf("Available MIDI Sources (Inputs):\n");
    ItemCount numSources = MIDIGetNumberOfSources();
    if (numSources == 0) {
        printf("  (none)\n");
    }
    for (ItemCount i = 0; i < numSources; i++) {
        MIDIEndpointRef ep = MIDIGetSource(i);
        char name[256] = "Unknown";
        getEndpointName(ep, name, sizeof(name));
        bool online = isEndpointOnline(ep);
        printf("  [%u] %s %s\n", (unsigned)i, name, online ? "" : "(offline)");
    }

    printf("\nAvailable MIDI Destinations (Outputs):\n");
    ItemCount numDests = MIDIGetNumberOfDestinations();
    if (numDests == 0) {
        printf("  (none)\n");
    }
    for (ItemCount i = 0; i < numDests; i++) {
        MIDIEndpointRef ep = MIDIGetDestination(i);
        char name[256] = "Unknown";
        getEndpointName(ep, name, sizeof(name));
        bool online = isEndpointOnline(ep);
        printf("  [%u] %s %s\n", (unsigned)i, name, online ? "" : "(offline)");
    }
}

static const char *getHomeDir(void) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    return home ? home : "/tmp";
}

static void printStatus(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "launchctl print gui/%d/%s 2>&1", (int)getuid(), SERVICE_LABEL);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        printf("Failed to check launchctl status.\n");
        return;
    }
    char buf[512];
    bool running = false;
    int pid = 0;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "state = running")) {
            running = true;
        }
        if (strstr(buf, "pid = ")) {
            char *p = strstr(buf, "pid = ");
            pid = atoi(p + 6);
        }
    }
    pclose(fp);

    if (running) {
        printf("[STATUS] Background service '%s' is RUNNING (PID %d)\n", SERVICE_LABEL, pid);
    } else {
        printf("[STATUS] Background service '%s' is NOT running.\n", SERVICE_LABEL);
    }

    const char *home = getHomeDir();
    char logPath[512];
    snprintf(logPath, sizeof(logPath), "%s/Library/Logs/midi-router.log", home);
    printf("Log file: %s\n", logPath);
    if (access(logPath, R_OK) == 0) {
        printf("--- Recent Log (last 5 lines) ---\n");
        snprintf(cmd, sizeof(cmd), "tail -n 5 '%s'", logPath);
        system(cmd);
    }
}

static int installService(void) {
    const char *home = getHomeDir();
    char binDir[512];
    char targetBin[512];
    char plistDir[512];
    char plistFile[512];
    char logDir[512];
    char logFile[512];

    snprintf(binDir, sizeof(binDir), "%s/.local/bin", home);
    snprintf(targetBin, sizeof(targetBin), "%s/midi-router", binDir);
    snprintf(plistDir, sizeof(plistDir), "%s/Library/LaunchAgents", home);
    snprintf(plistFile, sizeof(plistFile), "%s/%s.plist", plistDir, SERVICE_LABEL);
    snprintf(logDir, sizeof(logDir), "%s/Library/Logs", home);
    snprintf(logFile, sizeof(logFile), "%s/midi-router.log", logDir);

    mkdir(binDir, 0755);
    mkdir(plistDir, 0755);
    mkdir(logDir, 0755);

    // Get current binary executable path
    char currentBin[1024];
    uint32_t size = sizeof(currentBin);
    extern int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
    if (_NSGetExecutablePath(currentBin, &size) != 0) {
        strcpy(currentBin, targetBin);
    }

    // Copy current executable to targetBin if different
    if (strcmp(currentBin, targetBin) != 0) {
        char copyCmd[2048];
        snprintf(copyCmd, sizeof(copyCmd), "cp '%s' '%s' && chmod +x '%s'", currentBin, targetBin, targetBin);
        if (system(copyCmd) != 0) {
            fprintf(stderr, "Failed to copy executable to %s\n", targetBin);
            return 1;
        }
    }

    // Write LaunchAgent plist
    FILE *f = fopen(plistFile, "w");
    if (!f) {
        perror("Failed to create plist file");
        return 1;
    }

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
    fprintf(f, "<plist version=\"1.0\">\n");
    fprintf(f, "<dict>\n");
    fprintf(f, "    <key>Label</key>\n");
    fprintf(f, "    <string>%s</string>\n", SERVICE_LABEL);
    fprintf(f, "    <key>ProgramArguments</key>\n");
    fprintf(f, "    <array>\n");
    fprintf(f, "        <string>%s</string>\n", targetBin);
    if (strcmp(gConfig.sourcePattern, "Digitakt") != 0) {
        fprintf(f, "        <string>--source</string>\n");
        fprintf(f, "        <string>%s</string>\n", gConfig.sourcePattern);
    }
    if (strcmp(gConfig.destPattern, "Digitone") != 0) {
        fprintf(f, "        <string>--dest</string>\n");
        fprintf(f, "        <string>%s</string>\n", gConfig.destPattern);
    }
    if (gConfig.bidirectional) {
        fprintf(f, "        <string>--bidirectional</string>\n");
    }
    fprintf(f, "    </array>\n");
    fprintf(f, "    <key>RunAtLoad</key>\n");
    fprintf(f, "    <true/>\n");
    fprintf(f, "    <key>KeepAlive</key>\n");
    fprintf(f, "    <true/>\n");
    fprintf(f, "    <key>StandardOutPath</key>\n");
    fprintf(f, "    <string>%s</string>\n", logFile);
    fprintf(f, "    <key>StandardErrorPath</key>\n");
    fprintf(f, "    <string>%s</string>\n", logFile);
    fprintf(f, "</dict>\n");
    fprintf(f, "</plist>\n");
    fclose(f);

    printf("[OK] Wrote LaunchAgent: %s\n", plistFile);

    // Bootout old instance if running, then bootstrap new
    char launchCmd[1024];
    snprintf(launchCmd, sizeof(launchCmd), "launchctl bootout gui/%d/%s 2>/dev/null", (int)getuid(), SERVICE_LABEL);
    system(launchCmd);

    snprintf(launchCmd, sizeof(launchCmd), "launchctl bootstrap gui/%d '%s'", (int)getuid(), plistFile);
    int ret = system(launchCmd);
    if (ret != 0) {
        // Fallback for older launchctl syntax
        snprintf(launchCmd, sizeof(launchCmd), "launchctl load -w '%s'", plistFile);
        ret = system(launchCmd);
    }

    if (ret == 0) {
        printf("[OK] Background service installed and started successfully!\n");
        printf("     Binary:  %s\n", targetBin);
        printf("     Routing: '%s' -> '%s'%s\n",
               gConfig.sourcePattern, gConfig.destPattern,
               gConfig.bidirectional ? " (Bidirectional)" : "");
        printf("     Auto-starts on every login/boot.\n");
        printf("     Log file: %s\n", logFile);
        printf("\nTo check status anytime:  midi-router --status\n");
        printf("To view live logs:        tail -f %s\n", logFile);
        printf("To uninstall:             midi-router --uninstall\n");
        return 0;
    } else {
        fprintf(stderr, "[ERROR] Failed to load LaunchAgent via launchctl.\n");
        return 1;
    }
}

static int uninstallService(void) {
    const char *home = getHomeDir();
    char plistFile[512];
    snprintf(plistFile, sizeof(plistFile), "%s/Library/LaunchAgents/%s.plist", home, SERVICE_LABEL);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "launchctl bootout gui/%d/%s 2>/dev/null", (int)getuid(), SERVICE_LABEL);
    system(cmd);

    snprintf(cmd, sizeof(cmd), "launchctl unload '%s' 2>/dev/null", plistFile);
    system(cmd);

    if (remove(plistFile) == 0) {
        printf("[OK] LaunchAgent plist removed: %s\n", plistFile);
    } else {
        printf("[INFO] LaunchAgent plist did not exist or was already removed.\n");
    }
    printf("[OK] Background service uninstalled.\n");
    return 0;
}

static void printHelp(const char *progName) {
    printf("midi-router v%s - Ultra-low-latency macOS CoreMIDI router for Apple Silicon\n\n", VERSION);
    printf("Usage: %s [OPTIONS]\n\n", progName);
    printf("Options:\n");
    printf("  -s, --source <pattern>      Source device name pattern (default: \"Digitakt\")\n");
    printf("  -d, --dest <pattern>        Destination device name pattern (default: \"Digitone\")\n");
    printf("  -b, --bidirectional         Route both ways: Source -> Dest AND Dest -> Source\n");
    printf("  -v, --verbose               Log routed MIDI notes, CC, and transport messages\n");
    printf("  -vv                         Log all MIDI messages including clock ticks\n");
    printf("  -l, --list                  List all detected MIDI inputs and outputs\n");
    printf("  --install                   Install and start as background service (auto-starts on boot)\n");
    printf("  --uninstall                 Stop and remove background service\n");
    printf("  --status                    Check background service status and recent logs\n");
    printf("  -h, --help                  Show this help message\n\n");
    printf("Examples:\n");
    printf("  %s --list\n", progName);
    printf("  %s -v                       (Run in foreground with live MIDI monitor)\n", progName);
    printf("  %s --install                (Install background service to run on boot)\n", progName);
    printf("  %s --status                 (Check if background service is running)\n", progName);
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
        {"source",        required_argument, 0, 's'},
        {"dest",          required_argument, 0, 'd'},
        {"bidirectional", no_argument,       0, 'b'},
        {"verbose",       no_argument,       0, 'v'},
        {"list",          no_argument,       0, 'l'},
        {"install",       no_argument,       0, 1001},
        {"uninstall",     no_argument,       0, 1002},
        {"status",        no_argument,       0, 1003},
        {"help",          no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:d:bvlh", long_options, NULL)) != -1) {
        switch (opt) {
            case 's':
                strncpy(gConfig.sourcePattern, optarg, sizeof(gConfig.sourcePattern) - 1);
                break;
            case 'd':
                strncpy(gConfig.destPattern, optarg, sizeof(gConfig.destPattern) - 1);
                break;
            case 'b':
                gConfig.bidirectional = true;
                break;
            case 'v':
                gConfig.verbose++;
                break;
            case 'l':
                listDevices();
                return 0;
            case 1001:
                return installService();
            case 1002:
                return uninstallService();
            case 1003:
                printStatus();
                return 0;
            case 'h':
                printHelp(argv[0]);
                return 0;
            default:
                printHelp(argv[0]);
                return 1;
        }
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    printTimestamp();
    printf("Starting MIDI Router v%s (Apple Silicon Native)\n", VERSION);
    printf("Source Pattern:      '%s'\n", gConfig.sourcePattern);
    printf("Destination Pattern: '%s'\n", gConfig.destPattern);
    printf("Bidirectional:       %s\n", gConfig.bidirectional ? "Yes" : "No");
    printf("Verbose Monitor:     %s\n", gConfig.verbose ? (gConfig.verbose > 1 ? "Full (with clock)" : "Yes") : "No");
    fflush(stdout);

    OSStatus status = MIDIClientCreateWithBlock(CFSTR("MidiRouterClient"), &gClient, ^(const MIDINotification *message) {
        notifyCallback(message, NULL);
    });
    if (status != noErr) {
        fprintf(stderr, "Error: MIDIClientCreateWithBlock failed (%d)\n", (int)status);
        return 1;
    }

    status = MIDIOutputPortCreate(gClient, CFSTR("MidiRouterOutForward"), &gOutPortForward);
    if (status != noErr) {
        fprintf(stderr, "Error: MIDIOutputPortCreate failed (%d)\n", (int)status);
        return 1;
    }

    status = MIDIInputPortCreateWithBlock(gClient, CFSTR("MidiRouterInForward"), &gInPortForward, ^(const MIDIPacketList *pktlist, void *srcConnRefCon) {
        (void)srcConnRefCon;
        if (gDstEndpoint != 0) {
            MIDISend(gOutPortForward, gDstEndpoint, pktlist);
            if (gConfig.verbose > 0) {
                const MIDIPacket *packet = &pktlist->packet[0];
                for (UInt32 i = 0; i < pktlist->numPackets; ++i) {
                    logMidiMessage("-> [Digitakt -> Digitone]", packet->data, packet->length);
                    packet = MIDIPacketNext(packet);
                }
            }
        }
    });
    if (status != noErr) {
        fprintf(stderr, "Error: MIDIInputPortCreateWithBlock failed (%d)\n", (int)status);
        return 1;
    }

    if (gConfig.bidirectional) {
        MIDIOutputPortCreate(gClient, CFSTR("MidiRouterOutReverse"), &gOutPortReverse);
        MIDIInputPortCreateWithBlock(gClient, CFSTR("MidiRouterInReverse"), &gInPortReverse, ^(const MIDIPacketList *pktlist, void *srcConnRefCon) {
            MIDIEndpointRef revDst = (MIDIEndpointRef)(uintptr_t)srcConnRefCon;
            if (revDst != 0) {
                MIDISend(gOutPortReverse, revDst, pktlist);
                if (gConfig.verbose > 0) {
                    const MIDIPacket *packet = &pktlist->packet[0];
                    for (UInt32 i = 0; i < pktlist->numPackets; ++i) {
                        logMidiMessage("<- [Digitone -> Digitakt]", packet->data, packet->length);
                        packet = MIDIPacketNext(packet);
                    }
                }
            }
        });
    }

    // Initial connection attempt
    updateRouting();

    // Timer every 2.0 seconds to check endpoint health and auto-reconnect
    CFRunLoopTimerContext timerContext = {0, NULL, NULL, NULL, NULL};
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
        kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + 2.0,
        2.0,
        0, 0,
        timerCallback,
        &timerContext
    );
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer, kCFRunLoopDefaultMode);

    gRunLoop = CFRunLoopGetCurrent();
    CFRunLoopRun();

    // Cleanup
    if (timer) {
        CFRunLoopTimerInvalidate(timer);
        CFRelease(timer);
    }
    if (gInPortForward && gSrcEndpoint) {
        MIDIPortDisconnectSource(gInPortForward, gSrcEndpoint);
    }
    if (gInPortReverse && gConnected) {
        // Disconnect reverse if needed
    }
    if (gClient) {
        MIDIClientDispose(gClient);
    }

    printTimestamp();
    printf("MIDI Router terminated.\n");
    return 0;
}
