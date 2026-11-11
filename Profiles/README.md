# Runtime machine profiles

`MECHREVO/*.json` is the runtime profile set, one compact file per model. The files
started as copies of the C# app's `MechControl/Profiles/MECHREVO` at commit `6fa66cd`.
They are configuration, not proof that each model was physically tested. A profile
names registers but cannot widen what the app writes: the write allowlist is the same
for every model, and limits the EC reports replace the profile's.

The full OEM-extracted profiles that these were mapped from are in
`re/Profiles/MECHREVO`. The app does not load those.

The loader uses these files directly; there is no generated or normalized copy.
Packaging must place this directory next to the executable.
