/* midiread.c — read USB-MIDI from ANY USB MIDI-class device via usbfs.
 * No libusb, no kernel MIDI driver. Auto-detects the device by scanning for a
 * USB MIDI-streaming interface (bInterfaceClass=0x01 Audio, bInterfaceSubClass=0x03),
 * then claims it and reads its bulk/interrupt IN endpoint.
 *
 * Build: gcc -O2 -std=gnu99 midiread.c -o midiread
 * Run:   ./midiread              (auto-pick first MIDI device)
 *        ./midiread 1c75:0288    (optional VID:PID filter)
 * Needs write access to /dev/bus/usb/... (udev rule 99-usbmidi.rules, or sudo).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#define SYSUSB "/sys/bus/usb/devices"

typedef struct {
    char path[64];   /* /dev/bus/usb/BBB/DDD */
    int  iface;      /* MIDI streaming interface number */
    int  ep_in;      /* IN endpoint address (e.g. 0x81) */
    int  vid, pid;
} midi_dev;

static int read_sysfs(const char *dir, const char *file, const char *fmt) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, fmt, &v) != 1) v = -1;
    fclose(f);
    return v;
}

/* In an interface dir, find an IN endpoint (address high bit set) from ep_XX subdirs. */
static int find_in_endpoint(const char *iface_dir) {
    DIR *d = opendir(iface_dir);
    if (!d) return -1;
    struct dirent *e;
    int ep = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "ep_", 3) == 0) {
            int addr = (int)strtol(e->d_name + 3, NULL, 16);
            if (addr & 0x80) { ep = addr; break; }  /* IN endpoint */
        }
    }
    closedir(d);
    return ep;
}

/* Scan for a USB MIDI-streaming interface. Optional vid/pid filter (-1 = any). */
static int find_midi(midi_dev *out, int want_vid, int want_pid) {
    DIR *d = opendir(SYSUSB);
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d)) && found != 0) {
        /* interface dirs look like "1-1:1.1" (contain ':') */
        if (!strchr(e->d_name, ':')) continue;
        char idir[512];
        snprintf(idir, sizeof idir, "%s/%s", SYSUSB, e->d_name);
        int cls = read_sysfs(idir, "bInterfaceClass", "%x");
        int sub = read_sysfs(idir, "bInterfaceSubClass", "%x");
        if (cls != 0x01 || sub != 0x03) continue;   /* not Audio/MIDIStreaming */

        int iface = read_sysfs(idir, "bInterfaceNumber", "%x");
        int ep_in = find_in_endpoint(idir);
        if (iface < 0 || ep_in < 0) continue;

        /* parent device dir = name up to ':' (e.g. "1-1") */
        char parent[256];
        snprintf(parent, sizeof parent, "%.*s",
                 (int)(strchr(e->d_name, ':') - e->d_name), e->d_name);
        char pdir[512];
        snprintf(pdir, sizeof pdir, "%s/%s", SYSUSB, parent);
        int bus = read_sysfs(pdir, "busnum", "%d");
        int dev = read_sysfs(pdir, "devnum", "%d");
        int vid = read_sysfs(pdir, "idVendor", "%x");
        int pid = read_sysfs(pdir, "idProduct", "%x");
        if (bus <= 0 || dev <= 0) continue;
        if (want_vid >= 0 && (vid != want_vid || pid != want_pid)) continue;

        snprintf(out->path, sizeof out->path, "/dev/bus/usb/%03d/%03d", bus, dev);
        out->iface = iface; out->ep_in = ep_in; out->vid = vid; out->pid = pid;
        found = 0;
    }
    closedir(d);
    return found;
}

static const char *note_name(int nn) {
    static const char *names[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static char buf[8];
    snprintf(buf, sizeof buf, "%s%d", names[nn % 12], nn / 12 - 1);
    return buf;
}

int main(int argc, char **argv) {
    int want_vid = -1, want_pid = -1;
    if (argc > 1) {
        if (sscanf(argv[1], "%x:%x", &want_vid, &want_pid) != 2) {
            fprintf(stderr, "bad filter '%s' (expected VID:PID like 1c75:0288)\n", argv[1]);
            return 1;
        }
    }

    midi_dev md;
    if (find_midi(&md, want_vid, want_pid) != 0) {
        fprintf(stderr, "no USB MIDI-class device found\n");
        return 1;
    }
    fprintf(stderr, "MIDI device %04x:%04x at %s  (interface %d, EP 0x%02x)\n",
            md.vid, md.pid, md.path, md.iface, md.ep_in);

    int fd = open(md.path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n(need write access — udev rule or sudo)\n",
                md.path, strerror(errno));
        return 1;
    }
    unsigned int iface = md.iface;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        fprintf(stderr, "claim interface %u: %s\n", iface, strerror(errno));
        return 1;
    }
    fprintf(stderr, "reading ... (Ctrl-C to stop)\n");

    unsigned char buf[64];
    for (;;) {
        struct usbdevfs_bulktransfer bt;
        bt.ep = md.ep_in;
        bt.len = sizeof buf;
        bt.timeout = 1000;
        bt.data = buf;
        int r = ioctl(fd, USBDEVFS_BULK, &bt);
        if (r < 0) {
            if (errno == ETIMEDOUT) continue;
            fprintf(stderr, "bulk read: %s\n", strerror(errno));
            break;
        }
        for (int i = 0; i + 4 <= r; i += 4) {
            unsigned char cin = buf[i] & 0x0f;
            unsigned char a = buf[i + 1], b = buf[i + 2], c = buf[i + 3];
            int ch = (a & 0x0f) + 1;
            switch (cin) {
                case 0x9:
                    if (c > 0) printf("Note On   ch%-2d %-4s vel %d\n", ch, note_name(b), c);
                    else       printf("Note Off  ch%-2d %-4s\n", ch, note_name(b));
                    break;
                case 0x8: printf("Note Off  ch%-2d %-4s\n", ch, note_name(b)); break;
                case 0xB: printf("CC        ch%-2d cc%d = %d\n", ch, b, c); break;
                case 0xE: printf("PitchBend ch%-2d = %d\n", ch, (b | (c << 7)) - 8192); break;
                case 0xC: printf("Program   ch%-2d = %d\n", ch, b); break;
                case 0xD: printf("ChanPress ch%-2d = %d\n", ch, b); break;
                default:  printf("cin=0x%x: %02x %02x %02x\n", cin, a, b, c); break;
            }
            fflush(stdout);
        }
    }
    ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
    close(fd);
    return 0;
}
