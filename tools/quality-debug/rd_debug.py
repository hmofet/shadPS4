# Debug one pixel of one draw and log the value each SPIR-V id takes (last value wins, and the
# first 3 values of ids written more than once). Env: RD_CAP, RD_OUT, RD_EID, RD_X, RD_Y
import os, traceback
import renderdoc as rd

out_dir = os.environ["RD_OUT"]
eid = int(os.environ["RD_EID"]); x = int(os.environ["RD_X"]); y = int(os.environ["RD_Y"])
log = open(os.path.join(out_dir, "debug_%d_%d_%d.txt" % (eid, x, y)), "w", encoding="utf-8")
def p(*a):
    log.write(" ".join(str(v) for v in a) + "\n"); log.flush()

def val(v):
    n = max(1, v.columns) * max(1, v.rows)
    if v.type in (rd.VarType.Float, rd.VarType.Half, rd.VarType.Double):
        return [round(v.value.f32v[i], 6) for i in range(min(n, 4))]
    if v.type in (rd.VarType.SInt,):
        return [v.value.s32v[i] for i in range(min(n, 4))]
    if v.type in (rd.VarType.UInt, rd.VarType.Bool):
        return [v.value.u32v[i] for i in range(min(n, 4))]
    return str(v.type)

try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RD_CAP"], "", None)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    ctrl.SetFrameEvent(eid, True)
    inputs = rd.DebugPixelInputs()
    inputs.sample = 0
    trace = ctrl.DebugPixel(x, y, inputs)
    if trace is None or trace.debugger is None:
        p("NO TRACE")
    else:
        last = {}
        hist = {}
        steps = 0
        while True:
            states = ctrl.ContinueDebug(trace.debugger)
            if not states:
                break
            for st in states:
                steps += 1
                for ch in st.changes:
                    v = ch.after
                    last[v.name] = val(v)
                    hist.setdefault(v.name, [])
                    if len(hist[v.name]) < 3:
                        hist[v.name].append(val(v))
        p("steps", steps)
        for k in sorted(last, key=lambda s: (len(s), s)):
            p(k, last[k], "first:", hist[k] if len(hist[k]) > 1 else "")
        ctrl.FreeTrace(trace)
    p("DONE")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    p("ERROR", traceback.format_exc())
log.close()
os._exit(0)
