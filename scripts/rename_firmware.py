"""
PlatformIO post-build script: copy firmware.bin to convenient artifact names
in the same build directory.

Default outputs:
  .pio/build/default/firmware-x3-x4.bin
  .pio/build/x4-pro/firmware-x4-pro.bin
  .pio/build/x4-classic/firmware-x4-classic.bin
  .pio/build/sticky/firmware-sticky.bin
  .pio/build/x4-pro/firmware-x4-pro.bin

Release-candidate outputs when CROSSINK_RC_ARTIFACTS=1:
  .pio/build/default/firmware-x3-x4-<branch>-<hash>-RC.bin
  .pio/build/sticky/firmware-sticky-<branch>-<hash>-RC.bin
  .pio/build/x4-pro/firmware-x4-pro-<branch>-<hash>-RC.bin
  .pio/build/x4-classic/firmware-x4-classic-<branch>-<hash>-RC.bin

Release outputs when CROSSINK_RELEASE_VERSION is set:
  .pio/build/default/firmware-x3-x4-v<version>.bin
  .pio/build/x4-pro/firmware-x4-pro-v<version>.bin
  .pio/build/x4-classic/firmware-x4-classic-v<version>.bin
  .pio/build/sticky/firmware-sticky-v<version>.bin
  .pio/build/x4-pro/firmware-x4-pro-v<version>.bin
"""

import os
import re
import shutil
import subprocess
import sys


def _copy_artifact(src, dst):
    shutil.copy(src, dst)
    print(f'Firmware copied to: {dst}')


def _get_project_option(env, name):
    try:
        value = env.GetProjectOption(name)
    except Exception:
        return None
    if isinstance(value, str):
        value = value.strip()
    return value or None


def _get_git_value(project_dir, *args, fallback):
    try:
        return subprocess.check_output(
            ['git', *args],
            text=True,
            stderr=subprocess.PIPE,
            cwd=project_dir,
        ).strip()
    except Exception:
        return fallback


def _get_git_branch(project_dir):
    branch = _get_git_value(
        project_dir,
        'rev-parse',
        '--abbrev-ref',
        'HEAD',
        fallback='unknown',
    )
    if branch == 'HEAD':
        return _get_git_value(
            project_dir,
            'rev-parse',
            '--short',
            'HEAD',
            fallback='unknown',
        )
    return branch


def _sanitize_branch(branch):
    if branch.startswith('release/'):
        branch = branch[len('release/'):]
    branch = branch.strip()
    branch = re.sub(r'[^A-Za-z0-9._-]+', '-', branch)
    branch = branch.strip('-.')
    return branch or 'unknown'


def _get_firmware_device_type(env):
    return _get_project_option(env, 'custom_firmware_device_type') or env['PIOENV']


def _get_rc_artifact_name(project_dir, env):
    device_type = _get_firmware_device_type(env)
    branch = (
        _get_project_option(env, 'custom_rc_branch')
        or os.environ.get('CROSSINK_RC_BRANCH')
        or _get_git_branch(project_dir)
    )
    short_hash = (
        _get_project_option(env, 'custom_rc_hash')
        or os.environ.get('CROSSINK_RC_HASH')
        or _get_git_value(
        project_dir,
        'rev-parse',
        '--short',
        'HEAD',
        fallback='00000',
    )
    )
    branch = _sanitize_branch(branch)
    short_hash = re.sub(r'[^A-Za-z0-9]+', '', short_hash)[:12] or '00000'
    return f'firmware-{device_type}-{branch}-{short_hash}-RC.bin'


def _is_rc_artifact_build(env):
    flag = _get_project_option(env, 'custom_rc_artifacts') or os.environ.get('CROSSINK_RELEASE_VERSION')
    return str(flag).strip().lower() in {'1', 'true', 'yes', 'on'}


def _get_release_version(env):
    version = _get_project_option(env, 'custom_release_version') or os.environ.get('CROSSINK_RELEASE_VERSION')
    if not version:
        return None
    version = version.strip()
    if not version:
        return None
    if not version.startswith('v'):
        version = f'v{version}'
    return re.sub(r'[^A-Za-z0-9._-]+', '-', version)


def rename_firmware(source, target, env):
    project_dir = env['PROJECT_DIR']
    src = str(target[0])
    build_dir = os.path.dirname(src)

    device_type = _get_firmware_device_type(env)
    default_dst = os.path.join(build_dir, f'firmware-{device_type}.bin')
    _copy_artifact(src, default_dst)

    if _is_rc_artifact_build(env):
        rc_dst = os.path.join(build_dir, _get_rc_artifact_name(project_dir, env))
        _copy_artifact(src, rc_dst)

    release_version = _get_release_version(env)
    if release_version:
        release_dst = os.path.join(build_dir, f'firmware-{device_type}-{release_version}.bin')
        _copy_artifact(src, release_dst)


try:
    Import('env')                                           # noqa: F821  # type: ignore[name-defined]
    env.AddPostAction(                                      # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.bin',
        rename_firmware,
    )
except NameError:
    print('rename_firmware.py: must be run via PlatformIO', file=sys.stderr)
