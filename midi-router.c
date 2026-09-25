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
#include <ctype.h>
#include <getopt.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#define VERSION "1.1.0"
#define SERVICE_LABEL "com.stevenrobinson.midi-router"
#define MAX_DESTINATIONS 8
#define MAX_RULES 16

typedef struct {
    char name[128];
    char sourcePattern[64];
    int numDestPatterns;
    char destPatterns[MAX_DESTINATIONS][64];
    bool filterRealtime;

    // Dynamic runtime state
    MIDIEndpointRef srcEndpoint;
    char resolvedSrcName[128];
    bool srcConnected;

    int numResolvedDests;
    MIDIEndpointRef destEndpoints[MAX_DESTINATIONS];
    char resolvedDestNames[MAX_DESTINATIONS][128];

    MIDIPortRef inPort;
} MidiRouteRule;

static MidiRouteRule gRules[MAX_RULES];
static int gNumRules = 0;
static int gVerbose = 0; // 0 = off, 1 = notes/cc/transport, 2 = all including clock
static char gCustomConfigPath[512] = {0};

static CFRunLoopRef gRunLoop = NULL;
static MIDIClientRef gClient = 0;
static MIDIPortRef gOutPort = 0;

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

static const char *getHomeDir(void) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    return home ? home : "/tmp";
}

static void getConfigPath(char *buf, size_t maxLen) {
    if (gCustomConfigPath[0] != '\0') {
        strncpy(buf, gCustomConfigPath, maxLen - 1);
        buf[maxLen - 1] = '\0';
        return;
    }
    const char *home = getHomeDir();
    snprintf(buf, maxLen, "%s/.config/midi-router/routes.conf", home);
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

static char *trimWhitespace(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static void initDefaultRules(void) {
    gNumRules = 2;
    memset(gRules, 0, sizeof(gRules));

    // Rule 1: DT2 -> DN2 (Master Clock & Transport)
    snprintf(gRules[0].name, sizeof(gRules[0].name), "DT2 -> DN2 (Master Clock & Transport)");
    strncpy(gRules[0].sourcePattern, "Digitakt", sizeof(gRules[0].sourcePattern) - 1);
    gRules[0].numDestPatterns = 1;
    strncpy(gRules[0].destPatterns[0], "Digitone", sizeof(gRules[0].destPatterns[0]) - 1);
    gRules[0].filterRealtime = false;

    // Rule 2: KeyStep -> DT2 & DN2 (Controller Fan-Out, Clock Filtered)
    snprintf(gRules[1].name, sizeof(gRules[1].name), "KeyStep -> DT2 & DN2 (Controller Fan-Out)");
    strncpy(gRules[1].sourcePattern, "KeyStep", sizeof(gRules[1].sourcePattern) - 1);
    gRules[1].numDestPatterns = 2;
    strncpy(gRules[1].destPatterns[0], "Digitakt", sizeof(gRules[1].destPatterns[0]) - 1);
    strncpy(gRules[1].destPatterns[1], "Digitone", sizeof(gRules[1].destPatterns[1]) - 1);
    gRules[1].filterRealtime = true;
}

static void writeDefaultConfigFile(const char *path) {
    char dir[512];
    strncpy(dir, path, sizeof(dir) - 1);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755);
    }

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f, "# ==============================================================================\n");
    fprintf(f, "# MIDI Router Configuration\n");
    fprintf(f, "# https://github.com/Steven-Robinson/midi-router\n");
    fprintf(f, "#\n");
    fprintf(f, "# Syntax:\n");
    fprintf(f, "#   route <SourcePattern> -> <DestPattern1>, <DestPattern2>, ... [options]\n");
    fprintf(f, "#\n");
    fprintf(f, "# Options:\n");
    fprintf(f, "#   filter-realtime   Strips Real-Time Clock, Start, Stop, Continue, Active Sensing\n");
    fprintf(f, "#                     (Use for controllers to prevent clock conflicts with DT2)\n");
    fprintf(f, "# ==============================================================================\n\n");

    fprintf(f, "# Rule 1: DT2 -> DN2 (Master Clock, Transport & Channel Data Bridge)\n");
    fprintf(f, "# Digitakt II acts as master hardware brain. Clock and transport pass through.\n");
    fprintf(f, "route Digitakt -> Digitone\n\n");

    fprintf(f, "# Rule 2: KeyStep -> DT2 & DN2 (Controller Fan-Out for Auto Channels)\n");
    fprintf(f, "# Broadcasts to both units with real-time clock/transport filtered out.\n");
    fprintf(f, "# Switch channels on KeyStep:\n");
    fprintf(f, "#   - Channel 14 -> DT2 Auto Channel (plays active sampler track)\n");
    fprintf(f, "#   - Channel 10 -> DN2 Auto Channel (plays active synth track)\n");
    fprintf(f, "route KeyStep -> Digitakt, Digitone filter-realtime\n");

    fclose(f);
}

static void loadConfigFile(void) {
    char confPath[512];
    getConfigPath(confPath, sizeof(confPath));

    if (access(confPath, R_OK) != 0) {
        initDefaultRules();
        writeDefaultConfigFile(confPath);
        return;
    }

    FILE *f = fopen(confPath, "r");
    if (!f) {
        initDefaultRules();
        return;
    }

    gNumRules = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && gNumRules < MAX_RULES) {
        char *p = trimWhitespace(line);
        if (*p == '#' || *p == '\0') continue;

        if (strncmp(p, "route ", 6) != 0) continue;
        p += 6;

        char *arrow = strstr(p, "->");
        if (!arrow) continue;

        *arrow = '\0';
        char *srcPart = trimWhitespace(p);
        char *dstPart = trimWhitespace(arrow + 2);

        if (strlen(srcPart) == 0 || strlen(dstPart) == 0) continue;

        MidiRouteRule *rule = &gRules[gNumRules];
        memset(rule, 0, sizeof(MidiRouteRule));

        strncpy(rule->sourcePattern, srcPart, sizeof(rule->sourcePattern) - 1);

        // Check for options at the end of dstPart
        char *optFilter = strstr(dstPart, "filter-realtime");
        if (optFilter) {
            rule->filterRealtime = true;
            *optFilter = '\0';
            dstPart = trimWhitespace(dstPart);
        }

        // Parse comma-separated destinations
        char *destTok = strtok(dstPart, ",");
        while (destTok && rule->numDestPatterns < MAX_DESTINATIONS) {
            char *cleanDest = trimWhitespace(destTok);
            if (strlen(cleanDest) > 0) {
                strncpy(rule->destPatterns[rule->numDestPatterns], cleanDest, sizeof(rule->destPatterns[0]) - 1);
                rule->numDestPatterns++;
            }
            destTok = strtok(NULL, ",");
        }

        if (rule->numDestPatterns > 0) {
            snprintf(rule->name, sizeof(rule->name), "Rule %d (%s -> %s%s)",
                     gNumRules + 1, rule->sourcePattern, rule->destPatterns[0],
                     rule->numDestPatterns > 1 ? ", ..." : "");
            gNumRules++;
        }
    }
    fclose(f);

    if (gNumRules == 0) {
        initDefaultRules();
    }
}

static void logMidiMessage(const char *ruleName, const UInt8 *data, UInt16 length) {
    if (length == 0 || gVerbose == 0) return;

    UInt8 status = data[0];
    if (status == 0xF8) {
        if (gVerbose >= 2) {
            printTimestamp();
            printf("[%s] [Clock]\n", ruleName);
        }
        return;
    }
    if (status == 0xFE) return; // Active sensing

    printTimestamp();
    printf("[%s] ", ruleName);

    if (status >= 0x80 && status <= 0xEF) {
        UInt8 type = status & 0xF0;
        UInt8 ch = (status & 0x0F) + 1;
        UInt8 d1 = (length > 1) ? data[1] : 0;
        UInt8 d2 = (length > 2) ? data[2] : 0;

        const char *chAnnotation = "";
        if (ch == 14) chAnnotation = " (DT2 Auto Channel)";
        else if (ch == 10) chAnnotation = " (DN2 Auto Channel)";

        switch (type) {
            case 0x80: {
                int oct = ((int)d1 / 12) - 1;
                printf("[CH %2d%s] Note Off: %3d (%-2s%d), Vel: %3d\n", ch, chAnnotation, d1, NOTE_NAMES[d1 % 12], oct, d2);
                break;
            }
            case 0x90: {
                int oct = ((int)d1 / 12) - 1;
                if (d2 == 0) {
                    printf("[CH %2d%s] Note Off: %3d (%-2s%d)\n", ch, chAnnotation, d1, NOTE_NAMES[d1 % 12], oct);
                } else {
                    printf("[CH %2d%s] Note On:  %3d (%-2s%d), Vel: %3d\n", ch, chAnnotation, d1, NOTE_NAMES[d1 % 12], oct, d2);
                }
                break;
            }
            case 0xA0:
                printf("[CH %2d%s] Poly Aftertouch: Note %d, Pressure %d\n", ch, chAnnotation, d1, d2);
                break;
            case 0xB0:
                printf("[CH %2d%s] Control Change: CC# %3d = %3d\n", ch, chAnnotation, d1, d2);
                break;
            case 0xC0:
                printf("[CH %2d%s] Program Change: %3d\n", ch, chAnnotation, d1);
                break;
            case 0xD0:
                printf("[CH %2d%s] Channel Aftertouch: %3d\n", ch, chAnnotation, d1);
                break;
            case 0xE0: {
                int bend = ((int)d2 << 7 | d1) - 8192;
                printf("[CH %2d%s] Pitch Bend: %+5d\n", ch, chAnnotation, bend);
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
                printf("[Raw %02X] len %u\n", status, (unsigned)length);
                break;
        }
    }
    fflush(stdout);
}

static void filterAndForwardPacketList(MIDIPortRef outPort, MIDIEndpointRef dest, const MIDIPacketList *pktlist, bool filterRealtime) {
    if (!dest) return;

    if (!filterRealtime) {
        // Fast path: pass unaltered
        MIDISend(outPort, dest, pktlist);
        return;
    }

    // Filter real-time messages (0xF8 - 0xFF)
    Byte buffer[4096];
    MIDIPacketList *filteredList = (MIDIPacketList *)buffer;
    MIDIPacket *curPkt = MIDIPacketListInit(filteredList);

    const MIDIPacket *pkt = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; ++i) {
        Byte cleanData[256];
        ByteCount cleanLen = 0;

        for (UInt16 b = 0; b < pkt->length && cleanLen < sizeof(cleanData); ++b) {
            Byte byte = pkt->data[b];
            if (byte >= 0xF8) {
                // Filter out Real-Time Clock (0xF8), Start (0xFA), Continue (0xFB), Stop (0xFC), Active Sensing (0xFE)
                continue;
            }
            cleanData[cleanLen++] = byte;
        }

        if (cleanLen > 0) {
            MIDIPacket *nextPkt = MIDIPacketListAdd(filteredList, sizeof(buffer), curPkt, pkt->timeStamp, cleanLen, cleanData);
            if (nextPkt != NULL) {
                curPkt = nextPkt;
            }
        }
        pkt = MIDIPacketNext(pkt);
    }

    if (filteredList->numPackets > 0) {
        MIDISend(outPort, dest, filteredList);
    }
}

static void updateAllRouting(void) {
    for (int r = 0; r < gNumRules; r++) {
        MidiRouteRule *rule = &gRules[r];

        // 1. Resolve source endpoint
        char currentSrcName[128] = {0};
        MIDIEndpointRef newSrc = findEndpoint(true, rule->sourcePattern, currentSrcName, sizeof(currentSrcName));

        // 2. Resolve destination endpoints
        MIDIEndpointRef newDests[MAX_DESTINATIONS] = {0};
        char currentDstNames[MAX_DESTINATIONS][128] = {{0}};
        int resolvedCount = 0;

        for (int d = 0; d < rule->numDestPatterns; d++) {
            newDests[d] = findEndpoint(false, rule->destPatterns[d], currentDstNames[d], sizeof(currentDstNames[d]));
            if (newDests[d] != 0) {
                resolvedCount++;
            }
        }

        bool sourceChanged = (newSrc != rule->srcEndpoint);

        // If source was connected and is now gone or changed
        if (rule->srcConnected && (sourceChanged || newSrc == 0)) {
            if (rule->inPort && rule->srcEndpoint) {
                MIDIPortDisconnectSource(rule->inPort, rule->srcEndpoint);
            }
            rule->srcConnected = false;
            rule->srcEndpoint = 0;
            printTimestamp();
            printf("[DISCONNECTED] %s: source '%s' went offline.\n", rule->name, rule->resolvedSrcName);
            fflush(stdout);
        }

        // Update destination pointers
        for (int d = 0; d < rule->numDestPatterns; d++) {
            rule->destEndpoints[d] = newDests[d];
            strncpy(rule->resolvedDestNames[d], currentDstNames[d], sizeof(rule->resolvedDestNames[d]) - 1);
        }
        rule->numResolvedDests = resolvedCount;

        // Connect if source is present and at least one destination is available
        if (newSrc != 0 && resolvedCount > 0) {
            if (!rule->srcConnected || sourceChanged) {
                rule->srcEndpoint = newSrc;
                strncpy(rule->resolvedSrcName, currentSrcName, sizeof(rule->resolvedSrcName) - 1);

                OSStatus st = MIDIPortConnectSource(rule->inPort, rule->srcEndpoint, (void *)(uintptr_t)r);
                if (st == noErr) {
                    rule->srcConnected = true;
                    printTimestamp();
                    printf("[CONNECTED] %s: '%s' -> [", rule->name, rule->resolvedSrcName);
                    for (int d = 0; d < rule->numDestPatterns; d++) {
                        if (rule->destEndpoints[d] != 0) {
                            printf("%s'%s'", (d > 0 ? ", " : ""), rule->resolvedDestNames[d]);
                        }
                    }
                    printf("]%s\n", rule->filterRealtime ? " (Real-Time Clock Filtered)" : " (Full Pass-Through)");
                    fflush(stdout);
                } else {
                    printTimestamp();
                    printf("[ERROR] %s: MIDIPortConnectSource failed (%d)\n", rule->name, (int)st);
                }
            }
        }
    }
}

static void timerCallback(CFRunLoopTimerRef timer, void *info) {
    (void)timer;
    (void)info;
    updateAllRouting();
}

static void notifyCallback(const MIDINotification *message, void *refCon) {
    (void)refCon;
    switch (message->messageID) {
        case kMIDIMsgObjectAdded:
        case kMIDIMsgObjectRemoved:
        case kMIDIMsgSetupChanged:
        case kMIDIMsgPropertyChanged:
            updateAllRouting();
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
        printf("  [%u] %s %s\n", (unsigned)i, name, online ? "[ONLINE]" : "[OFFLINE]");
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
        printf("  [%u] %s %s\n", (unsigned)i, name, online ? "[ONLINE]" : "[OFFLINE]");
    }
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

    char confPath[512];
    getConfigPath(confPath, sizeof(confPath));

    const char *home = getHomeDir();
    char logPath[512];
    snprintf(logPath, sizeof(logPath), "%s/Library/Logs/midi-router.log", home);

    if (running) {
        printf("[STATUS] Background service '%s' is RUNNING (PID %d)\n", SERVICE_LABEL, pid);
    } else {
        printf("[STATUS] Background service '%s' is NOT running.\n", SERVICE_LABEL);
    }
    printf("Config file: %s\n", confPath);
    printf("Log file:    %s\n\n", logPath);

    loadConfigFile();
    printf("Configured Routes (%d):\n", gNumRules);
    for (int r = 0; r < gNumRules; r++) {
        MidiRouteRule *rule = &gRules[r];
        char srcName[128] = {0};
        MIDIEndpointRef src = findEndpoint(true, rule->sourcePattern, srcName, sizeof(srcName));

        printf("  %d. %s\n", r + 1, rule->name);
        printf("     Source:       '%s' %s\n", rule->sourcePattern, src ? "[ONLINE]" : "[OFFLINE / WAITING]");
        printf("     Destinations: ");
        for (int d = 0; d < rule->numDestPatterns; d++) {
            char dstName[128] = {0};
            MIDIEndpointRef dst = findEndpoint(false, rule->destPatterns[d], dstName, sizeof(dstName));
            printf("'%s' %s%s", rule->destPatterns[d], dst ? "[ONLINE]" : "[OFFLINE]",
                   (d + 1 < rule->numDestPatterns ? ", " : ""));
        }
        printf("\n");
        printf("     Real-Time:    %s\n", rule->filterRealtime ? "FILTERED (Clock/Transport stripped for controller)" : "PASS-THROUGH (Master Clock active)");
        printf("     Status:       %s\n\n", src ? "ACTIVE / READY" : "WAITING FOR DEVICE");
    }

    if (access(logPath, R_OK) == 0) {
        printf("--- Recent Log (last 6 lines) ---\n");
        snprintf(cmd, sizeof(cmd), "tail -n 6 '%s'", logPath);
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
    char confPath[512];

    snprintf(binDir, sizeof(binDir), "%s/.local/bin", home);
    snprintf(targetBin, sizeof(targetBin), "%s/midi-router", binDir);
    snprintf(plistDir, sizeof(plistDir), "%s/Library/LaunchAgents", home);
    snprintf(plistFile, sizeof(plistFile), "%s/%s.plist", plistDir, SERVICE_LABEL);
    snprintf(logDir, sizeof(logDir), "%s/Library/Logs", home);
    snprintf(logFile, sizeof(logFile), "%s/midi-router.log", logDir);
    getConfigPath(confPath, sizeof(confPath));

    mkdir(binDir, 0755);
    mkdir(plistDir, 0755);
    mkdir(logDir, 0755);

    // Ensure config file exists
    if (access(confPath, R_OK) != 0) {
        writeDefaultConfigFile(confPath);
    }

    // Copy executable if installing from build directory
    char currentBin[1024];
    uint32_t size = sizeof(currentBin);
    extern int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
    if (_NSGetExecutablePath(currentBin, &size) != 0) {
        strcpy(currentBin, targetBin);
    }

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

    // Restart service via launchctl
    char launchCmd[1024];
    snprintf(launchCmd, sizeof(launchCmd), "launchctl bootout gui/%d/%s 2>/dev/null", (int)getuid(), SERVICE_LABEL);
    system(launchCmd);

    snprintf(launchCmd, sizeof(launchCmd), "launchctl bootstrap gui/%d '%s'", (int)getuid(), plistFile);
    int ret = system(launchCmd);
    if (ret != 0) {
        snprintf(launchCmd, sizeof(launchCmd), "launchctl load -w '%s'", plistFile);
        ret = system(launchCmd);
    }

    if (ret == 0) {
        printf("[OK] Background service installed and started successfully!\n");
        printf("     Binary:  %s\n", targetBin);
        printf("     Config:  %s\n", confPath);
        printf("     Logs:    %s\n", logFile);
        printf("     Auto-starts headless on every login/boot.\n\n");
        printf("To check status:   midi-router --status\n");
        printf("To stream logs:    tail -f %s\n", logFile);
        printf("To uninstall:      midi-router --uninstall\n");
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
    printf("  -c, --config <file>         Path to routes configuration file\n");
    printf("  -v, --verbose               Log routed MIDI notes, CC, and transport messages\n");
    printf("  -vv                         Log all MIDI messages including clock ticks\n");
    printf("  -l, --list                  List all detected MIDI inputs and outputs\n");
    printf("  --install                   Install/update headless background LaunchAgent\n");
    printf("  --uninstall                 Stop and remove background service\n");
    printf("  --status                    Check service status, active routes, and logs\n");
    printf("  -h, --help                  Show this help message\n\n");
    printf("Default Rules (configured in ~/.config/midi-router/routes.conf):\n");
    printf("  1. DT2 -> DN2:      Full pass-through of Master Clock, Transport & Notes\n");
    printf("  2. KeyStep -> Both: Fan-out to DT2 & DN2 with Real-Time Clock filtered out\n");
    printf("                      (KeyStep Ch 14 -> DT2 Auto Ch, KeyStep Ch 10 -> DN2 Auto Ch)\n\n");
    printf("Examples:\n");
    printf("  %s --status                 (Check health of background service & routes)\n", progName);
    printf("  %s -v                       (Run live MIDI monitor in terminal)\n", progName);
    printf("  %s --list                   (List connected hardware USB MIDI endpoints)\n", progName);
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
        {"config",        required_argument, 0, 'c'},
        {"verbose",       no_argument,       0, 'v'},
        {"list",          no_argument,       0, 'l'},
        {"install",       no_argument,       0, 1001},
        {"uninstall",     no_argument,       0, 1002},
        {"status",        no_argument,       0, 1003},
        {"help",          no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:vlh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                strncpy(gCustomConfigPath, optarg, sizeof(gCustomConfigPath) - 1);
                break;
            case 'v':
                gVerbose++;
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

    loadConfigFile();

    char confPath[512];
    getConfigPath(confPath, sizeof(confPath));

    printTimestamp();
    printf("Starting MIDI Router v%s (Apple Silicon Native)\n", VERSION);
    printf("Config File: %s (%d rules configured)\n", confPath, gNumRules);
    for (int r = 0; r < gNumRules; r++) {
        printf("  Rule %d: '%s' -> [", r + 1, gRules[r].sourcePattern);
        for (int d = 0; d < gRules[r].numDestPatterns; d++) {
            printf("%s'%s'", (d > 0 ? ", " : ""), gRules[r].destPatterns[d]);
        }
        printf("]%s\n", gRules[r].filterRealtime ? " (Clock/Transport Filtered)" : " (Pass-Through)");
    }
    printf("Verbose Monitor: %s\n", gVerbose ? (gVerbose > 1 ? "Full (with clock)" : "Yes") : "No");
    fflush(stdout);

    OSStatus status = MIDIClientCreateWithBlock(CFSTR("MidiRouterClient"), &gClient, ^(const MIDINotification *message) {
        notifyCallback(message, NULL);
    });
    if (status != noErr) {
        fprintf(stderr, "Error: MIDIClientCreateWithBlock failed (%d)\n", (int)status);
        return 1;
    }

    status = MIDIOutputPortCreate(gClient, CFSTR("MidiRouterOut"), &gOutPort);
    if (status != noErr) {
        fprintf(stderr, "Error: MIDIOutputPortCreate failed (%d)\n", (int)status);
        return 1;
    }

    // Create an input port for each rule
    for (int r = 0; r < gNumRules; r++) {
        char portName[64];
        snprintf(portName, sizeof(portName), "MidiRouterIn_Rule%d", r + 1);
        CFStringRef cfPortName = CFStringCreateWithCString(kCFAllocatorDefault, portName, kCFStringEncodingUTF8);

        status = MIDIInputPortCreateWithBlock(gClient, cfPortName, &gRules[r].inPort, ^(const MIDIPacketList *pktlist, void *srcConnRefCon) {
            int ruleIdx = (int)(uintptr_t)srcConnRefCon;
            if (ruleIdx < 0 || ruleIdx >= gNumRules) return;
            MidiRouteRule *rule = &gRules[ruleIdx];

            for (int d = 0; d < rule->numDestPatterns; d++) {
                if (rule->destEndpoints[d] != 0) {
                    filterAndForwardPacketList(gOutPort, rule->destEndpoints[d], pktlist, rule->filterRealtime);
                }
            }

            if (gVerbose > 0) {
                const MIDIPacket *packet = &pktlist->packet[0];
                for (UInt32 p = 0; p < pktlist->numPackets; ++p) {
                    logMidiMessage(rule->sourcePattern, packet->data, packet->length);
                    packet = MIDIPacketNext(packet);
                }
            }
        });
        CFRelease(cfPortName);

        if (status != noErr) {
            fprintf(stderr, "Error: MIDIInputPortCreateWithBlock failed for rule %d (%d)\n", r + 1, (int)status);
            return 1;
        }
    }

    // Initial connection attempt
    updateAllRouting();

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
    for (int r = 0; r < gNumRules; r++) {
        if (gRules[r].inPort && gRules[r].srcEndpoint) {
            MIDIPortDisconnectSource(gRules[r].inPort, gRules[r].srcEndpoint);
        }
    }
    if (gClient) {
        MIDIClientDispose(gClient);
    }

    printTimestamp();
    printf("MIDI Router terminated.\n");
    return 0;
}
