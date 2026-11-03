"""Find out why pdf.js refuses to render the first page."""
import glob
import os

from playwright.sync_api import sync_playwright

files = [p for p in sorted(glob.glob("docs/*.pdf"))
         if 0.3 <= os.path.getsize(p) / 1048576.0 <= 1.5][:1]

with sync_playwright() as pw:
    b = pw.chromium.launch(channel="msedge")
    pg = b.new_page()
    msgs = []
    pg.on("console", lambda m: msgs.append("%s: %s" % (m.type, m.text)))
    pg.on("pageerror", lambda e: msgs.append("pageerror: %s" % e))
    pg.goto("http://127.0.0.1:8090", wait_until="networkidle")

    out = pg.evaluate("""async () => {
      try {
        const r = await fetch('/api/v1/uploads', {method:'POST',
          headers:{'Content-Type':'application/json'},
          body: JSON.stringify({files:[{name:'x.pdf',size:1000}]})});
        const lib = window.pdfjsLib;
        const buf = new Uint8Array(1024);
        const doc = await lib.getDocument({data: buf}).promise;
        return {ok:true, pages: doc.numPages, ver: lib.version};
      } catch (e) {
        return {ok:false, err: String(e && e.message || e),
                ver: window.pdfjsLib && window.pdfjsLib.version};
      }
    }""")
    print("direct getDocument on garbage bytes:", out)

    pg.set_input_files("#picker", files)
    pg.wait_for_timeout(4000)
    html = pg.evaluate(
        "document.querySelector('.thumb') ? document.querySelector('.thumb').innerHTML.slice(0,120) : 'no .thumb'")
    print("thumb slot html:", html)
    print("console:", msgs[-6:])
    b.close()
