"""Keep the App example's documented files identical to its compiled sources."""

from pathlib import Path
import sys


def check_block(document: str, filename: str, source: str) -> None:
    marker = f"<!-- app-readme-example: {filename} -->\n```cpp\n"
    if document.count(marker) != 1:
        raise ValueError(f"expected one fenced example for {filename}")
    example, separator, _ = document.split(marker, 1)[1].partition("\n```")
    if not separator or example + "\n" != source:
        raise ValueError(f"example differs from tests/app/{filename}")


def main(root: Path) -> int:
    fixtures = root / "tests/app"
    documents = {
        "libraries/app/README.md": (
            "readme_example.hxx", "readme_example.cpp", "readme_example_main.cpp",
        ),
        "docs/runtime/asio-app.md": ("readme_example_main.cpp",),
    }
    for path, filenames in documents.items():
        document = (root / path).read_text(encoding="utf-8")
        for filename in filenames:
            try:
                check_block(document, filename, (fixtures / filename).read_text(encoding="utf-8"))
            except ValueError as error:
                print(f"{path}: {error}", file=sys.stderr)
                return 1
    print("App README/runtime examples match the compiled fixture sources")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]))
