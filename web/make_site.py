#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Code branched off of Felucca - Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Make the Argo site (GitHub Pages):

  index.html                  the landing page: what Argo is and the ways in (install, editor, manual, download, source)
  manual/index.html           README.md as a page
  firmware/argo-VER.fwsc       the package (+ LICENSE, LICENSING.md, LICENSES/: the package holds
                              JieLi SDK files under Apache-2.0, see LICENSING.md)
  webapp/installer/index.html index_pkg.html, self-contained (fm1pkg.js, fm1ota.js, metadata inlined)
  webapp/editor/index.html    editor.html (+ fukiai.ttf, FUKIAI-LICENSE.txt)
  src/                        not touched

Every page of the site (the landing page, the manual and the installer) wears the same header (dress()). The editor
is an app with a header of its own and is left as it is.

  web/make_site.py build/argo-X.Y.fwsc X.Y OUT_DIR

The package must be one made by tools/fm1pkg_make.py (Argo's own loader, from Felucca: no vendor files).
Its identity (FM-1_050) is read from the package; the device must report it after
the install.
"""
import html
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F
REPO = "https://github.com/echoe/argo_fm1"
EMU = "https://joakimeriksson.github.io/fm1-emu/?fw=local&mhz=100"   # (the web emulator the README points to)


def strip_module(src):
    src = re.sub(r"^export\s+", "", src, flags=re.M)
    return re.sub(r"^import .*?;\n", "", src, flags=re.M)


def product_of(raw):
    """the package identity: one marker byte after each of the first 20 blocks (fm1pkg.js productOf)"""
    return "".join(chr((m - i - 1) & 0xFF) for i in range(BLOCKS) if (m := raw[i * BLK + KEEP]) != 0x7D)

# --- the site's pages (after fm1-loom's tools/make_pages.py: landing page, manual, a header on every page) ----------

# the site's header, in the colours the installer and the editor already use (--bg, --fg, --dim, --line)
SITE_CSS = """<style>
.site-top { max-width: 64rem; margin: 0 auto; display: flex; flex-wrap: wrap; align-items: center; gap: .3rem 1.4rem;
  padding: 12px 16px; border-bottom: 1px solid var(--line, #888); font: 600 15px/1.4 -apple-system, BlinkMacSystemFont, "Helvetica Neue", Arial, sans-serif; }
.site-top .brand { color: var(--fg); text-decoration: none; font-size: 20px; font-weight: 700; letter-spacing: .02em; }
.site-top nav { display: flex; flex-wrap: wrap; gap: .2rem 1.1rem; margin-left: auto; }
.site-top nav a { color: var(--dim); text-decoration: none; padding: 2px 0; border-bottom: 2px solid transparent; }
.site-top nav a:hover, .site-top nav a:focus-visible { color: var(--fg); }
.site-top nav a[aria-current="page"] { color: var(--fg); border-bottom-color: var(--fg); }
@media (max-width: 520px) { .site-top nav { margin-left: 0; width: 100%; } }
</style>"""

SECTIONS = [("webapp/installer/", "Install"), ("webapp/editor/", "Editor"), ("manual/", "Manual")]
ELSEWHERE = [(EMU, "Emulator"), (REPO, "Source")]


def dress(page, here, depth):
    """a page of the site with the site's header; here: its section ("" the front page), depth: how many folders
    down it is (so that the header's links are relative, as the installer's own are)"""
    up = "../" * depth
    links = "".join(f'<a href="{up}{d}"' + (' aria-current="page"' if d == here else "") + f">{t}</a>" for d, t in SECTIONS)
    links += "".join(f'<a href="{html.escape(u)}">{t}</a>' for u, t in ELSEWHERE)
    top = (f'<header class="site-top"><a class="brand" href="{up or "./"}"' + (' aria-current="page"' if not here else "") +
           f'>Argo</a><nav aria-label="Argo">{links}</nav></header>')
    if page.count("</head>") != 1 or not re.search(r"<body[^>]*>", page):
        raise SystemExit(f"dress: {here or 'index'}: no head or body")
    page = page.replace("</head>", SITE_CSS + "</head>")
    return re.sub(r"(<body[^>]*>)", lambda m: m.group(1) + top, page, count=1)


def shell(title, description, body, css=""):
    """a whole page (the colours and type of web/index_pkg.html), for the landing page and the manual"""
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<meta name="description" content="{html.escape(description, quote=True)}">
<style>
  :root {{ --bg: #fff; --fg: #000; --dim: #777; --line: #ccc; color-scheme: light; }}
  @media (prefers-color-scheme: dark) {{
    :root:not([data-theme="light"]) {{ --bg: #000; --fg: #fff; --dim: #888; --line: #444; color-scheme: dark; }}
  }}
  :root[data-theme="dark"] {{ --bg: #000; --fg: #fff; --dim: #888; --line: #444; color-scheme: dark; }}
  body {{ margin: 0; background: var(--bg); color: var(--fg);
         font: 16px/1.6 -apple-system, BlinkMacSystemFont, "Helvetica Neue", Arial, sans-serif; }}
  main {{ max-width: 42rem; margin: 0 auto; padding: 40px 16px 64px; }}
  h1 {{ font-size: 32px; line-height: 1.1; margin: 0 0 16px; }}
  h2 {{ font-size: 20px; margin: 2rem 0 .5rem; padding-bottom: 2px; border-bottom: 1px solid var(--line); }}
  h3 {{ font-size: 16px; margin: 1.4rem 0 .3rem; }}
  a {{ color: inherit; }}
  code {{ font-size: .9em; }}
  pre {{ overflow-x: auto; padding: 8px 12px; border: 1px solid var(--line); }}
  .small {{ color: var(--dim); font-size: 14px; }}
{css}</style>
</head>
<body>
<main>
{body}
</main>
</body>
</html>
"""


LANDING_CSS = """  .lede { font-size: 18px; margin: 0 0 24px; }
  .ways { display: grid; gap: 12px; margin: 0 0 24px; }
  .ways a { display: block; text-decoration: none; border: 1px solid var(--line); padding: 12px 16px; }
  .ways a:hover, .ways a:focus-visible { border-color: var(--fg); outline: none; }
  .ways strong { display: block; font-size: 18px; }
  .ways span { color: var(--dim); font-size: 14px; }
"""

LANDING = """<h1>Argo</h1>
<p class="lede">Argo is a multi-engine synthesizer firmware for the M-VAVE FM-1, built on Felucca. It adds eight
sequences per track that you can swap between live (and chain), a preset swap on any step, a generative arpeggiator,
playable chord modes and more synth engines. It takes the sampling engines out.</p>
<nav class="ways" aria-label="Get Argo">
<a href="webapp/installer/"><strong>Install</strong><span>Connect your FM-1 by USB and install from Chrome or Edge. There is nothing to install on your computer.</span></a>
<a href="webapp/editor/"><strong>Editor</strong><span>Edit patches and projects, and back up the whole FM-1, from the browser.</span></a>
<a href="manual/"><strong>Manual</strong><span>The README: what is different from Felucca, and how to use it.</span></a>
<a href="__EMU__"><strong>Try it in an emulator</strong><span>Joakim Eriksson's FM-1 emulator: load the package you downloaded into it.</span></a>
<a href="firmware/__PKG__"><strong>Download __PKG__</strong><span>For the command-line installer: <code>python3 tools/fm1_install.py __PKG__</code></span></a>
<a href="__REPO__"><strong>Source</strong><span>On GitHub under GPL-3.0-only.</span></a>
</nav>
<p class="small"><b>Version __VERSION__.</b> Installing firmware is at your own risk. The installer can put M-VAVE's own
firmware back (&ldquo;Return to official V15&rdquo;). Argo is a branch of
<a href="https://github.com/hugelton/Felucca">Felucca</a> by Leo Kuroshita (H&uuml;gelton Instruments). M-VAVE and FM-1
are trademarks of their owners; Argo is not affiliated with or endorsed by them.</p>"""


def _href(url, blob="blob"):
    """a link of the README that points into the repository, as a link into it on GitHub"""
    return url if re.match(r"(?:[a-z][a-z0-9+.-]*:|#|/)", url, re.I) else f"{REPO}/{blob}/main/{url}"


def _inline(s):
    """the README's inline markdown: `code`, ![image](path), [link](url), **bold**, *italic*"""
    s = html.escape(s, quote=False)
    codes = []
    s = re.sub(r"`([^`]+)`", lambda m: (codes.append(f"<code>{m[1]}</code>"), f"\0{len(codes) - 1}\0")[1], s)
    s = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)", lambda m: f'<img src="{_href(m[2], "raw")}" alt="{m[1]}" style="max-width:100%">', s)
    s = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", lambda m: f'<a href="{_href(m[2])}">{m[1]}</a>', s)
    s = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", s)
    s = re.sub(r"(?<![\w*])\*(?![\s*])(.+?)(?<![\s*])\*(?![\w*])", r"<em>\1</em>", s)
    return re.sub(r"\0(\d+)\0", lambda m: codes[int(m[1])], s)


def markdown(text):
    """README.md as HTML: headings, paragraphs, nested lists, fenced code and the inline forms above (all of it that
    the README uses; not a general Markdown)"""
    out, para, lists, fence = [], [], [], None

    def flush():
        if para:
            out.append("<p>" + _inline(" ".join(para)) + "</p>")
            para.clear()

    def close_lists():
        while lists:
            out.append("</li></ul>")
            lists.pop()

    for line in text.splitlines():
        if fence is not None:
            if line.startswith("```"):
                out.append("<pre><code>" + html.escape("\n".join(fence)) + "</code></pre>")
                fence = None
            else:
                fence.append(line)
            continue
        if line.startswith("```"):
            flush()
            close_lists()
            fence = []
            continue
        if not line.strip():
            flush()
            continue
        head = re.match(r"(#{1,6})\s+(.*?)\s*#*$", line)
        item = re.match(r"(\s*)(?:[-*+]|\d+\.)\s+(.*)", line)
        if head:
            flush()
            close_lists()
            n, title = len(head[1]), head[2]
            slug = re.sub(r"[^a-z0-9 -]", "", re.sub(r"`", "", title.lower())).strip().replace(" ", "-")
            out.append(f'<h{n} id="{slug}">{_inline(title)}</h{n}>')
        elif item:
            flush()
            indent = len(item[1].expandtabs(4))
            while lists and indent < lists[-1]:
                out.append("</li></ul>")
                lists.pop()
            if lists and indent == lists[-1]:
                out.append("</li>")
            else:
                out.append("<ul>")
                lists.append(indent)
            out.append("<li>" + _inline(item[2]))
        elif lists and line[0] in " \t":
            out.append(" " + _inline(line.strip()))       # a list item's next line
        else:
            close_lists()
            para.append(line.strip())
    flush()
    close_lists()
    if fence is not None:
        raise SystemExit("README.md: a code block is not closed")
    return "\n".join(out)


def main(pkg, version, out):
    pkg, out = Path(pkg), Path(out)
    raw = pkg.read_bytes()
    product = product_of(raw)
    if not re.fullmatch(r"FM-1_050", product):
        raise SystemExit(f"{pkg}: identity {product!r} is not an Argo package (FM-1_050)")
    if b"FELUCCA-LOADER-1" not in raw:              # Felucca loader marker (Argo keeps it): never publish a package with vendor files
        raise SystemExit(f"{pkg}: no Felucca/Argo loader in it; the site ships only fm1pkg_make.py packages "
                         "(a package patched from an official one carries vendor files)")
    page = (HERE / "index_pkg.html").read_text(encoding="utf-8")
    lib = strip_module((HERE / "fm1pkg.js").read_text(encoding="utf-8")) + "\n" + \
        strip_module((HERE / "fm1ota.js").read_text(encoding="utf-8")) + "\n" + \
        strip_module((HERE / "fm1backup.js").read_text(encoding="utf-8"))
    name = f"argo-{re.sub(r'[^A-Za-z0-9.-]', '-', version)}.fwsc"
    meta = json.dumps({"version": version, "product": product, "pkg": "../../firmware/" + name})
    for mark in ("/*LIB*/", "/*META*/"):
        if page.count(mark) != 1:
            raise SystemExit(f"index_pkg.html must contain {mark} once; update make_site.py")
    page = page.replace("/*LIB*/", lib).replace("/*META*/", meta)
    inst, ed, fw = out / "webapp" / "installer", out / "webapp" / "editor", out / "firmware"
    for d in (inst, ed, fw):
        d.mkdir(parents=True, exist_ok=True)
    for old in [*fw.glob("argo-*.fwsc"), *fw.glob("felucca-*.fwsc")]:   # one package: the current one
        old.unlink()
    (inst / "index.html").write_text(page, encoding="utf-8")
    shutil.copy(pkg, fw / name)
    lic = HERE.parent / "LICENSES"                  # the package holds JieLi SDK files (Apache-2.0): their
    (fw / "LICENSES").mkdir(exist_ok=True)          # licence travels next to it, with Argo's own
    names = sorted(f.name for f in lic.glob("*.txt"))
    for n in names:
        shutil.copy(lic / n, fw / "LICENSES" / n)
    (fw / "LICENSES" / "index.html").write_text(    # the installer links this folder: Pages lists no folders
        '<!doctype html><meta charset="utf-8"><title>Argo licences</title><h1>Licence texts</h1><ul>'
        + "".join(f'<li><a href="{n}">{n}</a></li>' for n in names)
        + '</ul><p><a href="../LICENSING.md">LICENSING.md</a> · <a href="../LICENSE">LICENSE (GPL-3.0)</a></p>\n',
        encoding="utf-8")
    for doc in ("LICENSE", "LICENSING.md"):
        shutil.copy(HERE.parent / doc, fw / doc)
    shutil.copy(HERE / "editor.html", ed / "index.html")
    for f in ("fukiai.ttf", "FUKIAI-LICENSE.txt", "fm1backup.js"):
        if (HERE / f).exists():
            shutil.copy(HERE / f, ed / f)
    landing = LANDING.replace("__VERSION__", html.escape(version)).replace("__PKG__", name).replace("__REPO__", REPO) \
        .replace("__EMU__", html.escape(EMU))
    (out / "index.html").write_text(
        shell("Argo", "Argo is a multi-engine synthesizer firmware for the M-VAVE FM-1, built on Felucca.", landing, LANDING_CSS),
        encoding="utf-8")
    (out / "manual").mkdir(exist_ok=True)                 # the README, as a page
    (out / "manual" / "index.html").write_text(
        shell("Argo manual", "What Argo adds to Felucca on the M-VAVE FM-1, and how to use it.",
              markdown((HERE.parent / "README.md").read_text(encoding="utf-8"))), encoding="utf-8")
    for path, here, depth in (("index.html", "", 0), ("manual/index.html", "manual/", 1),     # the header on each page
                              ("webapp/installer/index.html", "webapp/installer/", 2)):
        f = out / path
        f.write_text(dress(f.read_text(encoding="utf-8"), here, depth), encoding="utf-8")
    print(f"site: {out}: index.html, manual/, webapp/installer ({len(page)} B), webapp/editor, firmware/{name} ({len(raw)} B, {product})")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:4])
