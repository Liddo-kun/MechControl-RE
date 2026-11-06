# NVIDIA thermal channels: is there a VRM temperature?

Measured 2026-11-05 on Jon's RTX 5090 Laptop (driver 617.14), read-only. Probe:
`C:\Temp\tandem\therm-probe\` (`ThermProbe.exe [samples]`, output `run1.txt`, `run2.txt`).

**Answer: no.** The GPU reports no VRM temperature through any NVAPI interface we have.

- **ThermDeviceGetInfo** (0x6FF0350C, size 0x6A8 v1): 18 devices, classes 0x80-0x86, all newer
  than the public header (no I2C sensor classes 0x41-0x46). Info data is all zero.
- **ThermChannelGetInfo** (0x0BC8163D, size 0xAB0 v2): 19 channels. Ch 0 GPU_MAX (hotspot),
  ch 1 GPU_AVG (core), ch 2 MEMORY; ch 3-18 use types 16-55, not in the public header. No
  BOARD or PWR_SUPPLY channel. Primary channels: GPU_AVG 1, GPU_MAX 0, MEMORY 2.
- **ThermChannelGetStatus** (0x65FE3AAD, size 0xA8 v2, 1/256 C): only ch 1 (core) and ch 2
  (memory) read real values. All others, hotspot included, return 0xFF00 (255.0 C), the
  "unavailable" marker. Matches NVIDIA hiding hotspot on RTX 50.
- **ThermalPmumonThermChannelsGetSamples** (0xA7F97CF3, size 0x35B8 v1): the PMU's own sample
  history has the same 0xFF00 for every channel except core and memory, so the values are
  hidden before they reach any user-mode call.
- **ThermalPolicyGetInfo** (0x2F69F8E5, size 0x34C v1): policies watch only ch 1 (89 C; 75/87 C),
  ch 0 (105 C) and ch 2 (103 C). Nothing watches a VR channel.
- **Thermal monitors**: no class 1 (VOLTAGE_REGULATOR, the VR_HOT signal) on this GPU
  (classes 7, 8, 5, 4, 4, 5, 5, 5, 3; see `nvidia_gpu_limiters.md`).
- **Power devices** (type 23 GPU ADC, type 24 computed rails) report power, current and voltage
  only.

## mVolt+ extra temperatures

mVolt+ shows hotspot ("Direct GPU sensor"), GPU_CH0-3, GPU_OFFSET_AVG, SYS_TSENSE and 16 front
memory sites, so it reads past the 0xFF00 mask some other way (not traced). None is a VRM:
TSENSE is NVIDIA's on-die sensor, and mVolt+'s own tooltip says SYS_TSENSE "does not mean a
motherboard, VRM or SYS clock sensor" (`C:\Temp\tandem\mvolt-strings.txt`).

## GPU I2C route (tried 2026-11-05, closed)

Probe: `C:\Temp\tandem\i2c-probe\` (`I2cProbe.exe volt|edid|ports|calib|scan <port>`). It never
writes to the bus: only plain reads (address + read bit, no register pointer).
VBIOS parser: `C:\Temp\tandem\vbios-survey\i2c_dump.py`.

- **The GPU doesn't talk to its VRs over I2C.** VoltVoltDevicesGetInfo (0xA38ACF9D, size 0xF48
  v1): 4 voltage devices (two each for voltage domains 1 and 4), all with I2C device index 0xFF
  (none). They're driven by PWM (period field 180), 444-1280 mV in 5 mV steps.
- **The VBIOS lists no I2C devices.** The DCB I2C Devices Table (32 entries) is all "skip" on this
  VBIOS, on the MECHREVO 5080/4070/4060, the Tongfang 5060 and the ASUS ProArt 5090. The Razer 5090
  and Lenovo 5070 Ti list one unknown type 0xE0 at 7-bit 0x61 on the secondary port; the ASUS
  desktop Astral lists two type 0x45 at 0x30/0x35. The CCB header names port 2 primary and port 1
  secondary.
- **NVAPI refuses raw port access.** NvAPI_I2CReadEx (0x4D7B0709, NV_I2C_INFO_V3, 64 bytes) by port
  ID returns -1 on every port 0-15, real or not, and the PRIVILEGE flag changes nothing. The
  display path works (panel EDID via display mask 0x100), so the struct is right. Its "comm"
  flavor ACKs 0x30 and 0x50-0x77 with 00, which is the eDP panel's I2C-over-AUX responder, not a
  GPU bus.

## Other routes (not tried)

- **EC thermistors.** Investigated in `ec_thermistors.md`.
- **The VR controller's own telemetry pins.** If the board photo shows a controller that supports
  PMBus/SMBus, its bus most likely goes to the EC or the chipset (PCH) SMBus, not the GPU.
