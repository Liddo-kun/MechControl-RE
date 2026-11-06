# EC Trace Log Decoded Analysis

## Source Files

- `decompiled/Define/ECSpec.cs` -- Main EC register definitions
- `decompiled/Define/RamFan1_ECSpec.cs` -- Fan table base addresses (Fan 1, single-fan layout)
- `decompiled/Define/RamFan1p5_ECSpec.cs` -- Fan table base addresses (Fan 1.5, adds control/status regs)
- `decompiled/Define/RamFan2_ECSpec.cs` -- Fan table base addresses (Fan 2, dual-fan with F1/F2/F3 tables)

## Register Address Reference (hex from decimal constants)

### Fan Table Base Addresses (RamFan1 / RamFan1p5 layout)

| Hex      | Decimal | Name                          |
|----------|---------|-------------------------------|
| 0x0F00   | 3840    | ADDR_CPU_FAN_TABLE_TEMP_UP0   |
| 0x0F10   | 3856    | ADDR_CPU_FAN_TABLE_TEMP_DOWN0 |
| 0x0F20   | 3872    | ADDR_CPU_FAN_TABLE_DUTY0      |
| 0x0F30   | 3888    | ADDR_GPU_FAN_TABLE_TEMP_UP0   |
| 0x0F40   | 3904    | ADDR_GPU_FAN_TABLE_TEMP_DOWN0 |
| 0x0F50   | 3920    | ADDR_GPU_FAN_TABLE_DUTY0      |

### Fan Table Base Addresses (RamFan2 layout -- dual fan)

| Hex      | Decimal | Name                            |
|----------|---------|---------------------------------|
| 0x0F00   | 3840    | ADDR_TABLE_F1_CPU_TEMP_UP0      |
| 0x0F10   | 3856    | ADDR_TABLE_F1_CPU_TEMP_DOWN0    |
| 0x0F20   | 3872    | ADDR_TABLE_F1_DUTY0             |
| 0x0F30   | 3888    | ADDR_TABLE_F1_GPU_TEMP_UP0      |
| 0x0F40   | 3904    | ADDR_TABLE_F1_GPU_TEMP_DOWN0    |
| 0x0F50   | 3920    | ADDR_TABLE_F2_CPU_TEMP_UP0      |
| 0x0F60   | 3936    | ADDR_TABLE_F2_CPU_TEMP_DOWN0    |
| 0x0F70   | 3952    | ADDR_TABLE_F2_DUTY0             |
| 0x0F80   | 3968    | ADDR_TABLE_F2_GPU_TEMP_UP0      |
| 0x0F90   | 3984    | ADDR_TABLE_F2_GPU_TEMP_DOWN0    |
| 0x0FA0   | 4000    | ADDR_TABLE_F3_CPU_TEMP_UP0      |
| 0x0FB0   | 4016    | ADDR_TABLE_F3_CPU_TEMP_DOWN0    |
| 0x0FC0   | 4032    | ADDR_TABLE_F3_DUTY0             |
| 0x0FD0   | 4048    | ADDR_TABLE_F3_GPU_TEMP_UP0      |
| 0x0FE0   | 4064    | ADDR_TABLE_F3_GPU_TEMP_DOWN0    |

### RamFan1p5 Extra Registers

| Hex      | Decimal | Name                                  |
|----------|---------|---------------------------------------|
| 0x0F5D   | 3933    | ADDR_RAMFAN1P5_TABLE_STATUS1          |
| 0x0F5E   | 3934    | ADDR_RAMFAN1P5_TABLE_STATUS2          |
| 0x0F5F   | 3935    | ADDR_RAMFAN1P5_TABLE_CTRL             |
| 0x0786   | 1926    | ADDR_TimAP_TccOffset_Setting          |
| 0x0787   | 1927    | ADDR_TimAP_FanSwitchSpeedT100mSec     |

---

## All ECWRITE Operations Decoded

### Sequence Table (217 writes total)

Each fan table register is at `base + index`, where index = 0..15 (0x00..0x0F).

| #   | Address | Value (hex) | Value (dec) | Register Name                                  |
|-----|---------|-------------|-------------|-------------------------------------------------|
| 1   | 0x07C6  | 0x00        | 0           | ADDR_AP_OEM_BYTE6                               |
| 2   | 0x0706  | 0x40        | 64          | ADDR_AP_BIOS_CONTROL_BYTE                       |
| 3   | 0x0744  | 0x00        | 0           | ADDR_MYFAN2_L2_PWM / ADDR_ConfigurableTGP_VALUE |
| 4   | 0x0751  | 0x00        | 0           | ADDR_MAFAN_CONTROL_BYTE                         |
| 5   | 0x07C5  | 0x68        | 104         | ADDR_AP_OEM_BYTE5                               |
| 6   | 0x0F00  | 0x30        | 48          | CPU_TEMP_UP[0]                                  |
| 7   | 0x0F11  | 0x2E        | 46          | CPU_TEMP_DOWN[1]                                |
| 8   | 0x0F20  | 0x00        | 0           | CPU_DUTY[0]                                     |
| 9   | 0x0727  | 0x80        | 128         | ADDR_ESHUTTER_STATUS                            |
| 10  | 0x0F01  | 0x34        | 52          | CPU_TEMP_UP[1]                                  |
| 11  | 0x0F12  | 0x32        | 50          | CPU_TEMP_DOWN[2]                                |
| 12  | 0x0F21  | 0x3C        | 60          | CPU_DUTY[1]                                     |
| 13  | 0x0F02  | 0x38        | 56          | CPU_TEMP_UP[2]                                  |
| 14  | 0x0726  | 0x00        | 0           | ADDR_AP_OEM_BYTE9                               |
| 15  | 0x0F13  | 0x36        | 54          | CPU_TEMP_DOWN[3]                                |
| 16  | 0x0F22  | 0x46        | 70          | CPU_DUTY[2]                                     |
| 17  | 0x0F03  | 0x3C        | 60          | CPU_TEMP_UP[3]                                  |
| 18  | 0x0F14  | 0x3A        | 58          | CPU_TEMP_DOWN[4]                                |
| 19  | 0x0751  | 0x00        | 0           | ADDR_MAFAN_CONTROL_BYTE                         |
| 20  | 0x0727  | 0x80        | 128         | ADDR_ESHUTTER_STATUS                            |
| 21  | 0x0726  | 0x00        | 0           | ADDR_AP_OEM_BYTE9                               |
| 22  | 0x0F23  | 0x50        | 80          | CPU_DUTY[3]                                     |
| 23  | 0x0F04  | 0x40        | 64          | CPU_TEMP_UP[4]                                  |
| 24  | 0x0F15  | 0x3E        | 62          | CPU_TEMP_DOWN[5]                                |
| 25  | 0x0F24  | 0x64        | 100         | CPU_DUTY[4]                                     |
| 26  | 0x0F05  | 0x44        | 68          | CPU_TEMP_UP[5]                                  |
| 27  | 0x0F16  | 0x42        | 66          | CPU_TEMP_DOWN[6]                                |
| 28  | 0x0783  | 0x00        | 0           | ADDR_PL1_SETTING_VALUE                          |
| 29  | 0x0F25  | 0x6E        | 110         | CPU_DUTY[5]                                     |
| 30  | 0x0F06  | 0x48        | 72          | CPU_TEMP_UP[6]                                  |
| 31  | 0x0F17  | 0x46        | 70          | CPU_TEMP_DOWN[7]                                |
| 32  | 0x0F26  | 0x78        | 120         | CPU_DUTY[6]                                     |
| 33  | 0x0784  | 0x00        | 0           | ADDR_PL2_SETTING_VALUE                          |
| 34  | 0x0785  | 0x00        | 0           | ADDR_PL4_SETTING_VALUE                          |
| 35  | 0x0F07  | 0x4C        | 76          | CPU_TEMP_UP[7]                                  |
| 36  | 0x0F18  | 0x4A        | 74          | CPU_TEMP_DOWN[8]                                |
| 37  | 0x0F27  | 0x8C        | 140         | CPU_DUTY[7]                                     |
| 38  | 0x0F08  | 0x50        | 80          | CPU_TEMP_UP[8]                                  |
| 39  | 0x0F19  | 0x4E        | 78          | CPU_TEMP_DOWN[9]                                |
| 40  | 0x0F28  | 0xA0        | 160         | CPU_DUTY[8]                                     |
| 41  | 0x0F09  | 0x55        | 85          | CPU_TEMP_UP[9]                                  |
| 42  | 0x0F1A  | 0x52        | 82          | CPU_TEMP_DOWN[10]                               |
| 43  | 0x0F29  | 0xB4        | 180         | CPU_DUTY[9]                                     |
| 44  | 0x0F0A  | 0xFF        | 255         | CPU_TEMP_UP[10]                                 |
| 45  | 0x0F1B  | 0xFF        | 255         | CPU_TEMP_DOWN[11]                               |
| 46  | 0x0744  | 0x37        | 55          | ADDR_MYFAN2_L2_PWM / ADDR_ConfigurableTGP_VALUE |
| 47  | 0x0787  | 0x00        | 0           | ADDR_TimAP_FanSwitchSpeedT100mSec               |
| 48  | 0x0F2A  | 0xC8        | 200         | CPU_DUTY[10]                                    |
| 49  | 0x0F0B  | 0xFF        | 255         | CPU_TEMP_UP[11]                                 |
| 50  | 0x0F1C  | 0xFF        | 255         | CPU_TEMP_DOWN[12]                               |
| 51  | 0x0F2B  | 0xC8        | 200         | CPU_DUTY[11]                                    |
| 52  | 0x0F0C  | 0xFF        | 255         | CPU_TEMP_UP[12]                                 |
| 53  | 0x0743  | 0x00        | 0           | ADDR_MYFAN2_L1_PWM                              |
| 54  | 0x0F1D  | 0xFF        | 255         | CPU_TEMP_DOWN[13]                               |
| 55  | 0x0F2C  | 0xC8        | 200         | CPU_DUTY[12]                                    |
| 56  | 0x0F0D  | 0xFF        | 255         | CPU_TEMP_UP[13]                                 |
| 57  | 0x0F1E  | 0xFF        | 255         | CPU_TEMP_DOWN[14]                               |
| 58  | 0x0F2D  | 0xC8        | 200         | CPU_DUTY[13]                                    |
| 59  | 0x0F0E  | 0xFF        | 255         | CPU_TEMP_UP[14]                                 |
| 60  | 0x0F1F  | 0xFF        | 255         | CPU_TEMP_DOWN[15]                               |
| 61  | 0x0F2E  | 0xC8        | 200         | CPU_DUTY[14]                                    |
| 62  | 0x0F0F  | 0xFF        | 255         | CPU_TEMP_UP[15]                                 |
| 63  | 0x0F2F  | 0xC8        | 200         | CPU_DUTY[15]                                    |
| 64  | 0x0F30  | 0x2E        | 46          | GPU_TEMP_UP[0]                                  |
| 65  | 0x0F41  | 0x2C        | 44          | GPU_TEMP_DOWN[1]                                |
| 66  | 0x0F50  | 0x00        | 0           | GPU_DUTY[0]                                     |
| 67  | 0x0F31  | 0x32        | 50          | GPU_TEMP_UP[1]                                  |
| 68  | 0x0F42  | 0x30        | 48          | GPU_TEMP_DOWN[2]                                |
| 69  | 0x0F51  | 0x3C        | 60          | GPU_DUTY[1]                                     |
| 70  | 0x0F32  | 0x36        | 54          | GPU_TEMP_UP[2]                                  |
| 71  | 0x0F43  | 0x34        | 52          | GPU_TEMP_DOWN[3]                                |
| 72  | 0x0F52  | 0x46        | 70          | GPU_DUTY[2]                                     |
| 73  | 0x0F33  | 0x3A        | 58          | GPU_TEMP_UP[3]                                  |
| 74  | 0x0F44  | 0x38        | 56          | GPU_TEMP_DOWN[4]                                |
| 75  | 0x0F53  | 0x50        | 80          | GPU_DUTY[3]                                     |
| 76  | 0x0F34  | 0x3E        | 62          | GPU_TEMP_UP[4]                                  |
| 77  | 0x0F45  | 0x3C        | 60          | GPU_TEMP_DOWN[5]                                |
| 78  | 0x0F54  | 0x64        | 100         | GPU_DUTY[4]                                     |
| 79  | 0x0F35  | 0x42        | 66          | GPU_TEMP_UP[5]                                  |
| 80  | 0x0F46  | 0x40        | 64          | GPU_TEMP_DOWN[6]                                |
| 81  | 0x0F55  | 0x6E        | 110         | GPU_DUTY[5]                                     |
| 82  | 0x0F36  | 0x46        | 70          | GPU_TEMP_UP[6]                                  |
| 83  | 0x0F47  | 0x44        | 68          | GPU_TEMP_DOWN[7]                                |
| 84  | 0x0F56  | 0x78        | 120         | GPU_DUTY[6]                                     |
| 85  | 0x0F37  | 0x4A        | 74          | GPU_TEMP_UP[7]                                  |
| 86  | 0x0F48  | 0x48        | 72          | GPU_TEMP_DOWN[8]                                |
| 87  | 0x0F57  | 0x8C        | 140         | GPU_DUTY[7]                                     |
| 88  | 0x0F38  | 0x4E        | 78          | GPU_TEMP_UP[8]                                  |
| 89  | 0x0F49  | 0x4C        | 76          | GPU_TEMP_DOWN[9]                                |
| 90  | 0x0F58  | 0xA0        | 160         | GPU_DUTY[8]                                     |
| 91  | 0x0F39  | 0x51        | 81          | GPU_TEMP_UP[9]                                  |
| 92  | 0x0F4A  | 0x50        | 80          | GPU_TEMP_DOWN[10]                               |
| 93  | 0x0F59  | 0xB4        | 180         | GPU_DUTY[9]                                     |
| 94  | 0x0F3A  | 0xFF        | 255         | GPU_TEMP_UP[10]                                 |
| 95  | 0x0F4B  | 0xFF        | 255         | GPU_TEMP_DOWN[11]                               |
| 96  | 0x0F5A  | 0xC8        | 200         | GPU_DUTY[10]                                    |
| 97  | 0x0F3B  | 0xFF        | 255         | GPU_TEMP_UP[11]                                 |
| 98  | 0x0F4C  | 0xFF        | 255         | GPU_TEMP_DOWN[12]                               |
| 99  | 0x0F5B  | 0xC8        | 200         | GPU_DUTY[11]                                    |
| 100 | 0x0F3C  | 0xFF        | 255         | GPU_TEMP_UP[12]                                 |
| 101 | 0x0F4D  | 0xFF        | 255         | GPU_TEMP_DOWN[13]                               |
| 102 | 0x0F5C  | 0xC8        | 200         | GPU_DUTY[12]                                    |
| 103 | 0x0F3D  | 0xFF        | 255         | GPU_TEMP_UP[13]                                 |
| 104 | 0x0F4E  | 0xFF        | 255         | GPU_TEMP_DOWN[14]                               |
| 105 | 0x0F5D  | 0xC8        | 200         | GPU_DUTY[13]                                    |
| 106 | 0x0F3E  | 0xFF        | 255         | GPU_TEMP_UP[14]                                 |
| 107 | 0x0F4F  | 0xFF        | 255         | GPU_TEMP_DOWN[15]                               |
| 108 | 0x0F5E  | 0xC8        | 200         | GPU_DUTY[14]                                    |
| 109 | 0x0F3F  | 0xFF        | 255         | GPU_TEMP_UP[15]                                 |
| 110 | 0x0F5F  | 0xC8        | 200         | GPU_DUTY[15]                                    |
| 111 | 0x07C6  | 0x04        | 4           | ADDR_AP_OEM_BYTE6                               |
| 112 | 0x07C6  | 0x00        | 0           | ADDR_AP_OEM_BYTE6                               |
| 113 | 0x07C5  | 0x68        | 104         | ADDR_AP_OEM_BYTE5                               |
| 114--207 | ...  | ...         | ...         | **(Repeat of writes 6--110: identical second pass of the full fan table)** |
| 208 | 0x07C6  | 0x04        | 4           | ADDR_AP_OEM_BYTE6                               |
| 209 | 0x0748  | 0x8D        | 141         | ADDR_LIGHTBAR_CONTROL_BYTE                      |
| 210 | 0x0748  | 0x81        | 129         | ADDR_LIGHTBAR_CONTROL_BYTE                      |
| 211 | 0x0767  | 0x00        | 0           | ADDR_TRIGGER_BYTE                               |
| 212 | 0x07C5  | 0x68        | 104         | ADDR_AP_OEM_BYTE5                               |
| 213 | 0x0769  | 0x00        | 0           | ADDR_AP_EC_LOGO_R / ADDR_RGBKB_LEVEL_R          |
| 214 | 0x076A  | 0x00        | 0           | ADDR_AP_EC_LOGO_G / ADDR_RGBKB_LEVEL_G          |
| 215 | 0x076B  | 0x16        | 22          | ADDR_AP_EC_LOGO_B / ADDR_RGBKB_LEVEL_B          |
| 216 | 0x07AB  | 0x81        | 129         | ADDR_AP_EC_LOGO / ADDR_MyFanCCI_Mode_Index       |
| 217 | 0x078C  | 0x10        | 16          | ADDR_L4_PWM_DEFAULT_MYFAN2 / ADDR_AP_OEM_BYTE2  |

---

## Fan Table Writes Analysis (0x0F00+ Range)

The fan table occupies EC RAM at 0x0F00-0x0F5F. It uses the **RamFan1 / RamFan1p5 layout** (not RamFan2), consisting of 6 arrays of 16 entries each:

| Base Address | Range           | Array Name          | Description                         |
|--------------|-----------------|---------------------|-------------------------------------|
| 0x0F00       | 0x0F00 -- 0x0F0F | CPU_TEMP_UP[0..15]  | CPU temperature UP thresholds (C)   |
| 0x0F10       | 0x0F10 -- 0x0F1F | CPU_TEMP_DOWN[0..15]| CPU temperature DOWN thresholds (C) |
| 0x0F20       | 0x0F20 -- 0x0F2F | CPU_DUTY[0..15]     | Fan duty for CPU zone (PWM 0-200)   |
| 0x0F30       | 0x0F30 -- 0x0F3F | GPU_TEMP_UP[0..15]  | GPU temperature UP thresholds (C)   |
| 0x0F40       | 0x0F40 -- 0x0F4F | GPU_TEMP_DOWN[0..15]| GPU temperature DOWN thresholds (C) |
| 0x0F50       | 0x0F50 -- 0x0F5F | GPU_DUTY[0..15]     | Fan duty for GPU zone (PWM 0-200)   |

### Extracted CPU Fan Curve

The "TEMP_UP" values are the thresholds for ramping the fan speed **up** (ascending temperature).
The "TEMP_DOWN" values are the thresholds for ramping the fan speed **down** (hysteresis, descending temperature).
TEMP_DOWN[n] corresponds to the threshold to drop *from* step n back to step n-1, so TEMP_DOWN[0] is not written (implicit); TEMP_DOWN[1] is the first used.

| Step | TEMP_UP (C) | TEMP_DOWN (C) | Duty (PWM) | Duty (%) |
|------|-------------|---------------|------------|----------|
| 0    | 48          | --            | 0          | 0%       |
| 1    | 52          | 46            | 60         | 30%      |
| 2    | 56          | 50            | 70         | 35%      |
| 3    | 60          | 54            | 80         | 40%      |
| 4    | 64          | 58            | 100        | 50%      |
| 5    | 68          | 62            | 110        | 55%      |
| 6    | 72          | 66            | 120        | 60%      |
| 7    | 76          | 70            | 140        | 70%      |
| 8    | 80          | 74            | 160        | 80%      |
| 9    | 85          | 78            | 180        | 90%      |
| 10   | 255 (off)   | 255 (off)     | 200        | 100%     |
| 11   | 255 (off)   | 255 (off)     | 200        | 100%     |
| 12   | 255 (off)   | 255 (off)     | 200        | 100%     |
| 13   | 255 (off)   | 255 (off)     | 200        | 100%     |
| 14   | 255 (off)   | 255 (off)     | 200        | 100%     |
| 15   | 255 (off)   | 255 (off)     | 200        | 100%     |

**Note:** TEMP_DOWN[0] was not explicitly written in the trace; it is stored at 0x0F10 which was never seen in a write (TEMP_DOWN values start being written from index 1 at 0x0F11). Steps 10-15 are unused (sentinel value 0xFF=255).

### Extracted GPU Fan Curve

| Step | TEMP_UP (C) | TEMP_DOWN (C) | Duty (PWM) | Duty (%) |
|------|-------------|---------------|------------|----------|
| 0    | 46          | --            | 0          | 0%       |
| 1    | 50          | 44            | 60         | 30%      |
| 2    | 54          | 48            | 70         | 35%      |
| 3    | 58          | 52            | 80         | 40%      |
| 4    | 62          | 56            | 100        | 50%      |
| 5    | 66          | 60            | 110        | 55%      |
| 6    | 70          | 64            | 120        | 60%      |
| 7    | 74          | 68            | 140        | 70%      |
| 8    | 78          | 72            | 160        | 80%      |
| 9    | 81          | 76            | 180        | 90%      |
| 10   | 255 (off)   | 80            | 200        | 100%     |
| 11   | 255 (off)   | 255 (off)     | 200        | 100%     |
| 12   | 255 (off)   | 255 (off)     | 200        | 100%     |
| 13   | 255 (off)   | 255 (off)     | 200        | 100%     |
| 14   | 255 (off)   | 255 (off)     | 200        | 100%     |
| 15   | 255 (off)   | 255 (off)     | 200        | 100%     |

### Fan Curve Pattern Confirmed

**Yes -- the 0x0F00+ writes are a structured fan table writing CPU temp thresholds, GPU temp thresholds, and fan duty (PWM) values.**

The pattern is:

1. **CPU temperature UP thresholds** (0x0F00-0x0F0F): 10 active steps from 48C to 85C, then 6 sentinel 0xFF entries.
2. **CPU temperature DOWN thresholds** (0x0F10-0x0F1F): Hysteresis values 2-6C below the corresponding UP thresholds, providing debounce. Index 0 (0x0F10) is never written.
3. **CPU fan duty** (0x0F20-0x0F2F): PWM values from 0 (off) through 200 (100%), in 10 active steps.
4. **GPU temperature UP thresholds** (0x0F30-0x0F3F): 10 active steps from 46C to 81C (slightly lower than CPU), then 6 sentinel 0xFF entries.
5. **GPU temperature DOWN thresholds** (0x0F40-0x0F4F): Hysteresis values 2-4C below the corresponding UP thresholds. Index 0 (0x0F40) is never written.
6. **GPU fan duty** (0x0F50-0x0F5F): PWM values from 0 (off) through 200 (100%), in 10 active steps. Identical duty curve to CPU.

Key observations:

- The duty curves for CPU and GPU are **identical** (same PWM values at each step).
- GPU thresholds run about 2-4C **lower** than CPU thresholds (GPU starts ramping at 46C vs CPU at 48C).
- Hysteresis gap is consistently 2-6C (TEMP_DOWN is always below TEMP_UP), preventing oscillation.
- Only 10 of the 16 steps are actively used; steps 10-15 are filled with 0xFF/200 as padding/sentinel.
- The full fan table is written **three times** in the trace (passes 1, 2, and 3 are byte-identical), suggesting the app writes the table on initialization and again on mode-apply.

---

## Non-Fan-Table Writes Summary

These writes configure other EC registers during the same session:

| Address | Value | Register Name                   | Interpretation                                     |
|---------|-------|---------------------------------|----------------------------------------------------|
| 0x07C6  | 0x00  | ADDR_AP_OEM_BYTE6               | Clear OEM byte 6 (pre-init)                        |
| 0x0706  | 0x40  | ADDR_AP_BIOS_CONTROL_BYTE       | 0x40 = FanBoost_Mode flag in MyFanCTLByteFlag      |
| 0x0744  | 0x00  | ADDR_MYFAN2_L2_PWM              | Clear L2 PWM                                       |
| 0x0751  | 0x00  | ADDR_MAFAN_CONTROL_BYTE         | Normal_Mode (0x00)                                 |
| 0x07C5  | 0x68  | ADDR_AP_OEM_BYTE5               | 0x68 = 104 decimal, configuration flags             |
| 0x0727  | 0x80  | ADDR_ESHUTTER_STATUS            | Bit7 set (camera shutter status)                   |
| 0x0726  | 0x00  | ADDR_AP_OEM_BYTE9               | Clear                                              |
| 0x0783  | 0x00  | ADDR_PL1_SETTING_VALUE          | PL1 = 0 (use default)                              |
| 0x0784  | 0x00  | ADDR_PL2_SETTING_VALUE          | PL2 = 0 (use default)                              |
| 0x0785  | 0x00  | ADDR_PL4_SETTING_VALUE          | PL4 = 0 (use default)                              |
| 0x0744  | 0x37  | ADDR_ConfigurableTGP_VALUE      | TGP value = 55W                                    |
| 0x0787  | 0x00  | ADDR_TimAP_FanSwitchSpeedT100mSec | Fan switch speed delay = 0                        |
| 0x0743  | 0x00  | ADDR_MYFAN2_L1_PWM              | Clear L1 PWM                                       |
| 0x07C6  | 0x04  | ADDR_AP_OEM_BYTE6               | Set bit 2 (commit/apply flag)                      |
| 0x0748  | 0x8D  | ADDR_LIGHTBAR_CONTROL_BYTE      | 0x8D = Welcome + S3_Switch + S0_Switch + ApExit     |
| 0x0748  | 0x81  | ADDR_LIGHTBAR_CONTROL_BYTE      | 0x81 = Welcome + ApExit                            |
| 0x0767  | 0x00  | ADDR_TRIGGER_BYTE               | Clear triggers                                     |
| 0x0769  | 0x00  | ADDR_AP_EC_LOGO_R               | Logo R = 0                                         |
| 0x076A  | 0x00  | ADDR_AP_EC_LOGO_G               | Logo G = 0                                         |
| 0x076B  | 0x16  | ADDR_AP_EC_LOGO_B               | Logo B = 22                                        |
| 0x07AB  | 0x81  | ADDR_AP_EC_LOGO                 | Logo control = 0x81 (confirm/enable, User_Fan_Level1)|
| 0x078C  | 0x10  | ADDR_AP_OEM_BYTE2               | 0x10 = Turbo_Mode flag                             |

---

## Trace Execution Flow

1. **Initialization**: Clear AP_OEM_BYTE6, set BIOS control to FanBoost, clear fan control to Normal, set OEM byte 5, clear eShutter/OEM9.
2. **Fan table write (Pass 1)**: Write all 6 arrays (CPU temp up, CPU temp down, CPU duty, GPU temp up, GPU temp down, GPU duty) with 16 entries each. Interleaved with reads of control registers (0x0727, 0x0726, 0x0751).
3. **Power limit setup**: Clear PL1/PL2/PL4 settings, set ConfigurableTGP to 55W, clear fan switch speed.
4. **Commit**: Set AP_OEM_BYTE6 = 0x04 (apply).
5. **Fan table write (Pass 2)**: Identical re-write of the entire fan table.
6. **Commit again**: Set AP_OEM_BYTE6 = 0x04.
7. **Fan table write (Pass 3)**: Third identical write of the full fan table.
8. **Final commit**: Set AP_OEM_BYTE6 = 0x04.
9. **Lightbar/Logo setup**: Configure lightbar to 0x8D then 0x81, clear triggers, set logo color (R=0, G=0, B=22 -- deep blue), enable logo, set AP_OEM_BYTE2 to Turbo_Mode.
