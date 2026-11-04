# Runtime machine profiles

`MECHREVO/*.json` is the single runtime set (46 files), copied byte-for-byte from
`MechControl/Profiles/MECHREVO` in the read-only C# reference repository at commit
`6fa66cda2218fb3b6fda0b8b415544dd9f1b0596`. The upstream project is
https://github.com/Liddo-kun/MechControl-RE.
The root-level OEM archive was not imported. These files are configuration, not
proof that each model was physically tested. `verified` retains its source value.

The native loader uses these files directly; there is no second generated or
normalized runtime copy in the source tree. Packaging must place this directory
next to the executable. The hardware-neutral foundation UI does not select or
apply a machine profile yet.
