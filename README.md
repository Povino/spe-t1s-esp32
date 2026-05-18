# ESP32 LAN8651 10BASE-T1S Firmware

This repository contains ESP-IDF firmware used for practical experiments with a 10BASE-T1S Single Pair Ethernet multidrop network based on ESP32 development boards and LAN8651 MAC-PHY modules.

The code was prepared for the practical part of a diploma thesis focused on Single Pair Ethernet for sensor and IoT networks. It is intended for topology bring-up, connectivity verification, UDP throughput measurement, TCP throughput measurement, traffic-pattern testing, packet-loss observation, cable-length tests, PLCA configuration tests, and long-duration stability checks.

## Project overview

The repository contains four ESP-IDF projects:

| Folder | Purpose |
|---|---|
| `connectivity/` | Basic LAN8651 bring-up and UDP broadcast connectivity test. |
| `udp-measurement/` | Fixed-duration UDP measurement firmware with TX/RX counters, packet-loss calculation, and throughput output. |
| `tcp-measurement/` | Fixed-duration TCP measurement firmware with TCP client/server roles, record counters, and throughput output. |
| `traffic-patterns/` | UDP-based traffic-pattern firmware for testing selected sender and receiver scenarios on the multidrop bus. |

All projects use the same basic hardware assumptions:

- ESP32 development board
- Mikroe Two-Wire ETH Click board with LAN8651 10BASE-T1S MAC-PHY
- SPI connection between ESP32 and LAN8651
- Static IPv4 addressing in the `192.168.50.0/24` subnet
- Optional PLCA configuration
- Communication over the 10BASE-T1S bus

The firmware variants print structured result lines to the serial monitor. They do not automatically export results to CSV or spreadsheet files.

## Thesis context

This repository supports the experimental part of a diploma thesis by providing reproducible firmware for testing a 10BASE-T1S SPE network.

The firmware is used for:

- validating that LAN8651-based ESP32 nodes initialize correctly,
- verifying multidrop communication between several nodes,
- configuring PLCA node IDs and node count,
- measuring UDP payload throughput,
- measuring TCP payload throughput,
- measuring packet loss from received sequence numbers,
- testing the effect of cable length,
- comparing PLCA enabled and disabled configurations,
- testing selected communication patterns,
- running longer stability measurements.

The repository contains source code and build configuration only. Measurement logs and processed thesis results are not included unless added separately.

## Repository structure

```text
spe-t1s-esp32/
├── .gitignore
├── README.md
├── connectivity/
│   ├── CMakeLists.txt
│   ├── dependencies.lock
│   └── main/
│       ├── CMakeLists.txt
│       ├── Kconfig.projbuild
│       ├── idf_component.yml
│       └── t1s_lan8651_connectivity.c
├── udp-measurement/
│   ├── CMakeLists.txt
│   ├── dependencies.lock
│   └── main/
│       ├── CMakeLists.txt
│       ├── Kconfig.projbuild
│       ├── idf_component.yml
│       └── t1s_lan8651_udp_measurement.c
├── tcp-measurement/
│   ├── CMakeLists.txt
│   ├── dependencies.lock
│   └── main/
│       ├── CMakeLists.txt
│       ├── Kconfig.projbuild
│       ├── idf_component.yml
│       └── t1s_lan8651_tcp_measurement.c
└── traffic-patterns/
    ├── CMakeLists.txt
    ├── dependencies.lock
    └── main/
        ├── CMakeLists.txt
        ├── Kconfig.projbuild
        ├── idf_component.yml
        └── t1s_lan8651_traffic_patterns.c
```

### `connectivity/`

The `connectivity` project is intended for basic hardware and topology verification. It initializes the LAN8651, configures a static IP address, configures PLCA if enabled, and periodically sends a small UDP broadcast message.

It is useful for checking:

- SPI communication with the LAN8651,
- Ethernet link state,
- static IP configuration,
- PLCA ID and node-count configuration,
- basic UDP broadcast reception between nodes.

The firmware uses a conservative SPI clock for bring-up.

### `udp-measurement/`

The `udp-measurement` project is intended for practical UDP test runs. One configured sender transmits UDP broadcast packets for a fixed duration. Receiver nodes count packets and bytes per source node and print final result lines.

It is useful for:

- 2-node, 3-node, 4-node, and 5-node UDP broadcast measurements,
- cable-length tests,
- packet-loss measurements,
- long-duration stability tests,
- PLCA enabled/disabled comparison with the same traffic pattern.

The UDP measurement firmware supports one configured UDP broadcast sender. Other nodes receive and evaluate packets from the sender.

### `tcp-measurement/`

The `tcp-measurement` project is intended for TCP throughput measurements. One configured node acts as the TCP server and selected nodes act as TCP clients.

It is useful for:

- 2-node TCP baseline tests,
- 5-node many-to-one TCP tests,
- comparing TCP behaviour with UDP measurements,
- checking whether TCP communication remains stable on longer multidrop setups.

The TCP clients are selected by a bit mask. For example, a client mask of `30` selects nodes 1, 2, 3, and 4 when node 0 is used as the TCP server.

### `traffic-patterns/`

The `traffic-patterns` project is intended for testing selected logical traffic scenarios on the 10BASE-T1S bus.

It is useful for:

- one sender to all nodes,
- one sender to one selected logical destination,
- multiple simultaneous senders,
- many-to-one traffic scenarios.

The firmware uses UDP transport. Broadcast mode is used for one-to-many patterns. For selected one-to-one or many-to-one tests, the firmware can use application-level destination filtering. In that mode, traffic is transported on the local subnet, but only the configured logical destination counts the received packets in the result output.

## Hardware requirements

Tested hardware setup:

| Item | Notes |
|---|---|
| ESP32 development board | ESP32 DevKitC V4 and ESP32 DevKitV1 were used during development. |
| Mikroe Two-Wire ETH Click | LAN8651 10BASE-T1S MAC-PHY. |
| Single twisted pair cable | A twisted pair from UTP cable was used for final measurements. |
| USB cables | One USB connection per monitored/flashed ESP32 board. |
| Bus termination | Termination should be present only at both physical ends of the multidrop bus. |

### ESP32 to LAN8651 wiring

The firmware assumes the following ESP32 GPIO mapping:

| ESP32 pin | Two-Wire ETH Click / LAN8651 signal |
|---:|---|
| GPIO18 | SCK |
| GPIO19 | SDO |
| GPIO23 | SDI |
| GPIO25 | CS |
| GPIO22 | IRQ |
| 3V3 | 3.3 V |
| GND | GND |

The Click board `RST` pin is intentionally not connected. The firmware sets:

```c
phy_config.reset_gpio_num = -1;
```

This matches the tested setup.

### Hardware note about transistor Q1 on the Mikroe Two-Wire ETH Click board

During practical testing, the Mikroe Two-Wire ETH Click board required a hardware modification for reliable high-speed SPI communication with the ESP32. The board contains a transistor marked as `Q1` in the SPI chip-select path. In the tested setup, this transistor affected the chip-select signal timing and limited reliable SPI operation at higher clock rates.

The tested high-speed measurement setup used Click boards with `Q1` removed. With this modification and with the Click board `RST` pin left unconnected, the boards operated reliably at the SPI clock used for the final measurements. An unmodified Click board was observed to work at low SPI clock rates, but it failed or became unreliable at higher SPI clock rates.

### Wiring reliability note

Reliable jumper wiring is important, especially for the SPI and IRQ/INT connections. A node may initialize correctly and still fail under traffic if one of these signal connections is marginal. Symptoms can include very low packet counts, repeated send errors, or `last_errno=12` in traffic tests.

## Software requirements

The code was developed and tested with:

| Software | Version / note |
|---|---|
| ESP-IDF | Tested with ESP-IDF `v5.5.4` |
| ESP-IDF Component Manager | Used to download the LAN865x driver |
| LAN865x component | `espressif/lan865x` version resolved in `dependencies.lock` |
| Operating system | Windows was used during development - ESP-IDF itself is cross-platform |
| Serial driver | Depends on the ESP32 development board USB-UART chip, for example CP210x |

The ESP-IDF target is:

```text
esp32
```

## Dependency management

Each project contains its own `main/idf_component.yml`. The LAN865x driver is declared there:

```yaml
dependencies:
  idf:
    version: '>=5.5.0'

  espressif/lan865x: ^0.2.0
```

The resolved dependency versions are stored in `dependencies.lock`.

The `managed_components/` folders are generated by ESP-IDF Component Manager and are intentionally not committed.

## Setup from a clean checkout

Clone the repository:

```powershell
git clone https://github.com/Povino/spe-t1s-esp32.git
cd spe-t1s-esp32
```

Open an ESP-IDF terminal, or use the ESP-IDF PowerShell environment installed with Espressif-IDE.

Set the target for each project when building it for the first time:

```powershell
cd connectivity
idf.py set-target esp32
cd ..

cd udp-measurement
idf.py set-target esp32
cd ..

cd tcp-measurement
idf.py set-target esp32
cd ..

cd traffic-patterns
idf.py set-target esp32
cd ..
```

ESP-IDF will create local `sdkconfig` files. These files are node-specific and are not committed.

## Building the projects

### Connectivity firmware

```powershell
cd connectivity
idf.py reconfigure
idf.py build
```

### UDP measurement firmware

```powershell
cd udp-measurement
idf.py reconfigure
idf.py build
```

### TCP measurement firmware

```powershell
cd tcp-measurement
idf.py reconfigure
idf.py build
```

### Traffic-pattern firmware

```powershell
cd traffic-patterns
idf.py reconfigure
idf.py build
```

`idf.py reconfigure` is useful after changes to `CMakeLists.txt`, `Kconfig.projbuild`, or `idf_component.yml`. For ordinary `.c` file changes, `idf.py build` is sufficient.

## Configuration

Configuration is done using:

```powershell
idf.py menuconfig
```

The most important options are under the project menu.

### Common node configuration

| Option | Meaning |
|---|---|
| `APP_NODE_ID` | PLCA node ID and static IP selector. |
| `APP_NODE_COUNT` | Total number of nodes in the PLCA segment. |
| `APP_ENABLE_PLCA` | Enables or disables PLCA. |

Static IP assignment is derived from `APP_NODE_ID`:

| Node ID | Static IP |
|---:|---|
| 0 | `192.168.50.1` |
| 1 | `192.168.50.2` |
| 2 | `192.168.50.3` |
| 3 | `192.168.50.4` |
| 4 | `192.168.50.5` |

All nodes use:

```text
Netmask: 255.255.255.0
Gateway: 192.168.50.1
```

### Connectivity-specific configuration

| Option | Meaning |
|---|---|
| `APP_UDP_INTERVAL_MS` | Interval between periodic UDP broadcast messages. |

### UDP measurement-specific configuration

| Option | Meaning |
|---|---|
| `APP_TEST_SENDER_ID` | Node ID of the UDP sender. Other nodes receive only. |
| `APP_TEST_DURATION_S` | UDP test duration in seconds. |
| `APP_TEST_START_DELAY_S` | Delay before the sender starts transmitting. |
| `APP_TEST_PAYLOAD_BYTES` | Generated UDP measurement payload size in bytes. |
| `APP_TEST_TX_DELAY_US` | Delay between transmitted UDP packets. Use `0` for maximum send rate. |
| `APP_TEST_UDP_PORT` | UDP port used by the measurement test. |

Example UDP measurement settings:

```text
APP_TEST_PAYLOAD_BYTES = 1200
APP_TEST_TX_DELAY_US   = 0
APP_TEST_DURATION_S    = 30
APP_TEST_SENDER_ID     = 0
APP_TEST_UDP_PORT      = 5005
```

For long-duration stability testing, the duration can be increased if the configured Kconfig range allows it:

```text
APP_TEST_DURATION_S = 3600
```

The exact settings should be recorded together with each measurement run.

### TCP measurement-specific configuration

| Option | Meaning |
|---|---|
| `APP_TCP_SERVER_ID` | Node ID of the TCP server. |
| `APP_TCP_CLIENT_MASK` | Bit mask selecting TCP client nodes. |
| `APP_TEST_DURATION_S` | TCP test duration in seconds. |
| `APP_TEST_START_DELAY_S` | Delay before the TCP clients start transmitting. |
| `APP_TEST_PAYLOAD_BYTES` | Generated TCP test payload size in bytes. |
| `APP_TEST_TX_DELAY_US` | Delay between transmitted TCP records. Use `0` for maximum send rate. |
| `APP_TEST_TCP_PORT` | TCP port used by the measurement test. |

Example 5-node many-to-one TCP setup:

```text
APP_NODE_COUNT         = 5
APP_TCP_SERVER_ID      = 0
APP_TCP_CLIENT_MASK    = 30
APP_TEST_DURATION_S    = 30
APP_TEST_PAYLOAD_BYTES = 1200
APP_TEST_TX_DELAY_US   = 0
APP_TEST_TCP_PORT      = 5006
```

In this example, node 0 is the TCP server and nodes 1, 2, 3, and 4 are TCP clients.

### Traffic-pattern-specific configuration

| Option | Meaning |
|---|---|
| `APP_TEST_SENDER_MASK` | Bit mask selecting traffic-pattern sender nodes. |
| `APP_TEST_DEST_BROADCAST` | Selects broadcast-to-all mode when enabled. |
| `APP_TEST_DEST_NODE_ID` | Logical destination node ID when application-unicast mode is used. |
| `APP_TEST_DURATION_S` | Traffic-pattern test duration in seconds. |
| `APP_TEST_START_DELAY_S` | Delay before sender nodes start transmitting. |
| `APP_TEST_PAYLOAD_BYTES` | Generated UDP test payload size in bytes. |
| `APP_TEST_TX_DELAY_US` | Delay between transmitted UDP packets. Use `0` for maximum send rate. |
| `APP_TEST_UDP_PORT` | UDP port used by the traffic-pattern test. |

Sender mask examples:

| Sender mask | Active sender nodes |
|---:|---|
| `1` | Node 0 |
| `2` | Node 1 |
| `4` | Node 2 |
| `8` | Node 3 |
| `16` | Node 4 |
| `6` | Nodes 1 and 2 |
| `30` | Nodes 1, 2, 3, and 4 |

Example broadcast pattern:

```text
APP_NODE_COUNT         = 5
APP_TEST_SENDER_MASK   = 1
Destination mode       = Broadcast to all nodes
APP_TEST_DURATION_S    = 30
APP_TEST_PAYLOAD_BYTES = 1200
APP_TEST_TX_DELAY_US   = 0
APP_TEST_UDP_PORT      = 5005
```

This represents:

```text
Node 0 -> all nodes
```

Example application-unicast pattern:

```text
APP_NODE_COUNT          = 5
APP_TEST_SENDER_MASK    = 1
Destination mode        = Unicast to one destination node
APP_TEST_DEST_NODE_ID   = 4
APP_TEST_DURATION_S     = 30
APP_TEST_PAYLOAD_BYTES  = 1200
APP_TEST_TX_DELAY_US    = 0
APP_TEST_UDP_PORT       = 5005
```

This represents:

```text
Node 0 -> Node 4
```

Only the configured logical destination counts the packets in application-unicast mode.

## Flashing and monitoring

Replace `COM5`, `COM11`, and other ports with the actual serial ports on the host PC.

### Flash Node 0

```powershell
idf.py menuconfig
```

Set:

```text
APP_NODE_ID = 0
APP_NODE_COUNT = 2
```

Then flash and monitor:

```powershell
idf.py -p COM5 flash monitor
```

### Flash Node 1

```powershell
idf.py menuconfig
```

Set:

```text
APP_NODE_ID = 1
APP_NODE_COUNT = 2
```

Then flash and monitor:

```powershell
idf.py -p COM11 flash monitor
```

For multidrop tests, flash each physical board with a unique `APP_NODE_ID` while keeping the remaining test settings identical unless the selected test scenario requires otherwise.

Exit the ESP-IDF monitor with:

```text
Ctrl + ]
or
Ctrl + T Ctrl + X
```

## Running the connectivity firmware

Use the `connectivity/` project.

For a 2-node test:

| Node | COM example | Node ID | Node count | Expected IP |
|---|---:|---:|---:|---|
| Node 0 | `COM5` | 0 | 2 | `192.168.50.1` |
| Node 1 | `COM11` | 1 | 2 | `192.168.50.2` |

Expected successful output includes:

```text
Chip ID verified: LAN8651
PLCA coordinator enabled: node_id=0, node_count=2
Ethernet Link Up
Ethernet Got IP
```

or on a non-coordinator node:

```text
PLCA normal node enabled: node_id=1
Ethernet Link Up
Ethernet Got IP
```

When both nodes are running, each node should periodically receive UDP messages from the other node.

## Running the UDP measurement firmware

Use the `udp-measurement/` project.

For a 2-node UDP measurement:

| Node | Node ID | Role |
|---|---:|---|
| Node 0 | 0 | Sender |
| Node 1 | 1 | Receiver |

Recommended settings for a typical UDP measurement run:

```text
APP_NODE_COUNT = 2
APP_TEST_SENDER_ID = 0
APP_TEST_PAYLOAD_BYTES = 1200
APP_TEST_TX_DELAY_US = 0
APP_TEST_DURATION_S = 30
APP_TEST_UDP_PORT = 5005
```

Expected sender result:

```text
RESULT_TX node=0 packets=... errors=... udp_bytes=... payload_bytes=... duration_s=... udp_mbps=... payload_mbps=...
```

Expected receiver result:

```text
RESULT_RX local_node=1 src_node=0 packets=... expected=... lost=... loss_pct=... udp_bytes=... payload_bytes=... duration_s=... udp_mbps=... payload_mbps=...
```

A successful run should show:

- `errors=0` on the sender,
- all receiver nodes printing `RESULT_RX`,
- low or zero packet loss,
- no link-down events during the test.

## Running the TCP measurement firmware

Use the `tcp-measurement/` project.

For a 5-node many-to-one TCP measurement:

| Node | Node ID | Role |
|---|---:|---|
| Node 0 | 0 | TCP server |
| Node 1 | 1 | TCP client |
| Node 2 | 2 | TCP client |
| Node 3 | 3 | TCP client |
| Node 4 | 4 | TCP client |

Recommended settings:

```text
APP_NODE_COUNT = 5
APP_TCP_SERVER_ID = 0
APP_TCP_CLIENT_MASK = 30
APP_TEST_PAYLOAD_BYTES = 1200
APP_TEST_TX_DELAY_US = 0
APP_TEST_DURATION_S = 30
APP_TEST_TCP_PORT = 5006
```

Expected server result:

```text
RESULT_TCP_RX local_node=0 src_node=... records=... expected_records=... missing_records=... invalid_records=... record_bytes=... payload_bytes=... duration_s=... record_mbps=... payload_mbps=...
```

Expected client result:

```text
RESULT_TCP_TX node=... server=0 records=... errors=... record_bytes=... payload_bytes=... duration_s=... record_mbps=... payload_mbps=...
```

A successful TCP run should show:

- `errors=0` on the clients,
- `missing_records=0` on the server,
- `invalid_records=0` on the server,
- one result from each configured TCP client.

## Running the traffic-pattern firmware

Use the `traffic-patterns/` project.

### Example: Node 0 to all nodes

```text
APP_NODE_COUNT = 5
APP_TEST_SENDER_MASK = 1
Destination mode = Broadcast to all nodes
APP_TEST_PAYLOAD_BYTES = 1200
APP_TEST_TX_DELAY_US = 0
APP_TEST_DURATION_S = 30
APP_TEST_UDP_PORT = 5005
```

Expected output:

```text
Node 0:
RESULT_TRAFFIC_TX ...

Nodes 1, 2, 3, 4:
RESULT_TRAFFIC_RX ...
```

### Example: Node 0 to Node 4

```text
APP_NODE_COUNT = 5
APP_TEST_SENDER_MASK = 1
Destination mode = Unicast to one destination node
APP_TEST_DEST_NODE_ID = 4
APP_TEST_PAYLOAD_BYTES = 1200
APP_TEST_TX_DELAY_US = 0
APP_TEST_DURATION_S = 30
APP_TEST_UDP_PORT = 5005
```

Expected output:

```text
Node 0:
RESULT_TRAFFIC_TX ...

Node 4:
RESULT_TRAFFIC_RX ...
```

Nodes 1, 2, and 3 remain connected to the bus but should not count the packets in application-unicast mode.

### Example: Nodes 1 and 2 to all nodes

```text
APP_NODE_COUNT = 5
APP_TEST_SENDER_MASK = 6
Destination mode = Broadcast to all nodes
APP_TEST_PAYLOAD_BYTES = 1200
APP_TEST_TX_DELAY_US = 0
APP_TEST_DURATION_S = 30
APP_TEST_UDP_PORT = 5005
```

Expected output:

```text
Nodes 1 and 2:
RESULT_TRAFFIC_TX ...

Receiver nodes:
RESULT_TRAFFIC_RX for src_node=1 and src_node=2
```

### Example: Nodes 1, 2, 3, and 4 to Node 0

```text
APP_NODE_COUNT = 5
APP_TEST_SENDER_MASK = 30
Destination mode = Unicast to one destination node
APP_TEST_DEST_NODE_ID = 0
APP_TEST_PAYLOAD_BYTES = 1200
APP_TEST_TX_DELAY_US = 0
APP_TEST_DURATION_S = 30
APP_TEST_UDP_PORT = 5005
```

Expected output:

```text
Node 0:
RESULT_TRAFFIC_RX for src_node=1, 2, 3, and 4

Nodes 1, 2, 3, and 4:
RESULT_TRAFFIC_TX ...
```

## Multidrop topology

The physical bus should be wired as a line, not as a star.

Example 4-node topology:

```text
Node 0 ---- Node 1 ---- Node 2 ---- Node 3
```

Example 5-node topology:

```text
Node 0 ---- Node 1 ---- Node 2 ---- Node 3 ---- Node 4
```

Termination should be enabled only at the two physical ends of the bus.

Example for 4 nodes:

| Node | Termination |
|---|---|
| Node 0 | enabled |
| Node 1 | removed |
| Node 2 | removed |
| Node 3 | enabled |

Example for 5 nodes:

| Node | Termination |
|---|---|
| Node 0 | enabled |
| Node 1 | removed |
| Node 2 | removed |
| Node 3 | removed |
| Node 4 | enabled |

All nodes must use the same `APP_NODE_COUNT`.

## Useful commands

### Build

```powershell
idf.py build
```

Builds the current ESP-IDF project.

### Reconfigure

```powershell
idf.py reconfigure
```

Regenerates the CMake build configuration. Useful after editing `CMakeLists.txt`, `Kconfig.projbuild`, or `idf_component.yml`.

### Menu configuration

```powershell
idf.py menuconfig
```

Opens project configuration. Used to set node ID, node count, PLCA state, sender ID, client mask, sender mask, traffic mode, payload size, and test duration.

### Flash and monitor

```powershell
idf.py -p COM5 flash monitor
```

Builds if needed, flashes the firmware to the board on `COM5`, and starts the serial monitor.

### Monitor without flashing

```powershell
idf.py -p COM5 monitor
```

Starts only the serial monitor.

### Full clean

```powershell
idf.py fullclean
```

Removes build artifacts. Use this if the build system behaves unexpectedly after configuration changes.

### Log monitor output to a file in PowerShell

```powershell
idf.py -p COM5 monitor | Tee-Object -FilePath node0_COM5.txt
```

If Windows console encoding causes problems, set UTF-8 first:

```powershell
chcp 65001
$env:PYTHONIOENCODING = "utf-8"
$env:PYTHONUTF8 = "1"
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$OutputEncoding = [System.Text.UTF8Encoding]::new()
```

## Testing and validation

### Basic validation checklist

For each node:

1. Firmware flashes successfully.
2. LAN8651 chip ID is verified.
3. Ethernet starts.
4. Ethernet link goes up.
5. Static IP is assigned.
6. PLCA node ID is logged correctly.
7. The expected RX/TX output appears for the selected firmware variant.

Expected important log lines:

```text
Chip ID verified: LAN8651
Ethernet Started
Ethernet Link Up
Ethernet Got IP
```

For UDP measurement firmware:

```text
RESULT_TX ...
RESULT_RX ...
```

For TCP measurement firmware:

```text
RESULT_TCP_TX ...
RESULT_TCP_RX ...
```

For traffic-pattern firmware:

```text
RESULT_TRAFFIC_TX ...
RESULT_TRAFFIC_RX ...
```

### Known failure signs

The following messages usually indicate a wiring, SPI, reset, or hardware-timing problem:

```text
esp_eth_driver_install failed
LAN8651 not reachable over SPI
reset timeout
header bad
frame receive failed
```

In traffic tests, repeated sender-side failures may also appear as:

```text
last_errno=12
```

This means that the local sender ran out of transmit buffers or memory while trying to send. It can be caused by overly aggressive transmission, but it can also appear when SPI or IRQ wiring is unreliable.

Possible causes:

- `Q1` transistor not removed from the Two-Wire ETH Click board, which may affect SPI chip-select timing at higher SPI clock rates,
- bent, loose, or unreliable IRQ/INT jumper connection,
- incorrect SDO/SDI/SCK/CS wiring,
- missing common GND,
- wrong power level,
- RST pin connected when the firmware expects it to be unconnected,
- too high SPI clock for the specific wiring or board state,
- wrong COM port or board not flashed with the expected firmware,
- wrong PLCA node count,
- duplicate node IDs,
- incorrect sender mask or client mask for the intended test.

### Connectivity troubleshooting

If a node initializes but does not receive traffic:

- verify `APP_NODE_COUNT` is the same on all nodes,
- verify all `APP_NODE_ID` values are unique,
- verify PLCA is enabled or disabled consistently for the intended test,
- check bus polarity,
- check termination only at both physical ends,
- check that the topology is a line, not a star,
- verify that all nodes use the same UDP or TCP port,
- verify that the correct folder/project was built and flashed.

## Known limitations

- The high-speed measurement configuration assumes Two-Wire ETH Click boards with the `Q1` transistor removed. In the tested setup, an unmodified board with `Q1` installed was reliable only at lower SPI clock rates and failed at higher rates.
- Results are printed to the serial monitor. There is no automatic CSV export.
- `sdkconfig` is not committed because it is node-specific.
- The code assumes a fixed ESP32 GPIO mapping.
- The Click board reset pin is not controlled by firmware.
- High-speed reliability depends on the physical wiring and Click board hardware state.
- Traffic-pattern application-unicast mode uses application-level filtering and should not be interpreted as a separate low-level PHY mode.
- The firmware uses static IPv4 addressing derived from node ID.
- Test synchronization is based on configured start delays and manual reset/monitoring workflow.
- The measured application throughput is not equal to the 10BASE-T1S PHY line rate. It also includes ESP32, SPI, driver, lwIP, FreeRTOS, UDP/IP or TCP/IP, and firmware overhead.

## Reproducibility notes

To reproduce measurement results, document and keep consistent:

- ESP-IDF version,
- resolved component versions from `dependencies.lock`,
- ESP32 board type,
- LAN8651 Click board hardware state, especially whether the `Q1` transistor was removed,
- `RST` pin left unconnected and `phy_config.reset_gpio_num = -1`,
- GPIO wiring,
- cable type and length,
- bus topology,
- termination placement,
- PLCA enabled/disabled state,
- node IDs and node count,
- UDP sender node ID,
- TCP server node ID,
- TCP client mask,
- traffic-pattern sender mask,
- traffic-pattern destination mode,
- traffic-pattern destination node ID,
- UDP or TCP payload size,
- TX delay,
- test duration,
- serial logs from all nodes used in a test.

The code was developed and tested with ESP-IDF `v5.5.4`. Other ESP-IDF versions may work but should be verified.