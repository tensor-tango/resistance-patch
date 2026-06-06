/*
  Remastered Controls: Resistance - PPSSPP experimental port scaffold
  Based on TheFloW's Resistance RemasteredControls logic and the PPSSPP GTA port style.

  Idea:
  - Keep Resistance's built-in Resistance Plus / PS3 controller path.
  - Fake usbpspcm0: and PS3 controller packets like the Vita/Adrenaline plugin.
  - Instead of HEN syscall patching, patch the Resistance module's import stubs by NID.

  WARNING: Experimental. Needs testing with PPSSPP and your exact Resistance build.
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

// NIDs used by the original Resistance plugin.
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

PSP_MODULE_INFO("ResistancePPSSPP", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

static SceCtrlData g_pad;
static int g_init_mode = 0;

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

// Patch an import stub so calls from the game go to our replacement.
// PSP import stubs are normally two instructions. We replace with:
//   j replacement
//   nop
static void patchStub(u32 stub_addr, void *replacement) {
    u32 f = (u32)replacement;
    _sw(0x08000000 | ((f >> 2) & 0x03FFFFFF), stub_addr + 0x00); // j replacement
    _sw(0x00000000, stub_addr + 0x04);                           // nop
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
        ps3_buttons |= PS3_CTRL_LEFT | PS3_CTRL_L2; // Remap weapon select
    if (psp_buttons & PSP_CTRL_DOWN)
        ps3_buttons |= PS3_CTRL_DOWN | PS3_CTRL_R3; // Remap aim
    if (psp_buttons & PSP_CTRL_RIGHT)
        ps3_buttons |= PS3_CTRL_RIGHT | PS3_CTRL_R2; // Remap weapon select
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

    if (g_init_mode == 2) {
        memcpy(&g_pad, pad_data, sizeof(SceCtrlData));

        // Once Resistance Plus is active, hide normal PSP input from the game.
        // The game should read our fake PS3 packet through usbpspcm0: instead.
        pad_data->Buttons = 0;
        pad_data->Lx = 128;
        pad_data->Ly = 128;
        pad_data->Rsrv[0] = 128;
        pad_data->Rsrv[1] = 128;
    }

    return res;
}

static SceUID sceIoOpenPatched(const char *file, int flags, SceMode mode) {
    if (strcmp(file, FAKE_DEVNAME) == 0)
        return FAKE_UID;

    return sceIoOpen(file, flags, mode);
}

static int sceIoReadPatched(SceUID fd, void *data, SceSize size) {
    if (fd == FAKE_UID) {
        // Original usbpspcm0: reads block; this keeps the game thread sane.
        sceDisplayWaitVblankStart();

        int len = 0;

        if (g_init_mode == 0) {
            // Activate Resistance Plus.
            snprintf((char *)data, size, "%1d%1d", 1, 1);
            len = 3;
            g_init_mode++;
        } else if (g_init_mode == 1) {
            // Activate infected mode if desired.
            SceIoStat stat;
            memset(&stat, 0, sizeof(SceIoStat));
            int infected_mode = sceIoGetstat("ms0:/PSP/PLUGINS/resistance_infected.bin", &stat) >= 0 ||
                                sceIoGetstat("ms0:/seplugins/resistance_infected.bin", &stat) >= 0;

            snprintf((char *)data, size, "%1d%1d", 2, infected_mode);
            len = 3;
            g_init_mode++;
        } else {
            // Fake PS3 controls.
            // PPSSPP maps right analog to Rsrv[0]/Rsrv[1] when configured.
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
    if (cmd == 0x03415001) { // Fake connection for register.
        u32 conn[2];
        conn[0] = 0;
        conn[1] = 0x81;
        return sceKernelStartThread(*(u32 *)indata, sizeof(conn), &conn);
    } else if (cmd == 0x03415002) { // Fake success for unregister.
        return 0;
    } else if (cmd == 0x03435005) { // Fake devname for bind.
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

static void CheckModules(void) {
    SceUID modules[32];
    SceKernelModuleInfo info;
    int count = 0;

    if (sceKernelGetModuleIdList(modules, sizeof(modules), &count) < 0)
        return;

    for (int i = 0; i < count; i++) {
        memset(&info, 0, sizeof(info));
        info.size = sizeof(SceKernelModuleInfo);

        if (sceKernelQueryModuleInfo(modules[i], &info) < 0)
            continue;

        if (strcmp(info.name, "Resistance") == 0) {
            PatchResistanceModule(&info);
            return;
        }
    }
}

int module_start(SceSize argc, void *argp) {
    // Only run under PPSSPP.
    if (sceIoDevctl("kemulator:", EMULATOR_DEVCTL__IS_EMULATOR, NULL, 0, NULL, 0) == 0) {
        CheckModules();
    }

    return 0;
}
