"""CI provenance regression only; temporary repos are not installed SDK fixtures."""
from contextlib import ExitStack
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import run_package_contract as runner


class BuildBoundaryReached(Exception):
    """Stop the test instead of building unrelated native dependencies."""


class PackageProvenanceTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.root = base / 'consumer'
        self.salts = base / 'salts'
        self.vcpkg = base / 'runner vcpkg'
        self.other = base / 'VS' / 'VC' / 'vcpkg'
        self.toolchain = self.vcpkg / 'scripts/buildsystems/vcpkg.cmake'
        self.other_toolchain = self.other / 'scripts/buildsystems/vcpkg.cmake'
        for root in (self.root, self.salts, self.vcpkg, self.other):
            root.mkdir(parents=True)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            path = root / 'scripts/buildsystems/vcpkg.cmake'
            path.parent.mkdir(parents=True)
            path.write_text('# identity fixture, never used to configure an SDK\n')
            self.git(root, 'add', '.')
            self.git(root, '-c', 'user.name=Provenance Test',
                     '-c', 'user.email=test@localhost', 'commit', '-qm', 'fixture')
            self.git(root, 'branch', '-M', 'master')
        self.revision = self.git(self.vcpkg, 'rev-parse', 'HEAD').strip()
        self.env = dict(os.environ, VCPKG_ROOT=str(self.vcpkg),
                        DRIVER_SDK_VCPKG_ROOT=str(self.vcpkg),
                        DRIVER_SDK_VCPKG_REVISION=self.revision)
        self.manifest = self.root / 'evidence/package-linux-debug/manifest.json'
        self.cache = self.salts / 'build/linux-gcc-debug/CMakeCache.txt'
        self.configured_toolchain = None

    @staticmethod
    def git(root, *args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True)

    def command_boundary(self, argv, cwd, env, log):
        # Only native configure/build is simulated. Git identity reads are real.
        if argv[0] == 'git':
            return self.git(cwd, *argv[1:])
        if self.configured_toolchain is None or '--build' in argv:
            raise BuildBoundaryReached()
        if '--preset' in argv:
            self.cache.parent.mkdir(parents=True, exist_ok=True)
            self.cache.write_text(
                f'CMAKE_TOOLCHAIN_FILE:FILEPATH={self.configured_toolchain}\n')
        return ''

    def invoke(self, expected_salts_commit=None):
        with ExitStack() as stack:
            stack.enter_context(mock.patch.dict(os.environ, self.env, clear=True))
            stack.enter_context(mock.patch.object(runner, '__file__',
                str(self.root / 'orm/tests/driver/run_package_contract.py')))
            stack.enter_context(mock.patch.object(runner.platform, 'system', return_value='Linux'))
            stack.enter_context(mock.patch.object(runner.platform, 'machine', return_value='x86_64'))
            stack.enter_context(mock.patch.object(runner, 'run', side_effect=self.command_boundary))
            argv = ['run_package_contract.py',
                    '--salts-source', str(self.salts), '--config', 'debug']
            if expected_salts_commit is not None:
                argv.extend(['--expected-salts-commit', expected_salts_commit])
            stack.enter_context(mock.patch('sys.argv', argv))
            return runner.main()

    def assert_rejected(self, reason):
        try:
            self.invoke()
        except BuildBoundaryReached:
            self.fail(f'{reason}: native command was reached before rejection')
        except RuntimeError as error:
            self.assertRegex(str(error), reason)
        else:
            self.fail(f'{reason}: invalid provenance was accepted')

    def test_rejects_environment_root_drift_before_configure(self):
        self.env['VCPKG_ROOT'] = str(self.other)
        self.assert_rejected('vcpkg root mismatch')

    def test_rejects_revision_drift_before_configure(self):
        self.env['DRIVER_SDK_VCPKG_REVISION'] = '0' * 40
        self.assert_rejected('vcpkg revision mismatch')

    def test_rejects_missing_captured_identity_before_configure(self):
        del self.env['DRIVER_SDK_VCPKG_ROOT']
        self.assert_rejected('vcpkg cache identity is missing')

    def test_rejects_missing_actual_root_before_configure(self):
        del self.env['VCPKG_ROOT']
        self.assert_rejected('vcpkg cache identity is missing')

    def test_rejects_missing_toolchain_before_configure(self):
        self.toolchain.unlink()
        self.assert_rejected('vcpkg toolchain is missing')

    def test_rejects_non_master_salts_checkout(self):
        self.git(self.salts, 'checkout', '-qb', 'feature')
        self.assert_rejected('Salts source must be checked out at master')

    def test_accepts_detached_exact_release_commit(self):
        expected = self.git(self.salts, 'rev-parse', 'HEAD').strip()
        self.git(self.salts, 'checkout', '--detach', '-q', expected)
        with self.assertRaises(BuildBoundaryReached):
            self.invoke(expected)

    def test_rejects_wrong_exact_release_commit(self):
        expected = '0' * 40
        try:
            self.invoke(expected)
        except BuildBoundaryReached:
            self.fail('wrong exact release commit reached native build')
        except RuntimeError as error:
            self.assertRegex(str(error), 'Salts source commit mismatch')
        else:
            self.fail('wrong exact release commit was accepted')

    def test_records_matching_identity_before_configure(self):
        with self.assertRaises(BuildBoundaryReached):
            self.invoke()
        identity = json.loads(self.manifest.read_text()).get('vcpkg')
        self.assertIsNotNone(identity, 'manifest lacks the actual vcpkg identity')
        self.assertEqual(identity['root'], str(self.vcpkg.resolve()))
        self.assertEqual(identity['revision'], self.revision)
        self.assertEqual(identity['toolchain'], str(self.toolchain.resolve()))
        self.assertEqual(len(identity['toolchain_sha256']), 64)

    def test_rejects_configured_toolchain_drift_before_build(self):
        self.configured_toolchain = self.other_toolchain
        self.assert_rejected('configured vcpkg toolchain mismatch')

    def test_matching_configured_toolchain_reaches_build(self):
        self.configured_toolchain = self.toolchain
        with self.assertRaises(BuildBoundaryReached):
            self.invoke()
        identity = json.loads(self.manifest.read_text()).get('vcpkg')
        self.assertIsNotNone(identity, 'manifest lacks the checked CMake toolchain')
        self.assertEqual(identity['configured_toolchain'], str(self.toolchain.resolve()))


if __name__ == '__main__':
    unittest.main()
