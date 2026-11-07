# CivSwitch Serial Bridge

Send CivSwitch payloads to your Switch through an ESP32 over USB — no need to put your PC on the same Wi-Fi as the console.

The ESP32 creates its own isolated Wi-Fi network. The Switch connects to that. Your PC talks to the ESP32 over USB serial, and the ESP32 relays the payload to the Switch over TCP. Your PC never leaves its normal network.

```
PC (normal Wi-Fi) ---USB serial---> ESP32 ---Wi-Fi AP---> Switch (TCP :9026)
```

## Why

CivSwitch's `send_js.py` requires your PC and Switch on the same network. That means either connecting the Switch to your home Wi-Fi (risky — Nintendo servers) or switching your PC to an air-gapped network (annoying — you lose internet).

This puts the ESP32 in between so you don't have to choose. The Switch is on a network with no internet path, and your PC stays online.

## What you need

- An ESP32 dev board (any variant — WROOM-32, S2, S3, C3, whatever you have)
- USB cable to connect it to your PC
- CivSwitch already set up and the TCP loader running on the Switch (`:9026`)
- Python 3 + `pyserial` (`pip install pyserial`)
- Arduino IDE with the ESP32 board package installed

## Flash the ESP32

1. Open `esp32-bridge/esp32-bridge.ino` in Arduino IDE
2. Select your board (ESP32 Dev Module works for most boards)
3. Select the correct port
4. Upload

The serial monitor (115200 baud) will show the AP details when it boots.

## Connect the Switch

1. On the Switch, connect to the `Switch2Bridge` network (open, no password)
2. The ESP32 spoofs Nintendo's connectivity-check endpoints, so the Switch sees the network as online and connects straight through — no captive-portal popup
3. Note the Switch's IP (check Settings > Internet, or it's usually `192.168.4.2`)

## Send payloads

With CivSwitch's TCP loader running on the Switch (launch Civ7, Load Game if you have the persistent installer, or use whatever method you set up):

```sh
python3 send_serial.py 192.168.4.2 payload.js
```

If you have multiple serial devices plugged in or auto-detection doesn't find it:

```sh
# Mac
python3 send_serial.py --port /dev/cu.usbserial-0001 192.168.4.2 payload.js

# Linux
python3 send_serial.py --port /dev/ttyUSB0 192.168.4.2 payload.js

# Windows
python3 send_serial.py --port COM3 192.168.4.2 payload.js
```

The default TCP port is 9026. You can specify a different one:

```sh
python3 send_serial.py 192.168.4.2 9027 payload.js
```

## Serial commands

You can also talk to the ESP32 directly over serial (115200 baud):

| Command | What it does |
|---------|-------------|
| `SEND <ip> <port> <len>` | Relay a payload (followed by `<len>` bytes of data) |
| `STATUS` | Show number of connected clients |
| `SCAN` | List connected clients by MAC address |

## Config

Edit these lines at the top of the `.ino` if you want different network settings:

```c
const char* AP_SSID     = "Switch2Bridge";
const int   AP_CHANNEL  = 6;
const int   MAX_CLIENTS = 4;
const int   LED_PIN     = 2;   // set to -1 to disable LED
```

The AP is open by default (no password). If you want one, pass it to `WiFi.softAP()` in `setup()`.

## How it works

The ESP32 runs a soft AP with DHCP, a DNS server (resolves all queries to itself), and an HTTP server that mimics Nintendo's connectivity-check endpoints (`conntest.nintendowifi.net`, `ctest.*.nintendo.net`, etc.) so the Switch considers itself online and skips the captive-portal popup entirely. The connectivity-check responses are adapted from [90DNS](https://github.com/Thesola10/90dns) (GPL-2.0-or-later) via [fernando-granco/switch2-homebrew](https://github.com/fernando-granco/switch2-homebrew) (MIT).

When you run `send_serial.py`, it sends the payload over USB serial to the ESP32 with a small header (`SEND <ip> <port> <length>`). The ESP32 opens a TCP connection to the Switch on its own Wi-Fi network, sends the payload, does a TCP half-close (so the loader knows the payload is complete), reads back the response, and prints it over serial.

## License

MIT
