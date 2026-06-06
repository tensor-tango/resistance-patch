/*
  Remastered Controls: Resistance - PPSSPP port

  Keeps Resistance's built-in Resistance Plus / PS3 controller path by faking
  usbpspcm0: and PS3 controller packets under PPSSPP.

  Switch / m4xw PPSSPP notes:
  - Keep this as a PPSSPP PRX plugin, not a native Switch NRO/plugin.
  - The Switch standalone PPSSPP build can update extended PSP controls through
    sceCtrlPeekBufferPositive more consistently than through Read only, so both
    imports are patched.
  - The right analog bytes are read by SceCtrlData layout offsets to avoid SDK
    header naming differences (Rsrv[0]/Rsrv[1] vs Rx/Ry).
  - The fake USB stack reports an activated, cable-connected and established
    state so Resistance's PS3 controller success path can complete on Switch.
  - USB callback arguments are kept in static storage. Passing stack memory to
    sceKernelStartThread can race on Switch and break the PS3-mode handshake.
  - USB response packets are written manually. On Switch/m4xw PPSSPP, libc
    snprintf() can fail to populate the game's I/O buffer even though it works
    for the plugin's own stack buffers.
*/

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspusb.h>
#include <pspdisplay.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define EMULATOR_DEVCTL__IS_EMULATOR 0x00000003

#define FAKE_DEVNAME      "usbpspcm0:"
#define FAKE_UID          0x12345678
#define DEBUG_LOG_PATH    "ms0:/PSP/PLUGINS/resistance_switch_debug.log"
#define DEBUG_LOG_PATH_2  "ms0:/resistance_switch_debug.log"

#define PS3_CTRL_LEFT     0x8000
#define PS3_CTRL_DOWN     0x4000
#define PS3_CTRL_RIGHT    0x2000
#define PS3_CTRL_UP       0x1000
#define PS3_CTRL_START    0x0800
#define PS3_CTRL_R3       0x0400
#define PS3_CTRL_L3       0x0200
#define PS3_CTRL_SELECT   0x0100
#define PS3_CTRL_SQUARE   0x0080
#define PS3_CTRL_CROSS    0x0040
#define PS3_CTRL_CIRCLE   0x0020
#define PS3_CTRL_TRIANGLE 0x0010
#define PS3_CTRL_R1       0x0008
#define PS3_CTRL_L1       0x0004
#define PS3_CTRL_R2       0x0002
#define PS3_CTRL_L2       0x0001

#define FAKE_USB_STATE    (PSP_USB_ACTIVATED | PSP_USB_CABLE_CONNECTED | PSP_USB_CONNECTION_ESTABLISHED)
#define FAKE_USB_DRV_STARTED 1
#define FAKE_USB_DRV_STOPPED 2

#define NID_sceCtrlPeekBufferPositive 0x3A622550
#define NID_sceCtrlReadBufferPositive 0x1F803938
#define NID_sceIoOpen                 0x109F50BC
#define NID_sceIoRead                 0x6A638D83
#define NID_sceIoWrite                0x42EC03AC
#define NID_sceIoClose                0x810C4BC3
#define NID_sceIoDevctl               0x54F5FB11
#define NID_sceUsbStart               0xAE5DE6AF
#define NID_sceUsbStop                0xC2464FA0
#define NID_sceUsbActivate            0x586DB82C
#define NID_sceUsbDeactivate          0xC572A9C8
#define NID_sceUsbGetState            0xC21645A4
#define NID_sceUsbGetDrvState         0x112CC951
#define NID_sceUsbWaitState           0x5BE0E002
#define NID_sceUsbWaitCancel          0x1C360735

/* SceCtrlData layout in user mode:
   u32 TimeStamp; u32 Buttons; u8 Lx; u8 Ly; u8 Rsrv[6];
   PPSSPP exposes extended right analog at bytes 10/11.
*/
#define PAD_RX(p) (((unsigned char *)(p))[10])
#define PAD_RY(p) (((unsigned char *)(p))[11])

PSP_MODULE_INFO("ResistancePPSSPP", 0, 1, 5);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

static SceCtrlData g_pad;
static int g_init_mode = 0;
static int g_patched = 0;
static int g_usb_started = 0;
static int g_usb_activated = 0;
static int g_fake_open_count = 0;
static int g_packet_log_count = 0;
static int g_debug_log_ready = 0;
static u32 g_usb_callback_args[2] = { 0, 0x81 };

void _exit(int status) {
    sceKernelExitDeleteThread(status);
    while (1) {
        sceKernelDelayThread(1000000);
    }
}

static void debugLog(const char *fmt, ...) {
    char msg[256];
    char line[320];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    snprintf(line, sizeof(line), "[%08u] %s\n", sceKernelGetSystemTimeLow(), msg);

    SceUID fd = sceIoOpen(DEBUG_LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT, 0777);
    if (fd < 0)
        fd = sceIoOpen(DEBUG_LOG_PATH_2, PSP_O_WRONLY | PSP_O_CREAT, 0777);

    if (fd >= 0) {
        sceIoLseek(fd, 0, 2);
        sceIoWrite(fd, line, strlen(line));
        sceIoClose(fd);
    }
}

static void debugLogOnceInit(void) {
    if (g_debug_log_ready)
        return;

    g_debug_log_ready = 1;
    sceIoRemove(DEBUG_LOG_PATH);
    sceIoRemove(DEBUG_LOG_PATH_2);
    debugLog("module_start ResistancePPSSPP v1.5 manual-packet debug build");
}

static char hexNibble(unsigned int v) {
    v &= 0xF;
    return (v < 10) ? ('0' + v) : ('a' + (v - 10));
}

static void writeHex2(char *p, unsigned int v) {
    p[0] = hexNibble(v >> 4);
    p[1] = hexNibble(v);
}

static void writeHex4(char *p, unsigned int v) {
    p[0] = hexNibble(v >> 12);
    p[1] = hexNibble(v >> 8);
    p[2] = hexNibble(v >> 4);
    p[3] = hexNibble(v);
}

static void copyResponseBytes(void *data, SceSize size, const char *src, int len) {
    if (!data || size == 0)
        return;

    int n = (size < (SceSize)len) ? (int)size : len;
    memcpy(data, src, n);
}

static int ptrInModule(const SceKernelModuleInfo *info, u32 p) {
    for (int i = 0; i < 4; i++) {
        u32 start = info->segmentaddr[i];
        u32 size  = info->segmentsize[i];
        if (start && size && p >= start && p < start + size)
            return 1;
    }
    return 0;
}

static int rangeInModule(const SceKernelModuleInfo *info, u32 p, u32 len) {
    if (!ptrInModule(info, p))
        return 0;
    if (len == 0)
        return 0;
    if (!ptrInModule(info, p + len - 1))
        return 0;
    return 1;
}

static int safeStrEq(const SceKernelModuleInfo *info, u32 p, const char *s) {
    if (!ptrInModule(info, p))
        return 0;

    const char *q = (const char *)p;
    for (int i = 0; i < 64; i++) {
        if (!ptrInModule(info, (u32)(q + i)))
            return 0;
        if (q[i] != s[i])
            return 0;
        if (s[i] == '\0')
            return 1;
    }
    return 0;
}

static void patchStub(u32 stub_addr, void *replacement) {
    u32 f = (u32)replacement;
    _sw(0x08000000 | ((f >> 2) & 0x03FFFFFF), stub_addr + 0x00);
    _sw(0x00000000, stub_addr + 0x04);
}

static int patchImportByNid(const SceKernelModuleInfo *info, const char *lib, u32 nid, void *replacement) {
    int patched = 0;

    for (int seg = 0; seg < 4; seg++) {
        u32 start = info->segmentaddr[seg];
        u32 size  = info->segmentsize[seg];
        if (!start || size < sizeof(SceLibraryStubTable))
            continue;

        for (u32 p = start; p + sizeof(SceLibraryStubTable) <= start + size; p += 4) {
            SceLibraryStubTable *stub = (SceLibraryStubTable *)p;

            if (stub->stubcount == 0 || stub->stubcount > 256)
                continue;
            if (!safeStrEq(info, (u32)stub->libname, lib))
                continue;
            if (!rangeInModule(info, (u32)stub->nidtable, stub->stubcount * sizeof(u32)))
                continue;
            if (!rangeInModule(info, (u32)stub->stubtable, stub->stubcount * 8))
                continue;

            u32 *nids = (u32 *)stub->nidtable;
            for (int i = 0; i < stub->stubcount; i++) {
                if (nids[i] == nid) {
                    u32 stub_addr = (u32)stub->stubtable + i * 8;
                    patchStub(stub_addr, replacement);
                    debugLog("patch %s nid=%08x stub=%08x", lib, nid, stub_addr);
                    patched++;
                }
            }
        }
    }

    if (patched == 0)
        debugLog("patch MISS %s nid=%08x", lib, nid);

    return patched;
}

static u16 convertButtons(u32 psp_buttons) {
    u16 ps3_buttons = 0;

    if (psp_buttons & PSP_CTRL_LEFT)
        ps3_buttons |= PS3_CTRL_LEFT | PS3_CTRL_L2;
    if (psp_buttons & PSP_CTRL_DOWN)
        ps3_buttons |= PS3_CTRL_DOWN | PS3_CTRL_R3;
    if (psp_buttons & PSP_CTRL_RIGHT)
        ps3_buttons |= PS3_CTRL_RIGHT | PS3_CTRL_R2;
    if (psp_buttons & PSP_CTRL_UP)
        ps3_buttons |= PS3_CTRL_UP;
    if (psp_buttons & PSP_CTRL_START)
        ps3_buttons |= PS3_CTRL_START;
    if (psp_buttons & PSP_CTRL_SELECT)
        ps3_buttons |= PS3_CTRL_SELECT;
    if (psp_buttons & PSP_CTRL_SQUARE)
        ps3_buttons |= PS3_CTRL_SQUARE;
    if (psp_buttons & PSP_CTRL_CROSS)
        ps3_buttons |= PS3_CTRL_CROSS;
    if (psp_buttons & PSP_CTRL_CIRCLE)
        ps3_buttons |= PS3_CTRL_CIRCLE;
    if (psp_buttons & PSP_CTRL_TRIANGLE)
        ps3_buttons |= PS3_CTRL_TRIANGLE;
    if (psp_buttons & PSP_CTRL_RTRIGGER)
        ps3_buttons |= PS3_CTRL_R1;
    if (psp_buttons & PSP_CTRL_LTRIGGER)
        ps3_buttons |= PS3_CTRL_L1;

    return ps3_buttons;
}

static void stashPadAndHideFromGame(SceCtrlData *pad_data, int count) {
    if (!pad_data || count <= 0)
        return;

    memcpy(&g_pad, &pad_data[0], sizeof(SceCtrlData));

    if (g_init_mode >= 2) {
        for (int i = 0; i < count; i++) {
            pad_data[i].Buttons = 0;
            pad_data[i].Lx = 128;
            pad_data[i].Ly = 128;
            PAD_RX(&pad_data[i]) = 128;
            PAD_RY(&pad_data[i]) = 128;
        }
    }
}

static int sceCtrlPeekBufferPositivePatched(SceCtrlData *pad_data, int count) {
    int res = sceCtrlPeekBufferPositive(pad_data, count);
    stashPadAndHideFromGame(pad_data, count);
    return res;
}

static int sceCtrlReadBufferPositivePatched(SceCtrlData *pad_data, int count) {
    int res = sceCtrlReadBufferPositive(pad_data, count);
    stashPadAndHideFromGame(pad_data, count);
    return res;
}

static SceUID sceIoOpenPatched(const char *file, int flags, SceMode mode) {
    if (file && strcmp(file, FAKE_DEVNAME) == 0) {
        g_fake_open_count++;
        g_usb_started = 1;
        g_usb_activated = 1;
        debugLog("io open fake dev flags=%08x mode=%08x open_count=%d", flags, mode, g_fake_open_count);
        return FAKE_UID;
    }

    return sceIoOpen(file, flags, mode);
}

static void refreshPadForUsbPacket(void) {
    SceCtrlData fresh;
    memset(&fresh, 0, sizeof(fresh));

    int res = sceCtrlPeekBufferPositive(&fresh, 1);
    if (res >= 0) {
        memcpy(&g_pad, &fresh, sizeof(SceCtrlData));
    } else {
        debugLog("pad refresh failed res=%08x", res);
    }
}

static int sceIoReadPatched(SceUID fd, void *data, SceSize size) {
    if (fd == FAKE_UID) {
        sceDisplayWaitVblankStart();

        int len = 0;
        char log_packet[16];
        memset(log_packet, 0, sizeof(log_packet));

        if (g_init_mode == 0) {
            const char response[3] = { '1', '1', '\0' };
            copyResponseBytes(data, size, response, 3);
            memcpy(log_packet, response, 3);
            len = 3;
            debugLog("io read stage0 size=%u bytes=%02x %02x %02x text='%s' len=%d",
                     size, ((unsigned char *)data)[0], ((unsigned char *)data)[1], ((unsigned char *)data)[2], log_packet, len);
            g_init_mode++;
        } else if (g_init_mode == 1) {
            SceIoStat stat;
            memset(&stat, 0, sizeof(SceIoStat));
            int infected_mode = sceIoGetstat("ms0:/PSP/PLUGINS/resistance_infected.bin", &stat) >= 0 ||
                                sceIoGetstat("ms0:/seplugins/resistance_infected.bin", &stat) >= 0;
            char response[3] = { '2', infected_mode ? '1' : '0', '\0' };
            copyResponseBytes(data, size, response, 3);
            memcpy(log_packet, response, 3);
            len = 3;
            debugLog("io read stage1 size=%u infected=%d bytes=%02x %02x %02x text='%s' len=%d",
                     size, infected_mode, ((unsigned char *)data)[0], ((unsigned char *)data)[1], ((unsigned char *)data)[2], log_packet, len);
            g_init_mode++;
        } else {
            char response[14];
            u16 buttons;
            refreshPadForUsbPacket();
            buttons = convertButtons(g_pad.Buttons);

            response[0] = '0';
            writeHex4(&response[1], buttons);
            writeHex2(&response[5], PAD_RX(&g_pad));
            writeHex2(&response[7], PAD_RY(&g_pad));
            writeHex2(&response[9], g_pad.Lx);
            writeHex2(&response[11], g_pad.Ly);
            response[13] = '\0';

            copyResponseBytes(data, size, response, 14);
            memcpy(log_packet, response, 14);
            len = 14;

            g_packet_log_count++;
            if (g_packet_log_count <= 30 || (g_packet_log_count % 60) == 0) {
                debugLog("io read packet#%d size=%u buttons=%08x ps3=%04x rx=%02x ry=%02x lx=%02x ly=%02x bytes=%02x %02x %02x text='%s' len=%d",
                         g_packet_log_count,
                         size,
                         g_pad.Buttons,
                         buttons,
                         PAD_RX(&g_pad),
                         PAD_RY(&g_pad),
                         g_pad.Lx,
                         g_pad.Ly,
                         ((unsigned char *)data)[0],
                         ((unsigned char *)data)[1],
                         ((unsigned char *)data)[2],
                         log_packet,
                         len);
            }
        }

        return len;
    }

    return sceIoRead(fd, data, size);
}

static int sceIoWritePatched(SceUID fd, const void *data, SceSize size) {
    if (fd == FAKE_UID) {
        char preview[33];
        int n = size < 32 ? size : 32;
        memset(preview, 0, sizeof(preview));
        if (data && n > 0)
            memcpy(preview, data, n);
        debugLog("io write fake size=%u data='%s'", size, preview);
        return size;
    }

    return sceIoWrite(fd, data, size);
}

static int sceIoClosePatched(SceUID fd) {
    if (fd == FAKE_UID) {
        if (g_fake_open_count > 0)
            g_fake_open_count--;
        debugLog("io close fake open_count=%d", g_fake_open_count);
        return 0;
    }

    return sceIoClose(fd);
}

static int sceIoDevctlPatched(const char *dev, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen) {
    debugLog("devctl dev='%s' cmd=%08x inlen=%d outlen=%d in=%08x out=%08x",
             dev ? dev : "NULL", cmd, inlen, outlen, (u32)indata, (u32)outdata);

    if (cmd == 0x03415001) {
        g_usb_callback_args[0] = 0;
        g_usb_callback_args[1] = 0x81;
        g_usb_started = 1;
        g_usb_activated = 1;

        if (indata && inlen >= 4) {
            SceUID callback_thread = *(SceUID *)indata;
            int res = sceKernelStartThread(callback_thread, sizeof(g_usb_callback_args), g_usb_callback_args);
            debugLog("devctl connect callback thread=%08x res=%08x args=%08x,%08x", callback_thread, res, g_usb_callback_args[0], g_usb_callback_args[1]);
            return res;
        }

        debugLog("devctl connect no callback thread");
        return 0;
    } else if (cmd == 0x03415002) {
        g_usb_started = 1;
        g_usb_activated = 1;
        debugLog("devctl ack/start cmd 03415002");
        return 0;
    } else if (cmd == 0x03435005) {
        if (outdata)
            strcpy((char *)outdata, FAKE_DEVNAME);
        debugLog("devctl get devname -> %s", outdata ? (char *)outdata : "NULL");
        return 0;
    }

    int res = sceIoDevctl(dev, cmd, indata, inlen, outdata, outlen);
    debugLog("devctl passthrough cmd=%08x res=%08x", cmd, res);
    return res;
}

static int sceUsbStartPatched(const char *driverName, int size, void *args) {
    g_usb_started = 1;
    debugLog("usb start driver='%s' size=%d args=%08x", driverName ? driverName : "NULL", size, (u32)args);
    return 0;
}

static int sceUsbStopPatched(const char *driverName, int size, void *args) {
    g_usb_started = 0;
    g_usb_activated = 0;
    debugLog("usb stop driver='%s' size=%d args=%08x", driverName ? driverName : "NULL", size, (u32)args);
    return 0;
}

static int sceUsbActivatePatched(u32 pid) {
    g_usb_started = 1;
    g_usb_activated = 1;
    debugLog("usb activate pid=%08x", pid);
    return 0;
}

static int sceUsbDeactivatePatched(u32 pid) {
    g_usb_activated = 0;
    debugLog("usb deactivate pid=%08x", pid);
    return 0;
}

static int sceUsbGetStatePatched(void) {
    int state;
    if (g_usb_started || g_usb_activated || g_init_mode > 0 || g_fake_open_count > 0)
        state = FAKE_USB_STATE;
    else
        state = PSP_USB_CABLE_CONNECTED;

    debugLog("usb getState -> %08x started=%d activated=%d init=%d open=%d", state, g_usb_started, g_usb_activated, g_init_mode, g_fake_open_count);
    return state;
}

static int sceUsbGetDrvStatePatched(const char *driverName) {
    int state = (g_usb_started || g_usb_activated || g_init_mode > 0 || g_fake_open_count > 0) ? FAKE_USB_DRV_STARTED : FAKE_USB_DRV_STOPPED;
    debugLog("usb getDrvState driver='%s' -> %d", driverName ? driverName : "NULL", state);
    return state;
}

static int sceUsbWaitStatePatched(u32 state, s32 waitmode, u32 *timeout) {
    g_usb_started = 1;
    g_usb_activated = 1;
    debugLog("usb waitState state=%08x waitmode=%d timeout=%08x -> 0", state, waitmode, (u32)timeout);
    return 0;
}

static int sceUsbWaitCancelPatched(void) {
    debugLog("usb waitCancel -> 0");
    return 0;
}

static int PatchResistanceModule(const SceKernelModuleInfo *info) {
    int patched = 0;

    debugLog("patch module name='%s' text=%08x size=%08x", info->name, info->segmentaddr[0], info->segmentsize[0]);

    patched += patchImportByNid(info, "sceCtrl",          NID_sceCtrlPeekBufferPositive, sceCtrlPeekBufferPositivePatched);
    patched += patchImportByNid(info, "sceCtrl",          NID_sceCtrlReadBufferPositive, sceCtrlReadBufferPositivePatched);
    patched += patchImportByNid(info, "IoFileMgrForUser", NID_sceIoOpen,                 sceIoOpenPatched);
    patched += patchImportByNid(info, "IoFileMgrForUser", NID_sceIoRead,                 sceIoReadPatched);
    patched += patchImportByNid(info, "IoFileMgrForUser", NID_sceIoWrite,                sceIoWritePatched);
    patched += patchImportByNid(info, "IoFileMgrForUser", NID_sceIoClose,                sceIoClosePatched);
    patched += patchImportByNid(info, "IoFileMgrForUser", NID_sceIoDevctl,               sceIoDevctlPatched);
    patched += patchImportByNid(info, "sceUsb",           NID_sceUsbStart,               sceUsbStartPatched);
    patched += patchImportByNid(info, "sceUsb",           NID_sceUsbStop,                sceUsbStopPatched);
    patched += patchImportByNid(info, "sceUsb",           NID_sceUsbActivate,            sceUsbActivatePatched);
    patched += patchImportByNid(info, "sceUsb",           NID_sceUsbDeactivate,          sceUsbDeactivatePatched);
    patched += patchImportByNid(info, "sceUsb",           NID_sceUsbGetState,            sceUsbGetStatePatched);
    patched += patchImportByNid(info, "sceUsb",           NID_sceUsbGetDrvState,         sceUsbGetDrvStatePatched);
    patched += patchImportByNid(info, "sceUsb",           NID_sceUsbWaitState,           sceUsbWaitStatePatched);
    patched += patchImportByNid(info, "sceUsb",           NID_sceUsbWaitCancel,          sceUsbWaitCancelPatched);

    sceKernelDcacheWritebackAll();
    sceKernelIcacheClearAll();

    debugLog("patch total=%d", patched);
    return patched;
}

static int TryPatchOnce(void) {
    SceUID modules[64];
    SceKernelModuleInfo info;
    int count = 0;

    int res = sceKernelGetModuleIdList(modules, sizeof(modules), &count);
    if (res < 0) {
        debugLog("module list failed res=%08x", res);
        return 0;
    }

    for (int i = 0; i < count; i++) {
        memset(&info, 0, sizeof(info));
        info.size = sizeof(SceKernelModuleInfo);

        if (sceKernelQueryModuleInfo(modules[i], &info) < 0)
            continue;

        if (strcmp(info.name, "Resistance") == 0) {
            debugLog("found Resistance module uid=%08x", modules[i]);
            int patched = PatchResistanceModule(&info);
            if (patched > 0) {
                g_patched = 1;
                return patched;
            }
        }
    }

    return 0;
}

static int PatchThread(SceSize args, void *argp) {
    debugLog("patch thread start");

    int emu = sceIoDevctl("kemulator:", EMULATOR_DEVCTL__IS_EMULATOR, NULL, 0, NULL, 0);
    debugLog("kemulator check res=%08x", emu);
    if (emu != 0)
        return 0;

    for (int attempt = 0; attempt < 300 && !g_patched; attempt++) {
        int patched = TryPatchOnce();
        if (patched > 0) {
            debugLog("patch success attempt=%d patched=%d", attempt, patched);
            return 0;
        }

        if ((attempt % 30) == 0)
            debugLog("patch waiting attempt=%d", attempt);

        sceKernelDelayThread(100000);
    }

    debugLog("patch thread exit patched=%d", g_patched);
    return 0;
}

int module_start(SceSize argc, void *argp) {
    debugLogOnceInit();

    SceUID thid = sceKernelCreateThread("res_patch_thread", PatchThread, 0x18, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    debugLog("create patch thread thid=%08x", thid);
    if (thid >= 0) {
        int res = sceKernelStartThread(thid, 0, NULL);
        debugLog("start patch thread res=%08x", res);
    }

    return 0;
}
