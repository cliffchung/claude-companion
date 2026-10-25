#!/usr/bin/env python3
"""The image size guard (Plan 6, spec §6.5).

    python3 firmware/tools/check_image_size.py <partitions.csv> <claude_companion.bin>

Prints "image <n> B, slot <s> B, headroom <h> B (<p> %)" against the smallest app partition, and
"WARNING: under 1 MB of headroom" below 1,048,576 B. Exits 1 only when the image doesn't fit (ESP-IDF's own
check fails that build too), else 0. The build runs it after every app binary (firmware/CMakeLists.txt).
"""
import os
import sys

WARN_BELOW = 1024 * 1024


def parse_size(s):
    s = s.strip()
    mult = 1
    if s[-1:] in ('K', 'k'):
        mult, s = 1024, s[:-1]
    elif s[-1:] in ('M', 'm'):
        mult, s = 1024 * 1024, s[:-1]
    return int(s, 0) * mult


def app_slots(csv_text):
    sizes = []
    for line in csv_text.splitlines():
        cols = [c.strip() for c in line.split('#', 1)[0].split(',')]
        if len(cols) >= 5 and cols[1] == 'app' and cols[4]:
            sizes.append(parse_size(cols[4]))
    return sizes


def main(argv):
    if len(argv) != 3:
        print('usage: check_image_size.py <partitions.csv> <image.bin>', file=sys.stderr)
        return 2
    with open(argv[1], encoding='utf-8') as f:
        slots = app_slots(f.read())
    if not slots:
        print(f'check_image_size: no app partition in {argv[1]}', file=sys.stderr)
        return 2
    slot = min(slots)
    image = os.path.getsize(argv[2])
    headroom = slot - image
    print(f'image {image} B, slot {slot} B, headroom {headroom} B ({round(100 * headroom / slot)} %)')
    if headroom < 0:
        print(f'check_image_size: the image is {-headroom} B larger than the app slot', file=sys.stderr)
        return 1
    if headroom < WARN_BELOW:
        print('WARNING: under 1 MB of headroom')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
