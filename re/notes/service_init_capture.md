# GCUService Startup Capture Analysis

Captured: 2026-03-31 12:16:35 — GCUBridge service start → GCUService.exe init
Frida attached after 1.4s, hooks installed at +1.4s. Init writes ran +7s to +21.5s.
After +22s: only 0x7CC polling (watchdog keepalive). Init complete in ~20s.

---

## 1. EC Snapshot Diff: Stopped → Running

95 registers changed. Grouped by category:

### Control Registers (the interesting ones)

| Addr | Name | Stopped | Running | Notes |
|------|------|---------|---------|-------|
| 0x741 | OEM_Byte/FanAlert | 0x04 | 0x01 | Written 0x05 by service (snapshot shows 0x01 later) |
| 0x746 | DB_MAX | 0x05 | 0x19 (25) | Dynamic Boost max = 25W |
| 0x763 | CPU temp readback | 0x2D (45) | 0x00 | 105-45=60°C → 105-0=105°C (no limit active) |
| 0x764 | | 0x2D | 0x00 | Same pattern |
| 0x783 | PL1 | 0x00 | 0xD2 (210) | 210W! |
| 0x784 | PL2 | 0x00 | 0xD2 (210) | 210W |
| 0x785 | PL4 | 0x00 | 0xAD (173) | 173×2 = 346W |
| 0x786 | TCC_OFFSET | 0x8A | 0x94 | 0x80\|(105-85)=0x94 → 85°C limit |
| 0x787 | FAN_SWITCH_SPEED | 0x00 | 0x81 | Enabled, 100ms |
| 0x7A6 | BATT_MODE | 0x08 | 0x28 | Changed: Performance(0x08) → Stationary(0x28) |
| 0x7C6 | OEM6_APPLY | 0x00 | 0x04 | Apply flag set |
| 0x7D4 | | 0x00 | 0xFF | Unknown (possibly DB-related?) |
| 0x7D5 | | 0x05 | 0x19 (25) | Mirrors DB_MAX (0x746) |

### Fan Duty (read-only, changed because service loaded fan curves)

| Addr | Name | Stopped | Running |
|------|------|---------|---------|
| 0x75B | CPU fan duty | 0x32 (25%) | 0x40 (32%) |
| 0x75C | GPU fan duty | 0x32 (25%) | 0x00 (0%) |

### Fan Table (0xF00-0xF5F) — Full rewrite

CPU and GPU fan tables completely rewritten from garbage/defaults to proper curves.
See Section 3 for full fan curve values.

### Sensor/Status (volatile — changed due to thermal state, not writes)

0x449, 0x44C, 0x460-0x46F, 0x522-0x566, 0x600-0x6FF, 0x701, 0x703 — all thermal/voltage/timing registers that fluctuate.

---

## 2. EC Writes During Init (non-fan-table, chronological)

### Phase 1: Early handshake (+7.0s)

```
ECWRITE 0x709 = 0x05    ← NEW: unknown, not in our register set
ECWRITE 0x708 = 0x01    ← NEW: unknown, not in our register set
ECREAD  0x708 → 0x01    (verify)
```

**0x708/0x709 appear to be an EC handshake/unlock sequence.** Written very early, before anything else. Write 0x709=5 first, then 0x708=1, then verify 0x708 reads back 1. This could be enabling EC features or announcing that the OEM software is running.

### Phase 2: Identity + init registers (+13.9s)

```
ECREAD  0x741 → 0x04
ECWRITE 0x741 = 0x05    ← NEW: increments OEM_Byte from 4→5
IOCTL   0x9C40A4A0      ← NEW: unknown IOCTL (not read/write), returns 0x7D1C8086
```

**0x741 may be a "service running" counter or activation flag.** The OEM reads the current value and writes it +1. Or it's setting a specific mode byte.

### Phase 3: Handshake registers (+15.5s)

```
ECWRITE 0x709 = 0x05    (repeat)
ECWRITE 0x708 = 0x01    (repeat, verify reads 0x01)
ECWRITE 0x748 = 0x81    ← NEW: written 4+ times! (handshake/keepalive?)
ECWRITE 0x7A6 = 0x28    (BATT_MODE = Stationary)
ECWRITE 0x748 = 0x81    (again)
ECWRITE 0x766 = 0x93    ← NEW: writes BACK the same value read from Support2!
ECWRITE 0x748 = 0x81    (again)
ECWRITE 0x749 = 0x00    ← NEW: clear
ECWRITE 0x767 = 0x00    (TRIGGER clear — known)
ECWRITE 0x74A = 0x00    ← NEW: clear
ECWRITE 0x74B = 0x00    ← NEW: clear
ECWRITE 0x748 = 0x81    (4th time!)
```

**0x748 is the most suspicious register.** Written 4 times during a single init phase, always value 0x81 (bit 7 + bit 0). This reads like a "service alive" or "feature enable" register that gets toggled during the handshake sequence. Each write to 0x748 is interspersed with other writes, suggesting it's a confirmation/ack register.

**0x749/0x74A/0x74B are cleared to 0x00** — possibly command/parameter registers that must be zeroed during init.

**0x766 write-back is peculiar** — reads 0x93, writes 0x93. May be confirming to the EC that the software has read the capability bits, or it could trigger a side effect in the EC when written.

### Phase 4: More read-modify-writes (+16.0s)

```
ECWRITE 0x7A6 = 0x28    (BATT_MODE again — 2nd time)
ECWRITE 0x74E = 0x00    ← NEW: clear
ECWRITE 0x728 = 0x04    ← NEW: read 0x04, wrote back 0x04
ECWRITE 0x7A6 = 0x28    (BATT_MODE — 3rd time!)
```

### Phase 5: Fan + Power init (+17.2s)

```
ECWRITE 0x727 = 0xC0    (ESHUTTER — read-modify-write, same value)
ECWRITE 0x751 = 0x00    (FAN_CTRL = normal)
ECWRITE 0x726 = 0x80    (OEM9 — read-modify-write, same value)
ECWRITE 0x7C6 = 0x00    (OEM6_APPLY clear)
ECWRITE 0x7C5 = 0xE0    (OEM5_FANCURVE — bits 5,6,7 all set! 0xE0 = 1110_0000)
```

**0x7C5 = 0xE0 is critical.** We know bit 7 = fan curve enable, bit 5 = logo flag. But bit 6 is NEW — we've never written it. The OEM service sets all three upper bits. **Bit 6 might be a "service managed" or "custom profile active" flag.**

### Phase 6: Power limits + GPU + fan curve

```
ECWRITE 0x783 = 0xD2 (PL1=210W)
ECWRITE 0x784 = 0xD2 (PL2=210W)
ECWRITE 0x785 = 0xAD (PL4=346W)
ECWRITE 0x786 = 0x94 (TCC=85°C)
ECWRITE 0x744 = 0x37 (TGP=150W)
ECWRITE 0x745 = 0xFF (DB_TOTAL=enabled)
ECWRITE 0x746 = 0x19 (DB_MAX=25W)
ECWRITE 0x743 = 0x07 (DB_CTRL=TGP+DB on) — written 4 times!
ECWRITE 0x787 = 0x81 (FAN_SWITCH_SPEED=100ms)
ECWRITE 0x7C7 = 0x0C ← NEW: unknown (12), written during fan table batch
[Full CPU fan table 0xF00-0xF2F]
[Full GPU fan table 0xF30-0xF5F]
```

**0x7C7 = 0x0C** — written once, value 12. Near OEM5/OEM6 registers. Could be a fan curve parameter or profile selector.

### Phase 7: Apply + Logo

```
ECWRITE 0x7C6 = 0x04    (OEM6_APPLY — apply!)
ECREAD  0x7C6 → 0x04    (verify)
ECWRITE 0x7C6 = 0x00    (clear)
ECWRITE 0x767 = 0x00    (TRIGGER clear)
ECWRITE 0x7C5 = 0xE0    (again — same value)
ECWRITE 0x769 = 0x00    (LOGO_R = 0)
ECWRITE 0x76A = 0x32    (LOGO_G = 50)
ECWRITE 0x76B = 0x00    (LOGO_B = 0)
ECWRITE 0x7AB = 0x81    (LOGO brightness = high)
ECWRITE 0x78C = 0x10    (Logo commit)
```

### Phase 8: DOUBLE-WRITE — entire fan table + apply again

```
[Full CPU fan table 0xF00-0xF2F — identical values]
[Full GPU fan table 0xF30-0xF5F — identical values]
ECWRITE 0x7C6 = 0x04    (OEM6_APPLY — 2nd apply!)
```

Confirms the documented double-write pattern. Fan tables are written twice with an OEM6_APPLY between each.

### Phase 9: BIOS_CTRL

```
ECWRITE 0x706 = 0x41    ← NEW: BIOS_CTRL = 0x41 (65)
```

**0x706 (BIOS_CTRL) = 0x41.** Written near the end of init. Bits: 0x41 = 0100_0001. This could be signaling to the EC that the OEM software has completed initialization.

### Phase 10: Steady state (+22s onwards)

Only 0x7CC polling every ~1.5s (watchdog/heartbeat read). No more writes.

---

## 3. HID Commands During Init

All on ITE devices (3 handles: 0xd70, 0x1148, 0x11d8, 0x1284).

### Keyboard (zone 0, handle 0xd70 for KB)

```
[00 12 00 03 00 00 00 00 00]  → Reset/Prepare
[00 14 00 01 FF 00 00 00 00]  → Set KB color: R=255, G=0, B=0
[00 08 02 01 07 32 08 00 00]  → Apply KB: effect=0x07, brightness=0x32, speed=0x08
```

### Light Bar (zone 2, handle 0x1148)

```
[00 1A 02 01 04 00 00 00 01]  → Power ON light bar + save
[00 09 22 64 00 00 00 00 00]  → Query light bar
[00 14 02 01 FF 04 00 00 00]  → Set LB color: R=255, G=4, B=0
[00 08 22 01 01 64 01 00 00]  → Apply LB: effect=static, brightness=100%
[00 12 00 03 00 00 00 00 00]  → Reset (for next batch)
[00 08 22 01 01 64 01 00 00]  → Apply LB again (double-write)
```

### Logo (zone 3, handle 0x11d8)

```
[00 1A 03 01 04 00 00 00 01]  → Power ON logo + save
[00 09 23 64 00 00 00 00 00]  → Query logo
[00 14 03 01 00 FF 00 00 00]  → Set Logo color: R=0, G=255, B=0
[00 08 23 01 01 64 01 00 00]  → Apply Logo: effect=static, brightness=100%
```

### Zone 1 (unknown zone, handle 0x1284)

```
[00 1A 01 01 04 00 00 00 01]  → Power ON zone 1 + save
[00 09 21 4B 00 00 00 00 00]  → Query zone 1 (0x21 = zone cmd ID)
[00 14 01 01 FF 00 00 00 00]  → Set zone 1 color: R=255, G=0, B=0
[00 12 00 03 00 00 00 00 00]  → Reset
[00 08 21 05 05 4B 01 01 00]  → Apply zone 1: power=5?, effect=5, brightness=0x4B(75%), save=0
```

**Zone 1 is new!** Previously marked as "unused" in our reference. The OEM powers it on and sets it to R=255 with a breathing-like effect (0x05). Handle 0x1284 is a different PID/interface from the keyboard and light bar.

### Command 0x80 — NEW (ITE sync/query)

```
[00 80 00 00 00 00 00 00 00]  → SetFeature (sent ~6 times during init)
```

GetFeature response to 0x80: `[00 80 31 05 00 00 00 00 00]` — returns firmware version or status. Sent periodically between other HID ops. May be a "sync" or "ready check" command.

### Command 0xA1 — GetFeature status

```
[00 A1 01 00 03 01 00 08 00]  → Response to various queries
```

Returned consistently. Byte 4=3, byte 5=1, byte 7=8. Possibly reporting zone count, firmware version, or feature support.

---

## 4. Unknown IOCTL Codes

```
IOCTL 0x9C40A4A0: in=[00 00 00 00] out=[86 80 1C 7D]  (called twice)
IOCTL 0x9C40A500: in=[BB XX] out=[4 bytes]  (called 12 times, XX=00..0B)
```

**0x9C40A4A0** — Not EC read/write. Returns 0x7D1C8086. This is a PCI vendor/device ID! 0x8086 = Intel, 0x7D1C = likely a PCH/chipset device. This IOCTL reads hardware identity from the ACPI driver.

**0x9C40A500** — Batched ECSpec reads. Input is 2 bytes `[0xBB, index]` and returns 4 bytes of data. Reads indices 0x00-0x0B. This is reading ECSpec configuration data (model-specific parameters).

---

## 5. NEW Registers Not In Our WriteGuard/Reference

| Addr | Value | Purpose (inferred) | Criticality |
|------|-------|--------------------|-------------|
| **0x708** | 0x01 | **EC handshake/unlock** — written early, verified | HIGH |
| **0x709** | 0x05 | **EC handshake step 1** — always before 0x708 | HIGH |
| **0x741** | 0x05 | **OEM activation byte** — read 4, wrote 5 | MEDIUM |
| **0x748** | 0x81 | **Feature enable / ack register** — written 4x during init | HIGH |
| **0x749** | 0x00 | **Clear command param** | LOW |
| **0x74A** | 0x00 | **Clear command param** | LOW |
| **0x74B** | 0x00 | **Clear command param** | LOW |
| **0x74E** | 0x00 | **Clear command param** | LOW |
| **0x706** | 0x41 | **BIOS_CTRL** — end of init signal | MEDIUM |
| **0x728** | 0x04 | **Unknown** — read-modify-write, same value | LOW |
| **0x7C7** | 0x0C | **Unknown** — near OEM5/OEM6, maybe profile param | LOW |
| **0x766** | 0x93 | **Support2 write-back** — confirms capability to EC | MEDIUM |

---

## 6. Polling/Heartbeat After Init

After init completes (+22s), only one register is polled:

```
0x7CC — read every ~1.5s, always returns 0x81
```

This is the **watchdog register**. The OEM service reads it continuously. If the EC doesn't see reads on 0x7CC, it may assume the software crashed and revert to default behavior. **This could be why features stop working without the service — the EC detects no heartbeat.**

Other registers polled during init (but which settle into the 0x7CC-only pattern):
- 0x7CA, 0x74C, 0x78E, 0x766, 0x7A6, 0x727, 0x7C5, 0x742, 0x768

---

## 7. Recommendations for MechControl

### VERIFIED — Required for standalone operation:

Both are cleared by the EC when the OEM service disconnects. Set on app startup.

1. **0x741 activation byte = 0x05**
   - Write on app startup, before any feature writes
   - EC transforms value (write 0x05, reads back 0x01)
   - Without this, EC silently ignores PL/fan/GPU writes
   - Required for PL and GPU writes. Fan curves also need #2 below.

2. **0x7C6 fan curve mode flag = 0x04 (leave set)**
   - Write on app startup, do NOT clear to 0x00
   - This is a persistent mode flag, not a trigger
   - Without this, EC ignores fan table (0xF00-0xF5F) and runs fans at ~26% default
   - Not needed for PL/GPU writes — only for fan curves
   - Our initial Frida analysis was wrong: we saw 0x04 then 0x00 mid-sequence, but the final state after full OEM init is 0x04

### NOT required (tested):

- **0x7CC watchdog polling** — Tested, no effect on write persistence.
- **0x708/0x709 handshake** — Values persist after service stops. Already set from previous service run.
- **0x748 feature enable** — Value persists after service stops. Already set.

### Not tested (OEM does them but likely unnecessary):

- 0x706 = 0x41 (BIOS_CTRL) — persists, probably not needed
- 0x766 write-back — persists, probably not needed
- 0x749/0x74A/0x74B/0x74E clears — already 0x00
- 0x7C5 bit 6 — persists from previous service run
- Zone 1 HID — unknown lighting zone, not critical

---

## 8. Full Fan Curve Written During Init

### CPU Fan Table (10 active points)

| Point | Temp Up | Temp Down | Duty (raw) | Duty % |
|-------|---------|-----------|------------|--------|
| 0 | 48°C | — | 0 | 0% |
| 1 | 52°C | 46°C | 64 | 32% |
| 2 | 56°C | 50°C | 70 | 35% |
| 3 | 60°C | 54°C | 80 | 40% |
| 4 | 64°C | 58°C | 100 | 50% |
| 5 | 68°C | 62°C | 110 | 55% |
| 6 | 72°C | 66°C | 120 | 60% |
| 7 | 76°C | 70°C | 140 | 70% |
| 8 | 80°C | 74°C | 160 | 80% |
| 9 | 85°C | 78°C | 180 | 90% |
| 10-15 | 0xFF | 0xFF/82 | 180 | pad |

### GPU Fan Table (10 active points)

| Point | Temp Up | Temp Down | Duty (raw) | Duty % |
|-------|---------|-----------|------------|--------|
| 0 | 46°C | — | 0 | 0% |
| 1 | 50°C | 44°C | 60 | 30% |
| 2 | 54°C | 48°C | 70 | 35% |
| 3 | 58°C | 52°C | 80 | 40% |
| 4 | 62°C | 56°C | 102 | 51% |
| 5 | 66°C | 60°C | 110 | 55% |
| 6 | 70°C | 64°C | 120 | 60% |
| 7 | 74°C | 68°C | 140 | 70% |
| 8 | 78°C | 72°C | 160 | 80% |
| 9 | 81°C | 76°C | 180 | 90% |
| 10-15 | 0xFF | 0xFF/80 | 180 | pad |

---

## 9. Raw Data Files

- `frida_startup_log.txt` — Full Frida capture (573 lines)
- `ec_snap_1.bin` — EC snapshot with service stopped
- `ec_snap_2.bin` — EC snapshot with service running
- `capture_startup.py` — Script used for capture
