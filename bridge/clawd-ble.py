#!/usr/bin/env python3
# ───────────────────────────────────────────────────────────────
# Clawd BLE bridge — reads k=v lines on stdin (from clawd-server.js)
# and forwards them to the Artemis watch over BLE (Nordic UART).
# Runs forever: scans, connects, reconnects. Exits on stdin EOF.
# ───────────────────────────────────────────────────────────────
import asyncio
import sys

from bleak import BleakClient, BleakScanner

NAME = "Clawd Artemis"
NUS_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"


async def stdin_reader() -> asyncio.StreamReader:
    loop = asyncio.get_running_loop()
    reader = asyncio.StreamReader()
    await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
    return reader


async def write_line(client: BleakClient, line: bytes) -> None:
    if not line.endswith(b"\n"):
        line += b"\n"
    chunk = max(20, (client.mtu_size or 23) - 3)
    for i in range(0, len(line), chunk):
        await client.write_gatt_char(NUS_RX, line[i:i + chunk], response=False)


async def main() -> None:
    reader = await stdin_reader()
    latest = None  # last line seen while disconnected → replay on connect

    while True:
        dev = await BleakScanner.find_device_by_name(NAME, timeout=10)
        if dev is None:
            print("scan: watch not in range", flush=True)
            # keep draining stdin so the pipe never fills; remember only the latest
            try:
                while True:
                    line = await asyncio.wait_for(reader.readline(), timeout=8)
                    if not line:
                        return  # parent closed stdin → exit
                    latest = line
            except asyncio.TimeoutError:
                continue

        try:
            async with BleakClient(dev) as client:
                print(f"connected {dev.address}", flush=True)
                if latest:
                    await write_line(client, latest)
                    latest = None
                while True:
                    line = await reader.readline()
                    if not line:
                        return  # stdin EOF
                    await write_line(client, line)
        except Exception as e:  # disconnect, out of range, GATT error → rescan
            print(f"reconnect: {type(e).__name__}: {e}", flush=True)
            await asyncio.sleep(3)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
