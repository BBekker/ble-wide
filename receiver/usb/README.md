# Direct RTL8157 USB input (experimental)

This path replaces the Realtek Ethernet driver during capture. It bypasses macOS Ethernet/IP/UDP receive handling; Apple's USB host controller, DMA machinery, and USB completions still run. Live 160 Msps IQ8 reception and BLE decoding have been demonstrated on an M3 Pro with an RTL8157. The viewer now prefers this path on the tested adapter; cold initialization and broader hardware support remain unfinished.

## Automatic viewer reception

Run `python3 server.py --backend metal` with the router already exporting. The default `--transport auto` performs read-only IORegistry discovery and matches the tested USB VID/PID/revision (`0bda:8157`, `0x3000`), vendor configuration 1, and active network interface owning `--bind`. It never captures an unrelated adapter just because its chipset matches. Unsupported or ambiguous hardware uses UDP.

The viewer builds and stages the helper, starts its unprivileged sample receiver and decoder, then requests one macOS administrator authorization for the fixed USB executable. Approval is attempted once per viewer run. Declining, failing to receive IQ samples, or losing USB capture releases the adapter and falls back to UDP. `--transport udp` disables takeover; `--transport usb` reports failures without fallback. `--stream /name` attaches to an external receiver and does not auto-select hardware.

The page remains available during authorization and displays the transport and fallback reason under Receiver details. A lightweight `/receiver` endpoint exposes transport state and sample progress for local supervision. The router launcher starts its detached exporter before the viewer and supervises local sample progress, avoiding SSH while USB owns the adapter. It stops the viewer and releases USB before attempting router shutdown over SSH.

The viewer refreshes a shared-ring heartbeat while capture is active. `--lease` tells the helper to release USB if this heartbeat stops for five seconds, including an abrupt viewer exit. Normal shutdown sets an explicit stop request and waits for release. The helper is bounded to 24 hours and performs no permanent installation. Logs are copied to `build/live-receiver/` on shutdown; sample buffers remain temporary. Ordinary IP traffic through this adapter is unavailable during direct capture.

## Architecture

```
RTL8157 USB bulk endpoint 0x81
  -> root IOUSBHost helper, 32 reusable 64 KiB DMA buffers
  -> private 32 MiB shared transfer ring, one copy per USB completion
  -> unprivileged w1700k-stream: RTL8157 framing, Ethernet/IP/UDP validation
  -> existing reorder/sample ring -> unchanged BLE decoder and viewer
```

The USB helper is a separate process. The viewer, GPU channelizer and decoder do not need root. The helper has no general command interface and writes only the RTL8157's volatile receive-control registers. It requires the Realtek driver to have already initialized the adapter and established its PHY link; cold PHY/firmware loading is not implemented.

The shared ring has fixed geometry and single-producer/single-consumer ownership. Full rings drop whole transfers and count the drops; the writer never overwrites data still being consumed. The privileged writer checks file type, ownership, permissions, length and ABI, rejects symlinks, and uses byte-range locking to prevent a second producer. Create a fresh ring for each capture.

The frame parser handles RTL8157's **16-byte RX descriptor v2 and 16-byte alignment** (not the older RTL8156 framing). It strips Ethernet FCS, recognizes VLAN tags, rejects IPv4 fragments, validates header/UDP lengths and checksums, and filters to the configured destination IP and UDP ports. It accepts both IQ8 records in an 8224-byte payload through the existing ingest code.

## Build

On macOS:

```sh
./tools/run make -C receiver all usb-helper check-usb
```

The helper is an ad-hoc-signed local app bundle at `receiver/build/BLEUSBHelper.app`. Its bundle identifier is necessary for IOUSBHost's error handling on this Mac; a bare executable caused an exception inside the framework. No DriverKit extension, entitlements, SIP change, reduced startup security, or permanent privilege configuration is installed.

## Manual bounded capture

The router must already be transmitting the existing jumbo-frame IQ8 export to this computer's adapter MAC. USB takeover removes that adapter from normal macOS networking, including SSH to the router. This first implementation does not implement TCP/IP or ARP replies; the current router exporter supplies the destination MAC explicitly. A launcher that polls the router over SSH must not supervise a long USB capture without a separate control connection.

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

The warm-enable sequence leaves the adapter's out-of-band wake mode, sets a 9216-byte frame limit, a receive aggregation threshold that fits a 64 KiB transfer, and an 85 microsecond aggregation timer. All 32 DMA buffers are allocated and queued before receive is ungated. `--aggregate-kib 32` selects the smaller comparison setting; 64 is the tested default. With the current 8224-byte UDP payload, the larger setting usually aggregates seven Ethernet frames into a 58,016-byte USB completion.

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

The normal adapter recovered its 5 Gb/s link, address and MTU after each capture. SSH is unavailable during takeover; the updated launcher supervises the viewer locally. This implementation still relies on the stock driver having initialized the adapter before takeover. These short measurements do not establish cold-start support, long-duration stability, or removal of USB kernel processing.

## Sources and licensing

- [Apple IOUSBHost device capture](https://developer.apple.com/documentation/iousbhost/iousbhostobjectinitoptions/devicecapture): root can capture without the VM entitlement.
- Local macOS 26.2 SDK: IOUSBHostObject, IOUSBHostInterface, IOUSBHostPipe and descriptor APIs; `ioDataWithCapacity` allocates DMA buffers without a bounce copy.
- [Linux RTL8157 support](https://github.com/torvalds/linux/blob/58161ead29e50d70a317ea26b472e0c7508e69da/drivers/net/usb/r8152.c): `rx_desc_v2`, `r8157_rx_len`, `r8157_desc_init`, `rtl_enable`, `rtl8156_enable`, and register access.

`helper.mm` is GPL-2.0-only because its hardware enable sequence follows the Linux driver. It is a separate executable; the original frame parser, IPC transport and receiver changes are MIT. The GPL text is in `LICENSES/GPL-2.0` at the repository root.
