import os
import subprocess
import sys

CHUNK_SIZE = 0x8000
START_ADDR = 0x0
END_ADDR   = 0x400000
PORT       = "COM3"
BAUD       = 115200
CHIP       = "esp32c3"
ESPTOOL    = "./esptool"
OUTDIR     = "dump"

os.makedirs(OUTDIR, exist_ok=True)

chunks = []
cur = START_ADDR
while cur < END_ADDR:
    nxt = min(cur + CHUNK_SIZE, END_ADDR)
    chunks.append((cur, nxt - cur))
    cur = nxt

print(f"Reading {hex(START_ADDR)}..{hex(END_ADDR)} in {hex(CHUNK_SIZE)} chunks on {PORT} @ {BAUD}\n")

for addr, length in chunks:
    outfile = os.path.join(OUTDIR, f"flash_{hex(addr)}.bin")
    if os.path.exists(outfile) and os.path.getsize(outfile) == length:
        print(f"[{hex(addr)}] already present ({length} bytes), skipping")
        continue

    cmd = [
        ESPTOOL, "--chip", CHIP, "--port", PORT, "--baud", str(BAUD),
        "--after", "no-reset",
        "read-flash", hex(addr), hex(length), outfile,
    ]

    attempt = 0
    while True:
        attempt += 1
        print(f"[{hex(addr)} len {hex(length)}] attempt {attempt}...")
        result = subprocess.run(cmd)
        if result.returncode == 0 and os.path.exists(outfile) and os.path.getsize(outfile) == length:
            print(f"  OK {hex(addr)} -> {outfile}\n")
            break
        print(f"  FAILED attempt {attempt} for {hex(addr)} - retrying...\n")

full = os.path.join(OUTDIR, "flash_full.bin")
with open(full, "wb") as out:
    for addr, length in chunks:
        with open(os.path.join(OUTDIR, f"flash_{hex(addr)}.bin"), "rb") as f:
            out.write(f.read())

size = os.path.getsize(full)
print(f"Concatenated -> {full}")
print(f"Final size: {size} bytes (expect {END_ADDR - START_ADDR} = {hex(END_ADDR - START_ADDR)})")
if size != END_ADDR - START_ADDR:
    print("WARNING: size mismatch")
    sys.exit(1)
print("Done.")