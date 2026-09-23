# Pixel history on the eye scene target (1882x2117 R11G11B10, 4x MSAA) for a few pixels.
# Env: RD_CAP, RD_OUT, RD_PIXELS = "x,y;x,y;..."
import os, traceback
import renderdoc as rd

out_dir = os.environ["RD_OUT"]
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "hist.txt"), "w", encoding="utf-8")
def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n"); log.flush()

def col(c):
    return "(%.4f %.4f %.4f %.4f)" % tuple(c.floatValue[:4])

try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RD_CAP"], "", None)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    sf = ctrl.GetStructuredFile()
    names = {r.resourceId: r.name for r in ctrl.GetResources()}
    actions = {}
    def walk(al):
        for a in al:
            actions[a.eventId] = a
            walk(a.children)
    walk(ctrl.GetRootActions())

    targets = [t for t in ctrl.GetTextures()
               if t.width == 1882 and t.height == 2117 and t.format.Name() == "R11G11B10_FLOAT"]
    for t in targets:
        p("TARGET", int(t.resourceId), names.get(t.resourceId), "samples", t.msSamp)
    pixels = [tuple(int(v) for v in s.split(",")) for s in os.environ["RD_PIXELS"].split(";")]
    last_eid = max(actions)
    ctrl.SetFrameEvent(last_eid, True)
    for t in targets:
        for (x, y) in pixels:
            p("=== target", int(t.resourceId), "samples", t.msSamp, "pixel", x, y)
            sub = rd.Subresource(0, 0, 0)
            hist = ctrl.PixelHistory(t.resourceId, x, y, sub, rd.CompType.Typeless)
            for m in hist:
                a = actions.get(m.eventId)
                nm = a.GetName(sf) if a else "?"
                flags = []
                for f in ("sampleMasked", "backfaceCulled", "depthClipped", "viewClipped",
                          "scissorClipped", "shaderDiscarded", "depthTestFailed",
                          "stencilTestFailed", "predicationSkipped", "unboundPS", "directShaderWrite"):
                    if getattr(m, f, False):
                        flags.append(f)
                p("  eid %d prim %d %s | pass=%s %s | shaderOut=%s post=%s depth=%.5f" % (
                    m.eventId, m.primitiveID, nm[:60], m.Passed(), ",".join(flags),
                    col(m.shaderOut.col), col(m.postMod.col), m.postMod.depth))
    p("DONE")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    p("ERROR", traceback.format_exc())
log.close()
os._exit(0)
