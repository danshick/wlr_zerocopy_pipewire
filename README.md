# wlr_zerocopy_pipewire

## Not Maintained

This was an experiment that I created while building out xdg-desktop-portal-wlr. This project is no longer maintained as xdpw is now functional, and further along than this example, including pipewire 0.3 support.

## Building

Depends on pipewire 0.2.9 (pre-release of 0.3)

    meson build
    ninja -C build

## Running

    ./build/wlr_cpu_pipewire

## Tooling

In order to test this, it is recommended that you use gstreamer, as shown in the test_me script. That requires gstreamer, pipewire 0.2.9 built with the gstreamer plugins, and gstreamer-plugins-good.
