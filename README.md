# et-mode-app

et-mode-app is a front-end application for selecting the mode of operation
within EmComm Tools Community. It is a graphical wrapper around its command
line counterpart: `et-mode`.

1. On launch, the modes reported by `et-mode --list` are shown.
2. Selecting a mode (double-click, Enter, or **Start Mode**) runs
   `et-mode <mode-id>` in an embedded terminal. Any follow-up prompts
   (e.g. modem selection) and log output appear in that terminal.
3. **Cancel Mode** stops the terminal's process group and runs
   `et-mode none`, which stops all EmComm Tools services, then returns to
   the mode list.

Set `ET_MODE_CMD` to use a different `et-mode` executable (useful for
development).

## Build

1. Install development dependencies.

```bash
sudo apt install \
  libgtk-3-dev \
  libgtk-3-doc \
  libvte-2.91-dev
```

2. Compile.

```bash
make
```

3. Run.

```bash
./et-mode-app
```
