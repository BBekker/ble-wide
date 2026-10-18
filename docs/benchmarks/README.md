# Transport measurements

These files record measured results, including unsuccessful settings and validation limits. They are evidence from specific runs, not universal performance guarantees.

| Record | Coverage |
| --- | --- |
| [USB aggregation](usb-aggregation.json) | Aggregate size, coalescing, pending DMA requests, process/system CPU, and sample-loss counters |
| [RTL8157 initialization](usb-initialization.json) | PHY firmware upload, PHY power-down recovery, and USB reconfiguration |
| [UDP efficiency](udp-efficiency.json) | Networking and receiver efficiency measurements |

See the [receiver guide](../../receiver/README.md) and [USB implementation notes](../../receiver/usb/README.md) for interpretation. Decoder experiments remain in [research](../research/README.md); replay and measurement tools live in [`tools/benchmarks/`](../../tools/benchmarks/).
