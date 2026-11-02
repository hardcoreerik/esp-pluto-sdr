# Firmware compatibility review

Research date: 2026-11-02. Exact revisions and source owners are in sources.json.
Paths below are relative to the named upstream repository at that pinned revision.
All conclusions here are source observations or explicitly labeled engineering proposals.

## Identity comes before firmware selection

Fishball is not one reproducible hardware target. Tezuka's boards.json distinguishes
fishball, fishball7020, fishball_mini_7010, fishball_mini_7020 and plutoskyr2.
The independent Fishball7020 devkit README limits its reconstruction to the
Z7020/AD9361 Rev.A board; it expressly excludes unchanged use on Z7010/AD9363.
Erik confirmed this repository supplies his installed firmware. The exact installed
revision and factory/modern target are still unknown; repository HEAD is a research
snapshot, not proof of the device image.
Record PCB markings, model/context, boot files and RF front end before recommending an image.

## Review coverage

| Source | Reviewed areas | Important boundary |
|---|---|---|
| F5OEO/tezuka_fw | board registry; Fishball defconfigs; Fishball7020 DTS; common USB startup; network configuration; build recipe; kernel patch layout; license | Source/build configuration is not the running image |
| matsvandamme/fishball7020-fpga-devkit | factory/modern build split; USB gadget and daemon wrappers; package pin; kernel patch catalog; dual-RX decimation notes; offline DSP tests; board identity and provenance docs | Independent reconstruction, not a manufacturer release |
| plantjes1/Fishball_DATV | README, build-tree inventory, features and upstream lineage | Tezuka-derived DATV fork, not a universal factory firmware source |
| analogdevicesinc/libiio | network/IIOD client; command negotiation; AD9361 streaming example; per-file licensing; Zephyr layout | Desktop/Zephyr code is not an ESP-IDF port |
| analogdevicesinc/plutosdr-fw | pinned reference and file inventory | Baseline collected; full build and recursive dependency review pending |

Tracked file inventories are retained in evidence/. This is a focused host-compatibility
review, not a claim that every line, vendored dependency or bitstream has been audited.

## Tezuka

### Build and board configuration

boards.json maps fishball7020 to configs/fishball_maiasdr_7020_defconfig.
The selected defconfig specifies ARM Cortex-A9/NEON, an external Bootlin toolchain,
Buildroot userspace, ADI Linux commit bf1d49f17fa9e15bc40be2615ef67cdf023f660a,
and Xilinx U-Boot commit a09d2660433d3d1ade6a14a82b21a13c2bbb3821.
buildroot.version pins 2026.05.2 with separate official/mirror archive checksums.
getbuildroot.sh obtains Buildroot and applies local patches. Those dependency archives
were not built or fetched recursively in this session.

board/tezuka/fishball7020/dts/fishball.dts enables adi,2rx-2tx-mode-enable
and an AD9361 DDS compatible string. The comments state bootloader environment
control; actual runtime context remains the authority for enabled channels.

### USB and IIOD

board/tezuka/common/overlay_base/etc/init.d/S23udc selects ncm.usb0,
ecm.usb0 or rndis.0 from usb_ethernet_mode. It assembles a composite gadget:
network, mass storage, ACM and FunctionFS IIO, with optional audio functions.
It generates /etc/libiio.ini model, serial and firmware context attributes.
FORCE_MODEL may override model text: a model string alone cannot authenticate hardware.
VID 0456 and a Pluto product identity are therefore insufficient board identifiers.

The same script starts iiod with FunctionFS options and, on dual-core systems,
real-time priority and CPU affinity. A dma_test_skip boot parameter suppresses
normal IIO gadget/daemon startup. A network link alone does not prove IIOD is ready.
S40network also exposes USB mode settings in config.txt.

The Fishball configs enable libiio, IIOD, USB/local/network backends and libad9361-iio.
Resolve the actual libiio package version from the selected Buildroot tree/patches
before claiming a daemon version. A top-level libiio enable flag is not a version pin.

### Extended transports and applications

Tezuka includes Maia and application/dashboard services. Fishball_DATV's README
reports an 8-bit streaming mode and performance figures. Treat those as upstream
claims, not P4 results. Standard signed-16 IIO support comes first; any packed-8
extension needs its own negotiated contract and reference capture.
The ESP component should not depend on MQTT, a web dashboard or DATV applications.

## Independent Fishball7020 devkit

### Two complete system paths

README.md and firmware/README.md describe the factory reconstruction using vendor
Linux 5.15 and Buildroot; firmware-modern/README.md describes ADI Linux 6.12
and Debian armhf. Modern setup.sh pins kernel commit
947298737646475ebb557be24d6ee9fc7901e0a9. FPGA work still involves an XSA or
a separately built design. No Vivado, AMD IP, factory image or bitstream was executed here.

### Networking and package compatibility

firmware-modern/debian/overlay/usr/local/sbin/fishball-usb-gadget builds
RNDIS by default, ACM and FunctionFS IIO. It uses Pluto VID/PID 0456:b673,
persists identity and derives MAC addresses from the serial. Creation and USB
binding are separate to let IIOD populate FunctionFS before enumeration.
fishball-iiod falls back to network-only operation if USB FunctionFS is absent.
This differs materially from assuming all networking failures are RF failures.

overlay/etc/apt/preferences.d/fishball-libiio.pref pins libiio 0.26-2.
Consequently a client that only supports a newer binary IIOD protocol would be
an unsuitable baseline. The source comments link the pin to cyclic-buffer/kernel ABI
behavior; reproduce before relying on the same TX path elsewhere.

### RX2 and decimation

docs/both-receive-channels.md and firmware/patches/0021* describe a correction
to filtering both RX channels when FPGA decimation is active. The upstream author
reports aliasing on the stock second channel. We have not reproduced that measurement.
The host must discover scan channels and record FPGA/image provenance; two exposed
channels do not by themselves establish equivalent filtering or coherence.

### TX state and watchdogs

The factory and modern patch catalogs describe mute-on-stop, starvation detection,
latched disable, thermal limits and cached attenuation fixes. Modern patch 0019
addresses reset of a cached attenuation value. The userspace fishball-rf-quiesce
wrapper exposes persistent overrides, so protections can be configured off.
Do not advertise these protections as universal Fishball or Tezuka behavior.
RX bring-up must not copy an example that starts a TX buffer as a side effect.
The upstream default self-test also exercises internal TX paths: it is not a
read-only inspection command. Only offline tests were executed here.

## Fishball DATV fork

plantjes1/Fishball_DATV identifies itself as Tezuka-derived. Its README emphasizes
RX/TX channel selection, packed-8 mode, audio gadget and SD boot. Its smaller
source snapshot should be treated as a specific fork/revision rather than the
canonical source for every factory Fishball image. Compare exact selected board,
patches and IIOD package before applying its behavior to another firmware.

## Licensing and reuse

Tezuka and Fishball_DATV root LICENSE texts are GPL-3.0; the independent devkit
root LICENSE is GPL-2.0. Upstream components and proprietary FPGA IP retain
their own terms. libiio has mixed licensing: context.c, channel.c, device.c and
xml.c declare MIT; network.c and iiod-client.c declare LGPL-2.1-or-later.
The existence of MIT core files does not make its network client MIT.
No third-party source has been copied into this project. Resolve the component
license and a per-file reuse inventory before implementation incorporates code.

## Decision

Use an endpoint-configurable, capability-discovering IIOD client as the shared
baseline. Prove it over Ethernet where available, then validate P4 USB RNDIS.
Maintain profiles for tested firmware revisions, not broad brand-name promises.
Keep Tezuka-specific packed formats and devkit-specific TX controls optional.
