"""Apply or verify the pinned SIMDe native __m64 streaming-store fix."""

import argparse
from pathlib import Path
import re
import subprocess


PIN = "71fd833d9666141edcd1d3c109a80e228303d8d7"


def verify_patched_source(checkout, patch):
    """Require every actual new-side hunk, independently of git's exit status."""
    target = None
    expected = None
    lines = []
    hunks = 0

    def finish():
        nonlocal hunks
        if expected is None:
            return
        if target is None or not lines or len(lines) != expected:
            raise ValueError("Invalid SIMDe patch hunk")
        path = checkout / target
        if path.resolve() != path or not path.is_file():
            raise ValueError(f"SIMDe source verification failed: invalid source path {target}")
        if "\n".join(lines) + "\n" not in path.read_text():
            raise ValueError(f"SIMDe source verification failed: missing patched hunk in {target}")
        hunks += 1

    for line in patch.read_text().splitlines():
        if line.startswith("diff --git "):
            finish()
            target, expected, lines = None, None, []
        elif line.startswith("+++ b/"):
            target = Path(line[6:])
            if target.is_absolute() or ".." in target.parts:
                raise ValueError("Invalid SIMDe patch target")
        elif line.startswith("@@ "):
            finish()
            header = re.fullmatch(r"@@ -\d+(?:,\d+)? \+\d+(?:,(\d+))? @@.*", line)
            if header is None:
                raise ValueError("Invalid SIMDe patch header")
            expected, lines = int(header.group(1) or "1"), []
        elif expected is not None:
            if line.startswith((" ", "+")):
                lines.append(line[1:])
            elif not line.startswith(("-", "\\ No newline")):
                raise ValueError("Invalid SIMDe patch content")
    finish()
    if not hunks:
        raise ValueError("SIMDe patch has no verifiable hunks")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sdk = Path(__file__).resolve().parents[1]
    parser.add_argument("checkout", type=Path, nargs="?", default=sdk / "thirdparty/simde")
    parser.add_argument("--check", action="store_true", help="Verify only; do not modify the checkout")
    args = parser.parse_args()
    checkout = args.checkout.resolve()
    patch = sdk / "patches/simde-native-m64-stream.patch"

    def git(*arguments, check=False):
        return subprocess.run(["git", "-C", str(checkout), *arguments], check=check,
                              cwd=checkout, capture_output=True, text=True, timeout=30)

    if not checkout.is_dir():
        parser.error(f"SIMDe checkout does not exist: {checkout}")
    repository = git("rev-parse", "--show-toplevel")
    if repository.returncode or Path(repository.stdout.strip()).resolve() != checkout:
        parser.error("SIMDe checkout must be the repository root, not a subdirectory")
    revision = git("rev-parse", "HEAD", check=True).stdout.strip()
    if revision != PIN:
        parser.error(f"Expected SIMDe {PIN}, got {revision}; use the SDK's pinned submodule revision")
    if git("apply", "--reverse", "--check", str(patch)).returncode == 0:
        try:
            verify_patched_source(checkout, patch)
        except ValueError as error:
            parser.error(str(error))
        print(f"PASS: SIMDe {PIN} with native __m64 streaming-store fix")
        return
    if args.check:
        parser.error("Native __m64 streaming-store fix is missing or incomplete; run without --check")
    applicability = git("apply", "--check", str(patch))
    if applicability.returncode:
        parser.error("Cannot apply the SIMDe fix without conflicting with existing edits:\n" +
                     applicability.stderr.strip())
    git("apply", str(patch), check=True)
    git("apply", "--reverse", "--check", str(patch), check=True)
    try:
        verify_patched_source(checkout, patch)
    except ValueError as error:
        parser.error(str(error))
    print(f"Applied native __m64 streaming-store fix to SIMDe {PIN}")


if __name__ == "__main__":
    main()
