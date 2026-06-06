Working Right-Stick Camera / analog camera plugin for Resistance Retribution in PPSSPP.

Specifically for the US version of the game: `UCUS98668`.

## Desktop PPSSPP install

Unpack the plugin folder called `resistance_remastered_ppsspp`, containing `plugin.ini` and `resistance_remastered.prx`, into PPSSPP's `PSP/PLUGINS` directory, so the full path looks like:

```text
.../PPSSPP/PSP/PLUGINS/resistance_remastered_ppsspp
```

## Nintendo Switch / m4xw PPSSPP standalone install

For the standalone Switch port by m4xw, PPSSPP's files are under `/switch/ppsspp`, so install the plugin here:

```text
/switch/ppsspp/PSP/PLUGINS/resistance_remastered_ppsspp/
```

The folder must contain:

```text
plugin.ini
resistance_remastered.prx
```

Then map the right stick in PPSSPP:

```text
Settings -> Controls -> Control mapping -> Extended PSP controls
```

Bind the Switch right stick to the extended PSP right analog directions.

## Switch branch changes

This branch adapts the plugin behavior for Switch/m4xw PPSSPP:

- patches both `sceCtrlReadBufferPositive` and `sceCtrlPeekBufferPositive`;
- refreshes pad state directly before building the fake USB/PS3 controller packet;
- reads PPSSPP extended right analog bytes by `SceCtrlData` layout offsets to avoid SDK header naming differences;
- preserves the original USB packet lengths including the trailing NUL byte;
- waits longer for the Resistance game module before giving up.

## Build

Use a PSP SDK toolchain:

```bash
make clean
make
```

Copy the resulting `resistance_remastered.prx` next to `plugin.ini` in the plugin folder.

Thanks to Freakler and TheOfficialFlow.

Search terms:

- Resistance Retribution PPSSPP right analog stick
- Resistance Retribution dual analog PPSSPP
- Resistance Retribution right stick camera mod
- Resistance Retribution Remastered Controls PPSSPP
- Resistance Retribution UCUS98668 plugin
- PSP PLUGINS resistance_remastered.prx
