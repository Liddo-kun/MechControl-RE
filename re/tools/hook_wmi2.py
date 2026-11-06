"""Hook wmiprvse.exe - improved version that writes full buffers to file"""
import frida, sys, subprocess, time, json

JS = r"""
'use strict';
function findExport(dll,name) {
    var m=Process.findModuleByName(dll);
    if(!m)return null;
    try{return m.getExportByName(name);}catch(e){return null;}
}
var fn=findExport('ntdll.dll','NtDeviceIoControlFile');
if(fn){
    Interceptor.attach(fn,{
        onEnter:function(a){
            this.ioctl=a[5].toUInt32();
            this.ib=a[6];this.is=a[7].toUInt32();
            this.ob=a[8];this.os=a[9].toUInt32();
        },
        onLeave:function(ret){
            // Capture ALL IOCTLs in 0x224000 range
            if((this.ioctl&0xFFF000)!==0x224000)return;
            var inBytes=[];
            for(var i=0;i<Math.min(this.is,512);i++){
                try{inBytes.push(this.ib.add(i).readU8());}catch(e){break;}
            }
            var outBytes=[];
            for(var i=0;i<Math.min(this.os,512);i++){
                try{outBytes.push(this.ob.add(i).readU8());}catch(e){break;}
            }
            send({
                t:'io',
                ioctl:'0x'+this.ioctl.toString(16),
                inSz:this.is,outSz:this.os,
                inB:inBytes,outB:outBytes,
                ret:ret.toUInt32()
            });
        }
    });
    send({t:'ready'});
}
"""

OUTPATH = 'C:/Users/Jon/clean/wmi_ioctl_dump.jsonl'
outfile = open(OUTPATH, 'w')

def on_message(msg, data):
    if msg['type']=='send':
        p=msg['payload']
        if p.get('t')=='ready':
            print("[*] Hook ready. Toggle dimming now...")
        elif p.get('t')=='io':
            outfile.write(json.dumps(p)+'\n')
            outfile.flush()
            ioctl=p['ioctl']
            inSz=p['inSz']
            print(f"  [{ioctl}] in={inSz} out={p['outSz']}")
    elif msg['type']=='error':
        print(f"[ERR] {msg.get('description','?')}")

def main():
    r=subprocess.run(['powershell','-Command','(Get-Process -Name wmiprvse).Id'],capture_output=True,text=True)
    pids=[int(x.strip()) for x in r.stdout.strip().split('\n') if x.strip()]
    if not pids:
        print("No wmiprvse.exe found"); sys.exit(1)
    print(f"PIDs: {pids}")
    sessions=[]
    for pid in pids:
        try:
            s=frida.attach(pid)
            sc=s.create_script(JS)
            sc.on('message',on_message)
            sc.load()
            sessions.append(s)
            print(f"[*] Attached PID {pid}")
        except Exception as e:
            print(f"[!] PID {pid}: {e}")
    print("[*] Toggle dimming. Press Ctrl+C to stop.")
    try:
        while True: time.sleep(1)
    except KeyboardInterrupt:
        pass
    for s in sessions:
        try: s.detach()
        except: pass
    outfile.close()
    print(f"[*] Saved to {OUTPATH}")

if __name__=='__main__': main()
