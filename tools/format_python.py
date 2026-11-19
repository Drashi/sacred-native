#!/usr/bin/env python3
"""Layout of the project's Python sources (CONTRIBUTING.md, "Code style"): one statement per line, the body of a
compound statement on its own lines, a blank line after a block that is followed by more code at the block header's
level, and PEP 8 blank lines before definitions. Only layout changes: the syntax tree of every file stays the same.

    tools/format_python.py FILE...           rewrite the files
    tools/format_python.py --check FILE...   list the files that need it (and lines over 120 columns); exit 1
"""
import argparse
import ast
import io
import sys
import tokenize

COMPOUND = {"if", "elif", "else", "for", "while", "with", "try", "except", "finally", "def", "class", "async"}
CONTINUE = {"elif", "else", "except", "finally"}
LIMIT = 120


def logical_lines(src):
    lines, line = [], []
    for t in tokenize.generate_tokens(io.StringIO(src).readline):
        if t.type in (tokenize.NL, tokenize.COMMENT, tokenize.INDENT, tokenize.DEDENT, tokenize.ENCODING,
                      tokenize.ENDMARKER):
            continue

        line.append(t)
        if t.type == tokenize.NEWLINE:
            lines.append(line)
            line = []

    return lines


def indentation(text):
    return len(text) - len(text.lstrip())


def split_once(src):
    """Split the first one-line compound statement or ';' found; None when there is none."""
    lines = src.splitlines(keepends=True)
    for logical in logical_lines(src):
        first = logical[0]
        indent = indentation(lines[first.start[0] - 1])
        depth = 0
        lambdas = 0
        header = first.type == tokenize.NAME and first.string in COMPOUND
        for k, t in enumerate(logical):
            if t.type == tokenize.OP and t.string in "([{":
                depth += 1
            elif t.type == tokenize.OP and t.string in ")]}":
                depth -= 1
            elif depth == 0 and t.type == tokenize.NAME and t.string == "lambda":
                lambdas += 1
            elif depth == 0 and t.type == tokenize.OP and t.string == ":" and header:
                if lambdas:
                    lambdas -= 1
                    continue

                after = logical[k + 1]
                if after.type != tokenize.NEWLINE and after.start[0] == t.start[0]:
                    row = t.start[0] - 1
                    text = lines[row]
                    lines[row] = text[:t.end[1]] + "\n" + " " * (indent + 4) + text[after.start[1]:]
                    return "".join(lines)

                header = False
            elif depth == 0 and t.type == tokenize.OP and t.string == ";":
                after = logical[k + 1]
                row = t.start[0] - 1
                text = lines[row]
                if after.type == tokenize.NEWLINE:
                    lines[row] = text[:t.start[1]].rstrip() + text[t.end[1]:]
                else:
                    lines[row] = text[:t.start[1]].rstrip() + "\n" + " " * indent + text[after.start[1]:]

                return "".join(lines)

    return None


def blank_after_blocks(src):
    lines = src.splitlines(keepends=True)
    logical = logical_lines(src)
    rows = set()
    for previous, current in zip(logical, logical[1:]):
        if current[0].start[1] >= indentation(lines[previous[0].start[0] - 1]):
            continue

        if current[0].type == tokenize.NAME and current[0].string in CONTINUE:
            continue

        between = lines[previous[-1].start[0]:current[0].start[0] - 1]
        if any(not line.strip() for line in between):
            continue

        rows.add(previous[-1].end[0])

    for row in sorted(rows, reverse=True):
        lines.insert(row, "\n")

    return "".join(lines)


def blank_before_definitions(src):
    lines = src.splitlines(keepends=True)
    logical = logical_lines(src)
    inserts = []
    for previous, current in zip(logical, logical[1:]):
        t = current[0]
        if not (t.type == tokenize.NAME and t.string in ("def", "class", "async") or t.string == "@"):
            continue

        if previous[0].string == "@" or previous[0].start[1] < t.start[1]:
            continue  # under a decorator, or the first statement of a block

        need = 2 if t.start[1] == 0 else 1
        row = t.start[0] - 1
        while row > 0 and lines[row - 1].lstrip().startswith("#"):
            row -= 1  # comments directly above belong to the definition

        have = 0
        while row - have > 0 and not lines[row - have - 1].strip():
            have += 1

        if have < need:
            inserts.append((row, need - have))

    for row, count in sorted(inserts, reverse=True):
        lines[row:row] = ["\n"] * count

    return "".join(lines)


def layout(src):
    while (split := split_once(src)) is not None:
        src = split

    return blank_before_definitions(blank_after_blocks(src))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()
    status = 0
    for path in args.files:
        with open(path, encoding="utf-8") as stream:
            old = stream.read()

        new = layout(old)
        if ast.dump(ast.parse(old)) != ast.dump(ast.parse(new)):
            print(f"{path}: layout would change the syntax tree; left alone", file=sys.stderr)
            status = 1
            continue

        long_lines = [n for n, line in enumerate(new.splitlines(), 1) if len(line) > LIMIT]
        for n in long_lines:
            print(f"{path}:{n}: longer than {LIMIT} columns", file=sys.stderr)
            status = 1

        if new != old:
            if args.check:
                print(f"{path}: needs tools/format_python.py", file=sys.stderr)
                status = 1
            else:
                with open(path, "w", encoding="utf-8") as stream:
                    stream.write(new)

    return status


if __name__ == "__main__":
    sys.exit(main())
