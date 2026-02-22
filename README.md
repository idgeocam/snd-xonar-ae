# snd-xonar-ae — Linux kernel module for ASUS XONAR AE headphone/speaker switching

A Linux kernel module that adds headphone/speaker output switching for the **ASUS XONAR AE** (PCIe) sound card.

Under Linux, the XONAR AE works out of the box via `snd-usb-audio` (it's a USB Audio Class 2.0 device behind an internal PCIe-to-USB bridge), but the output is stuck on headphones — there's no way to switch to speakers. This module fixes that.

## The problem

The XONAR AE has two physical outputs (headphones and speakers) controlled by an internal relay. Under Windows, the official ASUS driver lets you pick one or the other. Under Linux, `snd-usb-audio` handles audio playback fine, but exposes no control for the output switch — because it doesn't exist in the USB Audio spec. ASUS implemented it as a **vendor-specific USB control request** that the Linux kernel knows nothing about.

## How it was reverse-engineered

The protocol was identified by capturing USB traffic between the Windows driver and the card using Wireshark + USBPcap:

1. **Device identification**: VID `0x0B05`, PID `0x180F`, USB Audio Class 2.0
2. **Topology** (from the configuration descriptor):
   - 5 interfaces: Audio Control, 2× Playback Streaming, 1× Capture Streaming, HID
   - Output Terminal 7 (type `0x0301` — Speaker) fed by Feature Unit 13
   - 3 Clock Sources (IDs 18, 19, 22)
3. **The switching command**: Among dozens of standard UAC2 control transfers (volume, mute, sample rate), one stood out — a `SET CUR` request targeting **Output Terminal 7** with **Control Selector 0x08**.

   In the USB Audio 2.0 spec, Output Terminal control selectors only go up to `0x03` (Copy Protect, Connector, Overload). **CS=0x08 doesn't exist in the spec** — it's a vendor-specific extension by ASUS that controls the physical output relay.

### The protocol

```
SET CUR (bRequest = 0x01)
bmRequestType = 0x21 (Host→Device, Class, Interface)
wValue        = 0x0800 (CS=0x08, CN=0x00)
wIndex        = 0x0700 (Entity ID=7, Interface=0)
wLength       = 2

data = [0x01, 0x03]  →  Speakers (7.1, 8 channels)
data = [0x02, 0x03]  →  Headphones (stereo, 2 channels)
```

The current mode can be read back via the standard **Connector Control** (CS=0x02) on the same Output Terminal — the device reports `bNrChannels = 8` for speakers or `bNrChannels = 2` for headphones.

### Why a kernel module?

Several userspace approaches were attempted and all failed:

| Approach | Result |
|---|---|
| `pyusb` with `detach_kernel_driver` | Card disappears from ALSA entirely |
| Direct `ioctl(USBDEVFS_CONTROL)` | `EBUSY` — `snd-usb-audio` holds the interface |
| `USBDEVFS_DISCONNECT_CLAIM` | Control works, but driver fails to reattach properly |
| `modprobe -r snd-usb-audio` | Kills active audio streams |

The root cause: `snd-usb-audio` owns the USB interfaces and blocks all userspace access, even on endpoint 0.

**The solution**: `usb_control_msg()` called from **kernel space** on endpoint 0 does **not** require interface ownership. A kernel module can send the vendor-specific control transfer while `snd-usb-audio` continues operating normally. Audio playback is uninterrupted during the switch.

## Installation

### Prerequisites

```bash
# Arch Linux
sudo pacman -S linux-headers

# Debian/Ubuntu
sudo apt install linux-headers-$(uname -r)

# Fedora
sudo dnf install kernel-devel
```

### Build and install

```bash
git clone https://github.com/idgeocam/snd-xonar-ae.git
cd snd-xonar-ae
make
sudo mkdir -p /lib/modules/$(uname -r)/extra
sudo cp snd-xonar-ae.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
```

### Load

```bash
sudo modprobe snd-xonar-ae
```

### Load automatically at boot

```bash
echo "snd-xonar-ae" | sudo tee /etc/modules-load.d/xonar-ae.conf
```

### After a kernel update

```bash
cd snd-xonar-ae
make clean && make
sudo cp snd-xonar-ae.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
sudo modprobe snd-xonar-ae
```

## Usage

```bash
# Check current output
cat /sys/module/snd_xonar_ae/parameters/output

# Switch to speakers
echo speakers | sudo tee /sys/module/snd_xonar_ae/parameters/output

# Switch to headphones
echo headphones | sudo tee /sys/module/snd_xonar_ae/parameters/output
```

### Sudoers rule (optional, for scripts/GUIs)

To allow switching without a password prompt:

```bash
echo "$USER ALL=(ALL) NOPASSWD: /usr/bin/tee /sys/module/snd_xonar_ae/parameters/output" \
  | sudo tee /etc/sudoers.d/xonar-ae
sudo chmod 440 /etc/sudoers.d/xonar-ae
```

## Waybar integration

A toggle widget for Waybar (Hyprland, Sway, etc.):

**`~/.config/waybar/scripts/xonar-toggle.sh`**:
```bash
#!/bin/bash
SYSFS="/sys/module/snd_xonar_ae/parameters/output"

case "$1" in
    toggle)
        current=$(cat "$SYSFS" 2>/dev/null)
        if [ "$current" = "speakers" ]; then
            echo headphones | sudo tee "$SYSFS" > /dev/null
        else
            echo speakers | sudo tee "$SYSFS" > /dev/null
        fi
        ;;
esac

current=$(cat "$SYSFS" 2>/dev/null)
if [ "$current" = "speakers" ]; then
    echo '{"text": "󰓃", "tooltip": "Output: Speakers", "class": "speakers"}'
else
    echo '{"text": "󰋋", "tooltip": "Output: Headphones", "class": "headphones"}'
fi
```

**Waybar config**:
```json
"custom/xonar": {
    "exec": "~/.config/waybar/scripts/xonar-toggle.sh",
    "return-type": "json",
    "interval": 5,
    "on-click": "~/.config/waybar/scripts/xonar-toggle.sh toggle",
    "format": "{}"
}
```

## How the module works

The module is intentionally minimal (~200 lines). Here's what it does:

1. **`module_init`**: Finds the XONAR AE by iterating USB devices (`usb_for_each_dev`) and matching VID:PID `0B05:180F`. Reads the current output state.

2. **Sysfs parameter**: Exposes `/sys/module/snd_xonar_ae/parameters/output` as a readable/writable parameter via `module_param_cb`. Reading it queries the device; writing triggers the switch.

3. **`xonar_switch()`**: Sends a `usb_control_msg()` SET CUR on endpoint 0. This is the key insight — kernel-space control transfers on EP0 bypass interface ownership, so `snd-usb-audio` is unaffected and audio keeps playing.

4. **`xonar_get_status()`**: Sends a GET CUR for the standard Connector control (CS=0x02) on Output Terminal 7. The response's `bNrChannels` field tells us which output is active (8 = speakers, 2 = headphones).

5. **`module_exit`**: Releases the USB device reference. That's it.

## Hardware notes

- The XONAR AE is internally a USB Audio Class 2.0 device connected via a PCIe-to-USB bridge. It may not appear in `lsusb` output but is visible via `lsusb -t` and `cat /proc/asound/card*/usbid`.
- The headphone/speaker switch is a **hardware relay** — you cannot have both outputs active simultaneously. This is a physical limitation of the card.
- The device supports 7.1 surround (8 channels) via speakers, and stereo (2 channels) via headphones.

## Compatibility

Tested on:
- Arch Linux, kernel 6.18.x
- ASUS XONAR AE (VID:PID `0B05:180F`)

Should work on any Linux distribution with kernel ≥ 5.x. May also work with other XONAR USB-based cards if they use the same vendor-specific control (untested — PRs welcome).

## License

GPL v2 — same as the Linux kernel.
