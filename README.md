# Spotlight

A lightweight fullscreen X11 spotlight + zoom tool for Linux (X11).

Inspired by [boomer by Tsoding](https://github.com/tsoding/boomer). Special thanks to Tsoding for his great content!

---

## Controls

| Action                    | Control            |
| ------------------------- | ------------------ |
| Zoom in                   | Scroll Up          |
| Zoom out                  | Scroll Down        |
| Increase spotlight radius | Ctrl + Scroll Up   |
| Decrease spotlight radius | Ctrl + Scroll Down |
| Toggle spotlight          | `F`                |
| Reset zoom                | `0`                |
| Quit                      | `Q` or `Esc`       |

---

## Dependencies

Ubuntu / Debian:

```bash
sudo apt install libx11-dev libxrandr-dev libgl-dev libglew-dev
```

Arch:

```bash
sudo pacman -S libx11 libxrandr mesa glew
```

Fedora:

```bash
sudo dnf install libX11-devel libXrandr-devel mesa-libGL-devel glew-devel
```

Void:

```bash
sudo xbps-install -S libX11-devel libXrandr-devel MesaLib-devel glew-devel
```

---

## Notes

Currently supports:

- Linux
- X11
- GLX/OpenGL

**Wayland support is not implemented.**

**The application currently captures a static snapshot of the desktop at startup and renders it with zoom and spotlight effects.**

---

## License

MIT License

Feel free to do whatever you want!
