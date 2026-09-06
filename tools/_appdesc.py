"""Prints the esp_app_desc_t from the head of an ESP-IDF application image."""
import sys

d = open(sys.argv[1], 'rb').read()
if len(d) < 0x120 or d[0] != 0xE9:
    print("no application image at this offset")
    raise SystemExit

B = 0x20  # 24-byte image header + one 8-byte segment header


def field(off, length):
    return d[B + off:B + off + length].split(b'\x00')[0].decode('ascii', 'replace')


print(f"project      {field(48, 32)}")
print(f"version      {field(16, 32)}")
print(f"built        {field(96, 16)} {field(80, 16)}")
print(f"idf version  {field(112, 32)}")
print(f"elf sha256   {d[B + 144:B + 176].hex()}")
