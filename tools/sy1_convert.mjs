#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// The editor's .sy1 importer (SY1 in web/editor.html) as a command: a .sy1 on stdin or as an argument -> "id value" lines (what the
// editor sends to the device; engine values at P_E0 = 106 + i) for tests/sy1_render.c, or with --report the applied / skipped lists.
//   node tools/sy1_convert.mjs PATCH.sy1 [--pe0 106] [--report]
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(join(here, "../web/editor.html"), "utf8");
const proto = html.slice(html.indexOf("/*PROTO-BEGIN*/"), html.indexOf("/*PROTO-END*/"));
const { SY1 } = vm.runInNewContext(proto + ";({SY1})", { console });
const args = process.argv.slice(2);
const pe0 = args.includes("--pe0") ? +args[args.indexOf("--pe0") + 1] : 106;
const file = args.find((a, i) => !a.startsWith("--") && args[i - 1] !== "--pe0");
const r = SY1.toLead(SY1.parse(readFileSync(file || 0, "utf8")));
if (args.includes("--report")) {
  for (const a of r.applied) console.log(`${a.level.padEnd(9)} ${a.name}: ${a.from.join(" ")} -> ${a.to}`);
  for (const a of r.skipped) console.log(`skipped   ${a.name}: ${a.why}`);
} else for (const [id, v] of SY1.toDevice(r, pe0, 1000)) console.log(id, v);
