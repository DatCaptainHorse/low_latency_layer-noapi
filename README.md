# low_latency_layer

> [!IMPORTANT]
> **As of v0.2.0, this layer is opt-in.** Set `LOW_LATENCY_LAYER=1` in your environment to enable it globally. Alternatively, this environment variable may be added to per-game Steam launch options to enable it on a case-by-case basis.

A C++23 implicit Vulkan layer that reduces click-to-photon latency.

# Dependencies

- [CMake](https://cmake.org): A cross-platform, open-source build system generator.
- [Vulkan Headers](https://github.com/KhronosGroup/Vulkan-Headers): Vulkan header files and API registry.
- [Vulkan Utility Libraries](https://github.com/KhronosGroup/Vulkan-Utility-Libraries): Library to share code across various Vulkan repositories.

# Building from Source and Installation

Clone this repo.

```
    $ git clone https://github.com/DatCaptainHorse/low_latency_layer-noapi
    $ cd low_latency_layer
```

Create an out-of-tree build directory (creatively we'll use 'build') and install.

> ⚠️ **WARNING:** You are likely going to have to install your distro's `vulkan-headers`, `vulkan-utility-libraries`, and possibly even `cmake` packages before proceeding. If you see an error here their absence is almost certainly the reason.

```
    $ cmake -B build ./
    $ cd ./build
    $ sudo make install
```

# Usage and Configuration

Provided the layer was enabled with `LOW_LATENCY_LAYER=1`, most Vulkan titles should just.. work.

As long as your GPU supports `VK_EXT_present_timing` and generally modern Vulkan.. yep, no Reflex, no Anti-Lag, no XeLL or other vendor-junk.

Also there's a hotkey (for Linux only) `Shift + F10` that toggles layer on/off as wanted. Give it a go!

| Variable | Description |
| :--- | :--- |
| `LOW_LATENCY_LAYER` | Expose to enable the layer. |
| `LOW_LATENCY_LAYER_MODE` | `off,drain,deadline` - defaults to `auto` which picks `deadline` if `VK_EXT_present_timing` is available. |
| `LOW_LATENCY_LAYER_FPS_LIMIT` | Allow limiting FPS, defaults to 0 (uncapped) |
| `LOW_LATENCY_LAYER_MARGIN_US` | The amount of slack allowed for timing in microseconds, defaults to 1000. |
| `LOW_LATENCY_LAYER_NO_PRESENT_TIMING` | Force-disable `VK_EXT_present_timing` even if supported. |
| `LOW_LATENCY_LAYER_QUEUE_DEPTH` | How many frames can wait in queue, defaults to 0 which will lower FPS throughput, set to 1 if you want more FPS (at cost of latency). |
| `LOW_LATENCY_LAYER_NO_TIMESTAMPS` | Force-disable timestamp query injections, which cause a crash on exit in certain cases (harmless but annoying?). |
| `LOW_LATENCY_LAYER_HOTKEY` | Disable or change default hotkey, by default F10. |
| `LOW_LATENCY_LAYER_START_DISABLED` | The layer won't be active until toggled on by hotkey. |
| `LOW_LATENCY_LAYER_DEBUG` | Enable some debug logging and timing info dumps to stderr. |
| `DISABLE_LOW_LATENCY_LAYER` | Expose to disable the layer. This takes precedence over the `LOW_LATENCY_LAYER` enable environment variable. |

**Steam launch options example:**
```
LOW_LATENCY_LAYER=1 %command%
```

# Credits

This would have not been possible without the original work of https://github.com/Korthos-Software/low_latency_layer

Now everyone can enjoy, with every game :)

# License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
