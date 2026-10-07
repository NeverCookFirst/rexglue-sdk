"""Verify preparation of the exact public SIMDe pin in disposable offline clones."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

PIN = '71fd833d9666141edcd1d3c109a80e228303d8d7'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sdk = Path(__file__).resolve().parents[2]
    parser.add_argument('output', type=Path)
    parser.add_argument('--checkout', type=Path, default=sdk/'thirdparty/simde')
    parser.add_argument('--preparer', type=Path, default=sdk/'scripts/prepare_simde.py')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source, preparer = args.checkout.resolve(), args.preparer.resolve()
    patch = sdk/'patches/simde-native-m64-stream.patch'
    git_executable = shutil.which('git')
    if git_executable is None:
        parser.error('Git is required for the offline public-source fixture')
    checks = []

    def command(argv, cwd, *, expect=0, oracle=None):
        result = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=15)
        if result.returncode != expect or (oracle and oracle not in result.stdout+result.stderr):
            raise AssertionError(f'Expected exit{expect}, oracle{oracle!r}; got{result.returncode}\n'
                                 + result.stdout + result.stderr)
        return result

    def git(checkout, *arguments):
        return command([git_executable, '-C', str(checkout), *arguments], output)

    def identity():
        return dict(head=git(source, 'rev-parse', 'HEAD').stdout,
                    status=git(source, 'status', '--porcelain=v1', '--untracked-files=all').stdout,
                    header_sha256=digest(source/'simde/x86/sse.h'))

    original = identity()
    git(source, 'cat-file', '-e', PIN+'^{commit}')
    public = subprocess.run([git_executable, '-C', str(source), 'show', PIN+':simde/x86/sse.h'],
                            capture_output=True, check=True, timeout=15).stdout
    with tempfile.TemporaryDirectory(prefix='simde-preparation-', dir=output) as temporary:
        sandbox = Path(temporary)
        unrelated = sandbox/'unrelated-cwd'
        unrelated.mkdir()

        def clone(name):
            checkout = sandbox/name
            command([git_executable, 'clone', '--config', 'core.autocrlf=false',
                     '--shared', '--no-checkout', str(source), str(checkout)], unrelated)
            git(checkout, 'checkout', '--detach', PIN)
            assert (checkout/'simde/x86/sse.h').read_bytes() == public
            return checkout

        def prepare(checkout, *options, expect=0, oracle=None):
            return command([sys.executable, str(preparer), str(checkout), *options], unrelated,
                           expect=expect, oracle=oracle)

        expected = clone('independent-patch')
        git(expected, 'apply', '--check', str(patch))
        git(expected, 'apply', str(patch))
        expected_sha = digest(expected/'simde/x86/sse.h')
        checks.append('actual public pin independently patched with real Git')

        checkout = clone('prepare-under-test')
        before = digest(checkout/'simde/x86/sse.h')
        prepare(checkout, '--check', expect=2, oracle='Native __m64 streaming-store fix is missing or incomplete')
        assert digest(checkout/'simde/x86/sse.h') == before
        checks.append('missing patch check rejects without source writes')
        for options in ((), ('--check',)):
            prepare(checkout/'simde/x86', *options, expect=2,
                    oracle='must be the repository root, not a subdirectory')
        assert digest(checkout/'simde/x86/sse.h') == before
        checks.append('apply/check reject actual repository subdirectory')
        prepare(checkout, oracle='Applied native __m64 streaming-store fix')
        assert digest(checkout/'simde/x86/sse.h') == expected_sha
        for options in (('--check',), ()):
            prepare(checkout, *options, oracle='PASS: SIMDe')
            assert digest(checkout/'simde/x86/sse.h') == expected_sha
        checks.append('apply/check/repeat from unrelated cwd match independent patch exactly')
        # Represents an existing checkout prepared before root/content hardening.
        for options in (('--check',), ()):
            prepare(expected, *options, oracle='PASS: SIMDe')
            assert digest(expected/'simde/x86/sse.h') == expected_sha
        checks.append('old independently prepared checkout accepted unchanged')
        with (checkout/'simde/x86/sse.h').open('a') as stream:
            stream.write('\n/* Fixture unrelated edit outside patched hunks. */\n')
        edited = digest(checkout/'simde/x86/sse.h')
        prepare(checkout, '--check', oracle='PASS: SIMDe')
        prepare(checkout, oracle='PASS: SIMDe')
        assert digest(checkout/'simde/x86/sse.h') == edited
        checks.append('unrelated local edit preserved')
        header = (checkout/'simde/x86/sse.h').read_text()
        old = '__builtin_nontemporal_store(a_.i64[0], HEDLEY_REINTERPRET_CAST(int64_t*, mem_addr));'
        assert header.count(old) == 1
        (checkout/'simde/x86/sse.h').write_text(header.replace(old, old.replace('i64[0]', 'i64[1]')))
        partial_sha = digest(checkout/'simde/x86/sse.h')
        prepare(checkout, '--check', expect=2, oracle='Native __m64 streaming-store fix is missing or incomplete')
        prepare(checkout, expect=2, oracle='Cannot apply the SIMDe fix without conflicting with existing edits')
        assert digest(checkout/'simde/x86/sse.h') == partial_sha
        checks.append('partial patch check/apply reject without overwriting edits')

        wrong = clone('wrong-revision')
        tree = git(wrong, 'rev-parse', PIN+'^{tree}').stdout.strip()
        commit = command([git_executable, '-C', str(wrong), '-c', 'user.name=Fixture',
                          '-c', 'user.email=fixture@example.invalid', 'commit-tree', tree,
                          '-p', PIN, '-m', 'Fixture wrong revision with identical public tree'], unrelated).stdout.strip()
        git(wrong, 'checkout', '--detach', commit)
        prepare(wrong, expect=2, oracle=f'Expected SIMDe {PIN}, got {commit}')
        checks.append('wrong revision rejected even with identical tree')

        noop = clone('false-success-git')
        wrapper = '''import runpy, subprocess, sys
real_run = subprocess.run
reverse_calls = 0
mode, preparer, checkout = sys.argv[1:4]
def fake_run(argv, *args, **kwargs):
    global reverse_calls
    if isinstance(argv, list) and argv[0] == 'git' and 'apply' in argv:
        if '--reverse' in argv:
            reverse_calls += 1
            if mode == 'apply' and reverse_calls == 1:
                return subprocess.CompletedProcess(argv, 1, '', 'fixture patch missing')
        return subprocess.CompletedProcess(argv, 0, '', '')
    return real_run(argv, *args, **kwargs)
subprocess.run = fake_run
sys.argv = [preparer, checkout]
runpy.run_path(preparer, run_name='__main__')
'''
        for mode in ('already', 'apply'):
            command([sys.executable, '-c', wrapper, mode, str(preparer), str(noop)], unrelated,
                    expect=2, oracle='SIMDe source verification failed: missing patched hunk')
            assert digest(noop/'simde/x86/sse.h') == before
        checks.append('false already-applied and no-op apply Git success rejected by actual source')
    assert identity() == original, 'Original SIMDe checkout changed'
    checks.append('original checkout HEAD/status/source preserved')
    report = dict(passed=True, pin=PIN, actual_public_source=True, offline=True,
                  preparer_sha256=digest(preparer), patch_sha256=digest(patch),
                  public_header_sha256=hashlib.sha256(public).hexdigest(),
                  prepared_header_sha256=expected_sha, checks=checks)
    (output/'verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'PASS: {len(checks)} offline exact-public-pin preparation checks; original checkout unchanged')


if __name__ == '__main__':
    main()
