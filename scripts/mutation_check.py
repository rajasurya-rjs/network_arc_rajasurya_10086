#!/usr/bin/env python3
"""Shows that the unit tests catch real protocol bugs, not just the happy path.

Copies the source tree into a temporary directory, plants one deliberate bug
at a time (a "mutant"), rebuilds the unit tests there, and runs them. Every
mutant must make at least one test case fail. The working tree is never
modified.

    python3 scripts/mutation_check.py      (or: make mutation-check)
"""

import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (description, [(file, original text, mutated text), ...])
MUTANTS = [
    ("parser consumes one byte past the body (eats the next request's first byte)",
     [("src/request_parser.cpp",
       "    consume(body_length_);\n    pending_.stream_end",
       "    consume(body_length_ + (buffered_bytes() > body_length_ ? 1 : 0));\n    pending_.stream_end")]),
    ("parser ignores Content-Length (every body treated as empty)",
     [("src/request_parser.cpp", "    body_length = value;\n", "    body_length = 0 * value;\n")]),
    ("parser assumes one read == one request (drops bytes after the first request)",
     [("src/request_parser.cpp",
       "    pending_.stream_end = stream_offset_;\n",
       "    pending_.stream_end = stream_offset_;\n    consume(buffered_bytes());\n")]),
    ("connection treats a short write as complete (unsent bytes are lost)",
     [("src/connection.cpp", "            output_sent_ += count;", "            output_sent_ = output_.size();")]),
    ("connection answers only one request per readiness event (breaks pipelining)",
     [("src/connection.cpp",
       "        queue_response(std::move(response));\n    }\n    return responded;",
       "        queue_response(std::move(response));\n        break;\n    }\n    return responded;"),
      ("src/connection.cpp", "        if (!progress) break;\n    }", "        if (progress || !progress) break;\n    }")]),
    ("connection ignores 'Connection: close'",
     [("src/connection.cpp", "        if (!request.wants_keep_alive()) response.close_connection = true;\n", "")]),
    ("connection keeps going after a framing error",
     [("src/connection.cpp",
       "            response.close_connection = true;\n            logger_.info(\"[conn \", id_, \"] #\", stats_.requests + 1, \" unparseable",
       "            logger_.info(\"[conn \", id_, \"] #\", stats_.requests + 1, \" unparseable")]),
    ("connection sends a body in the response to HEAD",
     [("src/connection.cpp", "        if (request.method == \"HEAD\") response.omit_body = true;\n", "")]),
    ("connection calls close() at once instead of the lingering close",
     [("src/connection.cpp", "            begin_lingering_close(now);", "            close_now(\"mutant\");")]),
    ("router no longer requires Host for HTTP/1.1",
     [("src/router.cpp", "    if (host_headers == 0 && request.version == HttpVersion::Http11) {",
       "    if (host_headers == 0 && request.version == HttpVersion::Http11 && false) {")]),
    ("calculator ignores overflow (wraps around)",
     [("src/calculator.cpp", "    if (overflow) {", "    if (overflow && false) {")]),
]


def main():
    survivors = 0
    with tempfile.TemporaryDirectory(prefix="calc_mutants_") as scratch:
        for name in ("src", "include", "tests", "Makefile"):
            source = os.path.join(ROOT, name)
            target = os.path.join(scratch, name)
            (shutil.copytree if os.path.isdir(source) else shutil.copy)(source, target)

        for description, edits in MUTANTS:
            originals = {}
            for path, old, new in edits:
                full = os.path.join(scratch, path)
                text = open(full).read()
                originals.setdefault(full, text)  # first-seen content, restored afterwards
                if old not in text:
                    sys.exit(f"mutant '{description}' no longer applies to {path}; update the script")
                open(full, "w").write(text.replace(old, new, 1))
            try:
                build = subprocess.run(["make", "-s", "build/unit_tests"], cwd=scratch, capture_output=True, text=True)
                if build.returncode != 0:
                    sys.exit(f"mutant '{description}' failed to compile:\n{build.stderr}")
                run = subprocess.run(["./build/unit_tests"], cwd=scratch, capture_output=True, text=True, timeout=300)
                failing = [line.strip()[6:] for line in run.stdout.splitlines() if line.startswith("  FAIL")]
                caught = run.returncode != 0
                survivors += not caught
                print(f"{'CAUGHT  ' if caught else 'SURVIVED'} {description}")
                for name in failing[:3]:
                    print(f"           fails: {name}")
                if len(failing) > 3:
                    print(f"           ... and {len(failing) - 3} more")
            finally:
                for full, text in originals.items():
                    open(full, "w").write(text)

    print(f"\n{len(MUTANTS) - survivors}/{len(MUTANTS)} planted bugs caught by the unit tests")
    return 1 if survivors else 0


if __name__ == "__main__":
    sys.exit(main())
