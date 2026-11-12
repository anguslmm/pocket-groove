# PlatformIO extra script (post:) for the USB audio builds (env:cardputer-adv-usb,
# env:usb-audio-spike).
#
# 1. RAM: the prebuilt TinyUSB audio driver reserves a 3104-byte FIFO for an
#    audio OUT endpoint (CFG_TUD_AUDIO_ENABLE_EP_OUT = 1 in the core's
#    tusb_config.h). Our device has no OUT endpoint, so the driver only ever
#    stores the buffer's address (audiod_init: tu_fifo_config) and resets its
#    indices (tu_fifo_clear); it reads or writes the bytes only when an OUT
#    endpoint is opened (audio_device.c @ 88c7f0b81: audiod_set_interface,
#    audiod_rx_xfer_isr). So this build links a copy of that archive member
#    (audio_device.c.obj) whose `.bss.ep_out_sw_buf` section is renamed into
#    `.rtc_noinit.*`: the linker puts it in RTC slow memory, which is not part
#    of the heap. 3104 bytes more heap, same code. The copy defines every
#    symbol the member does, so the archive member itself is never extracted
#    (if it ever were, the link would fail with "multiple definition").
#    The build fails if the section is not there (a different library) or if
#    the result does not land at an RTC address (checked in the ELF).
#
# 2. `pio run -e cardputer-adv-usb -t flashmode`: reboots a running USB-audio
#    firmware into the ROM download mode without a button, so that ANY env
#    can be flashed next, e.g. going back:
#      pio run -e cardputer-adv-usb -t flashmode && pio run -e cardputer-adv -t upload
#    It is the same 1200-baud "touch" that `-t upload` of this env does
#    (board_upload.use_1200bps_touch): the firmware's CDC port sees the line
#    coding 1200 and restarts into the ROM, which comes back as the chip's
#    USB-Serial/JTAG port (303a:1001). Then it waits for that port like
#    `-t upload` does. Against the normal firmware the touch does nothing, and
#    the device is already where esptool can reach it.
#
# Only toolchain binaries are used (ar, objcopy, readelf, nm), never host tools.
import os
import re
import subprocess

Import("env")  # noqa: F821

OUT_FIFO_SECTION = ".bss.ep_out_sw_buf"
OUT_FIFO_TARGET = ".rtc_noinit.pg_unused_audio_out_fifo"
RTC_SLOW_LO, RTC_SLOW_HI = 0x50000000, 0x50002000


def _tool(name):
    cc = env.subst("$CC")  # noqa: F821
    return env.WhereIs(cc.replace("gcc", name)) or cc.replace("gcc", name)  # noqa: F821


def _tinyusb_lib():
    libs = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")  # noqa: F821
    mcu = env.BoardConfig().get("build.mcu", "esp32s3")  # noqa: F821
    path = os.path.join(libs, mcu, "lib", "libarduino_tinyusb.a")
    if not os.path.isfile(path):
        raise RuntimeError("[usb] %s not found" % path)
    return path


def _make_driver_copy(target, source, env):
    out = str(target[0])
    workdir = os.path.dirname(out)
    os.makedirs(workdir, exist_ok=True)
    member = "audio_device.c.obj"
    subprocess.check_call([_tool("ar"), "x", str(source[0]), member], cwd=workdir, stdin=subprocess.DEVNULL, timeout=120)
    extracted = os.path.join(workdir, member)
    secs = subprocess.run([_tool("readelf"), "-S", "-W", extracted], capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=120).stdout
    if not re.search(r"\]\s+%s\s+NOBITS" % re.escape(OUT_FIFO_SECTION), secs):
        raise RuntimeError("[usb] %s has no NOBITS section %s: the prebuilt TinyUSB changed, review tools/pio_usb.py"
                           % (member, OUT_FIFO_SECTION))
    subprocess.check_call([_tool("objcopy"), "--rename-section", "%s=%s" % (OUT_FIFO_SECTION, OUT_FIFO_TARGET),
                           extracted, out], stdin=subprocess.DEVNULL, timeout=120)
    os.remove(extracted)
    print("[usb] linking the TinyUSB audio driver with its unused OUT FIFO (3104 B) in RTC slow memory", flush=True)
    return 0


def _check_elf(target, source, env):
    elf = str(target[0])
    r = subprocess.run([_tool("nm"), "-S", elf], capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=120)
    hits = [l.split() for l in r.stdout.splitlines() if l.endswith(" ep_out_sw_buf")]
    if len(hits) != 1:
        raise RuntimeError("[usb] ep_out_sw_buf: expected one symbol in %s, found %d" % (elf, len(hits)))
    addr = int(hits[0][0], 16)
    if not RTC_SLOW_LO <= addr < RTC_SLOW_HI:
        raise RuntimeError("[usb] ep_out_sw_buf is at 0x%08x, not in RTC slow memory" % addr)
    print("[usb] unused audio OUT FIFO at 0x%08x (RTC slow memory): OK" % addr)
    return 0


_copy = env.Command(  # noqa: F821
    os.path.join("$BUILD_DIR", "usb_trim", "audio_device_out_fifo_in_rtc.o"),
    _tinyusb_lib(),
    env.Action(_make_driver_copy, "[usb] relocating the unused audio OUT FIFO"),  # noqa: F821
)
env.Depends("$BUILD_DIR/${PROGNAME}.elf", _copy)  # noqa: F821
env.Append(LINKFLAGS=[_copy[0].get_abspath()])  # noqa: F821  (objects in LINKFLAGS precede the libraries)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", env.Action(_check_elf, "[usb] checking the OUT FIFO placement"))  # noqa: F821


def _flashmode(target, source, env):
    from platformio.device.list.util import list_serial_ports

    if not env.subst("$UPLOAD_PORT"):
        env.AutodetectUploadPort()
    before = list_serial_ports()
    env.TouchSerialPort("$UPLOAD_PORT", 1200)
    port = env.WaitForNewSerialPort(before)
    print("Download mode: flash with any env now (the device stays in it until flashed or power-cycled). "
          "Port: %s" % port)
    return 0


env.AddCustomTarget(  # noqa: F821
    name="flashmode",
    dependencies=None,
    actions=[_flashmode],
    title="Reboot into download mode",
    description="1200-baud touch: the running USB-audio firmware restarts into the ROM download mode",
    always_build=True,
)
