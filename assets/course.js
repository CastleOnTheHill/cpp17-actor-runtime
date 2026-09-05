// No analytics, dependencies, network requests, or automatic mastery claims.
document.querySelectorAll('[data-walkthrough]').forEach(root => {
  const frames = [...root.querySelectorAll('[data-frame]')];
  const prev = root.querySelector('[data-prev]');
  const next = root.querySelector('[data-next]');
  const counter = root.querySelector('[data-counter]');
  let index = 0;
  function render() {
    frames.forEach((frame, n) => { frame.hidden = n !== index; });
    counter.textContent = `${index + 1} / ${frames.length}`;
    prev.disabled = index === 0;
    next.disabled = index === frames.length - 1;
  }
  prev.addEventListener('click', () => { if (index > 0) { --index; render(); } });
  next.addEventListener('click', () => { if (index < frames.length - 1) { ++index; render(); } });
  render();
});
document.querySelectorAll('[data-download]').forEach(button => {
  button.addEventListener('click', () => {
    const answer = document.getElementById(button.dataset.download);
    const feedback = button.parentElement.querySelector('[data-feedback]');
    if (!answer.value.trim()) { feedback.textContent = '先写下你的推导；不确定的部分也可以保留。'; return; }
    const blob = new Blob([`# ${document.title}\n\n${answer.value}\n`], {type: 'text/markdown;charset=utf-8'});
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url; link.download = `${answer.id}-answer.md`;
    document.body.append(link); link.click(); link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    feedback.textContent = '已请求浏览器下载答案；请检查下载结果。这不代表通过考核。';
  });
});
