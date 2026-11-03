"""P11-5 独立复核：Excel 数据透视表。

四路交叉，层层加码：

  1. zipfile + ElementTree —— 三个部件的 XML 到底长什么样，元素顺序是否符合
     CT_PivotTableDefinition 的 sequence（顺序错了 Excel 直接报"内容有问题"）；
     sheet → pivotTable → cacheDefinition → records 的关系链是否闭合，以及
     workbook 级的 <pivotCaches> 是否指到了同一个缓存部件。
  2. openpyxl 的透视表对象模型 —— 第三方实现能不能把这份定义完整解析出来
     （轴、字段下标、dataField 的 fld/subtotal、location 的 ref）。
  3. **独立重算** —— 从源表数据用纯 Python 重新聚合一遍，再按"行标签 / 列标签"
     去对目标区域的每个格子。这一步不看 DocCore 的中间结果，只信源数据和成品，
     算错了立刻暴露。
  4. 回环 —— 把成品喂回 `doccore xlsx --mode save`（另一条代码路径：读入 +
     透传写回），三个部件必须原样存活，openpyxl 再读一遍网格不能变。这一步专门
     盯"读入的透视表不 dirty 就该字节级透传"这条规则。
"""
import pathlib
import subprocess
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET

import openpyxl

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "out")
ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "Release" / "doccore.exe"

NS = {"m": "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
      "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships"}

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


def part_names(path):
    with zipfile.ZipFile(path) as z:
        return z.namelist()


XLSX = OUT / "p11_pivot.xlsx"
check(XLSX.exists(), "p11_pivot.xlsx missing")
if not XLSX.exists():
    print("p11_pivot.xlsx not found - run doccore_tests first")
    sys.exit(1)

names = part_names(XLSX)

# ---- 1. 部件与内容类型 ---------------------------------------------------
for part in ("xl/pivotTables/pivotTable1.xml",
             "xl/pivotCache/pivotCacheDefinition1.xml",
             "xl/pivotCache/pivotCacheRecords1.xml"):
    check(part in names, f"missing part {part}")
    check(names.count(part) == 1, f"duplicate part {part}")

ct = read_part(XLSX, "[Content_Types].xml")
for ctype in ("spreadsheetml.pivotTable+xml",
              "spreadsheetml.pivotCacheDefinition+xml",
              "spreadsheetml.pivotCacheRecords+xml"):
    check(ctype in ct, f"content type missing {ctype}")

# ---- 2. pivotTableDefinition 的结构与元素顺序 ----------------------------
table_xml = read_part(XLSX, "xl/pivotTables/pivotTable1.xml")
troot = ET.fromstring(table_xml)
check(troot.tag.endswith("}pivotTableDefinition"), "root is not pivotTableDefinition")
check(troot.get("name") == "PivotTable1", "pivot table name")
check(troot.get("cacheId") == "1", "cacheId")
check(troot.get("dataOnRows") == "0", "dataOnRows should be 0")

# CT_PivotTableDefinition 的 sequence：顺序错了 Excel 会判定文件损坏。
ORDER = ["location", "pivotFields", "rowFields", "colFields", "pageFields",
         "rowItems", "colItems", "dataFields", "pivotTableStyleInfo"]
present = [c.tag.split("}")[1] for c in troot]
present = [n for n in present if n in ORDER]
check(present == sorted(present, key=ORDER.index), f"element order wrong: {present}")

loc = troot.find("m:location", NS)
check(loc is not None, "location missing")
if loc is not None:
    check(loc.get("ref") == "A3:D7", f"location ref {loc.get('ref')} != A3:D7")
    check(loc.get("firstHeaderRow") == "1", "firstHeaderRow")
    check(loc.get("firstDataRow") == "1", "firstDataRow")
    check(loc.get("firstDataCol") == "1", "firstDataCol")

pfs = troot.find("m:pivotFields", NS)
check(pfs is not None and pfs.get("count") == "3", "pivotFields count")
axes = [f.get("axis") for f in (pfs if pfs is not None else [])]
check(axes[0] == "axisRow", f"field 0 axis {axes[0]}")
check(axes[1] == "axisCol", f"field 1 axis {axes[1]}")
check(axes[2] is None, f"measure field must have no axis, got {axes[2]}")
data_flag = [f.get("dataField") for f in (pfs if pfs is not None else [])]
check(data_flag[2] == "1", "measure field must carry dataField=1")
# 行/列字段要列出 item，否则 Excel 认为取值集合未知。
items0 = pfs[0].find("m:items", NS) if pfs is not None else None
check(items0 is not None and items0.get("count") == "2", "row field items")

rf = troot.find("m:rowFields", NS)
cf = troot.find("m:colFields", NS)
check(rf is not None and rf[0].get("x") == "0", "rowFields must point at field 0")
check(cf is not None and cf[0].get("x") == "1", "colFields must point at field 1")

df = troot.find("m:dataFields", NS)
check(df is not None and df.get("count") == "1", "dataFields count")
if df is not None:
    d0 = df[0]
    check(d0.get("fld") == "2", f"dataField fld {d0.get('fld')} != 2")
    check(d0.get("subtotal") == "sum", f"dataField subtotal {d0.get('subtotal')}")
    check(d0.get("name") == "求和项:Amount", f"dataField name {d0.get('name')}")

ri = troot.find("m:rowItems", NS)
ci = troot.find("m:colItems", NS)
check(ri is not None and ri.get("count") == "3", "rowItems count (2 + grand)")
check(ci is not None and ci.get("count") == "3", "colItems count (2 + grand)")
if ri is not None:
    check(ri[-1].get("t") == "grand", "last row item must be the grand total")

# ---- 3. 关系链 -----------------------------------------------------------
sheet_rels = read_part(XLSX, "xl/worksheets/_rels/sheet2.xml.rels")
check("relationships/pivotTable" in sheet_rels, "sheet -> pivotTable rel")
check('Target="../pivotTables/pivotTable1.xml"' in sheet_rels, "sheet rel target")

table_rels = read_part(XLSX, "xl/pivotTables/_rels/pivotTable1.xml.rels")
check("relationships/pivotCacheDefinition" in table_rels, "pivotTable -> cache rel")
check('Target="../pivotCache/pivotCacheDefinition1.xml"' in table_rels, "cache rel target")

cache_rels = read_part(XLSX, "xl/pivotCache/_rels/pivotCacheDefinition1.xml.rels")
check("relationships/pivotCacheRecords" in cache_rels, "cache -> records rel")

wb_xml = read_part(XLSX, "xl/workbook.xml")
wroot = ET.fromstring(wb_xml)
pc = wroot.find("m:pivotCaches", NS)
check(pc is not None, "workbook.xml has no <pivotCaches>")
if pc is not None:
    check(len(pc) == 1, f"pivotCaches entries {len(pc)}")
    entry = pc[0]
    check(entry.get("cacheId") == "1", f"pivotCaches cacheId {entry.get('cacheId')}")
    rid = entry.get("{http://schemas.openxmlformats.org/officeDocument/2006/relationships}id")
    check(rid is not None, "pivotCaches entry has no r:id")
    # 顺着 r:id 到 workbook.xml.rels 里找，必须指回同一个缓存部件。
    wb_rels_xml = read_part(XLSX, "xl/_rels/workbook.xml.rels")
    wrels = ET.fromstring(wb_rels_xml)
    target = None
    for rel in wrels:
        if rel.get("Id") == rid:
            target = rel.get("Target")
            check(rel.get("Type", "").endswith("/pivotCacheDefinition"),
                  f"workbook rel type {rel.get('Type')}")
    check(target == "pivotCache/pivotCacheDefinition1.xml",
          f"workbook rel target {target}")
    # <pivotCaches> 必须排在 <sheets> 之后（sequence 要求）。
    tags = [c.tag.split("}")[1] for c in wroot]
    check(tags.index("pivotCaches") > tags.index("sheets"), "pivotCaches before sheets")

# ---- 4. 缓存定义与记录 ---------------------------------------------------
cache_xml = read_part(XLSX, "xl/pivotCache/pivotCacheDefinition1.xml")
croot = ET.fromstring(cache_xml)
check(croot.get("recordCount") == "5", f"recordCount {croot.get('recordCount')}")
check(croot.get("refreshOnLoad") == "1", "refreshOnLoad must be 1")
src = croot.find("m:cacheSource/m:worksheetSource", NS)
check(src is not None, "worksheetSource missing")
if src is not None:
    check(src.get("ref") == "A1:C6", f"worksheetSource ref {src.get('ref')}")
    check(src.get("sheet") == "Data", f"worksheetSource sheet {src.get('sheet')}")

cfields = croot.find("m:cacheFields", NS)
check(cfields is not None and cfields.get("count") == "3", "cacheFields count")
shared = {}
if cfields is not None:
    for f in cfields:
        name = f.get("name")
        si = f.find("m:sharedItems", NS)
        vals = []
        if si is not None:
            for item in si:
                vals.append(item.get("v"))
        shared[name] = vals
check(shared.get("Region") == ["East", "West"], f"Region shared items {shared.get('Region')}")
check(shared.get("Product") == ["Gadget", "Widget"],
      f"Product shared items {shared.get('Product')}")
check(shared.get("Amount") == [], f"measure field must not have items: {shared.get('Amount')}")
# 纯数值字段要带 minValue/maxValue，Excel 用它决定轴的范围。
amount_si = None
for f in (cfields if cfields is not None else []):
    if f.get("name") == "Amount":
        amount_si = f.find("m:sharedItems", NS)
check(amount_si is not None and amount_si.get("minValue") == "3", "Amount minValue")
check(amount_si is not None and amount_si.get("maxValue") == "20", "Amount maxValue")

records_xml = read_part(XLSX, "xl/pivotCache/pivotCacheRecords1.xml")
rroot = ET.fromstring(records_xml)
check(rroot.get("count") == "5", f"records count {rroot.get('count')}")
recs = []
for r in rroot:
    vals = []
    for cell in r:
        tag = cell.tag.split("}")[1]
        if tag == "x":
            vals.append(("x", int(cell.get("v") or 0)))
        elif tag == "n":
            vals.append(("n", float(cell.get("v"))))
        else:
            vals.append((tag, cell.get("v")))
    recs.append(vals)
check(len(recs) == 5, f"record rows {len(recs)}")
# 每条记录的 x 下标必须落在对应字段的 sharedItems 范围内。
for rec in recs:
    for i, (kind, v) in enumerate(rec):
        if kind != "x":
            continue
        key = ["Region", "Product", "Amount"][i]
        check(0 <= v < max(1, len(shared.get(key, []))),
              f"record index {v} out of range for {key}")

# ---- 5. openpyxl 的对象模型 ---------------------------------------------
wbo = openpyxl.load_workbook(XLSX)
check(wbo.sheetnames == ["Data", "Pivot"], f"sheet names {wbo.sheetnames}")
pivots = wbo["Pivot"]._pivots
check(len(pivots) == 1, f"openpyxl sees {len(pivots)} pivot tables")
if pivots:
    p = pivots[0]
    check(p.name == "PivotTable1", f"openpyxl name {p.name}")
    check(p.cacheId == 1, f"openpyxl cacheId {p.cacheId}")
    check(p.dataOnRows is False, "openpyxl dataOnRows")
    check(p.location.ref == "A3:D7", f"openpyxl location {p.location.ref}")
    check(len(p.pivotFields) == 3, "openpyxl pivotFields")
    check(p.rowFields[0].x == 0, "openpyxl rowFields")
    check(p.colFields[0].x == 1, "openpyxl colFields")
    check(len(p.dataFields) == 1, "openpyxl dataFields")
    if p.dataFields:
        d = p.dataFields[0]
        check(d.fld == 2, f"openpyxl dataField fld {d.fld}")
        check(d.subtotal == "sum", f"openpyxl subtotal {d.subtotal}")
    check(p.pivotTableStyleInfo.name == "PivotStyleLight16", "openpyxl style")
    # 缓存
    c = p.cache
    check(c.recordCount == 5, f"openpyxl recordCount {c.recordCount}")
    check(c.cacheSource.worksheetSource.ref == "A1:C6", "openpyxl cacheSource ref")
    check(c.cacheSource.worksheetSource.sheet == "Data", "openpyxl cacheSource sheet")
    check(len(c.cacheFields) == 3, "openpyxl cacheFields")
    check(c.cacheFields[0].name == "Region", "openpyxl cacheField 0 name")
    # 记录能反推出源数据：把 x 下标翻成文本，与源表逐行比对（多重集）。
    rebuilt = []
    for r in c.records.r:
        row = []
        # Record 不是序列，取值要走 _fields。
        for i, cell in enumerate(r._fields):
            if cell.__class__.__name__ == "Index":
                si = c.cacheFields[i].sharedItems._fields
                row.append(si[cell.v].v)
            else:
                row.append(cell.v)
        rebuilt.append(tuple(row))
    expected_rows = [("East", "Widget", 10), ("East", "Gadget", 20),
                     ("West", "Widget", 5), ("West", "Gadget", 7),
                     ("East", "Widget", 3)]
    check(sorted(rebuilt, key=str) == sorted(expected_rows, key=str),
          f"cache records rebuild {sorted(rebuilt, key=str)}")

# ---- 6. 独立重算：只信源数据与成品网格 -----------------------------------
src_ws = wbo["Data"]
raw = []
for row in src_ws.iter_rows(min_row=2, max_row=6, max_col=3, values_only=True):
    if row[0] is None:
        continue
    raw.append((row[0], row[1], row[2]))
check(len(raw) == 5, f"source rows {len(raw)}")

cells = {}
for region, product, amount in raw:
    cells[(region, product)] = cells.get((region, product), 0) + amount
row_total = {}
for (region, product), v in cells.items():
    row_total[region] = row_total.get(region, 0) + v
col_total = {}
for (region, product), v in cells.items():
    col_total[product] = col_total.get(product, 0) + v
grand = sum(cells.values())
check(grand == 45, f"independent grand total {grand}")

pv = wbo["Pivot"]
# ref=A3:D7 → 第 3 行是字段名，第 4 行是列标签，第 5..7 行是数据 + 总计。
col_labels = [pv.cell(row=4, column=c).value for c in range(2, 5)]
check(col_labels == ["Gadget", "Widget", "总计"], f"column labels {col_labels}")

seen_pairs = 0
for r in range(5, 8):
    label = pv.cell(row=r, column=1).value
    if label is None or label == "总计":
        continue
    for c, clabel in zip(range(2, 5), col_labels):
        got = pv.cell(row=r, column=c).value
        if clabel == "总计":
            want = row_total.get(label, 0)
        else:
            want = cells.get((label, clabel), 0)
            seen_pairs += 1
        check(got == want, f"cell ({label},{clabel}) = {got}, want {want}")
check(seen_pairs == 4, f"cross-checked {seen_pairs} label/value pairs, want 4")

# 总计行
check(pv.cell(row=7, column=1).value == "总计", "grand total row label")
for c, clabel in zip(range(2, 5), col_labels):
    got = pv.cell(row=7, column=c).value
    want = grand if clabel == "总计" else col_total.get(clabel, 0)
    check(got == want, f"grand row ({clabel}) = {got}, want {want}")

# ---- 7. 回环：读入 + 透传写回 -------------------------------------------
if EXE.exists():
    with tempfile.TemporaryDirectory() as tmp:
        rt = pathlib.Path(tmp) / "rt.xlsx"
        proc = subprocess.run([str(EXE), "xlsx", str(XLSX), "--mode", "save",
                               "--out", str(rt)],
                              capture_output=True, text=True)
        check(proc.returncode == 0, f"round trip failed: {proc.stderr[:200]}")
        if rt.exists():
            rt_names = part_names(rt)
            for part in ("xl/pivotTables/pivotTable1.xml",
                         "xl/pivotCache/pivotCacheDefinition1.xml",
                         "xl/pivotCache/pivotCacheRecords1.xml"):
                check(part in rt_names, f"round trip lost {part}")
                # 未刷新就该字节级透传。
                check(read_part(rt, part) == read_part(XLSX, part),
                      f"round trip changed {part}")
            check("<pivotCaches>" in read_part(rt, "xl/workbook.xml"),
                  "round trip lost workbook pivotCaches")
            wb2 = openpyxl.load_workbook(rt)
            pv2 = wb2["Pivot"]
            same = all(pv2.cell(row=r, column=c).value == pv.cell(row=r, column=c).value
                       for r in range(3, 8) for c in range(1, 5))
            check(same, "round trip changed the pivot grid")
            check(len(wb2["Pivot"]._pivots) == 1, "round trip lost the pivot table")
else:
    fails.append(f"doccore.exe not found at {EXE}")

print(f"{checks} checks, {len(fails)} failures")
for f in fails:
    print("  FAIL", f)
sys.exit(1 if fails else 0)
