#!/usr/bin/env python3
"""Generate MicroPython's build-time headers for the UnoDOS PYRT port.

MicroPython's real build drives QSTR / module / version header generation from
py/mkrules.mk + py/py.mk.  UnoDOS builds from build.sh, not make, so this script
replays those exact steps (see py/py.mk:227-257 and py/mkrules.mk:116-161) into a
build/genhdr directory the compile step then -I's.

  mkupy.py --top pc64/upy --port pc64/upy_port --build BUILD \\
           --cpp "x86_64-w64-mingw32-gcc -E" \\
           --cflags "<the compile CFLAGS>" -- <source.c ...>

Produces BUILD/genhdr/{mpversion.h, qstrdefs.generated.h, moduledefs.h,
root_pointers.h, compressed.data.h}.
"""
import argparse, os, subprocess, sys, shlex

def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        sys.exit("mkupy: command failed (%d): %s" % (r.returncode, " ".join(cmd[:6]) + " ..."))
    return r

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", required=True)     # vendored MicroPython (has py/)
    ap.add_argument("--port", required=True)    # upy_port (mpconfigport.h, qstrdefsport.h)
    ap.add_argument("--build", required=True)
    ap.add_argument("--cpp", required=True)      # preprocessor command
    ap.add_argument("--cflags", required=True)   # the compile CFLAGS (string)
    ap.add_argument("sources", nargs="+")
    a = ap.parse_args()

    top   = os.path.abspath(a.top)
    port  = os.path.abspath(a.port)
    build = os.path.abspath(a.build)
    gen = os.path.join(build, "genhdr")
    os.makedirs(gen, exist_ok=True)
    py_src = os.path.join(top, "py")
    PY = sys.executable
    cpp = shlex.split(a.cpp)
    cflags = shlex.split(a.cflags)
    # QSTR-scan flags = the compile flags + -DNO_QSTR (py/mkrules.mk:117)
    qcflags = cflags + ["-DNO_QSTR"]

    def mqd(*args):   # makeqstrdefs.py
        run([PY, os.path.join(py_src, "makeqstrdefs.py")] + list(args))

    # 1. version header (py/py.mk:227) — no .git in the vendored tree, so seed
    #    a version file makeversionhdr.py falls back to.
    open(os.path.join(top, "MICROPY_VERSION"), "w").write("v1.24.1\n") \
        if not os.path.exists(os.path.join(top, ".git")) else None
    run([PY, os.path.join(py_src, "makeversionhdr.py"), os.path.join(gen, "mpversion.h")],
        cwd=top)

    # 2. qstr.i.last — preprocess every source hunting QSTR/MP_REGISTER_* uses
    #    (py/mkrules.mk:122). "pp" mode: pp <cpp...> output <f> cflags <...>
    #    cxxflags <...> sources <...> dependencies <...> changed_sources <...>
    qi = os.path.join(gen, "qstr.i.last")
    mqd("pp", *cpp, "output", qi,
        "cflags", *qcflags, "cxxflags", *qcflags,
        "sources", *a.sources,
        "dependencies",
        "changed_sources", *a.sources)

    # 3. split + cat for qstr / module / root_pointer / compress (mkrules.mk:124-161)
    for kind, coll in (("qstr", "qstrdefs.collected.h"),
                       ("module", "moduledefs.collected"),
                       ("root_pointer", "root_pointers.collected"),
                       ("compress", "compressed.collected")):
        d = os.path.join(gen, kind)
        mqd("split", kind, qi, d, "_")
        mqd("cat", kind, "_", d, os.path.join(gen, coll))

    # 4. qstrdefs.preprocessed.h then qstrdefs.generated.h (py/py.mk:240-243)
    #    cat qstrdefs.h qstrdefsport.h collected | sed wrap | cpp | sed unwrap
    parts = [os.path.join(py_src, "qstrdefs.h"),
             os.path.join(port, "qstrdefsport.h"),
             os.path.join(gen, "qstrdefs.collected.h")]
    cat = b"".join(open(p, "rb").read() for p in parts)
    # sed 's/^Q(.*)/"&"/' : quote each Q(...) line so the preprocessor leaves it
    wrapped = b"".join(
        (b'"' + ln + b'"\n') if ln.startswith(b"Q(") else (ln + b"\n")
        for ln in cat.replace(b"\r\n", b"\n").split(b"\n"))
    pp = subprocess.run(cpp + cflags + ["-I", port, "-I", top, "-I", gen, "-"],
                        input=wrapped, stdout=subprocess.PIPE)
    if pp.returncode != 0:
        sys.exit("mkupy: qstr preprocess failed")
    # sed 's/^"\(Q(.*)\)"/\1/' : unwrap
    out = []
    for ln in pp.stdout.replace(b"\r\n", b"\n").split(b"\n"):
        s = ln.strip()
        if s.startswith(b'"Q(') and s.endswith(b')"'):
            out.append(s[1:-1])
        else:
            out.append(ln)
    prep = os.path.join(gen, "qstrdefs.preprocessed.h")
    open(prep, "wb").write(b"\n".join(out))
    with open(os.path.join(gen, "qstrdefs.generated.h"), "wb") as f:
        run([PY, os.path.join(py_src, "makeqstrdata.py"), prep], stdout=f)

    # 5. moduledefs.h, root_pointers.h, compressed.data.h (py/py.mk:250-257,245-247)
    with open(os.path.join(gen, "moduledefs.h"), "wb") as f:
        run([PY, os.path.join(py_src, "makemoduledefs.py"),
             os.path.join(gen, "moduledefs.collected")], stdout=f)
    with open(os.path.join(gen, "root_pointers.h"), "wb") as f:
        run([PY, os.path.join(py_src, "make_root_pointers.py"),
             os.path.join(gen, "root_pointers.collected")], stdout=f)
    with open(os.path.join(gen, "compressed.data.h"), "wb") as f:
        run([PY, os.path.join(py_src, "makecompresseddata.py"),
             os.path.join(gen, "compressed.collected")], stdout=f)

    print("mkupy: genhdr ready in", gen)

if __name__ == "__main__":
    main()
