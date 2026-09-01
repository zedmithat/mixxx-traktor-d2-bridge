from pathlib import Path
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MONITOR = ROOT / "systemd" / "zed-usb-monitor.py"


with tempfile.TemporaryDirectory() as temp_dir:
    base = Path(temp_dir)
    state = base / "state"
    # The parser intentionally accepts only the production mount roots. Replace
    # the temporary prefix while keeping the test independent of root access.
    source = (
        "42 31 8:1 / /media/pi/REKORDBOX rw,nosuid,nodev - "
        "vfat /dev/sda1 rw\n"
    )
    production_mount = "/media/pi/REKORDBOX"
    # Unit-test parsing with an existing production-style path is performed by
    # importing the module and temporarily replacing os.path checks.
    import importlib.util
    spec = importlib.util.spec_from_file_location("zed_usb_monitor", MONITOR)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    original_isdir = module.os.path.isdir
    original_access = module.os.access
    module.os.path.isdir = lambda path: path == production_mount
    module.os.access = lambda path, mode: path == production_mount
    devices = module.mounted_media(source.splitlines())
    module.os.path.isdir = original_isdir
    module.os.access = original_access
    assert devices == [{
        "label": "REKORDBOX",
        "mount": "/media/pi/REKORDBOX",
        "source": "/dev/sda1",
        "fstype": "vfat",
    }]
    content = module.render_state(devices)
    assert "PRESENT\t1" in content
    assert "LABEL\tREKORDBOX" in content
    assert "MOUNT\t/media/pi/REKORDBOX" in content
    assert module.publish(state, content)
    assert not module.publish(state, content)

    partitions = [
        {"label": "sdb1-usb-JMicron_Tech_DD5"},
        {"label": "Yeni Birim"},
        {"label": "sdb4-usb-JMicron_Tech_DD5"},
    ]
    partitions.sort(key=module.device_sort_key)
    assert partitions[0]["label"] == "Yeni Birim"

    empty = module.render_state([])
    assert "COUNT\t0" in empty and "PRESENT\t0" in empty

print("ZED_USB_MONITOR_TEST_OK atomic=true scan=false mount=REKORDBOX")
