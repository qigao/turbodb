"""Restore published SDKs with release-API SHA-256 checks before configuration."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile


def run(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--rid', required=True)
parser.add_argument('--host-rid')
parser.add_argument('--profile')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
versions = {repo: os.environ[key] for repo, key in {
    'vcpkg-cache': 'VCPKG_CACHE_VERSION', 'salts-base': 'SALTS_BASE_VERSION',
    'cmeta': 'CMETA_VERSION', 'salts-utils': 'SALTS_UTILS_VERSION',
    'salts-cnet': 'SALTS_CORE_VERSION'}.items()}
profiles = {
    'windows-x64': ('windows-msvc-x64', 'x64-windows'),
    'linux-x64': ('linux-clang-x64', 'x64-linux'),
    'linux-arm64': ('linux-clang-arm64', 'arm64-linux'),
    'macos-arm64': ('macos-clang-arm64', 'arm64-osx'),
    'android-arm64-v8a': ('android-arm64', 'arm64-android'),
    'android-x64': ('android-x64', 'x64-android'),
}
profile, triplet = profiles[args.rid]
profile = args.profile or profile
host = args.host_rid or args.rid
if host.startswith('android-'):
    raise ValueError('--host-rid must select the machine running the generators')
if profile == 'windows-llvm-mingw-x64':
    triplet = 'qigao-x64-windows-llvm-mingw'
if profile.startswith('linux-gcc-'):
    triplet = 'arm64-linux' if profile.endswith('arm64') else 'x64-linux'

destination = root / 'build/dependencies'
destination.mkdir(parents=True, exist_ok=True)
infra = Path(os.environ['VCPKG_CACHE_REPOSITORY_ROOT'])
if run('git', '-C', infra, 'rev-parse', 'HEAD') != run('git', '-C', infra, 'rev-parse', 'v' + versions['vcpkg-cache'] + '^{commit}'):
    raise ValueError('VCPKG_CACHE_REPOSITORY_ROOT must select the pinned published release')
if run('git', '-C', infra, 'status', '--porcelain'):
    raise ValueError('The selected vcpkg-cache release checkout has local modifications')
spec = importlib.util.spec_from_file_location('archive_transport', infra / 'scripts/setup-compiler-toolchain.py')
transport = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transport)
archives = destination / 'archives'
archives.mkdir(exist_ok=True)
environment = {'VCPKG_CACHE_REPOSITORY_ROOT': infra.as_posix(), 'VCPKG_TARGET_TRIPLET': triplet}


def release(repo):
    data = json.loads(run('gh', 'api', f'repos/qigao/{repo}/releases/tags/v{versions[repo]}'))
    if data['draft'] or data['prerelease'] or data['tag_name'] != 'v' + versions[repo]:
        raise ValueError('Expected an exact published stable release: ' + repo)
    return data


def download(data, name):
    assets = [a for a in data['assets'] if a['name'] == name]
    if len(assets) != 1 or not re.fullmatch(r'sha256:[0-9a-f]{64}', assets[0].get('digest', '')):
        raise ValueError('Missing unique SHA-256-qualified release asset: ' + name)
    asset = assets[0]
    archive = archives / name
    if archive.exists():
        with archive.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != asset['digest'][7:] or archive.stat().st_size != asset['size']:
            raise ValueError('Cached release archive differs from its published digest: ' + name)
    else:
        transport.verified_download(asset['browser_download_url'], archive, asset['digest'][7:], asset['size'])
    return archive, asset['digest'][7:]


def sdk(repo, prefix, config, asset_profile):
    selected = release(repo)
    name = f'{repo}-{versions[repo]}-{asset_profile}.tar'
    if repo == 'salts-base':
        metadata, _ = download(selected, f'{repo}-{versions[repo]}-manifest.json')
        manifest = json.loads(metadata.read_text(encoding='utf-8-sig'))
        producer = next(s for s in manifest['sdks'] if s['platform'] == asset_profile)
        asset = next(a for a in selected['assets'] if a['name'] == name)
        if (manifest['schemaVersion'] != 1 or manifest['name'] != 'SaltsBase'
                or manifest['repository'] != 'https://github.com/qigao/salts-base'
                or manifest['version'] != versions[repo] or manifest['tag'] != selected['tag_name']
                or manifest['ci']['conclusion'] != 'success' or producer['file'] != name
                or producer['sizeBytes'] != asset['size']
                or 'sha256:' + producer['sha256'] != asset['digest']):
            raise ValueError('SaltsBase release manifest does not qualify the selected SDK')
    archive, digest = download(selected, name)
    target = destination / f'{repo}-{versions[repo]}-{asset_profile}'
    receipt = {'sha256': digest, 'repository': repo, 'version': versions[repo], 'profile': asset_profile}
    if target.exists():
        if json.loads((target / 'receipt.json').read_text()) != receipt:
            raise ValueError('SDK receipt differs; choose a fresh destination: ' + str(target))
    else:
        # Python's data filter rejects absolute/escaping paths and special files.
        with tempfile.TemporaryDirectory(dir=destination) as temporary:
            staging = Path(temporary) / 'sdk'
            staging.mkdir()
            with tarfile.open(archive) as package:
                package.extractall(staging, filter='data')
            (staging / 'receipt.json').write_text(json.dumps(receipt) + '\n')
            staging.rename(target)
    if not (target / config).is_file():
        raise ValueError('Released SDK is incomplete: ' + str(target / config))
    environment[prefix + '_ROOT'] = target.as_posix()
    environment[prefix.replace('SALTSCORE', 'SALTS_CORE') + '_VERSION'] = versions[repo]


sdk('salts-base', 'SALTS_BASE', 'lib/cmake/SaltsBase/SaltsBaseConfig.cmake', profile)
sdk('cmeta', 'CMETA', 'lib/cmake/CMeta/CMetaConfig.cmake', profile)
sdk('salts-utils', 'SALTS_UTILS', 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake', profile)
cnet_profile = {'linux-clang-x64': 'linux-x64', 'linux-clang-arm64': 'linux-arm64',
                'macos-clang-arm64': 'macos-arm64',
                'linux-clang-x64-tsan': 'linux-x64-tsan',
                'windows-llvm-mingw-x64': 'windows-x64'}.get(profile, profile)
sdk('salts-cnet', 'SALTSCORE', 'lib/cmake/SaltsCore/SaltsCoreConfig.cmake', cnet_profile)

# The shared helper owns preinstalled vcpkg and generator archive extraction.
cache = release('vcpkg-cache')
for name, payload, required, prefix in [
    (f'installed-{triplet}.tar.xz', 'vcpkg_installed', f'{triplet}/include/zstd.h', 'VCPKG_INSTALLED_DIR'),
    ('Qigao.Re2c.Binary.4.6.7.nupkg', 'tools/' + host,
     'bin/re2c.exe' if host.startswith('windows') else 'bin/re2c', 'RE2C_ROOT'),
]:
    archive, digest = download(cache, name)
    target = destination / (Path(name).stem + '-' + host)
    lock = destination / (name + '.lock.json')
    # Host tool bytes are also published as public release assets.
    source = {'kind': 'url', 'url': next(a['browser_download_url'] for a in cache['assets'] if a['name'] == name)}
    if name.endswith('.nupkg'):
        source = {'kind': 'package', 'feed': 'https://nuget.pkg.github.com/qigao/index.json',
                  'id': 'Qigao.Re2c.Binary', 'version': '4.6.7'}
    if name.endswith('.tar.xz'):
        os.environ['VCPKG_TARGET_TRIPLET'] = triplet
        run('python', infra / 'scripts/restore-native-dependency.py', 'resolve-release',
            root / 'cmake/dependencies/vcpkg-cache.json', '--output-lock', lock)
    else:
        lock.write_text(json.dumps({'schema': 1, 'source': source, 'sha256': digest}) + '\n')
    command = ['python', str(infra / 'scripts/restore-native-dependency.py'), 'locked', str(lock),
               '--destination', str(target), '--payload', payload, '--require', required, '--reuse']
    if name.endswith('.nupkg'):
        command += ['--archive', str(archive), '--executable', required]
    result = json.loads(run(*command))
    environment[prefix] = result['root']
environment['VCPKG_CACHE_VERSION'] = versions['vcpkg-cache']
output = root / 'build/native-sdk-environment.json'
output.write_text(json.dumps(environment, indent=2) + '\n')
print(output)
