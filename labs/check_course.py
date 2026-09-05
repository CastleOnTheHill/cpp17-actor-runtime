"""Read-only structural QA for locally authored course HTML, using stdlib only."""
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


class Page(HTMLParser):
    def __init__(self, path):
        super().__init__(convert_charrefs=True)
        self.path = path
        self.ids = set()
        self.refs = []
        self.duplicates = []
        self.title = False
        self.feed(path.read_text(encoding="utf-8"))

    def handle_starttag(self, tag, attrs):
        data = dict(attrs)
        if tag == "title":
            self.title = True
        if "id" in data:
            key = data["id"]
            if key in self.ids:
                self.duplicates.append(key)
            self.ids.add(key)
        for attr in ("href", "src"):
            if attr in data:
                self.refs.append(data[attr])


pages = {p.resolve(): Page(p) for p in [ROOT / "course.html",
         *sorted((ROOT / "lessons").glob("*.html")),
         *sorted((ROOT / "reference").glob("*.html"))]}
errors = []
checked = 0
for path, page in pages.items():
    if not page.title:
        errors.append(f"{path.name}: missing title")
    if page.duplicates:
        errors.append(f"{path.name}: duplicate ids {page.duplicates}")
    for raw in page.refs:
        ref = urlsplit(raw)
        if ref.scheme or ref.netloc:
            continue
        target = (path.parent / unquote(ref.path)).resolve() if ref.path else path
        checked += 1
        if not target.exists():
            errors.append(f"{path.name}: broken local reference {raw}")
        elif ref.fragment and target.suffix == ".html":
            target_page = pages.get(target)
            if target_page is None:
                target_page = Page(target)
            if unquote(ref.fragment) not in target_page.ids:
                errors.append(f"{path.name}: missing anchor {raw}")
if errors:
    raise SystemExit("\n".join(errors))
print(f"[PASS] {len(pages)} HTML pages, {checked} local references; targets and anchors exist")
print("Structural checks do not replace browser layout/interaction inspection.")
