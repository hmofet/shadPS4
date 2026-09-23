import os, renderdoc as rd
out = open(os.path.join(os.environ["RD_OUT"], "targets.txt"), "w")
cap = rd.OpenCaptureFile(); cap.OpenFile(os.environ["RD_CAP"], "", None)
r, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
out.write("disasm targets: %s\n" % list(ctrl.GetDisassemblyTargets(True)))
out.write("target encodings: %s\n" % list(ctrl.GetTargetShaderEncodings()))
out.close(); ctrl.Shutdown(); cap.Shutdown(); os._exit(0)
