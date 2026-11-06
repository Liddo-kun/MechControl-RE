#!/usr/bin/env python3
"""Trim a Frida startup capture log to reduce token count while preserving all EC data."""
import re, sys

input_path = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\Jon\clean\frida_startup_capture2_startup - trimmed.log'

with open(input_path, 'r') as f:
    lines = f.readlines()

output = []
duty_buf = []
noise = {'module': 0, 'wmi': 0, 'hid': 0, 'ioctl_a500': 0}

def duty_sig(line):
    m = re.search(r'CPU=(\d+%) GPU=(\d+%).*(7C5=\S+ 7C6=\S+ 706=\S+)', line)
    return m.groups() if m else None

def flush_duty():
    global duty_buf
    if not duty_buf:
        return
    if len(duty_buf) == 1:
        output.append(duty_buf[0])
    else:
        t0 = re.search(r'\[([\d:.]+)\]', duty_buf[0]).group(1)
        t1 = re.search(r'\[([\d:.]+)\]', duty_buf[-1]).group(1)
        base = re.sub(r'\[[\d:.]+\]', f'[{t0}~{t1}]', duty_buf[-1])
        output.append(f'{base.rstrip()} (x{len(duty_buf)})')
    duty_buf = []

def flush_noise():
    parts = []
    if noise['module']:  parts.append(f"{noise['module']} modules")
    if noise['wmi']:     parts.append(f"{noise['wmi']} WMI CoCreateInstance")
    if noise['hid']:     parts.append(f"{noise['hid']} HidD_SetFeature")
    if noise['ioctl_a500']: parts.append(f"{noise['ioctl_a500']} IOCTL_0x9C40A500")
    if parts:
        output.append(f'  [omitted: {", ".join(parts)}]')
    for k in noise: noise[k] = 0

def strip_hour(s):
    return re.sub(r'(\d+):(\d+:\d+\.\d+)', r'\2', s)

for raw in lines:
    line = raw.rstrip('\n')

    # --- Noise filtering ---
    if '[MODULE] Loaded:' in line:
        noise['module'] += 1; continue
    if 'CoCreateInstance' in line:
        noise['wmi'] += 1; continue
    if 'IOCTL_0x9C40A500' in line:
        noise['ioctl_a500'] += 1; continue
    if 'HidD_SetFeature' in line or 'HidD_GetFeature' in line or 'HidD_SetOutputReport' in line:
        noise['hid'] += 1; continue
    if re.match(r'\[(OK|INFO|NOTE|WARN|READY)\]', line):
        continue

    # --- DUTY lines ---
    if line.startswith('DUTY:'):
        flush_noise()
        line = strip_hour(line)
        line = re.sub(r'(CPU=\d+%)\(0x[0-9A-Fa-f]+\)', r'\1', line)
        line = re.sub(r'(GPU=\d+%)\(0x[0-9A-Fa-f]+\)', r'\1', line)
        sig = duty_sig(line)
        if duty_buf and duty_sig(duty_buf[-1]) == sig:
            duty_buf.append(line)
        else:
            flush_duty()
            duty_buf.append(line)
        continue

    # Non-DUTY: flush
    flush_duty()
    flush_noise()

    # --- ECWRITE ---
    m = re.match(r'\[\d+:(\d+:\d+\.\d+)\] ECWRITE addr=(0x[0-9A-Fa-f]+)(?:\(([^)]+)\))? val=(0x[0-9A-Fa-f]+)', line)
    if m:
        time, addr, name, val_hex = m.group(1), m.group(2), m.group(3), m.group(4)
        v = int(val_hex, 16)
        n = f'({name})' if name else ''
        output.append(f'[{time}] W {addr}{n}=0x{v:02X}')
        continue

    # --- ECREAD ---
    m = re.match(r'\[\d+:(\d+:\d+\.\d+)\] ECREAD addr=(0x[0-9A-Fa-f]+)(?:\(([^)]+)\))? \| in\(\d+\)=\[[^\]]+\] out\(\d+\)=\[([0-9A-Fa-f ]+)\]', line)
    if m:
        time, addr, name, out_hex = m.group(1), m.group(2), m.group(3), m.group(4)
        v = int(out_hex.strip().split()[0], 16)
        n = f'({name})' if name else ''
        output.append(f'[{time}] R {addr}{n}=0x{v:02X}')
        continue

    # --- IOCTL_0x9C40A4A0 ---
    if 'IOCTL_0x9C40A4A0' in line:
        m = re.match(r'\[\d+:(\d+:\d+\.\d+)\]', line)
        if m:
            output.append(f'[{m.group(1)}] IOCTL_0x9C40A4A0 (PCI vendor ID)')
        continue

    # --- Everything else: strip hour ---
    output.append(strip_hour(line))

flush_duty()
flush_noise()

with open(input_path, 'w', newline='\n') as f:
    for l in output:
        f.write(l + '\n')

print(f'{len(lines)} lines -> {len(output)} lines')
