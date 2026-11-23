# -*- mode: python ; coding: utf-8 -*-

a = Analysis(
    ['launcher\\StarFoxLauncher.py'],
    pathex=['launcher'],
    binaries=[],
    datas=[
        ('launcher\\assets\\starfox.ico', 'assets'),
        ('runtime\\starfox.png', 'assets'),
        ('runtime\\starfox_emblem.png', 'assets'),
        ('launcher\\starfox_version.txt', '.'),
        ('tools\\data\\expected-starfox102.json', 'tools\\data'),
        ('tools\\extract_nup.py', 'tools'),
    ],
    hiddenimports=['PIL._tkinter_finder', 'starfox_dump'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='StarFoxLauncher',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['launcher\\assets\\starfox.ico'],
    version='launcher\\starfox_launcher_ver.txt',
)
