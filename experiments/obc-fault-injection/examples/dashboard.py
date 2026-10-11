"""Run OBC firmware: examples/dashboard.py --firmware /path/to/image.out --web."""
import argparse
from pathlib import Path
import threading
import obcfi

root = Path(__file__).resolve().parents[4]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--web', action='store_true', help='Serve a local browser dashboard')
parser.add_argument('--port', type=int, default=0, help='HTTP port; 0 chooses a free port')
parser.add_argument('--firmware', type=Path, required=True, metavar='FILE',
                    help='OBC100 BE-32 ELF image; relative paths use the current directory')
parser.add_argument('--qemu', type=Path, default=root/'qemu/build/qemu-system-arm')
args = parser.parse_args()
args.firmware = args.firmware.expanduser().resolve()
if not args.firmware.is_file():
    parser.error(f'firmware file not found: {args.firmware}')
print(f'Firmware: {args.firmware}', flush=True)
with obcfi.Obc(args.firmware, args.qemu, web=args.web, web_port=args.port) as obc:
    obc.wait_for_boot()
    if obc.web_url:
        print(f'Dashboard: {obc.web_url}\nPress Ctrl+C to stop.', flush=True)
        try:
            while obc.running:
                threading.Event().wait(0.5)
        except KeyboardInterrupt:
            pass
    else:
        # No browser, display server, HTTP dependency or terminal required.
        print(obc.command('RTC_DATE'))
