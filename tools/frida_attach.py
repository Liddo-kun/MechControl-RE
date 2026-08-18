import frida, sys, time, json

pid = int(sys.argv[1])
duration = int(sys.argv[2])
script_path = sys.argv[3]
out_path = sys.argv[4] if len(sys.argv) > 4 else None

out = open(out_path, "w") if out_path else None

def emit(obj):
    line = json.dumps(obj)
    if out:
        out.write(line + "\n"); out.flush()
    else:
        print(line)

def on_message(msg, data):
    if msg.get("type") == "send":
        emit(msg["payload"])
    else:
        emit({"t": "error", "msg": str(msg)})

session = frida.attach(pid)
script = session.create_script(open(script_path).read())
script.on("message", on_message)
script.load()
print("attached to %d, capturing %ds -> %s" % (pid, duration, out_path or "stdout"))
time.sleep(duration)
session.detach()
if out: out.close()
print("done")
