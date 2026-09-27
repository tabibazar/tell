#!/usr/bin/env python3
"""Stream a camera on this Mac to watch's viewfinder, over watch's USB.

    tools/watch-cam.py              the default camera (0)
    tools/watch-cam.py --camera 1   another (--list shows them; an iPhone on
                                    Continuity Camera appears among them)
    tools/watch-cam.py --test       colour bars and a moving pattern instead,
                                    to check colours and speed with no camera
    tools/watch-cam.py --list       the cameras ffmpeg can see

ffmpeg reads the camera (avfoundation), crops the middle to watch's upright
240x280 and encodes each frame as a JPEG; each goes down watch's USB serial
as a line "!jpg N" and then its N bytes (main.c, watch_usb_task). watch turns
to her Viewfinder page when frames arrive and back to her face three seconds
after they stop.

watch is found by her MAC in tools/boards.tsv. The first run asks macOS for
camera access for the terminal it runs in; allow it. Needs ffmpeg (Homebrew)
and pyserial.
"""
import argparse
import importlib.util
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 240, 280
MAX_FRAME = 64 * 1024          # watch's CAM_MAX_BYTES


def relay():
    spec = importlib.util.spec_from_file_location("noise_relay", os.path.join(HERE, "noise-relay.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def ffmpeg_cmd(args):
    vf = "scale=-2:%d,crop=%d:%d" % (H, W, H)
    if args.test:
        src = ["-re", "-f", "lavfi", "-i", "testsrc2=size=%dx%d:rate=%d" % (W, H, args.fps)]
        vf = "null"
    else:
        # Cameras are fussy about the rate they are opened at; 30 is what
        # nearly all accept, and the output is thinned to --fps below.
        src = ["-f", "avfoundation", "-framerate", "30", "-i", "%s:none" % args.camera]
    return (["ffmpeg", "-hide_banner", "-loglevel", "error"] + src +
            ["-vf", vf, "-r", str(args.fps), "-q:v", str(args.quality),
             "-f", "image2pipe", "-vcodec", "mjpeg", "-"])


def frames(stream):
    """JPEGs out of ffmpeg's pipe: from each SOI to its EOI."""
    buf = b""
    while True:
        chunk = stream.read(8192)
        if not chunk:
            return
        buf += chunk
        while True:
            start = buf.find(b"\xff\xd8")
            if start < 0:
                buf = b""
                break
            end = buf.find(b"\xff\xd9", start + 2)
            if end < 0:
                buf = buf[start:]
                break
            yield buf[start:end + 2]
            buf = buf[end + 2:]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--camera", default="0", help="avfoundation video device, index or name")
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--quality", type=int, default=7, help="ffmpeg -q:v, 2 (best) .. 31")
    ap.add_argument("--test", action="store_true", help="a test pattern instead of a camera")
    ap.add_argument("--list", action="store_true", help="list the cameras and exit")
    args = ap.parse_args()

    if args.list:
        r = subprocess.run(["ffmpeg", "-hide_banner", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
                           capture_output=True, text=True)
        print("\n".join(l for l in r.stderr.splitlines() if "AVFoundation" in l and "]" in l))
        return 0

    r = relay()
    r.need_pyserial()
    import serial.tools.list_ports
    mac = r.registry().get("watch", r.WATCH_MAC)
    dev = r.find_port(mac.upper(), list(serial.tools.list_ports.comports()))
    if not dev:
        print("watch-cam: watch (%s) is not on USB" % mac, file=sys.stderr)
        return 1
    port = r.open_quietly(dev)
    time.sleep(0.5)                     # as usb-tell: bytes sent at once are lost

    ff = subprocess.Popen(ffmpeg_cmd(args), stdout=subprocess.PIPE)
    sent, t0, last_say = 0, time.time(), time.time()
    try:
        for jpg in frames(ff.stdout):
            if len(jpg) > MAX_FRAME:
                continue                # watch would refuse it; the next will be smaller
            port.write(b"!jpg %d\n" % len(jpg) + jpg)
            sent += 1
            now = time.time()
            if now - last_say >= 5:
                last_say = now
                print("watch-cam: %d frames, %.1f fps, last %d bytes" % (sent, sent / (now - t0), len(jpg)))
    except KeyboardInterrupt:
        pass
    finally:
        ff.terminate()
        port.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
