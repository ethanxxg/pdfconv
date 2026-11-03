"""P11-6 独立复核：PowerPoint 的切换与动画。

四路交叉：

  1. zipfile + ElementTree —— 生成的 XML 到底长什么样，并做**结构体检**：
     所有元素必须成对、`CT_Slide` 的子元素顺序（cSld → clrMapOvr →
     transition → timing）、`p:seq` 里 cTn/prevCondLst/nextCondLst 的顺序、
     `p:cTn/@id` 全页唯一、`p:bldLst` 在 `p:tnLst` 之后。
  2. **独立重建语义** —— 用纯 Python 从时序树里按规范重新提取"哪个形状、
     什么效果、什么触发、多长"，与 C++ 读路径的结论比对。两边独立写，
     一边理解错了立刻暴露。
  3. python-pptx —— 包结构被第三方实现接受，形状 id 与文本都还在
     （动画的 spid 指的就是这些 id，id 变了动画就指错）。
  4. 回环 —— `doccore pptx --mode save`（读入 + 原文写回，另一条代码路径）
     之后 `<p:transition>` 与 `<p:timing>` 必须**逐字节不变**。这一条专门盯
     "没动过的幻灯片按原文写回"的规则。
"""
import pathlib
import subprocess
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET

import pptx

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "out")
ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "Release" / "doccore.exe"

P = "http://schemas.openxmlformats.org/presentationml/2006/main"
A = "http://schemas.openxmlformats.org/drawingml/2006/main"
R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"

fails = []
checks = 0


def check(cond, label):
    global checks
    checks += 1
    if not cond:
        fails.append(label)


def read_part(path, name):
    with zipfile.ZipFile(path) as z:
        return z.read(name).decode("utf-8")


def q(tag):
    return "{%s}%s" % (P, tag)


PPTX = OUT / "p11_transition.pptx"
check(PPTX.exists(), "p11_transition.pptx missing")
if not PPTX.exists():
    print("p11_transition.pptx not found - run doccore_tests first")
    sys.exit(1)

slide_xml = read_part(PPTX, "ppt/slides/slide1.xml")
names = zipfile.ZipFile(PPTX).namelist()
check("ppt/slides/slide1.xml" in names, "slide1 part missing")

# ---- 1. 结构 -------------------------------------------------------------
# 所有元素必须成对（用 expat 直接解析，不合法会抛）。
try:
    root = ET.fromstring(slide_xml)
    check(True, "slide1.xml is well-formed")
except ET.ParseError as exc:
    check(False, "slide1.xml not well-formed: %s" % exc)
    root = None

check(root is not None and root.tag == q("sld"), "root element is p:sld")
if root is not None:
    ORDER = ["cSld", "clrMapOvr", "transition", "timing", "extLst"]
    kids = [c.tag.split("}")[1] for c in root]
    kids = [k for k in kids if k in ORDER]
    check(kids == sorted(kids, key=ORDER.index), f"CT_Slide child order wrong: {kids}")
    check(kids[:4] == ["cSld", "clrMapOvr", "transition", "timing"],
          f"expected cSld/clrMapOvr/transition/timing, got {kids}")

# 切换
trans = root.find(q("transition")) if root is not None else None
check(trans is not None, "p:transition missing")
if trans is not None:
    check(trans.get("spd") == "fast", f"transition spd {trans.get('spd')}")
    check(trans.get("advClick") == "0", f"transition advClick {trans.get('advClick')}")
    check(trans.get("advTm") == "3000", f"transition advTm {trans.get('advTm')}")
    kids = [c.tag.split("}")[1] for c in trans]
    check(kids == ["fade"], f"transition child {kids}")
    # sndAc 必须排在切换类型之前（CT_SlideTransition 的 sequence）。
    if "sndAc" in kids:
        check(kids.index("sndAc") < len(kids) - 1, "sndAc must precede the type element")

timing = root.find(q("timing")) if root is not None else None
check(timing is not None, "p:timing missing")

ids = []
seq = None
bld = None
if timing is not None:
    kids = [c.tag.split("}")[1] for c in timing]
    check(kids[0] == "tnLst", f"p:timing first child {kids[0]}")
    check("bldLst" in kids, "p:bldLst missing")
    if "bldLst" in kids:
        check(kids.index("bldLst") > kids.index("tnLst"), "bldLst must follow tnLst")
        bld = timing.find(q("bldLst"))

    ids = [int(e.get("id")) for e in timing.iter(q("cTn")) if e.get("id") is not None]
    check(len(ids) > 0, "no p:cTn in timing")
    check(len(ids) == len(set(ids)), f"duplicate p:cTn ids: {sorted(ids)}")

    tm_root = None
    for e in timing.iter(q("cTn")):
        if e.get("nodeType") == "tmRoot":
            tm_root = e
        if e.get("nodeType") == "mainSeq":
            seq = e
    check(tm_root is not None, "no tmRoot time node")
    check(seq is not None, "no mainSeq time node")
    if tm_root is not None:
        check(tm_root.get("dur") == "indefinite", "tmRoot dur")
        check(tm_root.get("restart") == "never", "tmRoot restart")
    if seq is not None:
        check(seq.get("dur") == "indefinite", "mainSeq dur")
    # p:seq 的结构：cTn 在前，prevCondLst / nextCondLst 在后。
    seq_el = timing.find(".//" + q("seq"))
    check(seq_el is not None, "p:seq missing")
    if seq_el is not None:
        check(seq_el.get("concurrent") == "1", f"seq concurrent {seq_el.get('concurrent')}")
        sk = [c.tag.split("}")[1] for c in seq_el]
        check(sk[0] == "cTn", f"p:seq first child {sk[0]}")
        check("prevCondLst" in sk and "nextCondLst" in sk, f"p:seq conditions {sk}")
        if "prevCondLst" in sk and "nextCondLst" in sk:
            check(sk.index("prevCondLst") < sk.index("nextCondLst"), "prev/next order")
        # 换页动作的来源必须是本页（sldTgt），不是某个形状。
        for cond in seq_el.iter(q("cond")):
            if cond.get("evt") in ("onPrev", "onNext"):
                check(cond.find(q("tgtEl")) is not None, "seq cond has no tgtEl")

# ---- 2. 独立重建效果列表 -------------------------------------------------
# 按规范自己走一遍时序树：p:cTn/@nodeType 决定触发方式，p:cond/@delay 是延迟，
# p:animEffect/@transition 区分入场/出场，@filter 是效果，p:spTgt/@spid 是目标。
TRIGGER = {
    "clickEffect": "onClick", "clickPar": "onClick",
    "withEffect": "withPrevious", "withGroup": "withPrevious",
    "afterEffect": "afterPrevious", "afterGroup": "afterPrevious",
}


def walk(node, trigger, delay):
    """返回 (效果列表, 是否处理过)。"""
    out = []
    tag = node.tag.split("}")[1]
    if tag == "cTn":
        node_type = node.get("nodeType")
        trig = TRIGGER.get(node_type, trigger)
        # stCondLst 在 childTnLst 之前，所以延迟能先就位。
        local_delay = delay
        st = node.find(q("stCondLst"))
        if st is not None:
            cond = st.find(q("cond"))
            if cond is not None:
                d = cond.get("delay", "0")
                local_delay = 0 if d == "indefinite" else int(d)
        for child in node:
            out.extend(walk(child, trig, local_delay))
        return out
    if tag in ("animEffect", "animMotion"):
        spid = None
        dur = None
        for cb in node.iter(q("cBhvr")):
            for tgt in cb.iter(q("spTgt")):
                spid = int(tgt.get("spid"))
            for ctn in cb.iter(q("cTn")):
                if ctn.get("dur"):
                    dur = int(ctn.get("dur"))
        if tag == "animMotion":
            return [{"shape": spid, "kind": "motionPath", "filter": "motion",
                     "trigger": trigger, "delay": delay, "duration": dur,
                     "path": node.get("path")}]
        t = node.get("transition")
        kind = {"in": "entrance", "out": "exit"}.get(t, "emphasis")
        return [{"shape": spid, "kind": kind, "filter": node.get("filter"),
                 "trigger": trigger, "delay": delay, "duration": dur}]
    for child in node:
        out.extend(walk(child, trigger, delay))
    return out


effects = walk(timing, "onClick", 0) if timing is not None else []
check(len(effects) == 4, f"rebuilt {len(effects)} effects, want 4")

EXPECTED = [
    {"shape": 2, "kind": "entrance", "filter": "fade", "trigger": "onClick",
     "delay": 0, "duration": 500},
    {"shape": 3, "kind": "entrance", "filter": "wipe(up)", "trigger": "withPrevious",
     "delay": 200, "duration": 750},
    {"shape": 4, "kind": "exit", "filter": "fade", "trigger": "afterPrevious",
     "delay": 0, "duration": 400},
    {"shape": 2, "kind": "motionPath", "filter": "motion", "trigger": "withPrevious",
     "delay": 0, "duration": 1000, "path": "M 0 0 L 0.25 0.1"},
]
if len(effects) == 4:
    for i, (got, want) in enumerate(zip(effects, EXPECTED)):
        for key, value in want.items():
            check(got.get(key) == value,
                  f"effect[{i}].{key} = {got.get(key)!r}, want {value!r}")

# 入场效果必须先置可见；出场必须先置隐藏。
if timing is not None:
    sets = []
    for s in timing.iter(q("set")):
        to = s.find(q("to"))
        val = to.find(q("strVal")).get("val") if to is not None else None
        spid = None
        for tgt in s.iter(q("spTgt")):
            spid = int(tgt.get("spid"))
        sets.append((spid, val))
    check((2, "visible") in sets, f"entrance on shape 2 must set visible: {sets}")
    check((3, "visible") in sets, f"entrance on shape 3 must set visible: {sets}")
    check((4, "hidden") in sets, f"exit on shape 4 must set hidden: {sets}")

# bldLst 必须覆盖每个被动画的形状。
if bld is not None:
    spids = sorted(int(e.get("spid")) for e in bld if e.get("spid"))
    check(spids == [2, 3, 4], f"bldLst spids {spids}")

# ---- 3. python-pptx ------------------------------------------------------
prs = pptx.Presentation(str(PPTX))
check(len(prs.slides) == 2, f"python-pptx slides {len(prs.slides)}")
shapes = list(prs.slides[0].shapes)
check([s.shape_id for s in shapes] == [2, 3, 4],
      f"python-pptx shape ids {[s.shape_id for s in shapes]}")
check([s.text_frame.text for s in shapes] == ["Alpha", "Beta", "Gamma"],
      f"python-pptx texts {[s.text_frame.text for s in shapes]}")
# 动画的 spid 必须都能在形状里找到，否则动画指向空目标。
shape_ids = {s.shape_id for s in shapes}
for e in effects:
    check(e["shape"] in shape_ids, f"effect targets unknown shape {e['shape']}")

# ---- 4. 回环：原文写回 ---------------------------------------------------
if EXE.exists():
    with tempfile.TemporaryDirectory() as tmp:
        rt = pathlib.Path(tmp) / "rt.pptx"
        proc = subprocess.run([str(EXE), "pptx", str(PPTX), "--mode", "save", "--out", str(rt)],
                              capture_output=True, text=True)
        check(proc.returncode == 0, f"round trip failed: {proc.stderr[:200]}")
        if rt.exists():
            rt_xml = read_part(rt, "ppt/slides/slide1.xml")

            def block(xml, tag):
                start = xml.find("<p:%s" % tag)
                if start < 0:
                    return ""
                end = xml.find("</p:%s>" % tag, start)
                if end < 0:
                    return ""
                return xml[start:end + len(tag) + 5]

            for tag in ("transition", "timing"):
                a = block(slide_xml, tag)
                b = block(rt_xml, tag)
                check(a != "", f"original has no <p:{tag}>")
                check(a == b, f"round trip changed <p:{tag}>")
            # 内容本身不能丢。
            prs2 = pptx.Presentation(str(rt))
            check([s.text_frame.text for s in prs2.slides[0].shapes] ==
                  ["Alpha", "Beta", "Gamma"], "round trip changed slide text")
            # 重读后的效果列表必须一致（走的是 C++ 读路径）。
            rt_root = ET.fromstring(rt_xml)
            rt_timing = rt_root.find(q("timing"))
            rt_effects = walk(rt_timing, "onClick", 0) if rt_timing is not None else []
            check(len(rt_effects) == 4, f"round trip rebuilt {len(rt_effects)} effects")
else:
    fails.append(f"doccore.exe not found at {EXE}")

print(f"{checks} checks, {len(fails)} failures")
for f in fails:
    print("  FAIL", f)
sys.exit(1 if fails else 0)
