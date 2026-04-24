---
name: "packet-buffer"
description: "Circular Packet buffer used between radio IRQ poll, MAC, network, and app layers — polled, single-threaded semantics."
domain: "firmware-data-structures"
confidence: "high"
source: "earned"
---

## Context
`firmware/src/system/packetBuffer.c/h` implements a fixed-capacity ring of `Packet` structs. Two buffers are instantiated in `lora/main.c`: `pRxBuf` (radio→app direction) and `pTxBuf` (app→radio direction). Consumers: `sx1262.c` (RX producer), `maclayer.c` / `networklayer.c` (middle), `terminal.c` / app layer (TX producer, RX consumer).

## Patterns

**Storage (globalInclude.h):**
- `PACKET_BUFFER_SIZE = 10` — fixed slot count
- Each slot is a full `Packet` struct (header + 50-byte data area, roughly 70 bytes)
- Struct layout (canonical order used by driver): `pktDir, pOwner, rssi, prssi, rxCnt, destination_adr, source_adr, sequence_num, control_mac, protocol_Ver, TTL, mesh_dest, mesh_tbl_entries, mesh_src, control_app, length, data[...]`
- `PacketBuffer { buffer[10]; read_pointer; write_pointer; data_size; }`

**Semantics (polled, single-threaded):**
- **No locks.** The entire firmware runs on a single main-loop thread; there is no radio NVIC IRQ (see `sx1262-driver` and `stm32u073-timer` skills — both polled). `volatile` is therefore unnecessary on the pointers, and isn't used.
- Producer calls: `write_packet()` — appends at `write_pointer`, wraps modulo 10, increments `data_size`.
- Consumer calls: `read_packet()` — FIFO from `read_pointer`, wraps, decrements `data_size`.
- Full check: `buffer_full()` returns `GLOB_ERROR_BUFFER_FULL` when `data_size >= PACKET_BUFFER_SIZE`.
- Empty check: `buffer_empty()` returns `GLOB_ERROR_BUFFER_EMPTY` when `data_size <= 0`.

**Overflow behavior:** `write_packet()` returns `GLOB_ERROR_WRITE_BUFFER_SIZE_EXCEEDED` when full — packet is **dropped**, no overwrite. Callers (sx1262 `receive_packet`, app layers) check `buffer_full()` BEFORE composing a packet and just skip when full. Terminal `send` command prints "Buffer full\n" to the user.

**Non-FIFO accessors (the interesting ones):**
- `search_packet_buffer(buf, src_adr, seq)` — linear scan for dedup (network layer uses this to drop already-forwarded packets).
- `search_pending_packet(buf, dir, owner)` — finds the first packet matching a `(Direction, Owner)` tuple, **copies it out, zeros the slot, and advances read_pointer**. This is how the layer handoff works: each layer pulls packets addressed to itself (`MAC`, `NET`, `APP`). Semantics are "pop first-matching", not strictly FIFO.
- `remove_packet_from_buffer(buf, pkt)` — compacts out a specific entry by shift-copying — O(N) move, acceptable because `PACKET_BUFFER_SIZE=10`.

**Producer/consumer wiring in lora/main.c:**
- Radio IRQ (polled via `radio_irq_handler`) → `receive_packet()` → `write_packet(pRxBuf)` on RX_DONE
- MAC layer reads `pRxBuf`, forwards to network layer via `search_pending_packet`
- App layer writes to `pTxBuf`; MAC pulls from `pTxBuf` and calls `transmitFrame()` → `radio_send`

## Examples
- packetBuffer.c:22 `write_packet` — the simple append path
- packetBuffer.c:47 `search_pending_packet` — layer-handoff copy-and-clear
- sx1262.c `receive_packet` — the RX producer (checks `buffer_full` first)
- maclayer.c / networklayer.c — `search_pending_packet(…, OWNER)` consumption pattern

## Anti-Patterns
- **Never** add an ISR producer without revisiting locking — current code assumes cooperative single-thread access. If radio ever moves to NVIC IRQ, `data_size` and the pointers need atomic updates (Cortex-M0+ has no LDREX/STREX — use PRIMASK disable/enable instead).
- Don't rely on strict FIFO order when `search_pending_packet` is in use — it skips non-matching entries, which reorders relative to a pure `read_packet` consumer.
- Don't send packets larger than `DATA_LEN` (50 bytes of payload) — `write_packet` does a struct copy and won't detect overflow; the sender must clamp `pkt.length`.
- Don't inspect `buffer[]` directly with indices outside `[read_pointer, read_pointer+data_size)` — stale slots are not guaranteed zero (only `search_pending_packet` zeroes its slot).
- Don't raise `PACKET_BUFFER_SIZE` blindly — each slot is ~70 bytes, and both Rx+Tx buffers live in BSS. 10+10 slots ≈ 1.4 KB of the 40 KB RAM. At 50 slots you'd start crowding the stack.
