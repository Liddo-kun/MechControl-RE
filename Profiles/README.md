# Runtime machine profiles

`MECHREVO/*.json` is the runtime profile set, one compact file per model. The files
started as copies of the C# app's `MechControl/Profiles/MECHREVO` at commit `6fa66cd`.
They are configuration, not proof that each model was physically tested. A profile
names registers but cannot widen what the app writes: the write allowlist is the same
for every model, and limits the EC reports replace the profile's.

They were mapped from the OEM's own model profiles, which are not part of this repository.

`boards.json` lists baseboard names per feature, for functions that no EC byte announces
and that therefore cannot be detected at runtime (AC auto boot). A feature is offered only
on a baseboard that contains a listed name as a whole word. Add a name only when the Linux
drivers list the board for that function or its EC firmware has been read; the file's note
gives the sources.

The loader uses these files directly; there is no generated or normalized copy.
Packaging must place this directory next to the executable.
