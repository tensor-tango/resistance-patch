/*
  Remastered Controls: Resistance - PPSSPP port

  Switch-safe branch:
  - Keeps Resistance's built-in Resistance Plus / PS3 controller path by faking usbpspcm0:.
  - Does not clear normal PSP input after g_init_mode == 2. This prevents controls from dying on PPSSPP builds where the USB Plus path does not fully activate.
*/

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspusb.h>
#include <pspdisplay.h>

#include <stdio.h>
#include <string.h>

#define EMULATOR_DEVCTL__IS_EMULATOR 0x00000003

#define FAKE_DEVNAME      "usbpspcm0:"
#define FAKE_UID          0x12345678

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

PSP_MODULE_INFO("ResistanceSwitchSafe", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

static SceCtrlData g_pad;
static int g_init_mode = 0;
static int g_patched = 0;

void _exit(int status) {
    sceKernelExitDeleteThread(status);
    while (1) {
        sceKernelDelayThread(1000000);
    }
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
                    patched++;
                }
            }
        }
    }

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

static int sceCtrlReadBufferPositivePatched(SceCtrlData *pad_data, int count) {
    int res = sceCtrlReadBufferPositive(pad_data, count);

    memcpy(&g_pad, pad_data, sizeof(SceCtrlData));

    return res;
}

static SceUID sceIoOpenPatched(const char *file, int flags, SceMode mode) {
    if (strcmp(file, FAKE_DEVNAME) == 0)
        return FAKE_UID;

    return sceIoOpen(file, flags, mode);
}

static int sceIoReadPatched(SceUID fd, void *data, SceSize size) {
    if (fd == FAKE_UID) {
        sceDisplayWaitVblankStart();

        int len = 0;

        if (g_init_mode == 0) {
            snprintf((char *)data, size, "%1d%1d", 1, 1);
            len = 3;
            g_init_mode++;
        } else if (g_init_mode == 1) {
            SceIoStat stat;
            memset(&stat, 0, sizeof(SceIoStat));
            int infected_mode = sceIoGetstat("ms0:/PSP/PLUGINS/resistance_infected.bin", &stat) >= 0 ||
                                sceIoGetstat("ms0:/seplugins/resistance_infected.bin", &stat) >= 0;

            snprintf((char *)data, size, "%1d%1d", 2, infected_mode);
            len = 3;
            g_init_mode++;
        } else {
            snprintf((char *)data, size, "%1d%04x%02x%02x%02x%02x",
                     0,
                     convertButtons(g_pad.Buttons),
                     g_pad.Rsrv[0],
                     g_pad.Rsrv[1],
                     g_pad.Lx,
                     g_pad.Ly);
            len = 14;
        }

        return len;
    }

    return sceIoRead(fd, data, size);
}

static int sceIoWritePatched(SceUID fd, const void *data, SceSize size) {
    if (fd == FAKE_UID)
        return size;

    return sceIoWrite(fd, data, size);
}

static int sceIoClosePatched(SceUID fd) {
    if (fd == FAKE_UID)
        return 0;

    return sceIoClose(fd);
}

static int sceIoDevctlPatched(const char *dev, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen) {
    if (cmd == 0x03415001) {
        u32 conn[2];
        conn[0] = 0;
        conn[1] = 0x81;
        return sceKernelStartThread(*(u32 *)indata, sizeof(conn), &conn);
    } else if (cmd == 0x03415002) {
        return 0;
    } else if (cmd == 0x03435005) {
        strcpy((char *)outdata, FAKE_DEVNAME);
        return 0;
    }

    return sceIoDevctl(dev, cmd, indata, inlen, outdata, outlen);
}

static int sceUsbStartPatched(const char *driverName, int size, void *args) {
    return 0;
}

static int sceUsbStopPatched(const char *driverName, int size, void *args) {
    return 0;
}

static int sceUsbActivatePatched(u32 pid) {
    return 0;
}

static int sceUsbDeactivatePatched(u32 pid) {
    return 0;
}

static int PatchResistanceModule(const SceKernelModuleInfo *info) {
    int patched = 0;

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

    sceKernelDcacheWritebackAll();
    sceKernelIcacheClearAll();

    return patched;
}

static int TryPatchOnce(void) {
    SceUID modules[64];
    SceKernelModuleInfo info;
    int count = 0;

    if (sceKernelGetModuleIdList(modules, sizeof(modules), &count) < 0)
        return 0;

    for (int i = 0; i < count; i++) {
        memset(&info, 0, sizeof(info));
        info.size = sizeof(SceKernelModuleInfo);

        if (sceKernelQueryModuleInfo(modules[i], &info) < 0)
            continue;

        if (strcmp(info.name, "Resistance") == 0) {
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
    if (sceIoDevctl("kemulator:", EMULATOR_DEVCTL__IS_EMULATOR, NULL, 0, NULL, 0) != 0)
        return 0;

    for (int attempt = 0; attempt < 120 && !g_patched; attempt++) {
        int patched = TryPatchOnce();
        if (patched > 0)
            return 0;

        sceKernelDelayThread(100000);
    }

    return 0;
}

int module_start(SceSize argc, void *argp) {
    SceUID thid = sceKernelCreateThread("res_switch_thread", PatchThread, 0x18, 0x10000, PSP_THREAD_ATTR_USER, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);

    return 0;
}
