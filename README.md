# MeshPi

Decentralized mesh network between Raspberry Pi (or any Linux with Wi-Fi) using BATMAN-adv over IBSS/Ad-Hoc.

## Overview

MeshPi turns a set of Wi-Fi-capable machines into a **multi-hop** mesh network — any node can reach any other, even if they are outside direct radio range, because intermediate nodes forward packets automatically.

The architecture combines two layers:

- **IBSS (Ad-Hoc)** — peer-to-peer Wi-Fi mode, no access point required.
- **BATMAN-adv** — Layer 2 mesh protocol. It routes packets between nodes that are not within direct radio range, creating a single virtual IP subnet on the `bat0` interface.

## Repository Contents

| File | Purpose |
|---|---|
| `script-mesh.sh` | Main script (bash): creates the mesh with automatic Wi-Fi interface detection |
| `meshpi-tui.c` | Text-based UI (ncurses) with 4 interactive functions |
| `meshpi-tui` | Pre-compiled binary (x86_64); recompile on ARM |
| `README.md` | This documentation |
| `LICENSE` | MIT license |
| `.gitignore` | Git ignore rules |
| `install.sh` | Dependency installer and TUI builder |

## Customization

All network settings are defined as variables at the top of `script-mesh.sh`. Edit them before running:

```bash
# --- CONFIGURATION (edit as needed) ---
SSID="MeshPi-Network"     # Name broadcast on the mesh
CONN_NAME="MeshPi"        # NetworkManager connection label
CHANNEL=6                 # Wi-Fi channel (fallbacks: 1, 11)
# --------------------------------------
```

Every machine on the same mesh must use the same SSID and channel. Only the IP address differs per node.

## Requirements

- Linux with `nmcli` (NetworkManager), `batctl` and `iw`
- Wi-Fi card that supports IBSS/Ad-Hoc mode (tested on Raspberry Pi 3/4 and notebooks with iwlwifi, mt76, ath10k)
- `sudo` access (the script runs privileged commands)
- For the TUI: `libncurses-dev` and `gcc`

### Installing Dependencies

```bash
sudo apt update
sudo apt install -y batctl iw libncurses-dev
sudo modprobe batman-adv
```

To load `batman-adv` automatically at boot:

```bash
echo batman-adv | sudo tee /etc/modules-load.d/batman-adv.conf
```

## Quick Start

### 1. Direct script (`script-mesh.sh`)

```bash
# Automatic Wi-Fi interface detection
sudo ./script-mesh.sh 192.168.1.10

# Or explicit interface (useful on Raspberry Pi — wlan0)
sudo ./script-mesh.sh wlan0 192.168.1.10
```

Assign a different IP to each node within the same /24 subnet:

| Node | Command | IP |
|---|---|---|
| PC1 | `./script-mesh.sh 192.168.1.10` | .10 |
| PC2 | `./script-mesh.sh 192.168.1.11` | .11 |
| PC3 | `./script-mesh.sh 192.168.1.12` | .12 |

### 2. TUI ncurses (`meshpi-tui`)

Compile once:

```bash
sudo apt install -y libncurses-dev gcc
gcc -o meshpi-tui meshpi-tui.c -lncurses
```

Run:

```bash
sudo ./meshpi-tui
```

#### Menu

| Option | Function |
|---|---|
| **1. Find network** | Scan active IBSS networks and list mesh neighbors |
| **2. Setup network** | Prompt for IP and run `script-mesh.sh` |
| **3. Restart network** | Tear down the mesh and recreate with a new IP |
| **4. Monitor network** | Live panel (2s refresh): Wi-Fi status, BATMAN neighbors, ping |
| **5. Quit** | |

Monitor shortcuts: `p` to change ping target IP, `r` to skip ping, `q` to quit.

## Network Verification

```bash
# List mesh neighbors (should show MAC addresses of other nodes)
batctl o

# Mesh traceroute (shows hop count to destination)
batctl tr 192.168.1.11

# Reachability test
ping -c 4 192.168.1.11

# Wi-Fi interface status
iwconfig
```

`batctl o` displays the BATMAN-adv originator table. The columns are:

- **Originator** — MAC address of the remote node
- **last-seen** — time since the last OGM update
- **Nexthop** — MAC address of the next hop towards the originator
- **outgoingIF** — output interface

Nodes may take up to **30 seconds** to discover each other (BATMAN-adv OGM interval).

## Internal Architecture

### `script-mesh.sh` Flow

```
  ┌─────────────────────────────────────────────────────────┐
  │ 1. Detect Wi-Fi interface (3 strategies + fallback)      │
  ├─────────────────────────────────────────────────────────┤
  │ 2. Install batctl/iw if missing                         │
  ├─────────────────────────────────────────────────────────┤
  │ 3. Load batman-adv module (modprobe)                    │
  ├─────────────────────────────────────────────────────────┤
  │ 4. Remove previous connection ($CONN_NAME)               │
  ├─────────────────────────────────────────────────────────┤
  │ 5. Create IBSS via nmcli (SSID: MeshPi-Network)          │
  │    - Channel 6 (fallback → 1 → 11)                      │
  ├─────────────────────────────────────────────────────────┤
  │ 6. Remove link-local IP from Wi-Fi interface            │
  │    (individual ip addr del — NOT ip addr flush)         │
  ├─────────────────────────────────────────────────────────┤
  │ 7. batctl if add <interface> → creates bat0             │
  ├─────────────────────────────────────────────────────────┤
  │ 8. Assign configured IP to bat0                         │
  ├─────────────────────────────────────────────────────────┤
  │ 9. Disable Wi-Fi power saving                           │
  └─────────────────────────────────────────────────────────┘
```

### Network Stack

```
┌──────────────────┐
│   Application    │  (ping, iperf3, etc.)
├──────────────────┤
│   IPv4 (bat0)    │  192.168.1.x/24
├──────────────────┤
│  BATMAN-adv L2   │  Multi-hop mesh routing
├──────────────────┤
│  IBSS (Ad-Hoc)   │  Peer-to-peer Wi-Fi (physical layer)
├──────────────────┤
│  Wi-Fi Card      │  wlan0 / wlp5s1 / etc.
└──────────────────┘
```

## Portability

### Raspberry Pi (ARM)

The `meshpi-tui` binary compiled on x86_64 will not run on ARM. Recompile on the Pi itself:

```bash
sudo apt install -y gcc libncurses-dev
gcc -o meshpi-tui meshpi-tui.c -lncurses
```

### Script Path in the TUI

`meshpi-tui.c` auto-detects the `script-mesh.sh` location at runtime: it tries the directory of the executable, then the current working directory, then the compile-time `SCRIPT_DIR` fallback. No manual configuration needed.

## Wi-Fi Interface Detection

Both the script and the TUI use three cascading strategies:

| Priority | Command | Coverage |
|---|---|---|
| 1st | `iw dev ... Interface` | Modern drivers (iwlwifi, mt76, ath10k) |
| 2nd | `ip link show ... wl*` | Any Linux |
| 3rd | `iwconfig ... IEEE` | Legacy drivers |
| Fallback | Prompt user | (TUI only) |

## Troubleshooting

### "Channel is disabled" on channel 6

Regulatory domain restriction. The script automatically tries channels 1 and 11. If the issue persists, set the correct regulatory domain for your country:

```bash
# Check current domain
iw reg get

# Set your country code (e.g., BR for Brazil, DE for Germany, US for USA)
sudo iw reg set <COUNTRY_CODE>
```

### "Address already assigned" in the IP removal loop

Harmless — it means there was no link-local IP to remove. The script continues normally.

### Interface reverts to "Mode:Managed"

NetworkManager may reclaim the interface. The script sets `nmcli device set managed no`, but restarting NetworkManager while the mesh is active will break the IBSS connection.

### Nodes do not show up in `batctl o`

1. Verify all nodes are on the same channel: `iwconfig` should show identical `Frequency` and `Cell` MAC.
2. Wait up to 30 seconds (BATMAN-adv OGM period).
3. Verify the SSID is identical on all machines.

### Slow or unstable mesh

- Power saving is disabled by the script (re-run it if power saving was re-enabled).
- On laptops, keep the power cable connected — some chips disable Wi-Fi on low battery.
- Test throughput with `iperf3`.

## Example Topology

```
[PC1] ── Wi-Fi ── [PC2] ── Wi-Fi ── [PC3]
  .10                .11                .12
       (in range)          (in range)
       └── not in direct range ──┘
       PC1 ↔ PC3 works via BATMAN-adv (hops through PC2)
```

## License

MIT