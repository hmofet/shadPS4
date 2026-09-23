# Save a draw's pixel-shader textures (rgb and alpha separately) and their view swizzle/format.
import os, traceback, renderdoc as rd
o = os.environ["RD_OUT"]; eid = int(os.environ["RD_EID"])
log = open(os.path.join(o, "tex_%d.txt" % eid), "w")
try:
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RD_CAP"], "", None)
    r, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    ctrl.SetFrameEvent(eid, True)
    pipe = ctrl.GetPipelineState()
    texs = {t.resourceId: t for t in ctrl.GetTextures()}
    for used in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel, True):
        d = used.descriptor; rid = d.resource
        t = texs.get(rid)
        if t is None: continue
        sw = d.swizzle
        log.write("idx %d res %d fmt(view) %s tex %s %dx%d swizzle %s %s %s %s firstMip %d numMips %d\n" % (
            used.access.index, int(rid), d.format.Name(), t.format.Name(), t.width, t.height,
            sw.red, sw.green, sw.blue, sw.alpha, d.firstMip, d.numMips))
        for mode, name in ((rd.AlphaMapping.Discard, "rgb"), (None, "a")):
            s = rd.TextureSave(); s.resourceId = rid; s.destType = rd.FileType.PNG; s.mip = 2
            if mode is None:
                s.channelExtract = 3
            else:
                s.alpha = mode
            ctrl.SaveTexture(s, os.path.join(o, "tex_%d_idx%d_%s.png" % (eid, used.access.index, name)))
    log.write("DONE\n")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    log.write("ERROR " + traceback.format_exc())
log.close(); os._exit(0)
