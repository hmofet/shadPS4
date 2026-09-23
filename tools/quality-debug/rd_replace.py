# Replace one pixel shader with an edited GLSL version, replay, and report pixels and the final
# eye. Env: RD_CAP, RD_OUT, RD_EID (a draw using the shader), RD_GLSL (edited source),
# RD_TAG (output name), RD_PIXELS, RD_SCENE (scene target id), RD_EYE (final eye id)
import os, traceback
import renderdoc as rd

o = os.environ["RD_OUT"]; tag = os.environ["RD_TAG"]
log = open(os.path.join(o, "replace_%s.txt" % tag), "w", encoding="utf-8")
def p(*a):
    log.write(" ".join(str(v) for v in a) + "\n"); log.flush()

try:
    cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RD_CAP"], "", None)
    r, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    rid = {int(t.resourceId): t.resourceId for t in ctrl.GetTextures()}
    scene = rid[int(os.environ["RD_SCENE"])]; eye = rid[int(os.environ["RD_EYE"])]
    pixels = [tuple(int(v) for v in s.split(",")) for s in os.environ["RD_PIXELS"].split(";")]
    last = ctrl.GetRootActions()[-1]
    while last.children:
        last = last.children[-1]

    def report(label):
        ctrl.SetFrameEvent(last.eventId, True)
        for (x, y) in pixels:
            v = ctrl.PickPixel(scene, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
            p(label, "scene", x, y, ["%.4f" % f for f in v.floatValue[:3]])
        s = rd.TextureSave(); s.resourceId = eye; s.destType = rd.FileType.PNG
        s.slice.sliceIndex = 0; s.alpha = rd.AlphaMapping.Discard
        ctrl.SaveTexture(s, os.path.join(o, "eye_%s_%s.png" % (tag, label)))

    report("before")
    ctrl.SetFrameEvent(int(os.environ["RD_EID"]), True)
    pipe = ctrl.GetPipelineState()
    orig = pipe.GetShader(rd.ShaderStage.Pixel)
    for path in os.environ["RD_GLSL"].split(";"):
        src = open(path, "rb").read()
        enc = rd.ShaderEncoding.SPIRV if path.endswith(".spv") else rd.ShaderEncoding.GLSL
        new_id, errors = ctrl.BuildTargetShader("main", enc, src,
                                                rd.ShaderCompileFlags(), rd.ShaderStage.Pixel)
        if new_id == rd.ResourceId.Null():
            p("build failed", path, errors)
            continue
        ctrl.ReplaceResource(orig, new_id)
        report(os.path.splitext(os.path.basename(path))[0])
        ctrl.RemoveReplacement(orig); ctrl.FreeTargetResource(new_id)
    p("DONE")
    ctrl.Shutdown(); cap.Shutdown()
except Exception:
    p("ERROR", traceback.format_exc())
log.close()
os._exit(0)
