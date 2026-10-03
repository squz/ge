#!/usr/bin/env python3
"""Hermetic consumer fixtures: production scripts, no keys, network or fastlane."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

GE = Path(__file__).resolve().parents[2]
FAKE = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
a = sys.argv[1:]
with open(os.environ['CALLS'], 'a') as f:
    f.write(json.dumps([name, a]) + '\n')
if name == 'bundle':
    sys.exit(99)  # Any bypass is a test failure.
if name == 'spyder':
    assert not any(k in os.environ for k in ['MATCH_PASSWORD', 'APP_STORE_CONNECT_API_KEY_KEY_ID', 'APP_STORE_CONNECT_API_KEY_ISSUER_ID', 'APP_STORE_CONNECT_API_KEY_KEY_PATH'])
    if a[:2] == ['secret', 'missing']:
        sys.exit(20 if a[-1] == os.environ.get('MISSING') else 0)
    lane = a[a.index('--') + 1]
    if lane == os.environ.get('FAIL_LANE'):
        sys.exit(37)
    if lane == 'build_ipa':
        out = next(x.split(':', 1)[1] for x in a if x.startswith('output_dir:'))
        pathlib.Path(out).mkdir(parents=True, exist_ok=True)
        pathlib.Path(out, 'fixture.ipa').touch()
    sys.exit(0)
if name == 'git':
    if a[:1] == ['-C']:
        a = a[2:]
    if a[0] == 'rev-parse':
        if '--show-superproject-working-tree' in a or '--show-toplevel' in a:
            print(os.environ['CONSUMER'])
        elif '--verify' in a:
            sys.exit(1)
        elif '--abbrev-ref' in a:
            print('master')
        else:
            print('a' * 40)
    elif a[0] == 'rev-list':
        print('12')
    elif a[:2] == ['worktree', 'add']:
        pathlib.Path(a[-2]).mkdir(parents=True, exist_ok=True)
    elif a[0] == 'status' and os.environ.get('DIRTY'):
        print(' M tracked')
    sys.exit(0)
'''


class ShipTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        # Copy production scripts without changing their contents.
        self.scripts = self.root / 'engine' / 'tools' / 'ship'
        shutil.copytree(GE / 'tools/ship', self.scripts)
        for cmd in ('git', 'spyder', 'bundle'):
            p = self.bin / cmd
            p.write_text(FAKE)
            p.chmod(0o755)
        self.calls = self.root / 'calls.jsonl'
        # Deliberately exclude the actual parent environment and credentials.
        self.env = dict(PATH=f'{self.bin}:/usr/bin:/bin:/opt/homebrew/bin',
                        HOME=str(self.root), CONSUMER=str(self.root),
                        CALLS=str(self.calls), SHIP_SCHEME='Fixture Game',
                        APP_ID='com.example.fixture', APPLE_TEAM_ID='EXAMPLE')
        (self.root / 'Makefile').write_text(
            f'ge := {GE}\nAPP_DISPLAY_NAME := Fixture Game\n'
            f'include {GE}/Module.mk\n'
            'ios:\n\t@echo generated > codegen\n')

    def run_script(self, script, *args, code=0, **env):
        r = subprocess.run(['bash', str(self.scripts / (script + '.sh')), *args],
                           cwd=self.root, env=self.env | env, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(r.returncode, code, r.stdout)
        return r.stdout

    def log(self):
        return [json.loads(x) for x in self.calls.read_text().splitlines()] if self.calls.exists() else []

    def spyder(self):
        return [a for name, a in self.log() if name == 'spyder']

    def assert_no_mutation(self):
        self.assertFalse((self.root / 'build').exists())
        self.assertFalse((self.root / 'codegen').exists())
        self.assertFalse(any(a[0] == 'fastlane' for a in self.spyder()))
        for name, a in self.log():
            if name == 'git':
                self.assertFalse(any(x in a for x in ('push', 'worktree', '-a', 'fetch')))

    def test_all_lanes_studio_project_and_confirmation(self):
        for lane in ('alpha', 'beta', 'release'):
            with self.subTest(lane=lane):
                self.calls.unlink(missing_ok=True)
                args = ['--lane', lane]
                if lane != 'alpha':
                    args += ['--version', '1.2.3']
                if lane == 'release':
                    args += ['--confirm']
                self.run_script('release', *args, STUDIO='other-studio',
                                SHIP_PROJECT='ios/My Game.xcodeproj')
                calls = self.spyder()
                self.assertTrue(all(a[a.index('--studio') + 1] == 'other-studio' for a in calls))
                self.assertEqual([a[-1] for a in calls if a[0] == 'secret'],
                                 ['match', 'pilot'] + (['deliver'] if lane == 'release' else []))
                fast = [a for a in calls if a[0] == 'fastlane']
                self.assertIn('project:ios/My Game.xcodeproj', fast[0])
                self.assertIn('scheme:Fixture Game', fast[0])
                self.assertIn('external:' + ('true' if lane == 'beta' else 'false'), fast[1])
                if lane == 'release':
                    self.assertEqual(fast[2], ['fastlane', '--studio', 'other-studio', '--confirm', '--', 'submit_release'])

    def test_missing_secrets_refuse_before_mutation(self):
        for lane, missing in [('alpha', 'match'), ('beta', 'pilot'), ('release', 'deliver')]:
            with self.subTest(lane=lane):
                self.run_script('release', '--lane', lane, '--version', '1.2.3',
                                '--confirm', code=20, MISSING=missing)
                self.assert_no_mutation()

    def test_confirmation_and_invalid_input(self):
        for args in [('--lane', 'release', '--version', '1.2.3'),
                     ('--lane', 'beta', '--version', 'v1.2.3'), ('--lane', 'typo')]:
            self.run_script('release', *args, code=1)
            self.assert_no_mutation()
        self.assertEqual(self.spyder(), [])

    def test_preflight_readonly_and_no_parent_credentials(self):
        self.assertIn('READY', self.run_script('preflight'))
        self.assertEqual(self.spyder()[-1], ['fastlane', '--studio', 'squz', '--',
                                           'sync_certs', 'app_identifier:com.example.fixture'])
        self.assertEqual(list(self.root.glob('**/*api_key*.json')), [])

    def test_preflight_failure_propagates(self):
        self.run_script('preflight', code=20, MISSING='pilot')
        self.assert_no_mutation()
        self.run_script('preflight', code=37, FAIL_LANE='sync_certs')

    def test_dirty_preflight_never_syncs(self):
        self.run_script('preflight', code=1, DIRTY='1')
        self.assertEqual(self.spyder(), [])

    def test_build_default_project_and_studio(self):
        self.run_script('build', '--tag', 'v1.2.3', '--lane', 'release')
        fast = [a for a in self.spyder() if a[0] == 'fastlane'][0]
        self.assertIn('squz', fast)
        self.assertIn('project:ios/build/xcode/Fixture Game.xcodeproj', fast)

    def test_build_early_refusal(self):
        self.run_script('build', '--tag', 'v1.2.3', '--lane', 'release', code=20, MISSING='match')
        self.assert_no_mutation()

    def test_build_and_upload_failure_propagate(self):
        self.run_script('build', '--tag', 'v1.2.3', '--lane', 'release', code=37, FAIL_LANE='build_ipa')
        for lane in ('build_ipa', 'upload_testflight', 'submit_release'):
            with self.subTest(lane=lane):
                self.calls.unlink()
                self.run_script('release', '--lane', 'release', '--version', '1.2.3',
                                '--confirm', code=37, FAIL_LANE=lane)
                fast = [a for a in self.spyder() if a[0] == 'fastlane']
                self.assertEqual(fast[-1][fast[-1].index('--') + 1], lane)

    def test_alpha_dry_run_only_needs_match(self):
        self.run_script('release', '--lane', 'alpha', SHIP_DRY_RUN='1', MISSING='pilot')
        self.assertEqual([a[-1] for a in self.spyder() if a[0] == 'secret'], ['match'])
        self.assertEqual(len([a for a in self.spyder() if a[0] == 'fastlane']), 1)

    def run_make(self, *args, **env):
        return subprocess.run(['/usr/bin/make', *args], cwd=self.root,
                              env=self.env | env, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)

    def test_make_checks_before_codegen(self):
        for args, env in [(('ship-alpha',), {'MISSING': 'match'}),
                          (('ship-beta', 'VERSION=1.2.3'), {'MISSING': 'pilot'}),
                          (('ship-release', 'VERSION=1.2.3', 'CONFIRM=1'), {'MISSING': 'deliver'}),
                          (('ship-release', 'VERSION=1.2.3'), {})]:
            r = self.run_make(*args, **env)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            self.assert_no_mutation()

    def test_make_codegen_failure_stops_before_build(self):
        with (self.root / 'Makefile').open('a') as f:
            f.write('\t@exit 42\n')
        r = self.run_make('ship-alpha')
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertFalse(any(a[0] == 'fastlane' for a in self.spyder()))

    def test_make_preflight_metadata_and_studio(self):
        r = self.run_make('ship-preflight', 'STUDIO=custom', 'APPLE_TEAM_ID=TEAM')
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertTrue(all(a[a.index('--studio') + 1] == 'custom' for a in self.spyder()))

    def test_make_forwards_studio_project(self):
        r = self.run_make('ship-alpha', 'STUDIO=custom', 'SHIP_PROJECT=ios/Custom Project.xcodeproj')
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertTrue((self.root / 'codegen').exists())
        self.assertTrue(all(a[a.index('--studio') + 1] == 'custom' for a in self.spyder()))
        fast = [a for a in self.spyder() if a[0] == 'fastlane'][0]
        self.assertIn('project:ios/Custom Project.xcodeproj', fast)


if __name__ == '__main__':
    unittest.main(verbosity=2)
