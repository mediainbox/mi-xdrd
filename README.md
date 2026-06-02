# xdrd

Server daemon for XDR-F1HD / XDR-GTK FM tuners. Exposes the tuner over the network so multiple clients can connect simultaneously.

Supports two independent transports that can run concurrently:

| Transport | Protocol | Default port |
|-----------|----------|-------------|
| **TCP raw** | Newline-delimited text | 7373 |
| **WebSocket** | RFC 6455, same text messages as WS frames | 7374 (optional) |

---

## Requirements

| Dependency | Linux | macOS |
|---|---|---|
| gcc | `apt install gcc` | Xcode CLT |
| OpenSSL | `apt install libssl-dev` | `brew install openssl` |
| pthreads | included in glibc | included in SDK |

---

## Build

```bash
# Linux
make

# macOS (OpenSSL path detected automatically via brew)
make

# Run the synthetic WebSocket test suite
make test
```

`make test` runs 11 tests covering the full WS stack without any hardware:

```
[PASS] ws_handshake: valid upgrade (RFC 6455 example key)
[PASS] ws_handshake: missing Sec-WebSocket-Key rejected
[PASS] ws_handshake: header name is case-insensitive
[PASS] ws_frame:    write/read roundtrip (short payload)
[PASS] ws_frame:    write/read roundtrip (200-byte, ext len)
[PASS] ws_frame:    masked client frame correctly unmasked
[PASS] ws_frame:    close frame returns 0
[PASS] ws_frame:    ping elicits pong, next frame returned
[PASS] auth_hash:   correct hash accepted
[PASS] auth_hash:   wrong hash rejected
[PASS] ws_auth:     full salt/SHA1 challenge-response flow
11 passed, 0 failed
```

---

## Usage

```
xdrd [ -s serial ] [ -t port ] [ -w wsport ] [ -u users ]
     [ -p password ] [ -f command ] [ -l command ]
     [ -hgxb ]
```

### Options

| Flag | Description | Default |
|------|-------------|---------|
| `-s` | Serial port of the tuner | `/dev/ttyUSB0` |
| `-t` | TCP raw port | `7373` |
| `-w` | WebSocket port (disabled if omitted) | — |
| `-p` | Password (required) | — |
| `-u` | Maximum simultaneous users | `10` |
| `-g` | Allow guest login (read-only, no password) | off |
| `-x` | Power tuner off when last user disconnects | off |
| `-f` | Command to run when first user connects | — |
| `-l` | Command to run when last user disconnects | — |
| `-b` | Run in background (daemon mode) | off |

### Examples

**TCP only (original behaviour):**
```bash
./xdrd -s /dev/ttyUSB0 -p mypassword
```

**TCP + WebSocket on separate ports:**
```bash
./xdrd -s /dev/ttyUSB0 -t 7373 -w 7374 -p mypassword
```

**macOS with USB modem:**
```bash
./xdrd -s /dev/tty.usbmodem21103 -t 7373 -w 7374 -p mypassword
```

**Background daemon:**
```bash
./xdrd -s /dev/ttyUSB0 -t 7373 -w 7374 -p mypassword -b
```

---

## Authentication

Both transports share the same challenge–response mechanism:

1. Server sends a 16-character random salt as plain text (or WS frame).
2. Client responds with `SHA1(salt + password)` as a lowercase hex string (40 chars).
3. Server replies with `a0` (rejected) or `a1` (accepted as guest). Authenticated users receive no reply — the tuner data stream starts immediately.

**With the `-g` flag** guests are allowed without a password and receive read-only access (commands they send are ignored).

---

## Transport comparison

### TCP raw

```
Client ──── TCP ──── xdrd ──── serial ──── tuner
```

Messages are newline-terminated UTF-8 strings sent directly over a TCP stream. No framing overhead.

**Pros**
- Minimal overhead — zero protocol bytes beyond the `\n` terminator.
- Works with netcat, telnet, and any raw TCP socket library.
- Compatible with the original xdr-gtk client and all legacy tools.
- Slightly lower latency at very high message rates.

**Cons**
- Not accessible from a browser without a proxy.
- Requires implementing the custom stream framing on the client side.
- Blocked by most corporate firewalls and reverse proxies (non-HTTP traffic).
- No standard tooling for debugging (no browser DevTools, no standard WS clients).

**Connect:**
```bash
nc localhost 7373
# Server sends: AbCdEfGh01234567
# You send:     <sha1(salt+password)>
```

---

### WebSocket

```
Browser / JS / Python ──── HTTP Upgrade ──── WS frames ──── xdrd ──── serial ──── tuner
```

Messages are identical to TCP raw but wrapped in RFC 6455 WebSocket frames. The same tuner data is broadcast to both TCP and WS clients simultaneously.

**Pros**
- **Browser native** — connect directly from any web page with `new WebSocket(...)`.
- Standard protocol — client libraries exist for every language (JS, Python, Go, Rust, etc.).
- Travels over HTTP port 80/443 through nginx/Apache reverse proxy with `proxy_pass`.
- Firewall and CDN friendly (looks like regular HTTP traffic).
- Built-in ping/pong keep-alive handled transparently by the server.
- Debuggable with browser DevTools Network tab, `wscat`, `websocat`.

**Cons**
- ~2–6 bytes of frame overhead per message (2 bytes header server→client; 6 bytes header + XOR mask client→server).
- Two `send()` syscalls per broadcast message instead of one (header + payload).
- HTTP upgrade handshake adds ~1 round-trip on connection setup.
- Incompatible with legacy xdr-gtk clients (they expect raw TCP).

> **In practice the overhead is negligible.** The serial port runs at 115 200 baud (~11.5 KB/s), which is the real throughput ceiling. WebSocket framing costs are completely absorbed by that limit.

---

## Testing the WebSocket port

A Python test client is included:

```bash
pip3 install websockets
python3 ws_client_test.py <password> [host] [port]
```

Example session:
```
Connecting to ws://localhost:7374 ...
[auth] salt received: XI0DO7AebkU3iDkR
[auth] sending hash:  8f14dd1d84ba5bb989edecdf618cf39db91e7101
[auth] authenticated OK
[tuner] Ss49.1,0,0,-1

Listening for tuner data (Ctrl+C to quit).
Type a command and press Enter to send (e.g. T87500):

T98700
[tuner] Ss62.3,1,1,-1
[tuner] P1234
[tuner] Rsome station name
```

---

## Protocol reference

Messages are newline-terminated ASCII strings. All commands and responses use the same format on both transports.

### Client → server (commands)

| Command | Example | Description |
|---------|---------|-------------|
| `T<freq>` | `T87500` | Tune to frequency in kHz |
| `M<mode>` | `M0` | Set mode (0=FM, 1=AM) |
| `Y<vol>` | `Y100` | Set volume (0–100) |
| `D<de>` | `D0` | De-emphasis (0=75µs, 1=50µs) |
| `A<agc>` | `A2` | AGC setting |
| `F<filter>` | `F3` | Filter bandwidth |
| `W<bw>` | `W180` | Bandwidth in kHz |
| `Z<ant>` | `Z0` | Antenna input |
| `G<gain>` | `G00` | RF gain |
| `V<daa>` | `V0` | DAA setting |
| `Q<sq>` | `Q0` | Squelch level |
| `C<rot>` | `C180` | Rotator position |
| `I<s>,<d>` | `I100,0` | Sampling interval and detector |
| `X` | `X` | Shutdown / disconnect |

### Server → client (tuner data)

| Message | Example | Description |
|---------|---------|-------------|
| `Ss<level>,<st>,<rds>,<mp>` | `Ss62.3,1,1,-1` | Signal level (dBf), stereo, RDS present, multipath |
| `T<freq>` | `T87500` | Frequency confirmed |
| `M<mode>` | `M0` | Mode confirmed |
| `P<pi>` | `P1234` | RDS Programme Identification code |
| `R<text>` | `Rsome station` | RDS RadioText or PS name |
| `a0` | | Auth failed |
| `a1` | | Accepted as guest |
| `X` | | Tuner powered off |

---

## Running without hardware (development)

Use `socat` to create a virtual serial port pair:

```bash
# Terminal 1 — virtual serial pair
brew install socat   # or apt install socat
socat -d -d pty,raw,echo=0 pty,raw,echo=0
# prints: PTY is /dev/ttys004  and  PTY is /dev/ttys005

# Terminal 2 — run xdrd on one end
./xdrd -s /dev/ttys004 -t 7373 -w 7374 -p mypassword

# Terminal 3 — inject fake tuner data from the other end
echo -e "T87500\nSs62.3,1,1,-1\n" > /dev/ttys005
```

---

## License

GPL-2.0-or-later — see [LICENSE](LICENSE).  
Original code © 2013–2023 Konrad Kosmatka — http://fmdx.pl/
