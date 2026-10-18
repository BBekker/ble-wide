# Direct RTL8157 USB input (experimental)

This path replaces the Realtek Ethernet driver during a bounded capture. It bypasses macOS Ethernet/IP/UDP receive handling; Apple's USB host controller, DMA machinery, and USB completions still run. It is **not yet a validated replacement for the live UDP receiver**.

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

Attach a decoder to `/ble-usb-test` as an ordinary user. `--probe` only captures the device and reads diagnostic registers. `--passive` receives without the volatile warm-enable sequence. `--location 0x...` selects a specific adapter when multiple RTL8157 devices are present. All capture modes temporarily take the adapter away from its normal driver. On termination, outstanding transfers are canceled/drained and the device is released for driver rematching. A stuck test is bounded by the development test supervisor; terminating the helper also closes its USB user client.

The experimental warm-enable sequence sets a 9216-byte frame limit, a receive aggregation threshold that fits a 32 KiB transfer, and an 85 microsecond aggregation timer. Those are initial settings to validate, not benchmarked optimal values.

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
- Initial control reads using ordinary `NSMutableData` returned `kIOReturnNotReady`. The current helper uses IOUSBHost-allocated DMA/control buffers and opens the vendor interface; that change still needs a live test.
- Sustained live USB throughput, loss and total-system CPU savings are unverified. The regular UDP receiver remains the default.

## Sources and licensing

- [Apple IOUSBHost device capture](https://developer.apple.com/documentation/iousbhost/iousbhostobjectinitoptions/devicecapture): root can capture without the VM entitlement.
- Local macOS 26.2 SDK: IOUSBHostObject, IOUSBHostInterface, IOUSBHostPipe and descriptor APIs; `ioDataWithCapacity` allocates DMA buffers without a bounce copy.
- [Linux RTL8157 support](https://github.com/torvalds/linux/blob/58161ead29e50d70a317ea26b472e0c7508e69da/drivers/net/usb/r8152.c): `rx_desc_v2`, `r8157_rx_len`, `r8157_desc_init`, `rtl_enable`, `rtl8156_enable`, and register access.

`helper.mm` is GPL-2.0-only because its hardware enable sequence follows the Linux driver. It is a separate executable; the original frame parser, IPC transport and receiver changes are MIT. The GPL text is in `LICENSES/GPL-2.0` at the repository root.
