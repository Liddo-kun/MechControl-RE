import frida, sys, time, threading

target = r"C:\Users\Jon\code\Mechcontrol\hwmonitor_1.65.1\HWMonitor_x64.exe"
duration = int(sys.argv[1]) if len(sys.argv) > 1 else 45

def on_message(msg, data):
    if msg.get("type") == "send":
        p = msg["payload"]
        t = p.get("t")
        if t == "qi":
            print("QI  %s -> %s" % (p["id"], p["ret"]))
        elif t == "call":
            print("CALL %s rv=%d" % (p["id"], p["rv"]))
            if "hex" in p:
                for line in p["hex"].splitlines():
                    print("    " + line)
        else:
            print(p)
    else:
        print(msg)

device = frida.get_local_device()
pid = device.spawn([target])
session = device.attach(pid)
script = session.create_script(open(r"C:\Users\Jon\code\Mechcontrol\tools\frida_nvapi_hook.js").read())
script.on("message", on_message)
script.load()
device.resume(pid)
print("spawned pid %d, capturing %ds..." % (pid, duration))
time.sleep(duration)
try:
    device.kill(pid)
except Exception:
    pass
print("done")
