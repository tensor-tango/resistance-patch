# Switch PPSSPP adaptation notes

These notes are for the `switch-safe` branch.

## Why this branch exists

The plugin works on desktop PPSSPP, but the m4xw Switch standalone port behaves differently enough that the Resistance Plus handshake can reach the fake USB path without the game fully switching into Plus mode.

The current Switch log confirms:

```text
THREAD_STARTED
KEMULATOR_CHECK_OK
RESISTANCE_MODULE_FOUND
USB_START_CALLED
USB_ACTIVATE_CALLED
DEVCTL_REGISTER_CALLED
DEVCTL_BIND_CALLED
FAKE_USB_OPEN_CALLED
SEND_PLUS_ACTIVATE_PACKET
```

So PRX loading, module discovery, import patching, USB start, devctl bind, fake usbpspcm0 open, and the first Plus packet are all reached.

## m4xw / Switch-specific assumptions

The Switch standalone build keeps PPSSPP files under `/switch/ppsspp/`. For the plugin, the expected memory-stick-relative path is still:

```text
PSP/PLUGINS/resistance_remastered_ppsspp/
```

The plugin should avoid relying on behavior that desktop PPSSPP happens to tolerate but Switch may not:

1. Do not blank normal PSP input while diagnosing Switch.
2. Avoid `snprintf()` for the fake USB protocol and debug logging.
3. Return exact payload sizes for the two handshake packets.
4. Prefer static marker logs or manual formatting.
5. Keep save states out of testing; boot the game fresh.

## Code changes to make in `main.c`

### 1. Handshake should return 2 bytes, not 3

Current desktop-tolerated behavior:

```c
snprintf((char *)data, size, "%1d%1d", 1, 1);
return 3;
```

Switch test behavior:

```c
((char *)data)[0] = '1';
((char *)data)[1] = '1';
return 2;
```

For infected mode:

```c
((char *)data)[0] = '2';
((char *)data)[1] = infected_mode ? '1' : '0';
return 2;
```

Reason: the game appears to expect two visible protocol bytes. Desktop PPSSPP tolerated returning the NUL terminator too; Switch may not.

### 2. Build PS3 pad packet manually

Instead of:

```c
snprintf((char *)data, size, "%1d%04x%02x%02x%02x%02x",
         0, buttons, rx, ry, lx, ly);
```

Use manual hex writing:

```c
static char hexDigit(unsigned int v) {
    v &= 0x0f;
    return v < 10 ? ('0' + v) : ('a' + v - 10);
}

static void makePadPacket(char *out, u16 buttons, unsigned int rx, unsigned int ry, unsigned int lx, unsigned int ly) {
    out[0] = '0';
    out[1] = hexDigit(buttons >> 12);
    out[2] = hexDigit(buttons >> 8);
    out[3] = hexDigit(buttons >> 4);
    out[4] = hexDigit(buttons);
    out[5] = hexDigit(rx >> 4);
    out[6] = hexDigit(rx);
    out[7] = hexDigit(ry >> 4);
    out[8] = hexDigit(ry);
    out[9] = hexDigit(lx >> 4);
    out[10] = hexDigit(lx);
    out[11] = hexDigit(ly >> 4);
    out[12] = hexDigit(ly);
    out[13] = 0;
}
```

Then return `14`, matching the desktop code.

### 3. Keep normal PSP input alive on Switch

Use:

```c
static int sceCtrlReadBufferPositivePatched(SceCtrlData *pad_data, int count) {
    int res = sceCtrlReadBufferPositive(pad_data, count);
    memcpy(&g_pad, pad_data, sizeof(SceCtrlData));
    return res;
}
```

Do not clear `Buttons`, `Lx`, `Ly`, `Rsrv[0]`, or `Rsrv[1]` in the Switch diagnostic branch.

### 4. If Plus still does not appear

Next suspects:

1. The USB register callback emulation:

```c
sceKernelStartThread(*(u32 *)indata, sizeof(conn), &conn);
```

2. The exact `conn[1]` value currently set to `0x81`.
3. An internal Resistance Plus state flag that may need to be forced by CWCheat.

## Test checklist

1. Build GitHub Actions from branch `switch-safe`.
2. Copy fresh `resistance_remastered.prx` to Switch.
3. Delete old logs.
4. Boot PPSSPP fresh, not from save state.
5. Start the game normally.
6. Move left stick, right stick, and press buttons.
7. Save/upload `resistance_switch_debug.txt`.
