#!/usr/bin/env python3
"""
ws_client_test.py — Interactive WS test client for xdrd.

Usage:
    python3 ws_client_test.py [password] [host] [port]

Defaults: password=test  host=localhost  port=7374

After connecting and authenticating it prints all messages received from the
tuner and lets you type commands to send (e.g. T87500 to tune to 87.5 MHz).
Press Ctrl+C to exit.
"""

import asyncio
import hashlib
import sys

try:
    import websockets
except ImportError:
    print("Missing dependency: pip3 install websockets")
    sys.exit(1)

PASSWORD = sys.argv[1] if len(sys.argv) > 1 else "test"
HOST     = sys.argv[2] if len(sys.argv) > 2 else "localhost"
PORT     = int(sys.argv[3]) if len(sys.argv) > 3 else 7374


async def recv_line(ws):
    """Receive one WS frame and return it stripped."""
    msg = await ws.recv()
    return msg.strip() if isinstance(msg, str) else msg.decode().strip()


async def main():
    uri = f"ws://{HOST}:{PORT}"
    print(f"Connecting to {uri} ...")

    async with websockets.connect(uri) as ws:
        # 1. Receive salt
        salt = await recv_line(ws)
        print(f"[auth] salt received: {salt}")

        # 2. Compute SHA1(salt + password)
        digest = hashlib.sha1((salt + PASSWORD).encode()).hexdigest()
        print(f"[auth] sending hash:  {digest}")
        await ws.send(digest)

        # 3. Check server response. Authenticated users get no 'a' frame at
        #    all, so don't block forever waiting for one on an idle tuner.
        try:
            response = await asyncio.wait_for(recv_line(ws), timeout=2)
        except asyncio.TimeoutError:
            response = None

        if response == "a0":
            print("[auth] FAILED — wrong password")
            return
        elif response == "a1":
            print("[auth] connected as GUEST (read-only)")
        else:
            print("[auth] authenticated OK")
            if response is not None:
                print(f"[tuner] {response}")

        print("\nListening for tuner data (Ctrl+C to quit).")
        print("Type a command and press Enter to send (e.g. T87500):\n")

        # 4. Receive tuner data and allow sending commands concurrently
        async def reader():
            async for message in ws:
                line = message.strip() if isinstance(message, str) else message.decode().strip()
                print(f"[tuner] {line}")

        async def writer():
            loop = asyncio.get_event_loop()
            while True:
                cmd = await loop.run_in_executor(None, sys.stdin.readline)
                cmd = cmd.strip()
                if cmd:
                    await ws.send(cmd + "\n")

        await asyncio.gather(reader(), writer())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nDisconnected.")
    except Exception as e:
        print(f"Error: {e}")
