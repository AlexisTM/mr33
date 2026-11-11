# OpenWrt for the Cisco Meraki MR33

Package development for the Meraki MR33 (codename *Stinkbug*) on OpenWrt 25.12.6, using the official SDK.

| Path | What |
|---|---|
| `sdk/` | Official OpenWrt 25.12.6 SDK for `ipq40xx/generic` (GCC 14.3, musl). Builds packages and kmods against the release kernel. |
| `src/` | Clone of `openwrt.git`, branch `mr33` at `v25.12.6`, the exact commit the SDK was built from. The SDK's `base` feed points at `src/package`. Not tracked here: changes to it are kept as patches in `patches/openwrt/`. Also the reference for DTS and board files. |
| `patches/openwrt/` | Patches for `src/`, applied with `git -C src am` |
| `config/` | Fragments appended to `sdk/.config` |
| `client/` | Python SDK and CLI for the spectrum analyzer |
| `packages/` | Local feed `custom` for your own packages. |
| `sdk-dl/` | SDK tarball plus the signed `sha256sums` it was checked against. |

The SDK doesn't produce firmware images. The MR33 runs the official 25.12.6 image, and your features are packages installed on top of it with `apk`.

Device wiki (flashing PDF, the patched `ubootwrite`, `mr33-uboot.bin`): <https://openwrt.org/toh/meraki/mr33>

## Hardware

From the upstream commit that added the board (`git -C src show 4943afd781`) and the DTS:

| | |
|---|---|
| SoC | Qualcomm IPQ4029, 4x Cortex-A7 |
| RAM | 256 MiB DDR3L |
| Flash | 128 MiB SLC NAND (about 106 MiB usable in UBI) |
| Ethernet | 1x GbE (AR8035 PHY), PoE powered, single `lan` port |
| Wi-Fi | IPQ4029 2.4 GHz 2x2 n, IPQ4029 5 GHz 2x2 ac, QCA9887 1x1 dual-band ac on PCIe |
| LEDs | RGBW status LED via TI LP5562 on I2C, orange fault LED, 2 RJ45 LEDs |
| Other | Reset button, TI CC2650 BLE on the second UART (unused by OpenWrt), AT24C64 EEPROM |
| Serial | 1x4 header, 3.3 V TTL, 115200 8N1. Pinout: VCC (white arrow), RX, TX, GND |

OpenWrt identifiers: target `ipq40xx`, subtarget `generic`, profile `meraki_mr33`, DT compatible `meraki,mr33`, package arch `arm_cortex-a7_neon-vfpv4`.

## Firmware

From <https://downloads.openwrt.org/releases/25.12.6/targets/ipq40xx/generic/>, checksums from the signed `sha256sums` in `sdk-dl/`:

| File | sha256 |
|---|---|
| `openwrt-25.12.6-ipq40xx-generic-meraki_mr33-squashfs-sysupgrade.bin` | `b8f1f6f91a376f2aa3c6737b3e79190b6a835da0f4dadc99aced6267ecd1e2af` |
| `openwrt-25.12.6-ipq40xx-generic-meraki_mr33-initramfs-uImage.itb` | `b846484919cb7dbc0fe02cf21e8b2f6468b0e9a4b3c73388d0672daa913aefa9` |

The sysupgrade image is the one you flash: a tar holding the FIT kernel (written to the UBI volume `part.safe`) and the squashfs root. The initramfs image runs entirely from RAM, for testing from a bootloader that can TFTP-boot it.

### First install (from Meraki firmware)

You need a 3.3 V USB-serial adapter (5 V or RS-232 levels will damage the board), an Ethernet cable, PoE power, and a private LAN with no internet connection (the upstream instructions insist on that).

This is the procedure from the commit that added the board. The wiki PDF has the full version and links to the two tools it needs:

1. Open the case and connect serial: RX, TX, GND. Leave VCC unconnected, the board has its own power.
2. On the PC run the patched `ubootwrite` (it needs little-endian support, the stock one won't work):
   `ubootwrite.py --write=mr33-uboot.bin`
3. Power on the MR33 with Ethernet and serial attached. The tool interrupts boot and pushes a modified U-Boot with Ethernet enabled into RAM. It takes 250 to 300 seconds. The low-profile header pins make bad contact easy; if the tool fails oddly, watch the console in a terminal program to see where it stops.
4. The modified U-Boot listens on `192.168.1.1`. Give your PC a static address in `192.168.1.0/24` and upload the image in binary mode:

   ```
   tftp 192.168.1.1
   binary
   put openwrt-25.12.6-ipq40xx-generic-meraki_mr33-squashfs-sysupgrade.bin
   ```

5. Wait for it to reboot, then `ssh root@192.168.1.1`.

`ubootwrite` only loads the modified U-Boot into RAM, so it's gone after the reboot and the stock Meraki U-Boot boots OpenWrt from `part.safe`. If an image ever fails to boot, recovery is this same serial procedure. Read the wiki page and PDF before opening the unit: they host the tool downloads and any caveats added since the board landed in 2018.

### Upgrading a unit already on OpenWrt

```sh
scp -O openwrt-25.12.6-ipq40xx-generic-meraki_mr33-squashfs-sysupgrade.bin root@192.168.1.1:/tmp/
ssh root@192.168.1.1 sysupgrade -v /tmp/openwrt-25.12.6-ipq40xx-generic-meraki_mr33-squashfs-sysupgrade.bin
```

Add `-n` to drop the existing config; you need it when coming from an old swconfig-era OpenWrt. `scp -O` forces the legacy protocol because OpenWrt's Dropbear has no SFTP server.

## SDK setup

Already done in this directory. To recreate it:

```sh
B=https://downloads.openwrt.org/releases/25.12.6/targets/ipq40xx/generic
mkdir sdk-dl && cd sdk-dl
curl -fO $B/sha256sums -fO $B/sha256sums.asc \
     -fO $B/openwrt-sdk-25.12.6-ipq40xx-generic_gcc-14.3.0_musl_eabi.Linux-x86_64.tar.zst
git clone --depth 1 https://git.openwrt.org/keyring.git /tmp/keyring
gpg --import /tmp/keyring/gpg/*.asc && gpg --verify sha256sums.asc sha256sums
grep sdk sha256sums | sha256sum -c -
cd .. && tar --zstd -xf sdk-dl/openwrt-sdk-*.tar.zst
mv openwrt-sdk-25.12.6-ipq40xx-generic_gcc-14.3.0_musl_eabi.Linux-x86_64 sdk
```

Then, in `sdk/`:

- `dl` is a symlink to `../src/dl`, so source tarballs are shared with `src/`.
- `feeds.conf` is `feeds.conf.default` with two changes. The `base` line becomes `src-link --root=package base /home/alexis/tools/mr33/src`, and a `src-link custom /home/alexis/tools/mr33/packages` line is added. The other feeds stay pinned to the release's commits.

```sh
./scripts/feeds update -a && ./scripts/feeds install -a
make defconfig
```

**Never run `./scripts/feeds install` inside `src/`.** It creates `src/package/feeds/`, which the SDK's `base` feed then picks up, shadowing the real feeds. If it happens: `rm -rf src/feeds src/package/feeds`, then in `sdk/` run `./scripts/feeds uninstall -a && ./scripts/feeds update -a && ./scripts/feeds install -a`.

Host packages (Ubuntu 26.04):

```sh
sudo apt install build-essential clang flex bison g++ gawk gettext git \
  libncurses-dev libssl-dev python3-dev python3-setuptools rsync swig \
  unzip zlib1g-dev file wget zstd
```

## Building a package

Always build by package name:

```sh
cd sdk
make package/<name>/compile -j16
```

Never run a bare `make`: the SDK selects every package by default and would build all of them. The result lands in `bin/packages/arm_cortex-a7_neon-vfpv4/<feed>/`, along with any dependencies the SDK had to build. As a reference, `horst` with its ncurses, zlib and libnl-tiny dependencies took about a minute.

When something fails: `make package/<name>/compile V=s -j1`. To start a package over: `make package/<name>/clean`.

## Installing on the device

Quick, one file:

```sh
scp -O bin/packages/arm_cortex-a7_neon-vfpv4/<feed>/<name>-*.apk root@192.168.1.1:/tmp/
ssh root@192.168.1.1 apk add --allow-untrusted /tmp/<name>-*.apk
```

`--allow-untrusted` is needed because individual `.apk` files aren't signed; only the repository index is.

As a trusted repository, which also resolves dependencies:

```sh
make package/index                          # signs packages.adb with sdk/private-key.pem
cd bin/packages/arm_cortex-a7_neon-vfpv4 && python3 -m http.server 8000
```

On the device, once:

```sh
scp -O sdk/public-key.pem root@192.168.1.1:/etc/apk/keys/mr33-sdk.pem   # from the PC
echo 'http://<pc-ip>:8000/custom/packages.adb' >> /etc/apk/repositories.d/customfeeds.list
apk update && apk add <name>
```

Add a line per feed directory you serve (`custom`, `base`, `packages`...).

## Adding features

### Your own package

One directory per package under `packages/`, each with an OpenWrt package Makefile:

```sh
cd sdk
./scripts/feeds update custom && ./scripts/feeds install -p custom -a
make package/<name>/compile -j16
```

Re-run the feeds line whenever you add a package or change its dependencies. Makefile reference: <https://openwrt.org/docs/guide-developer/packages>. Small in-tree examples to copy from: `src/package/utils/` and `src/package/network/services/`.

### Changing a core package

Core packages (hostapd, ath10k-ct, base-files...) build from `src/package`. Edit them in `src/` and bump `PKG_RELEASE` in the package Makefile, so `apk` sees your build as newer than the official one and doesn't swap it back on `apk upgrade`. Keep the change in this repo, not in `src/`: commit it in `src/`, export it with `git -C src format-patch -1 -o ../patches/openwrt`, and drop the commit again with `git -C src reset HEAD~1` if you want `src/` to stay on the tag. After adding a new package to `src/package`, run `./scripts/feeds update base && ./scripts/feeds install -p base <name>` in `sdk/`.

`src/` must stay on `v25.12.6`, or kmods and libraries stop matching the firmware. To move to a newer release, get the matching SDK and image, then `git -C src rebase --onto v25.12.N v25.12.6 mr33`.

### Kernel modules

`sdk/build_dir/target-arm_cortex-a7+neon-vfpv4_musl_eabi/linux-ipq40xx_generic/linux-6.12.112/` holds the release kernel's `.config` and `Module.symvers`, so kmods built here load on the official kernel. The kernel itself is fixed: check that `.config` before planning a feature that needs a kernel option.

### Spectral scan kmods

The release kernel already has `CONFIG_RELAY=y` and `CONFIG_DEBUG_FS=y`, and exports `relay_open`, so spectral scan only needs the Wi-Fi modules rebuilt. Everything needed lives in this repo; `src/` and `sdk/` stay pristine downloads:

```sh
git -C src am ../patches/openwrt/*.patch      # or: git -C src apply ../patches/openwrt/*.patch
cat config/sdk.config >> sdk/.config && make -C sdk defconfig
```

| File | What |
|---|---|
| `patches/openwrt/0001-*.patch` | Bumps `PKG_RELEASE` of mac80211 and ath10k-ct, so the rebuilt kmods win over the official ones in `apk` |
| `config/sdk.config` | `CONFIG_PACKAGE_ATH_DEBUG=y` and `CONFIG_PACKAGE_ATH_SPECTRAL=y` |

The SDK's `base` feed reads `src/package`, so the patch has to be applied in `src/` before building. `kmod-mac80211` depends on `hostapd-common`, so the first `package/mac80211/compile` also builds every hostapd variant (about 15 to 30 minutes). A parallel build can fail spuriously in a hostapd variant; running the same command again picks up where it stopped.

## Spectrum analyzer

Live, on-demand spectrum analysis in LuCI under **Status > Spectrum Analyzer**, laid out like the Meraki, Catalyst Center and Ekahau views: a density "snapshot" (frequency vs level, coloured by how often each level shows up) with average and peak hold, a waterfall, and per-channel occupancy, one panel per band. Two packages in `packages/`:

| Package | What |
|---|---|
| `spectrald` | C daemon, ubus object `spectral`. Drives ath10k spectral scan through debugfs, parses the FFT samples from the relay file and aggregates them on a frequency grid every 100 ms, one grid per band. |
| `luci-app-spectral` | The LuCI page. Band toggle 2.4 GHz / 5 GHz / both, which also sets what the radio scans. |

Scanning only runs while someone asks for it. Each `frames` call renews a lease (10 s by default); when it runs out the daemon disables spectral and removes the interface it created. Closing the tab stops everything within about 10 seconds. One session runs at a time: a viewer asking for the same settings joins it, one asking for different settings takes over.

### Modes

| Mode | Radio | What happens |
|---|---|---|
| Full spectrum (default) | idle | Adds a monitor interface `spectral<N>` and hops it through the band(s) in the widest chunks the band plan allows: 40 MHz in 2.4 GHz, 80 MHz in 5 GHz (DFS blocks included), 150 ms per chunk by default like the Meraki scanning radio. The page offers 10 to 250 ms; the daemon and the SDK take any value from 10 ms up. Nothing is transmitted. Bins scale with width (64 per 20 MHz) so every chunk has the same 344 kHz cells. A block the driver refuses is split into 20 MHz steps. |
| Channel | busy | Spectral on the operating channel of the existing interface. No channel change, clients keep working. |
| Channel | idle | Monitor interface tuned to one channel, 20/40/80 MHz. |
| Sweep | idle | Older mode: back-to-back passive scans in 20 MHz steps. Slower than full spectrum, kept for the CLI. |

No radio sees a whole band at once: like the Meraki and Catalyst scanning radios, this one captures one chunk at a time (80 MHz at most on the QCA9887; current Cisco radios grab up to 160 MHz) and the page stitches the chunks.

On this MR33 (2026-11-11): `phy0` is the QCA9887 (`ath10k_pci`, dual-band, disabled in `/etc/config/wireless`), the scanning radio. Full spectrum covers 2.4 + 5 GHz in 10 chunks: about 1.6 s per pass at the default 150 ms dwell, 450 ms at 30 ms, about 3000 to 4300 samples/s, 7 % CPU. Only phy0 sweeps well: the IPQ4029 radios need about 300 ms to resume spectral after each hop (2.5 s per 5 GHz pass, 46 samples/s), so use them in channel mode. `phy1` (IPQ4029 2.4 GHz, disabled) and `phy2` (IPQ4029 5 GHz, the AP on channel 140) can be watched in channel mode.

### Levels

Values are **dBm per 20 MHz**: a 20 MHz Wi-Fi signal reads about its RSSI, noise sits near -104 dBm in 5 GHz and -90 to -95 dBm in a busy 2.4 GHz. Each bin is `10·log10(bin²) - AGC gain + c`. The firmware's `noise + rssi` is only trusted on quiet samples (AGC gain near its maximum), where the daemon learns `c` (measured: -41.5 on QCA9887, -37.5 on IPQ4019, steady to about 2 dB). That matters for strong signals: `rssi` reads them up to 27 dB low, and the firmware's noise field changes with width (-93 at 20 MHz, -103 at 40/80). Reports with a strong `rssi` but empty bins are dropped (about 0.2 %). Occupancy counts bins 10 dB (`threshold`) above a per-chunk noise reference: the quietest capture median seen, rising only about 0.3 dB/s, so it doesn't depend on the firmware noise field and a transmission filling a whole 80 MHz chunk still counts.

A very strong signal near a chunk edge raises the floor of that whole chunk by leakage, about 35 dB below it. Catalyst shows the same steps at chunk boundaries.

### Build

```sh
cd sdk
./scripts/feeds update custom && ./scripts/feeds install -p custom spectrald luci-app-spectral
make package/mac80211/compile package/ath10k-ct/compile \
     package/spectrald/compile package/luci-app-spectral/compile -j16
make package/index
```

If a package fails with `-j16` but nothing in the log points at real code (seen with hostapd variants, rpcd, rpcd-mod-luci), rebuild that one package with `-j1`: the SDK's top-level parallel build is racy.

### Install

The rebuilt kmods land in `sdk/bin/targets/ipq40xx/generic/packages/`, the plugin in `sdk/bin/packages/arm_cortex-a7_neon-vfpv4/custom/`. `make package/index` signs both. Serve `sdk/bin` and, with the key installed as in [Installing on the device](#installing-on-the-device), point the MR33 at both indexes:

```sh
echo 'http://<pc-ip>:8000/targets/ipq40xx/generic/packages/packages.adb' >> /etc/apk/repositories.d/customfeeds.list
echo 'http://<pc-ip>:8000/packages/arm_cortex-a7_neon-vfpv4/custom/packages.adb' >> /etc/apk/repositories.d/customfeeds.list
```

Then on the MR33:

```sh
apk update
apk add -u kmod-cfg80211 kmod-mac80211 kmod-ath kmod-ath10k-ct spectrald luci-app-spectral
reboot
```

Upgrading only `spectrald` later doesn't restart the running daemon: follow it with `/etc/init.d/spectrald restart`.

Install the whole mac80211 family from the same build so the modules match. After the reboot, `ls /sys/kernel/debug/ieee80211/phy*/ath10k/spectral_scan_ctl` should list every radio, and `ubus call spectral radios` should report `"spectral": true`.

Verified on this unit (2026-11-11) with the stock CT firmware (`ath10k-firmware-qca9887-ct` and `-qca4019-ct` 2023.04.04): all three radios report about 3000 to 4300 samples/s, a 2.4 GHz covering sweep takes about 250 ms and a 5 GHz sweep (27 channels, 50 ms dwell) about 1.5 s. If a session ever runs without samples (the page says so after a few seconds), try `kmod-ath10k` with the non-CT `ath10k-firmware-qca9887` and `ath10k-firmware-qca4019`.

One session at a time: a viewer asking for different settings takes over the radio from the others (they all see the new session).

### Test on the MR33

The quickest route: copy the six `.apk` files over SSH and install them unsigned. No repository server needed.

**1. Reach the unit.** On the dock it's `ssh mr33` (see [Debug mr33](#debug-mr33)). On the home LAN, override the proxy so it goes through `wlp59s0`:

```sh
alias mr33ssh="ssh -o ProxyCommand='nc -6 %h%%wlp59s0 %p' mr33"
alias mr33scp="scp -O -o ProxyCommand='nc -6 %h%%wlp59s0 %p'"
mr33ssh true    # should log in without error
```

On the dock, use plain `ssh mr33` and `scp -O` in the steps below.

**2. Copy the packages** (from the repo root):

```sh
mr33scp sdk/bin/targets/ipq40xx/generic/packages/kmod-{cfg80211,mac80211,ath}-6.12.112.6.18.55-r3.apk \
        sdk/bin/targets/ipq40xx/generic/packages/kmod-ath10k-ct-6.12.112.2025.12.01~bb84e159-r2.apk \
        sdk/bin/packages/arm_cortex-a7_neon-vfpv4/custom/{spectrald,luci-app-spectral}-1.0.0-r*.apk \
        mr33:/tmp/
```

**3. Install and reboot:**

```sh
mr33ssh 'apk add --allow-untrusted /tmp/kmod-*.apk /tmp/spectrald-*.apk /tmp/luci-app-spectral-*.apk && reboot'
```

The four kmods have to go in together: they are one consistent build. `/tmp` is RAM, so the copies are gone after the reboot; the installed packages stay. Wi-Fi drops during the reboot, so expect to lose the connection for a minute or two if you're on the home LAN.

**4. Check the drivers and the service:**

```sh
mr33ssh 'ls /sys/kernel/debug/ieee80211/phy*/ath10k/spectral_scan_ctl'   # one line per radio
mr33ssh 'ubus call spectral radios'                                     # "spectral": true on each
mr33ssh 'for p in /sys/class/ieee80211/*; do echo "$(basename $p) $(basename $(readlink $p/device/driver))"; done'
```

The last line maps phys to drivers: `ath10k_pci` is the QCA9887 scanning radio, `ath10k_ahb` the two IPQ4029 radios.

**5. Test from the command line.** Full spectrum on the idle scanning radio (here `phy0`, the QCA9887, disabled in `/etc/config/wireless`), then the AP's own channel on `phy2`:

```sh
mr33ssh 'ubus call spectral start "{\"lease\":60}"; sleep 5; ubus call spectral status'
mr33ssh 'ubus call spectral start "{\"phy\":\"phy2\",\"mode\":\"channel\",\"lease\":60}"; sleep 5; ubus call spectral status'
mr33ssh 'ubus call spectral stop; logread -e spectrald | tail'
```

`status` should show `samples` and `rate` going up (several thousand samples/s). If they stay at 0 while `running` is true, the firmware isn't producing spectral reports: see the CT note under [Install](#install).

**6. Test the web page.** Forward the LuCI port and open <http://localhost:8033/cgi-bin/luci/admin/status/spectral>:

```sh
mr33ssh -L 8033:localhost:80 -N
```

The page starts by itself; leaving it stops the scan within 10 seconds (`ubus call spectral status` then reports `"reason": "idle"`).

**Undo.** Remove the plugin with `apk del luci-app-spectral spectrald`. To go back to the stock drivers, enable the official feeds and pin the release versions, then reboot:

```sh
apk update
apk add kmod-cfg80211=6.12.112.6.18.55-r2 kmod-mac80211=6.12.112.6.18.55-r2 \
        kmod-ath=6.12.112.6.18.55-r2 kmod-ath10k-ct=6.12.112.2025.12.01~bb84e159-r1
reboot
```

### ubus API

```sh
ubus call spectral radios
ubus call spectral start '{}'                                     # full spectrum, both bands, idle radio picked for you
ubus call spectral start '{"band":"5g","dwell":20,"lease":600}'   # full spectrum, 5 GHz only
ubus call spectral start '{"phy":"phy2","mode":"channel"}'        # watch the AP's own channel
ubus call spectral start '{"phy":"phy0","mode":"channel","freq":5180,"width":80}'
ubus call spectral frames '{"since":0}'
ubus call spectral status
ubus call spectral stop
```

`start` takes `mode` (`full`, `channel`, `sweep`), `phy` (optional for full), `band` (`2g`, `5g`, `all`), `dwell` (ms per chunk or channel, at least 10, no upper limit, default 150 in full mode; sweep mode clamps it to 65535), `freq`/`width` (channel mode on an idle radio), `bins` (channel and sweep), `all` (sweep every channel), `interval` (frame period, ms), `threshold` (occupancy, dB) and `lease` (s). It replies `{"ok": false, "error": ...}` on failure. Full mode's status lists its `chunks`.

`frames` returns `views`, one per band, each with a `grid` and its `frames`. Each frame carries three base64 byte arrays over `grid.n` cells, cell `i` centred on `grid.start + (i + 0.5) * grid.res` MHz:

| Field | Byte meaning |
|---|---|
| `max`, `avg` | dBm per 20 MHz + 160, 0 = no sample in this frame |
| `occ` | percent of bins above the capture's noise + `threshold`, 255 = no sample |

`gen` changes whenever a grid changes, so clients know to drop their history. `seq` is shared by all views.

### Python SDK (`client/`)

`client/mr33spectral` pulls the data onto a PC without LuCI: a small library plus a `mr33-spectral` command. No dependencies beyond Python 3.8. It talks to the same ubus object as the page, either over SSH (key login, nothing to type) or over HTTP through uhttpd's `/ubus` JSON-RPC endpoint (same login as LuCI; `root` already has access to `spectral`).

```sh
pip install -e client                                    # or run in place: cd client && python3 -m mr33spectral ...
mr33-spectral --via wlp59s0 radios                       # ssh alias mr33, link-local through wlp59s0
mr33-spectral --via wlp59s0 watch --band 5g              # live peak/occupancy per channel in the terminal
mr33-spectral --via wlp59s0 record --seconds 60 --out scan.jsonl
mr33-spectral --via wlp59s0 record --mode channel --phy phy2 --format csv --out ap.csv
MR33_PW=... mr33-spectral --url http://192.168.1.1 --password-env MR33_PW status
```

Without `--via`, `--ssh <alias>` uses your SSH config as is (the dock setup). SSH calls share one connection (`ControlMaster`), so polling stays cheap.

`record` writes one JSON object per band per frame (`seq`, `t_ms`, `band`, `grid`, and `max`/`avg`/`occ` arrays with `null` for unvisited cells), or with `--format csv` one row per cell with data: `t_ms, seq, band, freq_mhz, max_dbm, avg_dbm, occ_pct`. The session stops when the command exits.

In Python:

```python
from mr33spectral import Client

c = Client.ssh("mr33", ["ProxyCommand=nc -6 %h%%wlp59s0 %p"])      # or Client.http("http://192.168.1.1", "root", pw)
with c.session(band="all") as frames:                              # full spectrum; also mode="channel", phy=..., dwell=...
    for f in frames:                                               # Frame: band, seq, t_ms, freqs, max, avg, occ (NaN = no data)
        ch36 = f.channel(5180) if f.band == "5g" else None          # {"avg", "max", "occ"} over one 20 MHz channel
```

Polling renews the lease, so a stream runs as long as you iterate. `stream()` without start parameters follows whatever session is already running (the LuCI page's, for example). Tests: `python3 -m unittest discover -s client/tests`.

### Tests

```sh
make -C packages/spectrald/tests      # parser, dBm model vs fft_eval, grid, channel maths (ASan/UBSan)
```

`spectrald -d <dir>` reads the ath10k debugfs tree from `<dir>` instead of `/sys/kernel/debug/ieee80211`, which lets a fake relay file drive a full session on a PC.

## Beyond the SDK

- **Image with your packages built in:** the Image Builder for the same release (`openwrt-imagebuilder-25.12.6-ipq40xx-generic.Linux-x86_64.tar.zst`, same download dir) assembles a sysupgrade image from official packages plus local `.apk` files dropped in its `packages/` dir: `make image PROFILE=meraki_mr33 PACKAGES="..." FILES=...`.
- **Kernel config, DTS, drivers built into the kernel:** these need a full build from `src/`. The MR33 DTS is `target/linux/ipq40xx/files-6.12/arch/arm/boot/dts/qcom/qcom-ipq4029-mr33.dts`, the image recipe is `Device/meraki_mr33` in `target/linux/ipq40xx/image/generic.mk`. Keep `DEVICE_DTS_LOADADDR := 0x89000000` in `Device/meraki_common`: without it the Meraki bootloader drops the DTB at an unaligned address and the kernel hangs silently at boot (`f1c80445bd`).

## Debug mr33

The unit is `WhiteFlatty`, MAC `e0:cb:bc:31:89:b2`. When its IPv4 config doesn't match the LAN it vanishes from the FritzBox list and from IPv4 scans, but IPv6 link-local keeps working as long as the port has link. Debug over that.

### Find it

- Link-local address, from the MAC (EUI-64): `fe80::e2cb:bcff:fe31:89b2`.
- It ignores multicast echo, so `ping ff02::1%<if>` never shows it. Ping the exact address: `ping fe80::e2cb:bcff:fe31:89b2%<if>`. Works on the home LAN (`wlp59s0`) too when it's plugged in there.

### Direct link through the WD19TB dock

1. Thunderbolt security is `user`. The dock (`00398938-3a9d-d400-ffff-ffffffffffff`) is enrolled with policy `auto`, so it authorizes itself. It used to be `iommu`, which never auto-authorizes here because `iommu_dma_protection` is 0, and the USB hub and Ethernet stayed hidden. Check `cat /sys/bus/thunderbolt/devices/0-1/authorized`.
2. The NIC appears as `enp7s0u2u4` (r8152).
3. `nmcli con up mr33`. Do this after every link-up: NM autoconnects the `dock` DHCP profile, which waits for a lease the MR33 never serves, then tears the interface down with its link-local address, and SSH fails with `Connection closed by UNKNOWN port 65535`. `nmcli con up dock` switches back for normal wired networking.
   If the profile is gone: `nmcli con add type ethernet con-name mr33 ifname enp7s0u2u4 connection.autoconnect no ipv4.method disabled ipv6.method link-local`

### SSH and LuCI

- `~/.ssh/config` has `Host mr33`: root, `ProxyCommand nc -6 %h%%enp7s0u2u4 %p`. Plain ssh to a scoped link-local address fails with "Cannot assign requested address". Use `ssh mr33` and `scp -O file mr33:/tmp/`. Only `root` exists.
- Key login: `ssh mr33 'cat >> /etc/dropbear/authorized_keys' < ~/.ssh/id_ed25519.pub`
- Browsers can't open scoped link-local URLs. Forward a local port (8080 is taken on this laptop), then open <http://localhost:8033/cgi-bin/luci/>:
  `socat TCP4-LISTEN:8033,bind=127.0.0.1,reuseaddr,fork 'TCP6:[fe80::e2cb:bcff:fe31:89b2]:80,so-bindtodevice=enp7s0u2u4'`
  Or over SSH: `ssh -L 8033:localhost:80 mr33`.

### When it goes down

Symptoms: `cat /sys/class/net/enp7s0u2u4/carrier` reads `0`, NM shows the device `unavailable`, ping and LuCI time out.

1. Laptop side: dock still authorized, `enp7s0u2u4` still in `ip -br link`. Bounce the link with `sudo ip link set enp7s0u2u4 down && sudo ip link set enp7s0u2u4 up`.
2. Check the air: `nmcli -f SSID,SIGNAL dev wifi list --rescan yes`. If Sashimi is gone too, the whole unit is down, not only its port.
3. A LuCI change that cut the connection rolls back by itself after 90 s, unless it was applied unchecked. "Stop" on an interface is runtime only and is undone by a reboot.
4. Power-cycle: unplug the PoE injector's power for 10 s. The dock gives no PoE: the injector's data input goes to the dock, its PoE output to the MR33. Carrier flaps on and off during boot. Wait until it settles, then `nmcli con up mr33`. Wi-Fi comes up last.
5. No link after a clean boot means the config is broken. Use failsafe: power on, and when the status LED blinks green early in boot, press the reset button. The LED turns red. Failsafe sets `192.168.1.1/24` on the port with passwordless root SSH. Give the laptop an address (`nmcli con mod mr33 ipv4.method manual ipv4.addresses 192.168.1.2/24 && nmcli con up mr33`), `ssh root@192.168.1.1`, run `mount_root`, fix `/etc/config/network`, `reboot`. `firstboot && reboot` wipes the config entirely. Set `ipv4.method disabled` back on the profile afterwards.
6. Last resort: serial console, see [Hardware](#hardware).
