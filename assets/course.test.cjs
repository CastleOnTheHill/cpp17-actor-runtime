// Unit-test only: a minimal DOM double, not a browser rendering test.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
function button() {
  return { disabled: false, handlers: {}, addEventListener(type, fn) { this.handlers[type] = fn; } };
}
const prev = button(), next = button(), download = button();
const frames = [{}, {}, {}], counter = {}, feedback = {}, answer = { id: 'unit-answer', value: '' };
const root = {
  querySelectorAll: () => frames,
  querySelector: key => ({ '[data-prev]': prev, '[data-next]': next, '[data-counter]': counter })[key]
};
download.dataset = { download: 'unit-answer' };
download.parentElement = { querySelector: () => feedback };
let appended = 0, clicked = 0, removed = 0, revoked = 0, content;
const link = { click() { clicked++; }, remove() { removed++; } };
const document = {
  title: 'Unit test',
  querySelectorAll: key => key === '[data-walkthrough]' ? [root] : [download],
  getElementById: () => answer,
  createElement: () => link,
  body: { append() { appended++; } }
};
vm.runInNewContext(fs.readFileSync(__dirname + '/course.js', 'utf8'), {
  document,
  Blob: class { constructor(parts) { content = parts.join(''); } },
  URL: { createObjectURL: () => 'blob:unit-test', revokeObjectURL() { revoked++; } },
  setTimeout: callback => callback()
});
assert.equal(counter.textContent, '1 / 3');
assert.equal(prev.disabled, true);
assert.deepEqual(frames.map(f => f.hidden), [false, true, true]);
prev.handlers.click();
assert.equal(counter.textContent, '1 / 3');
next.handlers.click();
assert.equal(counter.textContent, '2 / 3');
assert.deepEqual(frames.map(f => f.hidden), [true, false, true]);
next.handlers.click();
assert.equal(next.disabled, true);
next.handlers.click();
assert.equal(counter.textContent, '3 / 3');
prev.handlers.click();
assert.equal(counter.textContent, '2 / 3');
assert.equal(next.disabled, false);
download.handlers.click();
assert.equal(clicked, 0);
assert.match(feedback.textContent, /先写下/);
answer.value = '我的独立推导';
download.handlers.click();
assert.equal(link.download, 'unit-answer-answer.md');
assert.match(content, /我的独立推导/);
assert.deepEqual([appended, clicked, removed, revoked], [1, 1, 1, 1]);
console.log('[PASS] stepper bounds/visibility and answer export feedback (DOM-double unit tests)');
