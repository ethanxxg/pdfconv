/* One nav for every page.
 *
 * The tool list lives in service/tools.py; this script is the only place the
 * browser learns it.  Hardcoding the links per page is how a new tool ends up
 * reachable from the grid but invisible from the page you are already on.
 */
(async () => {
  const el = document.getElementById("nav");
  if (!el) return;
  const here = (location.pathname.replace(/\/+$/, "") || "/");
  try {
    const r = await fetch("/api/v1/tools");
    if (!r.ok) return;
    const { tools } = await r.json();
    const links = tools.map(t =>
      `<a href="${t.path}"${t.path === here ? ' class="on"' : ""}>${t.label}</a>`);
    links.push(`<a href="/tools"${here === "/tools" ? ' class="on"' : ""}>全部工具</a>`);
    el.innerHTML = links.join("");
  } catch (e) {
    /* A nav that fails to load must not take the page down with it. */
  }
})();
