#!/usr/bin/env python3
"""Add the WirePlumber V4L2 hook version guard to producer package metadata."""

import argparse
from pathlib import Path
import re


DEBIAN_DEPENDENCY = "wireplumber (<< 0.5.16) | wireplumber (>= 0.5.18)"
RPM_DEPENDENCY = "(wireplumber < 0.5.16 or wireplumber >= 0.5.18)"


def add_debian_dependency(text, package):
    # Keep comments, continuation lines, and every other binary stanza intact.
    paragraphs = re.split(r"(\n[ \t]*\n)", text)
    targets = [
        i for i, paragraph in enumerate(paragraphs)
        for _ in re.finditer(r"^Package:[ \t]*" + re.escape(package) + r"[ \t]*$",
                             paragraph, re.MULTILINE)
    ]
    if len(targets) != 1:
        raise ValueError(f"expected exactly one Debian Package: {package} stanza")
    i = targets[0]
    paragraph = paragraphs[i]
    fields = list(re.finditer(r"^Depends:[^\n]*(?:\n[ \t][^\n]*)*",
                              paragraph, re.MULTILINE))
    if len(fields) != 1:
        raise ValueError(f"expected exactly one Depends field in {package}")
    field = fields[0]
    value = field.group()[len("Depends:"):].strip()
    if not value:
        raise ValueError(f"empty Depends field in {package}")
    dependencies = [" ".join(item.split()) for item in value.split(",")]
    if DEBIAN_DEPENDENCY in dependencies:
        return text
    original = field.group().rstrip()
    separator = "" if original.endswith(",") else ","
    updated = original + separator + "\n " + DEBIAN_DEPENDENCY
    paragraphs[i] = paragraph[:field.start()] + updated + paragraph[field.end():]
    return "".join(paragraphs)


def add_rpm_dependency(text, package):
    # Restrict insertion to the reviewed named subpackage. A changed layout must
    # stop the build instead of silently putting Requires on a different RPM.
    headers = list(re.finditer(r"^%package[ \t]+" + re.escape(package) + r"[ \t]*$",
                               text, re.MULTILINE))
    if len(headers) != 1:
        raise ValueError(f"expected exactly one RPM %package {package} section")
    header = headers[0]
    next_section = re.search(r"^%(?:package|description|prep|build|install|files)\b",
                             text[header.end():], re.MULTILINE)
    if next_section is None:
        raise ValueError(f"missing section after RPM %package {package}")
    end = header.end() + next_section.start()
    requirements = re.findall(r"^Requires:[ \t]*(.*)$", text[header.end():end],
                              re.MULTILINE)
    if any(" ".join(value.split()) == RPM_DEPENDENCY for value in requirements):
        return text
    return (text[:header.end()] + "\nRequires: " + RPM_DEPENDENCY
            + text[header.end():])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("format", choices=("debian", "rpm"))
    parser.add_argument("manifest", type=Path)
    parser.add_argument("package", help="runtime binary package or RPM subpackage")
    args = parser.parse_args()
    try:
        text = args.manifest.read_text(encoding="utf-8")
        transform = add_debian_dependency if args.format == "debian" else add_rpm_dependency
        updated = transform(text, args.package)
        if updated != text:
            args.manifest.write_text(updated, encoding="utf-8")
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"Anland WirePlumber dependency: {error}\n")


if __name__ == "__main__":
    main()
