"""System32 d3dcompiler_47 for the source overlay: Wine's d3dcompiler over a pinned vkd3d-shader, both built for x86_64 Windows.

Windows ships the HLSL compiler with the system and ANGLE (Firefox, Chromium) loads it from System32. Wine 10.0 bundles vkd3d 1.14,
which crashes on and rejects ANGLE's WebRender shaders; the pinned master revision compiles them.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tarfile
import urllib.request
import hashlib
import re

VKD3D_REVISION = 'dcefd8b0'
HEADERS = {
    'Vulkan-Headers': ('https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.335.0.tar.gz',
                       '269e95cc5138ea0a0d52fcb0ee19102add2560fedf5a43b1b5c17780c2775764'),
    'SPIRV-Headers': ('https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.335.0.tar.gz',
                      '1c47ca6342ebe86f57b46b8dbeb266fa655a1ca8e10d07e45370ff2d9c36312e'),
}
OBJECTS = ['asmparser', 'blob', 'bytecodewriter', 'compiler', 'reflection', 'utils', 'asmshader.yy', 'asmshader.tab']


def run(*args, cwd=None, env=None):
    subprocess.run([str(arg) for arg in args], cwd=cwd, env=env, check=True)


def headers(work):
    roots = {}
    for name, (url, digest) in HEADERS.items():
        archive = work / f'{name}.tar.gz'
        if not archive.exists():
            urllib.request.urlretrieve(url, archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            raise RuntimeError(f'{name} archive checksum mismatch')
        target = work / f'{name}-vulkan-sdk-1.4.335.0'
        if not target.exists():
            with tarfile.open(archive) as bundle:
                bundle.extractall(work, filter='data')
        roots[name] = target / 'include'
    return roots


def build(output, wine, work):
    """Builds d3dcompiler_47.dll into output from the configured x86_64 Wine build `wine`; `work` is a private scratch directory."""
    work.mkdir(parents=True, exist_ok=True)
    env = os.environ | {'PATH': subprocess.run(['brew', '--prefix', 'bison'], capture_output=True, text=True,
                                               check=True).stdout.strip() + '/bin:' + os.environ['PATH']}
    include = headers(work)
    source, build_dir = work / 'vkd3d', work / 'vkd3d-x64'
    if not (source / '.git').exists():
        run('git', 'clone', 'https://gitlab.winehq.org/wine/vkd3d.git', source)
    run('git', 'fetch', 'origin', cwd=source)
    run('git', 'reset', '--hard', VKD3D_REVISION, cwd=source)
    run('make', 'tools/widl/widl', cwd=wine, env=env)
    if not (source / 'configure').exists():
        run('./autogen.sh', cwd=source, env=env)
    build_dir.mkdir(exist_ok=True)
    if not (build_dir / 'Makefile').exists():
        run(source / 'configure', '--host=x86_64-w64-mingw32', '--enable-static', '--disable-shared', '--disable-tests',
            '--disable-doxygen-doc', '--without-ncurses', '--without-xcb', '--without-spirv-tools',
            'SONAME_LIBVULKAN=vulkan-1.dll', f'WIDL={wine / "tools/widl/widl"}', 'CFLAGS=-O2 -g -fno-lto',
            f'CPPFLAGS=-D__USE_MINGW_ANSI_STDIO=0 -I{include["Vulkan-Headers"]} -I{include["SPIRV-Headers"]}', cwd=build_dir, env=env)
    # Headers generated from IDL and the SPIR-V grammar, which the git tree does not carry.
    generated = sorted(set(re.findall(r'include/vkd3d_\w+\.h', (build_dir / 'Makefile').read_text())) |
                       {'include/private/spirv_grammar.h', 'include/private/vkd3d_version.h'})
    run('make', *generated, cwd=build_dir, env=env)
    run('make', '-j4', 'libvkd3d-shader.la', cwd=build_dir, env=env)
    # vkd3d calls _strtof_l, which Raven's ucrtbase lacks; the shim gives it through _strtod_l.
    shader = work / 'libvkd3d-shader-raven.a'
    run('x86_64-w64-mingw32-objcopy', '--redefine-sym', '_strtof_l=raven_strtof_l', '--redefine-sym',
        '__imp__strtof_l=__imp_raven_strtof_l', build_dir / '.libs/libvkd3d-shader.a', shader)
    shim = work / 'd3dcompiler_crt.o'
    run('x86_64-w64-mingw32-gcc', '-O2', '-fno-builtin', '-c', Path(__file__).with_name('d3dcompiler_crt.c'), '-o', shim)
    run('make', '-j4', *[f'dlls/d3dcompiler_47/x86_64-windows/{name}.o' for name in OBJECTS], 'dlls/d3dcompiler_47/version.res',
        'dlls/winecrt0/x86_64-windows/libwinecrt0.a', 'dlls/ucrtbase/x86_64-windows/libucrtbase.a',
        'dlls/kernel32/x86_64-windows/libkernel32.a', 'dlls/ntdll/x86_64-windows/libntdll.a', 'tools/winegcc/winegcc', cwd=wine, env=env)
    spec = next(line.split('=', 1)[1].strip() for line in (wine / 'Makefile').read_text().splitlines() if line.startswith('srcdir'))
    linked = work / 'd3dcompiler_47.dll'
    run(wine / 'tools/winegcc/winegcc', '-o', linked, '--wine-objdir', wine, '-b', 'x86_64-w64-mingw32', '-shared',
        Path(spec) / 'dlls/d3dcompiler_47/d3dcompiler_47.spec', '-Wb,--prefer-native',
        *[wine / f'dlls/d3dcompiler_47/x86_64-windows/{name}.o' for name in OBJECTS], wine / 'dlls/d3dcompiler_47/version.res',
        shader, build_dir / '.libs/libvkd3d-common.a', shim, *[wine / f'dlls/{name}/x86_64-windows/lib{name}.a' for name in
        ('winecrt0', 'ucrtbase', 'kernel32', 'ntdll')], cwd=wine)
    run('x86_64-w64-mingw32-strip', '-o', output, linked)
    shutil.copy2(source / 'COPYING', output.parent.parent / 'licenses/VKD3D.COPYING')
    return {'vkd3d': VKD3D_REVISION}
