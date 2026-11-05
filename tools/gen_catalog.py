#!/usr/bin/env python3
"""Generate web/catalog.html, an asset browser for checking rendered screens by eye.

Reads gfx_screens.csv and the PNGs in gfx/. Plain HTML, no build step. It only works
next to the local gfx folder; the images are game assets and stay on this machine.

Usage: python tools/gen_catalog.py
Serve from the project root: python -m http.server 8000, then open /web/catalog.html
"""
import collections
import csv
import glob
import html
import json
import os
import re
import sys

PAGE = """<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Asset-Katalog</title>
<style>
:root { --bg:#16171b; --card:#23252b; --text:#e6e6ea; --muted:#9a9ca6; --line:#383b44; --warn:#e0a13a; --bad:#e8655a; --check:#3a3c44; --ok:#6cc08b; }
@media (prefers-color-scheme: light) { :root:not([data-theme="dark"]) { --bg:#f4f4f6; --card:#fff; --text:#1c1d21; --muted:#60636d; --line:#d9dae0; --warn:#a86a00; --bad:#bf3a2f; --check:#c9cad0; --ok:#1f7a45; } }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--text); font:14px/1.45 system-ui,sans-serif; }
header { position:sticky; top:0; background:var(--bg); padding:12px 16px; border-bottom:1px solid var(--line); z-index:2; }
h1 { margin:0 0 4px; font-size:18px; }
.note { margin:0 0 8px; color:var(--muted); font-size:12px; }
.controls { display:flex; flex-wrap:wrap; gap:8px; align-items:center; }
input, select { background:var(--card); color:var(--text); border:1px solid var(--line); border-radius:6px; padding:6px 8px; font:inherit; max-width:100%; }
#q { flex:1 1 220px; }
#count { color:var(--muted); }
main { padding:12px 16px 40px; }
h2 { font-size:14px; margin:20px 0 8px; color:var(--muted); font-weight:600; overflow-wrap:anywhere; }
.grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(180px,1fr)); gap:10px; }
figure { margin:0; background:var(--card); border:1px solid var(--line); border-radius:6px; overflow:hidden; }
figure a { display:block; background-color:var(--check); }
img { display:block; width:100%; height:auto; image-rendering:pixelated; }
figcaption { padding:6px 8px; font-size:12px; color:var(--muted); overflow-wrap:anywhere; }
.tag { display:inline-block; margin-left:4px; font-weight:600; }
.fallback { color:var(--warn); }
.ok { color:var(--ok); }
.range { color:var(--bad); }
</style>
</head>
<body>
<header>
<h1>Asset-Katalog</h1>
<p class="note">Identische Bilder erscheinen nur einmal, unter der ersten Sprache in der Reihenfolge FR, GE, IT, SP, UK. Suche daher ohne Sprachkürzel, z. B. <code>title</code>.</p>
<div class="controls">
<input id="q" type="search" placeholder="Filtern, z. B. mg_101 oder title" aria-label="Filter">
<select id="status" aria-label="Status">
<option value="">Alle</option>
<option value="descriptor">Paarung vom Spiel belegt</option>
<option value="probable">Tilemap: Paarung wahrscheinlich</option>
<option value="exact">Tilemap: Regel, Größe passt (nicht belegt)</option>
<option value="fallback">Tilemap: Regel, geraten</option>
<option value="rule">Sprite: Paarung nach Regel</option>
<option value="range">Werte außerhalb des Bereichs</option>
</select>
<span id="count"></span>
</div>
</header>
<main id="out"></main>
<script>
const items = __DATA__;
const out = document.getElementById("out"), q = document.getElementById("q"),
      status = document.getElementById("status"), count = document.getElementById("count");
function render() {
  const needle = q.value.toLowerCase(), want = status.value;
  const shown = items.filter(i => (!needle || i.name.toLowerCase().includes(needle))
    && (!want || (want === "range" ? i.bad > 0 : i.how === want)));
  const groups = new Map();
  for (const i of shown) { if (!groups.has(i.group)) groups.set(i.group, []); groups.get(i.group).push(i); }
  out.replaceChildren();
  for (const [group, members] of groups) {
    const h = document.createElement("h2"); h.textContent = group + " (" + members.length + ")"; out.append(h);
    const grid = document.createElement("div"); grid.className = "grid";
    for (const m of members) {
      const fig = document.createElement("figure"), a = document.createElement("a"),
            img = document.createElement("img"), cap = document.createElement("figcaption");
      a.href = "../" + m.png; img.src = "../" + m.png; img.loading = "lazy"; img.alt = m.name;
      img.width = m.w; img.height = m.h;
      cap.textContent = m.name + " · " + m.w + "×" + m.h + " · " + (m.bpp === "Sprite" ? "Sprite" : m.bpp + " Bit");
      if (m.how === "descriptor") { const t = document.createElement("span"); t.className = "tag ok"; t.textContent = "belegt"; cap.append(t); }
      if (m.how === "probable") { const t = document.createElement("span"); t.className = "tag ok"; t.textContent = "wahrscheinlich"; cap.append(t); }
      if (m.how === "fallback") { const t = document.createElement("span"); t.className = "tag fallback"; t.textContent = "geraten"; cap.append(t); }
      if (m.bad > 0) { const t = document.createElement("span"); t.className = "tag range"; t.textContent = "außerhalb"; cap.append(t); }
      a.append(img); fig.append(a, cap); grid.append(fig);
    }
    out.append(grid);
  }
  count.textContent = shown.length + " von " + items.length + " Bildern";
}
q.addEventListener("input", render); status.addEventListener("change", render); render();
</script>
</body>
</html>
"""


def group_of(name):
    return name.split("#")[0]


def main():
    items = []
    with open("gfx_screens.csv", newline="") as f:
        for row in csv.DictReader(f):
            if not row["png"]:
                continue
            items.append({
                "name": row["map"], "group": group_of(row["map"]), "png": row["png"],
                "w": int(row["width"]), "h": int(row["height"]), "bpp": row["bpp"],
                "how": {"record": "descriptor", "probable": "probable"}.get(row["source"], row["tiles_chosen"]),
                "bad": int(row["bad_tile"]) + int(row["bad_color"]),
            })
    known = {i["png"] for i in items}
    for path in sorted(glob.glob("gfx/screen_*.png")):  # bitmaps are not in the CSV
        png = path.replace(os.sep, "/")
        if png not in known:
            name = re.sub(r"^gfx/screen_|\.png$", "", png)
            items.append({"name": name, "group": "Vollbilder (16 Bit)", "png": png,
                          "w": 256, "h": 192, "bpp": "16", "how": "exact", "bad": 0})
    if os.path.exists("gfx_sprites.csv"):
        with open("gfx_sprites.csv", newline="") as f:
            for row in csv.DictReader(f):
                if not row["png"]:
                    continue
                items.append({
                    "name": row["frames"], "group": "Sprites: " + group_of(row["frames"]),
                    "png": row["png"], "w": int(row["width"]), "h": int(row["height"]),
                    "bpp": "Sprite", "how": "descriptor" if row["source"].startswith("descriptor") else "rule",
                    "bad": int(row["bad"]),
                })
    os.makedirs("web", exist_ok=True)
    page = PAGE.replace("__DATA__", json.dumps(items, ensure_ascii=False))
    with open("web/catalog.html", "w", encoding="utf-8") as f:
        f.write(page)
    missing = [i["png"] for i in items if not os.path.exists(i["png"])]
    print(f"catalog: {len(items)} images, missing files: {len(missing)} {missing[:5]}")
    print(collections.Counter(i["how"] for i in items))
    return 0


if __name__ == "__main__":
    sys.exit(main())
