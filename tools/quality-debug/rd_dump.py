# Runs inside qrenderdoc (--python). Dumps a capture's actions with their colour and depth
# outputs, the textures they use, and saves every large render target at the end of the frame.
# Env: RD_CAP = capture path, RD_OUT = output dir.
import os, sys, traceback
import renderdoc as rd

cap_path = os.environ["RD_CAP"]
out_dir = os.environ["RD_OUT"]
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "dump.txt"), "w", encoding="utf-8")

def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n")
    log.flush()

try:
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(cap_path, "", None) != rd.ResultCode.Succeeded:
        raise RuntimeError("open failed")
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        raise RuntimeError("replay failed: %s" % result)
    sf = ctrl.GetStructuredFile()

    texs = {t.resourceId: t for t in ctrl.GetTextures()}
    names = {r.resourceId: r.name for r in ctrl.GetResources()}

    def tdesc(rid):
        t = texs.get(rid)
        if t is None:
            return str(rid)
        return "%s %dx%d a%d m%d %s" % (names.get(rid, rid), t.width, t.height, t.arraysize,
                                        t.mips, t.format.Name())

    p("TEXTURES (>=512 wide)")
    for rid, t in texs.items():
        if t.width >= 512:
            p("  ", tdesc(rid), "creation", t.creationFlags)

    p("ACTIONS")
    def walk(actions, depth):
        for a in actions:
            outs = [tdesc(o) for o in a.outputs if o != rd.ResourceId.Null()]
            dep = tdesc(a.depthOut) if a.depthOut != rd.ResourceId.Null() else ""
            p("%s%d %s | out=%s | depth=%s | idx=%d inst=%d" % (
                "  " * depth, a.eventId, a.GetName(sf), outs, dep, a.numIndices, a.numInstances))
            if a.children:
                walk(a.children, depth + 1)
    walk(ctrl.GetRootActions(), 0)

    # Final contents of every large colour target.
    last = ctrl.GetRootActions()[-1]
    while last.children:
        last = last.children[-1]
    ctrl.SetFrameEvent(last.eventId, True)
    for rid, t in texs.items():
        if t.width >= 900 and (t.creationFlags & rd.TextureCategory.ColorTarget):
            for sl in range(min(t.arraysize, 2)):
                s = rd.TextureSave()
                s.resourceId = rid
                s.destType = rd.FileType.PNG
                s.mip = 0
                s.slice.sliceIndex = sl
                s.alpha = rd.AlphaMapping.Discard
                fn = os.path.join(out_dir, "final_%s_s%d.png" % (str(int(rid)), sl))
                ctrl.SaveTexture(s, fn)
    p("DONE")
    ctrl.Shutdown()
    cap.Shutdown()
except Exception:
    p("ERROR", traceback.format_exc())
log.close()
os._exit(0)
