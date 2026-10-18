# Direct RTL8157 USB input (experimental)

This path replaces the Realtek Ethernet driver during capture. It bypasses macOS Ethernet/IP/UDP receive handling; Apple's USB host controller, DMA machinery, and USB completions still run. Live 160 Msps IQ8 reception and BLE decoding have been demonstrated on an M3 Pro with an RTL8157. The viewer prefers this path on the tested adapter. An independent USB/MAC/PHY initialization path is now implemented; PHY firmware upload, power-down recovery and USB reconfiguration have been tested on the physical adapter; a complete USB power-removal test is still pending. Broader hardware support remains unfinished.

## Automatic viewer reception

Run `python3 server.py --backend metal` with the router already exporting. The default `--transport auto` performs read-only IORegistry discovery and matches the tested USB VID/PID/revision (`0bda:8157`, `0x3000`) and network interface owning `--bind`. The link may be inactive: the helper can now initialize it. It never automatically captures an unrelated adapter just because its chipset matches. Unsupported or ambiguous hardware uses UDP. When no stock-driver interface exists, explicitly select the device with `--transport usb --usb-location 0x02200000` (substitute its actual IORegistry location ID). The destination IP still filters exported packets; it need not be assigned to a macOS interface during direct USB capture.

The viewer builds and stages the helper, starts its unprivileged sample receiver and decoder, then requests one macOS administrator authorization for the fixed USB executable. Approval is attempted once per viewer run. Declining, failing to receive IQ samples, or losing USB capture releases the adapter and falls back to UDP. `--transport udp` disables takeover; `--transport usb` reports failures without fallback. `--stream /name` attaches to an external receiver and does not auto-select hardware.

The page remains available during authorization and displays the transport and fallback reason under Receiver details. A lightweight `/receiver` endpoint exposes transport state and sample progress for local supervision. The router launcher starts its detached exporter before the viewer and supervises local sample progress, avoiding SSH while USB owns the adapter. It stops the viewer and releases USB before attempting router shutdown over SSH.

The viewer refreshes a shared-ring heartbeat while capture is active. `--lease` tells the helper to release USB if this heartbeat stops for five seconds, including an abrupt viewer exit. Normal shutdown sets an explicit stop request and waits for release. The helper is bounded to 24 hours and performs no permanent installation. Logs are copied to `build/live-receiver/` on shutdown; sample buffers remain temporary. Ordinary IP traffic through this adapter is unavailable during direct capture.

The helper uses [IORegisterForSystemPower](https://developer.apple.com/documentation/iokit/1557114-ioregisterforsystempower) to accept system sleep and reopen the same adapter after wake, within the original authorized process. It does not hold a sleep-prevention assertion or change power settings. Shared-ring states 4 (asleep) and 5 (resuming) keep the viewer from falling back during recovery. After wake it allows 15 seconds for the viewer heartbeat to return, and retries adapter initialization for up to 45 seconds. Ordinary heartbeat expiry still releases the adapter. Samples transmitted during sleep are lost; recovery cannot reconstruct them. Incrementing the ring's `reconnect_requested` counter exercises the same adapter reopen path while awake without authorizing another helper.

## Architecture

```
RTL8157 USB bulk endpoint 0x81
  -> root IOUSBHost helper, 32 reusable DMA buffers (256 KiB by default)
  -> private 8 MiB shared transfer ring, one copy per USB completion
  -> unprivileged w1700k-stream: RTL8157 framing, Ethernet/IP/UDP validation
  -> existing reorder/sample ring -> unchanged BLE decoder and viewer
```

The USB helper is a separate process. The viewer, GPU channelizer and decoder do not need root. The helper has no general command interface. Receive setup and firmware loading write volatile registers/RAM, not EEPROM or flash. A valid existing link uses the warm takeover path; an absent link or newly selected vendor configuration invokes independent initialization. `--initialize` forces the full sequence for diagnostics. The firmware upload and PHY power-down recovery have been verified on hardware. A complete USB power-removal test without prior stock-driver initialization remains unverified; see below.

The shared ring has fixed geometry and single-producer/single-consumer ownership. Full rings drop whole transfers and count the drops; the writer never overwrites data still being consumed. The privileged writer checks file type, ownership, permissions, length and ABI, rejects symlinks, and uses byte-range locking to prevent a second producer. Create a fresh ring for each capture.

The frame parser handles RTL8157's **16-byte RX descriptor v2 and 16-byte alignment** (not the older RTL8156 framing). It strips Ethernet FCS, recognizes VLAN tags, rejects IPv4 fragments, validates header/UDP lengths and checksums, and filters to the configured destination IP and UDP ports. It accepts both IQ8 records in an 8224-byte payload through the existing ingest code.

## Build

On macOS:

```sh
./tools/run make -C receiver all usb-helper check-usb check-usb-init
```

The helper is an ad-hoc-signed local app bundle at `receiver/build/BLEUSBHelper.app`. Its bundle identifier is necessary for IOUSBHost's error handling on this Mac; a bare executable caused an exception inside the framework. No DriverKit extension, entitlements, SIP change, reduced startup security, or permanent privilege configuration is installed.

## Manual bounded capture

The router must already be transmitting the existing jumbo-frame IQ8 export to this computer's adapter MAC. USB takeover removes that adapter from normal macOS networking, including SSH to the router. This first implementation does not implement TCP/IP or ARP replies; the router needs a permanent ARP entry for the destination IP and adapter MAC. The UDP exporter otherwise depends on dynamic ARP, which fails after a PHY reset while the stock driver is detached. A launcher that polls the router over SSH must not supervise a long USB capture without a separate control connection.

First start the unprivileged receiver using a new private file path:

```sh
./receiver/build/w1700k-stream --usb-ring /tmp/ble-usb-example.ring \
  --bind 10.77.0.10 --ports 2 --name /ble-usb-test --report 1
```

In another terminal, use one administrator authentication for a bounded test:

```sh
sudo ./receiver/build/BLEUSBHelper.app/Contents/MacOS/rtl8157-helper \
  --receive --seconds 15 --ring /tmp/ble-usb-example.ring --uid "$(id -u)"
```

Attach a decoder to `/ble-usb-test` as an ordinary user. `--probe` only captures the device and reads diagnostic registers. `--passive` receives without the volatile warm-enable sequence; it timed out after stock-driver shutdown on the tested adapter. `--location 0x...` selects a specific adapter when multiple RTL8157 devices are present. All capture modes temporarily take the adapter away from its normal driver. On termination, outstanding transfers are canceled/drained and the device is released for driver rematching. A stuck test is bounded by the development test supervisor; terminating the helper also closes its USB user client.

The warm-enable sequence leaves the adapter's out-of-band wake mode, sets a 9216-byte frame limit, a receive aggregation threshold that fits a 256 KiB transfer, and an 85 microsecond aggregation timer. All 32 DMA buffers are allocated and queued before receive is ungated. `--aggregate-kib 32|64|128|256` selects the request size and matching hardware threshold; 256 is the measured default. `--requests 8|16|32` controls pending DMA reads; retain 32 for reliable streaming, since the eight-request setting lost samples in a live trial. `--coalesce-us 1..524` controls the extra aggregation timer (default 85 µs); the limit follows the vendor driver’s 16-bit register in 8 ns units. A larger request is a ceiling, not a guarantee that each USB completion fills it. With the current 8224-byte UDP payload, the larger setting usually aggregates up to 31 Ethernet frames into a 256,928-byte USB completion (about 249,000 bytes on average).

## Development session: one authentication

Start the development session once:

```sh
./tools/usb-session start
./tools/usb-session run --log receiver/build/probe.json -- --probe
./tools/usb-session run --timeout 40 -- --receive --seconds 15 --ring /tmp/ble-usb-example.ring
./tools/usb-session stop
```

Only `start` opens an authentication dialog. Later `run` commands reuse the session. The receiver must create the ring before the receive test; `--uid` is supplied by the authenticated session. `status` reports whether it is ready.

`test-session.py` is a temporary root test runner, staged with the helper in a private temporary directory. It only executes that staged helper, accepts commands from the launching user's UID over a private Unix socket, bounds each test to 120 seconds, and expires after two hours. This intentionally trusts the developer's rebuilt helper for the duration of the session. It does not install a service or change sudoers. `session-client.py` submits tests to an existing session and **never initiates another authentication dialog**. Shut the session down with its `--stop` option when done.

Keep executable staging and ring files outside macOS's protected Documents directory. The initial Python test runner in Documents was denied by TCC even as root; the temporary-directory runner does not need access to those documents.

## Independent RTL8157 initialization

`rtl8157-init.h` ports the RTL_VER_16 path in Realtek's GPL `r8152` v2.22.1 (2026/06/03). This is a source-based port, not guessed register writes or a disassembly of Apple's driver. USB revision `0x3000` is checked before capture, and the silicon version in `PLA_TCR1 & 0x7cf0` must be `0x1030` before initialization writes.

The sequence selects USB configuration 1 if necessary; waits for autoload and a valid PHY state; disables USB/PHY sleep modes; loads USB and PLA MAC patches; uploads PHY microcode through the 2 KiB speed-up mailbox using 512-byte USB requests; programs the vendor PHY/SRAM calibration tables; sets jumbo receive/FIFO parameters; and resets/restarts full-duplex 10/100/1000/2500/5000 autonegotiation. It then uses the aggregated receive path. EEE and ALDPS are left disabled for continuous reception. The PHY mailbox, autoload, patch acknowledgements, chunk completion, reset and link waits are bounded. `--link-seconds 1..60` controls link negotiation's deadline (default 30 seconds). Viewer cancellation and the five-second lease are checked throughout initialization too.

For the receive-only backend, the router must keep a permanent ARP entry, for example `arp -i eth2 -s 10.77.0.10 80:3f:5d:d8:c7:02` for the tested setup. Substitute the actual port, destination and MAC. The router launcher's managed USB path now reads the selected adapter's MAC, installs and verifies this entry before capture, then restores the prior static entry or dynamic-neighbor policy on shutdown. Dynamic ARP alone stopped delivering samples after PHY reset in the hardware test. The router's BusyBox `ip` cannot modify neighbors; `arp -s` is used instead.

Firmware is compiled into the helper. It does not load arbitrary files as root. The bundled PHY version is `0x0014` (9,888 bytes), USB patch version 3 (740 bytes), and PLA patch version 1 (60 bytes). Valid equal/newer installed versions are skipped. As in Realtek's driver, an outside-loader version marker invalidates internal MAC version tags. PHY uploads must read back the expected firmware version; failed uploads never proceed to link negotiation. On error the helper attempts to release the PHY patch handshake, then releases USB for normal driver rematching.

The data and calibration tables are reproducible from this exact source:

- [Realtek source, pinned mirror commit d93c9f89](https://github.com/awesometic/realtek-r8152-dkms/blob/d93c9f89ca5a831c9437e35319c546e48c710f9a/src/r8152.c): `r8157_init`, `r8157_patch_code`, `rtl_ram_code_speed_up`, `r8157_hw_phy_cfg`, `rtl8157_up`, `rtl8152_set_speed`.
- Whole source SHA-256: `45b18282a0a9cc4ed007cba6bd77ac2fbb16d9015e2b4d95caee90edc0e715ce`.
- Run `python3 receiver/usb/import-rtl8157.py /path/to/that/r8152.c` to reproduce `rtl8157-data.h`. The importer rejects another source revision and records individual payload hashes. No firmware downloads occur at build or runtime.
- [Upstream Linux cross-check](https://github.com/torvalds/linux/blob/32c52ec87d6245ee2d12ed5dd509e40062939b50/drivers/net/usb/r8152.c). That revision implements the corresponding PHY initialization and `rtl8157-1.fw` loader. The firmware file was absent from the linux-firmware tree checked during this work; the bundled bytes come from Realtek's source above.

Within the single-authentication development session:

```sh
./tools/usb-session run --timeout 90 -- --probe --initialize
./tools/usb-session run --timeout 90 -- --probe --test-phy-power-down
```

The second test deliberately powers down the PHY and confirms link-down before initializing it. This demonstrates recovery from PHY power-down **only when run successfully on hardware**; it is not a substitute for physically removing power with the stock driver prevented from initializing the device first. Both commands have now succeeded on the physical RTL8157. `--test-reconfigure` additionally selects USB configuration 0 and then vendor configuration 1 while exclusive capture remains held. This also succeeded after fixing the interface-opening order: an interface must hold the device awake before configuration requests on this Mac. `--receive --initialize --seconds 30 --ring PATH` additionally exercises the normal sample path; the unprivileged receiver must create PATH first. `--passive` cannot be combined with initialization.

`check-usb-init` uses simulated USB/TGPHY mailboxes to check initialization from PHY power-down, newer-firmware preservation, repeated initialization, unknown silicon rejection before writes, stuck mailboxes/patch acknowledgements, failure at every upload chunk, incorrect firmware-version readback, and link timeout. The existing real-receiver synthetic USB tests continue to check IQ8/packed-14 framing independently. Simulated success does not establish physical cold-start support.

### Initialization hardware results

See [machine-readable measurements](initialization-results.json). All tests shared one authenticated temporary session, which was stopped afterward. The ordinary driver, 5 Gb/s link and MTU 9000 were restored, and the temporary router ARP entry was removed.

- The first forced initialization uploaded 9,888 bytes of PHY firmware from reported version `0x0000` to `0x0014`, verified the version readback, and negotiated 5 Gb/s. USB v3 and PLA v1 patches were already installed and therefore skipped; their upload paths have simulated coverage only.
- A powered-down PHY recovered its 5 Gb/s link. With a permanent destination ARP entry, the following 30-second capture decoded 3,202 CRC-valid and 833 CRC-failed BLE records. It had 976 missing 2,048-sample slices at startup (about 12.5 ms), with no later increase in the receiver's loss counter.
- The final test selected configuration 0 then configuration 1 while retaining exclusive USB ownership, powered down the PHY, reinitialized it, and captured for 30 seconds. It decoded 3,057 CRC-valid and 689 CRC-failed BLE records. Receiver and decoder reported zero sample gaps in the received stream; USB reported zero errors, timeouts and ring drops. There were zero malformed frames and receiver overruns. The normal driver did not run between unconfiguration, PHY power-down, initialization and capture.
- The final capture transferred 9,470,888,224 USB bytes at an average 2.524 Gb/s including startup, with 164,319 successful USB transfers. Steady one-second sample reports corresponded to approximately 160 Msps. The median calculated from nominal one-second reports was 160.19 Msps; report scheduling makes that an approximate rate, not a sample-clock calibration. Helper CPU was 5.02 seconds over 30.02 seconds, about 16.7% of one core.

USB configuration initially failed with `kIOReturnNotReady` when attempted immediately after device capture. Opening the existing interface first holds the device awake; configuration 1 → 0 → 1 then worked. Selecting a different pre-existing configuration and capturing a physically power-cycled adapter without prior stock-driver initialization are still unverified. These tests do not prove a fully driver-independent power-on boot or long-duration lossless operation; link-reset time before reception is outside the reported sample-loss counters.

## Validation and limits

- Synthetic USB aggregates pass through the real receiver to bit-exact IQ samples without UDP sockets (IQ8 and unaligned packed-14 input).
- Parser tests cover jumbo/two-record datagrams, VLANs, IP options, checksum failures, fragments, truncation and randomized malformed transfers.
- Ring tests cover capacity, backpressure, wraparound and cross-process producer exclusion.
- With SIP enabled, device capture succeeded using `DeviceCapture | DeviceSeize`, followed by normal-driver recovery.
- Initial control reads using ordinary `NSMutableData` returned `kIOReturnNotReady`. IOUSBHost-allocated control buffers plus an opened vendor interface successfully read and write the registers.
- Register access alone was insufficient: both native IOUSBHost and a separate libusb bulk-read probe timed out after capture. The stock driver had left `NOW_IS_OOB`, `MCU_BORW_EN`, and MCU speed-down enabled. Leaving that state using the corresponding `rtl8156_up` operations made bulk reception work without PHY reinitialization or firmware upload.
- Live hardware measurements and remaining limits are recorded below. UDP is the automatic fallback for unsupported hardware or capture failure.

### Hardware measurements, 2026-10-17

M3 Pro, RTL8157 revision `0x3000`, existing 5 Gb/s link, 160 Msps IQ8 export, two records per jumbo UDP datagram, Metal channelizer and four streaming decode workers. These are short sequential measurements on a running desktop, not an isolated CPU benchmark. BLE traffic changes between runs.

| Receive path | USB data rate | USB completions/s | Helper CPU, one core = 100% | Receiver CPU, one core = 100% | Whole-machine system CPU |
| --- | ---: | ---: | ---: | ---: | ---: |
| Stock driver + UDP | — | — | — | 38.2% | 19.40% |
| Direct USB, 32 KiB aggregation | 2.580 Gb/s | 12,999 | 38.0% | 11.4% | 8.54% |
| Direct USB, 64 KiB aggregation | 2.590 Gb/s | 5,618 | 18.3% | 11.4% | 6.19% |

The two USB captures lasted 20 seconds each. Rates include USB descriptors, Ethernet/IP/UDP headers and Ethernet FCS. They correspond to the 2.56 Gb/s IQ8 sample stream, not a 5 Gb/s saturation test. Helper CPU uses `getrusage`; receiver CPU uses process CPU-time differences. System CPU uses `HOST_CPU_LOAD_INFO` deltas over steady portions of each capture. The stock Realtek process also consumed approximately 38.2% of one core. Whole-machine busy CPU was 36.17%, 21.41%, and 19.31% respectively; unrelated desktop activity and different BLE packets prevent attributing every difference to this change.

Both 20-second USB captures had zero USB errors, timeouts, ring drops, malformed network frames and receiver overruns. Sample loss was nonzero at capture startup (6,093 slices with 32 KiB; 1,262 with 64 KiB), then remained unchanged in every subsequent one-second report. Do not describe these captures as globally lossless. The 64 KiB run delivered 781,508 valid IQ UDP datagrams to the receiver and 2,547 BLE packet records, including CRC failures, to the decoder output.

The final build, which queues DMA requests before ungating receive, passed a further 30-second capture: 2.589 Gb/s, 168,563 USB completions, 1,172,061 IQ datagrams, 3,337 CRC-valid BLE packets and 777 CRC-failed packet records. Helper CPU was 18.0% of one core. There were zero USB errors/timeouts, ring drops, malformed frames or receiver overruns. The initial 801 missing sample slices (1,640,448 samples, about 10.25 ms) did not increase during the remaining capture. This shorter startup gap is an observation from one run, not proof that startup loss is solved. Synthetic IQ8 and unaligned packed-14 integration tests also passed with the real receiver executable.

The normal adapter recovered its 5 Gb/s link, address and MTU after each capture. SSH is unavailable during takeover; the updated launcher supervises the viewer locally. These measurements used the earlier warm-takeover implementation and do not validate the new independent initialization path, long-duration stability, or removal of USB kernel processing.

### Aggregation and wakeup tuning, 2026-10-17

Selected defaults: **256 KiB batches, 85 µs aggregation timer, 32 pending DMA requests, 32 transfer-ring slots, and 500 µs empty-ring polling**. All buffers are queued before receive is ungated. This uses 8 MiB of USB DMA buffers and an 8 MiB IPC data ring. The ring uses ABI v2; rebuild both executables together, since old rings are rejected. Smaller comparison batches remain available through the helper CLI.

| Setting | USB completions/s at full rate | Helper CPU, one core = 100% | Steady sample gaps |
| --- | ---: | ---: | ---: |
| 64 KiB, 85 µs, 32 DMA requests | 5,617 | 17.43% | 0 |
| 128 KiB, 85 µs, 32 DMA requests | 2,645 | 18.65% | 0 |
| 256 KiB, 85 µs, 32 DMA requests | 1,303 | 12.63% | 0 |
| 256 KiB, 400 µs, 32 DMA requests | 1,295 | 12.85% | 0 |

These four comparisons were 30-second captures with 128 ring slots and 100 µs polling. The 64 KiB row uses the repeated warm baseline, avoiding its earlier delayed-source run. Raising the timer to 400 µs gave no meaningful benefit. The first larger ring consumed more memory and raised whole-machine CPU in some runs, so subsequent trials reduced it to 32 slots. Reducing pending DMA requests to eight also reduced CPU, but caused **194 lost sample slices during steady streaming**, with no USB errors or IPC ring drops. That configuration is rejected; a clean USB error count alone does not prove lossless samples.

The final configuration passed a 110-second capture with about 95 seconds of actual 160 Msps input, after the router was deliberately started later than the receiver. It decoded **10,431 CRC-valid BLE packets**, with **zero reported sample gaps, receiver overruns, malformed frames, USB errors or ring drops**. Thirteen USB timeouts occurred before transmission began. Receiver and decoder both reported zero sample loss. Ordinary takeover of an already-running stream still has a startup blind interval; several other tests recorded initial gaps, retained in the results rather than hidden.

Normalized by USB bytes received, helper CPU improved from **0.538 to 0.402 CPU-seconds/GB** (25%), and receiver CPU from **0.328 to 0.261 CPU-seconds/GB** (20%). Receiver context switches per GB fell about 47%. The slower empty-ring polling adds up to approximately 400 µs of requested sleep compared with the previous polling interval; operating-system scheduling can add latency. Steady whole-machine system CPU was **4.72%**, versus **4.66%** for the long 64 KiB baseline: effectively comparable in these sequential desktop runs, not evidence of an overall system CPU reduction. BLE traffic and desktop activity varied between runs.

[Full measurements](aggregation-results.json) retain all ten runs, including rejected settings, startup loss, CPU accounting, decoder counters, and the separate 30-second host-CPU windows. The final defaults have short hardware validation, not a guarantee of indefinite losslessness. Parser and real-receiver tests cover batches above 64 KiB, full-capacity slots, backpressure, wraparound, and bit-exact IQ8/packed-14 output. The next unimplemented optimization is event-driven ring notification in place of polling; sharing DMA buffers could also eliminate the helper-to-ring copy.

## Sources and licensing

- [Apple IOUSBHost device capture](https://developer.apple.com/documentation/iousbhost/iousbhostobjectinitoptions/devicecapture): root can capture without the VM entitlement.
- Local macOS 26.2 SDK: IOUSBHostObject, IOUSBHostInterface, IOUSBHostPipe and descriptor APIs; `ioDataWithCapacity` allocates DMA buffers without a bounce copy.
- [Linux RTL8157 support](https://github.com/torvalds/linux/blob/58161ead29e50d70a317ea26b472e0c7508e69da/drivers/net/usb/r8152.c): `rx_desc_v2`, `r8157_rx_len`, `r8157_desc_init`, `rtl_enable`, `rtl8156_enable`, and register access.

`helper.mm`, `rtl8157-init.h`, `rtl8157-data.h`, their importer and initialization tests are GPL-2.0-only; the hardware sequences and bundled data derive from the GPL Realtek/Linux drivers. They are part of a separate executable; the original frame parser, IPC transport and receiver changes are MIT. The GPL text is in `LICENSES/GPL-2.0` at the repository root.
