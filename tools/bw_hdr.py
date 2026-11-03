"""Windows HDR state of the connected screens: the launcher's Automatic HDR switch
(0.2.5 item 97, tools/launcher.pyw) and a test aid (item 45).

  py -3 tools/bw_hdr.py            show each active screen: HDR supported, on or off
  py -3 tools/bw_hdr.py on|off     turn Windows HDR on or off for every screen that supports it

Turning HDR on or off changes a Windows setting that every running program
sees. The launcher does it only for a play session the player asked for (its
Automatic HDR checkbox), on one screen, and puts it back when the session ends. By
hand (on|off): in the 0.2.5 run only with the lead's go, restored straight
after, and logged as a call. Showing the state changes nothing.

Uses the DisplayConfig API: DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2
and SET_HDR_STATE (Windows 11 24H2 and later), else the older
GET_ADVANCED_COLOR_INFO and SET_ADVANCED_COLOR_STATE. The functions raise
OSError when Windows won't answer; only the command line turns that into an
exit.
"""
import ctypes
import sys
from ctypes import wintypes

user32 = ctypes.WinDLL("user32")

QDC_ONLY_ACTIVE_PATHS = 2
GET_SOURCE_NAME = 1
GET_TARGET_NAME = 2
GET_ADVANCED_COLOR_INFO = 9
SET_ADVANCED_COLOR_STATE = 10
GET_ADVANCED_COLOR_INFO_2 = 15
SET_HDR_STATE = 16


class LUID(ctypes.Structure):
    _fields_ = [("LowPart", wintypes.DWORD), ("HighPart", wintypes.LONG)]


class PATH_SOURCE(ctypes.Structure):
    _fields_ = [("adapterId", LUID), ("id", ctypes.c_uint32), ("modeInfoIdx", ctypes.c_uint32),
                ("statusFlags", ctypes.c_uint32)]


class PATH_TARGET(ctypes.Structure):
    _fields_ = [("adapterId", LUID), ("id", ctypes.c_uint32), ("modeInfoIdx", ctypes.c_uint32),
                ("outputTechnology", ctypes.c_uint32), ("rotation", ctypes.c_uint32),
                ("scaling", ctypes.c_uint32), ("refreshNum", ctypes.c_uint32),
                ("refreshDen", ctypes.c_uint32), ("scanLineOrdering", ctypes.c_uint32),
                ("targetAvailable", wintypes.BOOL), ("statusFlags", ctypes.c_uint32)]


class PATH_INFO(ctypes.Structure):
    _fields_ = [("sourceInfo", PATH_SOURCE), ("targetInfo", PATH_TARGET), ("flags", ctypes.c_uint32)]


class MODE_INFO(ctypes.Structure):
    _fields_ = [("infoType", ctypes.c_uint32), ("id", ctypes.c_uint32), ("adapterId", LUID),
                ("data", ctypes.c_byte * 48)]


class HEADER(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("size", ctypes.c_uint32), ("adapterId", LUID),
                ("id", ctypes.c_uint32)]


class TARGET_NAME(ctypes.Structure):
    _fields_ = [("header", HEADER), ("flags", ctypes.c_uint32), ("outputTechnology", ctypes.c_uint32),
                ("edidManufactureId", ctypes.c_uint16), ("edidProductCodeId", ctypes.c_uint16),
                ("connectorInstance", ctypes.c_uint32), ("monitorFriendlyDeviceName", wintypes.WCHAR * 64),
                ("monitorDevicePath", wintypes.WCHAR * 128)]


class COLOR_INFO(ctypes.Structure):
    _fields_ = [("header", HEADER), ("value", ctypes.c_uint32), ("colorEncoding", ctypes.c_uint32),
                ("bitsPerColorChannel", ctypes.c_uint32)]


class COLOR_INFO_2(ctypes.Structure):
    _fields_ = [("header", HEADER), ("value", ctypes.c_uint32), ("colorEncoding", ctypes.c_uint32),
                ("bitsPerColorChannel", ctypes.c_uint32), ("activeColorMode", ctypes.c_uint32)]


class SET_STATE(ctypes.Structure):
    _fields_ = [("header", HEADER), ("value", ctypes.c_uint32)]


class SOURCE_NAME(ctypes.Structure):
    _fields_ = [("header", HEADER), ("viewGdiDeviceName", wintypes.WCHAR * 32)]


def active_paths():
    """The active display paths (each has a sourceInfo and a targetInfo)."""
    np, nm = ctypes.c_uint32(), ctypes.c_uint32()
    if user32.GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, ctypes.byref(np), ctypes.byref(nm)):
        raise OSError("GetDisplayConfigBufferSizes failed")
    paths = (PATH_INFO * np.value)()
    modes = (MODE_INFO * nm.value)()
    if user32.QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ctypes.byref(np), paths, ctypes.byref(nm), modes, None):
        raise OSError("QueryDisplayConfig failed")
    return list(paths[:np.value])


def targets():
    return [p.targetInfo for p in active_paths()]


def source_device(src):
    """A path source's GDI device name (\\\\.\\DISPLAY1 ...), as MONITORINFOEX names it."""
    n = SOURCE_NAME()
    n.header = header(src, GET_SOURCE_NAME, ctypes.sizeof(SOURCE_NAME))
    return n.viewGdiDeviceName if not user32.DisplayConfigGetDeviceInfo(ctypes.byref(n)) else ""


def primary_device():
    """The primary screen's GDI device name, or ""."""
    class MonitorInfoEx(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                    ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD),
                    ("szDevice", wintypes.WCHAR * 32)]
    user32.MonitorFromPoint.restype = wintypes.HMONITOR
    user32.MonitorFromPoint.argtypes = [wintypes.POINT, wintypes.DWORD]
    user32.GetMonitorInfoW.argtypes = [wintypes.HMONITOR, ctypes.c_void_p]
    mi = MonitorInfoEx()
    mi.cbSize = ctypes.sizeof(mi)
    if not user32.GetMonitorInfoW(user32.MonitorFromPoint(wintypes.POINT(0, 0), 1), ctypes.byref(mi)):
        return ""
    return mi.szDevice


def screen(device=None):
    """One screen as a dict (device, name, supported, on, api, target), the
    one with that GDI device name, else the primary; None if there's none."""
    want = (device or primary_device() or "").lower()
    found = None
    for p in active_paths():
        dev = source_device(p.sourceInfo)
        if found is None or dev.lower() == want:
            name, sup, on, api = state(p.targetInfo)
            found = {"device": dev, "name": name, "supported": sup, "on": on, "api": api,
                     "target": p.targetInfo}
            if dev.lower() == want:
                break
    return found


def header(t, typ, size):
    h = HEADER()
    h.type, h.size, h.adapterId, h.id = typ, size, t.adapterId, t.id
    return h


def state(t):
    """(name, supported, on, api) for one target."""
    n = TARGET_NAME()
    n.header = header(t, GET_TARGET_NAME, ctypes.sizeof(TARGET_NAME))
    name = n.monitorFriendlyDeviceName if not user32.DisplayConfigGetDeviceInfo(ctypes.byref(n)) else "?"
    c2 = COLOR_INFO_2()
    c2.header = header(t, GET_ADVANCED_COLOR_INFO_2, ctypes.sizeof(COLOR_INFO_2))
    if not user32.DisplayConfigGetDeviceInfo(ctypes.byref(c2)):
        sup = bool(c2.value & (1 << 4))          # highDynamicRangeSupported
        on = c2.activeColorMode == 2             # DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR
        return name or "?", sup, on, 2
    c = COLOR_INFO()
    c.header = header(t, GET_ADVANCED_COLOR_INFO, ctypes.sizeof(COLOR_INFO))
    if user32.DisplayConfigGetDeviceInfo(ctypes.byref(c)):
        return name or "?", False, False, 0
    return name or "?", bool(c.value & 1), bool(c.value & 2), 1


def set_state(t, api, on):
    s = SET_STATE()
    s.header = header(t, SET_HDR_STATE if api == 2 else SET_ADVANCED_COLOR_STATE, ctypes.sizeof(SET_STATE))
    s.value = 1 if on else 0
    return user32.DisplayConfigSetDeviceInfo(ctypes.byref(s))


def main():
    want = sys.argv[1].lower() if len(sys.argv) > 1 else None
    if want not in (None, "on", "off"):
        raise SystemExit(__doc__)
    try:
        found = targets()
    except OSError as e:
        raise SystemExit(str(e))
    for t in found:
        name, sup, on, api = state(t)
        print("%s: HDR %s, %s" % (name, "supported" if sup else "not supported", "on" if on else "off"))
        if want and sup and on != (want == "on"):
            r = set_state(t, api, want == "on")
            name, sup, on, api = state(t)
            print("  set %s: %s (result %d)" % (want, "on" if on else "off", r))


if __name__ == "__main__":
    main()
