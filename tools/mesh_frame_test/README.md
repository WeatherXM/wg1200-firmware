# Meshtastic frame wire-format test

Guards `main/ws/include/mesh_frame.h` — the PHY header layout, AES-CTR nonce
and counter handling, and every protobuf field number used by the Meshtastic
telemetry beacon.

```
./run.sh
```

Requires `g++` and `python3` with the `cryptography` package
(`pip install cryptography`).

## Why it exists

Nothing about a Meshtastic frame fails loudly. A wrong protobuf field number,
a wrong channel hash, or a wrong counter-increment direction all produce a
frame that transmits perfectly and is silently dropped or misparsed by every
receiver. The only feedback on hardware is "nothing showed up", which is not a
diagnosis.

So: `mesh_frame_test.cpp` builds frames with fixed inputs using the **real
firmware encoder**, and `verify.py` decodes them with an **independent**
implementation written from the Meshtastic spec, then asserts the values match.
The independence is the whole point — a decoder that shares assumptions with
the encoder proves nothing.

This has already earned its keep: it caught the CTR block counter incrementing
little-endian instead of big-endian. The first 16 bytes of every packet decoded
correctly and everything after them was garbage — the exact class of bug that
would have cost days of staring at a Wio Tracker screen.

## When to re-run

- Any change to `mesh_frame.h`
- After pulling a newer `meshtastic/protobufs`, to confirm no field numbers moved
- Before any hardware test session

## What it does not cover

Radio configuration: frequency, spreading factor, bandwidth, coding rate,
preamble and sync word all live in `WsHandler::sx126x_tx_meshtastic()` and can
only be verified on hardware or with a spectrum analyser / SDR capture.
