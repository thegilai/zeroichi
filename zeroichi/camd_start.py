"""Starts the camera server (camd.py) from its ready-compiled copy in
/var/cache/zeroichi/lib when there is one (made when Zero Ichi updates itself -
zeroichi.py's prepare_module_cache): Python then doesn't have to compile
camd.py first at every start, which takes a moment on a Pi Zero - and the
camera server is what the start-up waits for. Run like camd.py ("--listen")."""
import sys

sys.path.insert(0, "/var/cache/zeroichi/lib")
try:
    import camd
except ImportError:                              # (no usable copy: from the source)
    del sys.path[0]
    sys.modules.pop("camd", None)
    import camd
camd.run()
