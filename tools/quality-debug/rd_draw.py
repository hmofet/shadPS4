# For given draws, list the pixel shader's bound textures (with min/max per mip and slice) and
# save the shader disassembly. Env: RD_CAP, RD_OUT, RD_EIDS = "eid,eid"
import os, traceback
import renderdoc as rd

out_dir = os.environ["RD_OUT"]
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "draws.txt"), "w", encoding="utf-8")
def p(*a):
    log.write(" ".join(str(x) for x in a) + "\n"); log.flush()

try:
    cap = rd.OpenCaptureFile()
    cap.OpenFile(os.environ["RD_CAP"], "", None)
    result, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    names = {r.resourceId: r.name for r in ctrl.GetResources()}
    texs = {t.resourceId: t for t in ctrl.GetTextures()}
    targets = ctrl.GetDisassemblyTargets(True)
    for eid in [int(e) for e in os.environ["RD_EIDS"].split(",")]:
        ctrl.SetFrameEvent(eid, True)
        pipe = ctrl.GetPipelineState()
        p("===== EID", eid)
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Pixel):
            refl = pipe.GetShaderReflection(stage)
            if refl is None:
                continue
            p(" stage", stage, "shader", int(pipe.GetShader(stage)), names.get(pipe.GetShader(stage)))
            dis = ctrl.DisassembleShader(pipe.GetGraphicsPipelineObject(), refl, targets[0])
            with open(os.path.join(out_dir, "shader_%d_%s.txt" % (eid, str(stage).split(".")[-1])), "w", encoding="utf-8") as f:
                f.write(dis)
            for used in pipe.GetReadOnlyResources(stage, True):
                rid = used.descriptor.resource
                t = texs.get(rid)
                if t is None:
                    p("   ro", used.access.index, "non-texture", names.get(rid, rid))
                    continue
                p("   ro idx %d: %s  %dx%d a%d m%d %s" % (used.access.index, names.get(rid, rid)[:70],
                  t.width, t.height, t.arraysize, t.mips, t.format.Name()))
                for mip in range(min(t.mips, 8)):
                    for sl in range(min(t.arraysize, 3)):
                        mn, mx = ctrl.GetMinMax(rid, rd.Subresource(mip, sl, 0), rd.CompType.Typeless)
                        p("      mip %d slice %d min %s max %s" % (mip, sl,
                          ["%.4f" % v for v in mn.floatValue[:4]], ["%.4f" % v for v in mx.floatValue[:4]]))
            for used in pipe.GetReadWriteResources(stage, True):
                p("   rw", used.access.index, names.get(used.descriptor.resource))
    p("DONE")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    p("ERROR", traceback.format_exc())
log.close()
os._exit(0)
