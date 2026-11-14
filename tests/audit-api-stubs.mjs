// 単純な空実装を候補として列挙する。実装の正しさや完成を自動認定しない。
import { readFileSync } from 'node:fs';
const source = readFileSync(new URL('../src/hajimu_gui.c', import.meta.url), 'utf8');
const functions = new Map();
for (const match of source.matchAll(/static Value (fn_\w+)\(int argc, Value \*argv\)\s*\{([^{}]*)\}/g)) {
  const body = match[2].replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '').trim();
  const stripped = body.replace(/\(void\)\s*(?:argc|argv(?:\[\d+\])?)\s*;/g, '').trim();
  if (/^return hajimu_(?:null\(\)|number\(-?\d+(?:\.\d+)?\)|bool\((?:true|false)\)|string\("[^"\\]*"\));$/.test(stripped)) {
    functions.set(match[1], stripped);
  }
}
const table = source.slice(source.indexOf('static HajimuPluginFunc gui_functions[]'));
const candidates = [];
for (const match of table.matchAll(/\{"([^"]+)"\s*,\s*(fn_\w+)\s*,/g)) {
  if (functions.has(match[2])) candidates.push({ name: match[1], function: match[2], body: functions.get(match[2]) });
}
process.stdout.write(JSON.stringify({
  description: 'Review candidates only; constant-return helpers may be intentional. Complex stubs are not covered.',
  count: candidates.length,
  candidates,
}, null, 2) + '\n');
