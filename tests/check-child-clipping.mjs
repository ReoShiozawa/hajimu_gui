// 検査用GUIが書いたP6の親領域内外を論理座標で比較する。
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
const data = readFileSync(process.argv[2]);
const header = /^P6\s+(\d+)\s+(\d+)\s+255\n/.exec(data.subarray(0, 100).toString('ascii'));
assert(header, 'Expected P6 image');
const width = Number(header[1]), height = Number(header[2]);
assert.equal(data.length - header[0].length, width * height * 3);
const scale = width / 640;
const pixel = (x, y) => {
  const offset = header[0].length + (Math.floor(y * scale) * width + Math.floor(x * scale)) * 3;
  return [...data.subarray(offset, offset + 3)];
};
const inside = pixel(65, 60), outside = pixel(65, 160);
assert(inside[0] > 180 && inside[1] < 80, `Missing inner content: ${inside}`);
assert(outside.every(channel => channel >= 240), `Content escaped parent: ${outside}`);
console.log(`child clipping passed: inside=${inside}, outside=${outside}, scale=${scale}`);
