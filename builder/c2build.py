#!/usr/bin/env python3
"""Carmageddon 2 Android port - APK builder.

Builds a ready-to-install APK from YOUR OWN copy of the game (GOG release 2.1.0.28):

  installer --innoextract--> game files --lift.py--> C --zig cc--> libgame.so
  template APK (runtime, built by CI) + libgame.so + game data --> signed APK

    carma2-apk-builder                                   (GUI)
    carma2-apk-builder setup_carmageddon2_2.1.0.28.exe   (command line)
        [-o carmageddon2.apk] [--abi arm64-v8a,x86_64] [--jobs N]

The input can also be a folder with the installed game (the one containing CARMA2_HW.EXE).
"""
import argparse, concurrent.futures, glob, hashlib, io, os, platform, runpy, shutil, subprocess, sys, tarfile
import threading, time, urllib.request, zipfile

VERSION = '1.2.0'
ZIG_VERSION = '0.15.2'
ZIG_SHA256 = {   # https://ziglang.org/download/index.json
    'x86_64-linux': '02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239',
    'aarch64-linux': '958ed7d1e00d0ea76590d27666efbf7a932281b3d7ba0c6b01b0ff26498f667f',
    'x86_64-windows': '3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c',
    'aarch64-macos': '3cc2bab367e185cdfb27501c4b30b1b0653c28d9f73df8dc91488e66ece5fa6b',
    'x86_64-macos': '375b6909fc1495d16fc2c7db9538f707456bfc3373b14ee83fdd3e22b3d43f7f',
}
# the recompiled code, hooks and patches are made for exactly these files (GOG release 2.1.0.28)
GAME_SHA256 = {
    'CARMA2_HW.EXE': '9b896c2cbb170c01b3e9f904ce5e1808db29fe5b51184a5d55a6d19b1799b58d',
    '3dfx_win.bdd': '8ee8d5c1c359de9695b74f504faa6b522c7e01cdf2d88cea1c1cc389ec4e9301',
}
ABIS = {'arm64-v8a': 'aarch64-linux-android', 'x86_64': 'x86_64-linux-android'}
GEN_FLAGS = ['-O2', '-fPIC', '-ffreestanding', '-DC2_FREESTANDING', '-fno-strict-aliasing', '-fwrapv',
             '-fsigned-char', '-fvisibility=hidden', '-g0', '-w']

FROZEN = getattr(sys, 'frozen', False)
HERE = os.path.dirname(os.path.abspath(sys.executable if FROZEN else __file__))
RES = getattr(sys, '_MEIPASS', os.path.dirname(HERE))   # bundled files (frozen) or the repository
WIN = os.name == 'nt'
NOWINDOW = 0x08000000 if WIN else 0                       # don't flash console windows from the GUI


class BuildError(Exception):
    pass


def user_dir(kind):
    """kind: 'cache' (work files, toolchain) or 'config' (signing key)"""
    home = os.path.expanduser('~')
    if WIN:
        base = os.environ.get('LOCALAPPDATA' if kind == 'cache' else 'APPDATA') or home
    elif sys.platform == 'darwin':
        base = os.path.join(home, 'Library', 'Caches' if kind == 'cache' else 'Application Support')
    else:
        base = os.environ.get('XDG_CACHE_HOME' if kind == 'cache' else 'XDG_CONFIG_HOME') or \
            os.path.join(home, '.cache' if kind == 'cache' else '.config')
    return os.path.join(base, 'carma2-apk-builder')


def res(*p):
    return os.path.join(RES, *p)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def tool_cmd(name, *args):
    """run one of the bundled Python tools (lift.py, recode_video.py) in a child process"""
    if FROZEN:
        return [sys.executable, '--tool', name, *args]
    return [sys.executable, os.path.abspath(__file__), '--tool', name, *args]


def selftest():
    """CI check of a frozen build: everything the tools import is bundled"""
    import pefile, capstone, av, numpy, PIL.Image, tkinter, lzma, apk, gui  # noqa: F401
    capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for f in ('lift.py', 'recode_video.py', 'hooks.txt', 'hooks_bdd.txt', 'patches.txt'):
        assert os.path.isfile(res('tools', f)), f
    for f in ('rtapi.h', 'lifted.h', 'cpu.h', 'game_glue.c'):
        assert os.path.isfile(res('runtime', f)), f
    print('selftest ok: capstone %s, av %s, template %s' % (capstone.__version__, av.__version__,
                                                           os.path.isfile(res('template', 'template.apk'))))


def run_tool(name, args):
    if name == 'selftest':
        return selftest()
    script = res('tools', name + '.py')
    sys.argv = [script] + list(args)
    sys.path.insert(0, os.path.dirname(script))
    runpy.run_path(script, run_name='__main__')


class Builder:
    def __init__(self, src, out, abis=('arm64-v8a',), jobs=None, log=print, progress=None, template=None):
        self.src, self.out, self.abis = src, os.path.abspath(out), list(abis)
        self.jobs = jobs or os.cpu_count() or 4
        self.log = log
        self.progress = progress or (lambda frac, text: None)
        self.work = os.path.join(user_dir('cache'), 'work')
        self.template = template
        self.cancelled = False

    # ------------------------------------------------------------------------------------- helpers
    def run(self, cmd, cwd=None):
        if self.cancelled:
            raise BuildError('cancelled')
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=NOWINDOW)
        out = p.stdout.decode('utf-8', 'replace')
        if p.returncode:
            raise BuildError('%s failed (exit %d):\n%s' % (os.path.basename(cmd[0]) if cmd[0] != sys.executable
                                                           else ' '.join(cmd[1:4]), p.returncode, out[-4000:]))
        return out

    def parallel(self, jobs, what, frac0, frac1):
        """run (label, cmd) jobs in parallel with a progress range"""
        done = 0
        with concurrent.futures.ThreadPoolExecutor(self.jobs) as ex:
            futs = {ex.submit(self.run, cmd): label for label, cmd in jobs}
            try:
                for f in concurrent.futures.as_completed(futs):
                    f.result()
                    done += 1
                    self.progress(frac0 + (frac1 - frac0) * done / len(jobs), '%s (%d/%d)' % (what, done, len(jobs)))
            except BaseException:
                self.cancelled = True
                raise

    # ------------------------------------------------------------------------------------- tools
    def find_innoextract(self):
        exe = 'innoextract.exe' if WIN else 'innoextract'
        for d in (os.path.join(HERE, 'innoextract'), HERE, res('innoextract')):
            p = os.path.join(d, exe)
            if os.path.isfile(p):
                return p
        p = shutil.which('innoextract')
        if p:
            return p
        raise BuildError('innoextract not found (it is included in the release download; on Linux you can '
                         'also install it with your package manager), or select the installed game folder instead')

    def find_zig(self):
        exe = 'zig.exe' if WIN else 'zig'
        if os.environ.get('C2_ZIG'):
            return os.environ['C2_ZIG']
        for d in (os.path.join(HERE, 'zig'), res('zig'), os.path.join(user_dir('cache'), 'zig-' + ZIG_VERSION)):
            p = os.path.join(d, exe)
            if os.path.isfile(p):
                return p
        return self.download_zig()

    def download_zig(self):
        """light edition: fetch the pinned Zig toolchain once (checked against its SHA-256)"""
        m = platform.machine().lower()
        arch = {'amd64': 'x86_64', 'x86_64': 'x86_64', 'arm64': 'aarch64', 'aarch64': 'aarch64'}.get(m)
        osn = 'windows' if WIN else 'macos' if sys.platform == 'darwin' else 'linux'
        key = '%s-%s' % (arch, osn)
        if key not in ZIG_SHA256:
            raise BuildError('no Zig toolchain for this computer (%s); install zig %s and set C2_ZIG' % (key, ZIG_VERSION))
        name = 'zig-%s-%s' % (key, ZIG_VERSION)
        url = 'https://ziglang.org/download/%s/%s%s' % (ZIG_VERSION, name, '.zip' if WIN else '.tar.xz')
        dest = os.path.join(user_dir('cache'), 'zig-' + ZIG_VERSION)
        self.log('Downloading the Zig C compiler (one time, ~%s MB): %s' % ('90' if WIN else '50', url))
        os.makedirs(user_dir('cache'), exist_ok=True)
        tmp = dest + '.download'
        h = hashlib.sha256()
        with urllib.request.urlopen(url, timeout=60) as r, open(tmp, 'wb') as f:
            total = int(r.headers.get('Content-Length') or 0)
            got = 0
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
                h.update(b)
                got += len(b)
                if total:
                    self.progress(0.02 * got / total, 'downloading Zig %d%%' % (100 * got // total))
        if h.hexdigest() != ZIG_SHA256[key]:
            os.remove(tmp)
            raise BuildError('the downloaded Zig archive has the wrong checksum')
        unpack = dest + '.unpack'
        shutil.rmtree(unpack, ignore_errors=True)
        if WIN:
            zipfile.ZipFile(tmp).extractall(unpack)
        else:
            with tarfile.open(tmp, 'r:xz') as t:
                t.extractall(unpack, filter='tar') if hasattr(tarfile, 'data_filter') else t.extractall(unpack)
        os.remove(tmp)
        shutil.rmtree(dest, ignore_errors=True)
        os.replace(os.path.join(unpack, name), dest)
        shutil.rmtree(unpack, ignore_errors=True)
        return os.path.join(dest, 'zig.exe' if WIN else 'zig')

    def find_template(self):
        for p in (self.template, os.path.join(HERE, 'template.apk'), res('template', 'template.apk'),
                  os.path.join(RES, 'android', 'app', 'build', 'outputs', 'apk', 'release', 'app-release-unsigned.apk')):
            if p and os.path.isfile(p):
                return p
        raise BuildError('template.apk (the prebuilt runtime) not found')

    # ------------------------------------------------------------------------------------- steps
    def game_files(self):
        """locate (and extract if needed) the game; returns the folder with CARMA2_HW.EXE"""
        src = os.path.abspath(self.src)
        if os.path.isdir(src):
            root = src
        else:
            if not os.path.isfile(src):
                raise BuildError('not found: ' + src)
            root = os.path.join(self.work, 'game')
            stamp = os.path.join(root, '.source')
            key = '%s %d' % (os.path.basename(src), os.path.getsize(src))
            if not (os.path.isfile(stamp) and open(stamp).read() == key):
                self.log('Extracting the installer...')
                self.progress(0.03, 'extracting the installer')
                shutil.rmtree(root, ignore_errors=True)
                os.makedirs(root)
                self.run([self.find_innoextract(), '-s', '-d', root, src])
                with open(stamp, 'w') as f:
                    f.write(key)
        for dirpath, dirs, files in os.walk(root):
            if 'CARMA2_HW.EXE' in files:
                return dirpath
        raise BuildError('CARMA2_HW.EXE not found in ' + self.src)

    def check_game(self, game):
        for name, want in GAME_SHA256.items():
            p = os.path.join(game, name)
            if not os.path.isfile(p):
                raise BuildError(name + ' is missing')
            if sha256(p) != want:
                raise BuildError(name + ' is not the tested GOG version (2.1.0.28); other versions are not supported')
        self.log('Game files OK (GOG 2.1.0.28)')

    def recode_videos(self, game):
        vdir = os.path.join(self.work, 'videos')
        os.makedirs(vdir, exist_ok=True)
        jobs, out = [], {}
        for f in sorted(glob.glob(os.path.join(game, 'DATA', 'CUTSCENE', '*'))):
            if f.lower().endswith('.smk'):
                c2v = os.path.join(vdir, os.path.splitext(os.path.basename(f))[0] + '.c2v')
                out[f] = c2v
                if not os.path.isfile(c2v):
                    jobs.append((f, tool_cmd('recode_video', f, c2v)))
        if jobs:
            self.log('Recoding %d videos...' % len(jobs))
            self.parallel(jobs, 'recoding videos', 0.05, 0.12)
        return out

    def lift(self, game):
        gen = os.path.join(self.work, 'gen')
        stamp = os.path.join(gen, '.stamp')
        # re-lift when the recompiler, its patches or its hooks change
        key = VERSION + ' ' + ' '.join(sha256(res('tools', f))[:16] for f in ('lift.py', 'patches.txt', 'hooks.txt', 'hooks_bdd.txt'))
        if os.path.isfile(stamp) and open(stamp).read() == key:
            return gen
        self.log('Recompiling the game code (x86 -> C, a few minutes)...')
        shutil.rmtree(gen, ignore_errors=True)
        self.parallel([
            ('exe', tool_cmd('lift', os.path.join(game, 'CARMA2_HW.EXE'), os.path.join(gen, 'exe'), '--name', 'exe',
                             '--hooks', res('tools', 'hooks.txt'), '--patches', res('tools', 'patches.txt'))),
            ('bdd', tool_cmd('lift', os.path.join(game, '3dfx_win.bdd'), os.path.join(gen, 'bdd'), '--name', 'bdd',
                             '--hooks', res('tools', 'hooks_bdd.txt'))),
        ], 'recompiling', 0.12, 0.25)
        with open(stamp, 'w') as f:
            f.write(key)
        return gen

    def compile(self, gen, abi, frac0, frac1):
        zig = self.find_zig()
        target = ABIS[abi]
        obj = os.path.join(self.work, 'obj', abi)
        os.makedirs(obj, exist_ok=True)
        srcs = [(m, f) for m in ('exe', 'bdd') for f in sorted(glob.glob(os.path.join(gen, m, '*.c')))]
        srcs.append(('glue', res('runtime', 'game_glue.c')))
        jobs, objs = [], []
        for mod, c in srcs:
            o = os.path.join(obj, '%s_%s.o' % (mod, os.path.splitext(os.path.basename(c))[0]))
            objs.append(o)
            if os.path.isfile(o) and os.path.getmtime(o) >= os.path.getmtime(c):
                continue
            inc = ['-I', res('runtime')] + (['-I', os.path.join(gen, mod)] if mod != 'glue' else [])
            jobs.append((c, [zig, 'cc', '-target', target, *GEN_FLAGS, *inc, '-c', c, '-o', o]))
        if jobs:
            self.log('Compiling %d files for %s with %d jobs (this is the long part)...' % (len(jobs), abi, self.jobs))
            # partial outputs from an interrupted run are removed by zig itself; stale ones fail the mtime check
            self.parallel(jobs, 'compiling ' + abi, frac0, frac1 - 0.02)
        lib = os.path.join(self.work, 'lib', abi, 'libgame.so')
        os.makedirs(os.path.dirname(lib), exist_ok=True)
        self.log('Linking libgame.so (%s)...' % abi)
        self.progress(frac1 - 0.02, 'linking ' + abi)
        self.run([zig, 'cc', '-target', target, '-shared', '-nostdlib', '-s', '-Wl,-z,max-page-size=16384',
                  '-Wl,-soname,libgame.so', '-Wl,--no-undefined', '-Wl,-Bsymbolic', '-Wl,--gc-sections',
                  '-o', lib, *objs])
        return lib

    def game_assets(self, game, videos):
        """(path in the game dir, file on disk) for everything the game needs at run time"""
        files = []
        for name in ('CARMA2_HW.EXE', '3dfx_win.bdd', 'DATA.TWT'):
            files.append((name, os.path.join(game, name)))
        smk = {os.path.normcase(os.path.abspath(k)): v for k, v in videos.items()}
        for top in ('DATA', 'music'):
            base = os.path.join(game, top)
            if not os.path.isdir(base):
                if top == 'music':
                    self.log('note: no music folder, the game will run without CD music')
                    continue
                raise BuildError('DATA folder missing in ' + game)
            for dirpath, dirs, fs in os.walk(base):
                dirs.sort()
                rel = os.path.relpath(dirpath, game).replace(os.sep, '/')
                if rel.upper() == 'DATA/SAVEGAME':      # saves live in the app's save dir
                    dirs[:] = []
                    continue
                for fn in sorted(fs):
                    p = os.path.join(dirpath, fn)
                    if fn.lower().endswith('.smk'):
                        c2v = smk[os.path.normcase(os.path.abspath(p))]
                        files.append((rel + '/' + os.path.basename(c2v), c2v))
                    else:
                        files.append((rel + '/' + fn, p))
        return files

    def icons(self, game, template_zip):
        """the launcher icon from the game's own .ico, in the template's icon sizes"""
        icos = glob.glob(os.path.join(game, 'goggame-*.ico'))
        if not icos:
            return {}
        try:
            from PIL import Image
        except ImportError:
            return {}
        im = Image.open(icos[0])
        sizes = sorted(im.info.get('sizes', [im.size]))
        im.size = sizes[-1]
        icon = im.convert('RGBA')
        out = {}
        for name in template_zip.namelist():
            base = os.path.basename(name)
            if not (name.startswith('res/mipmap') and base in ('ic_launcher.png', 'ic_launcher_fg.png')):
                continue
            w, h = Image.open(io.BytesIO(template_zip.read(name))).size
            if base == 'ic_launcher.png':
                img = icon.resize((w, h), Image.LANCZOS)
            else:   # adaptive icon foreground: the icon in the safe zone of a 108dp canvas, black behind it
                img = Image.new('RGBA', (w, h), (0, 0, 0, 255))
                s = int(w * 0.72)
                img.alpha_composite(icon.resize((s, s), Image.LANCZOS), ((w - s) // 2, (h - s) // 2))
            b = io.BytesIO()
            img.save(b, 'PNG')
            out[name] = b.getvalue()
        return out

    def assemble(self, game, videos, libs):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import apk
        tpl = zipfile.ZipFile(self.find_template())
        assets = self.game_assets(game, videos)
        icons = self.icons(game, tpl)
        unsigned = os.path.join(self.work, 'unsigned.apk')
        self.log('Packing the APK (%d game files)...' % len(assets))
        z = apk.ZipWriter(unsigned)
        for info in tpl.infolist():
            n = info.filename
            if n.startswith('META-INF/') and n.rsplit('.', 1)[-1] in ('SF', 'RSA', 'DSA', 'EC', 'MF'):
                continue
            if n.startswith('lib/') and n.split('/')[1] not in libs:
                continue        # an ABI without game code would crash: leave it out
            if n.endswith('/libgame.so'):
                continue        # a development template may contain one
            data = icons.get(n) or tpl.read(info)
            so = n.endswith('.so')
            z.add(n, data, compress=info.compress_type != 0 and not so, align=16384 if so else 4)
        for abi, lib in libs.items():
            with open(lib, 'rb') as f:
                z.add('lib/%s/libgame.so' % abi, f.read(), compress=False, align=16384)
        manifest = []
        stamp = hashlib.sha256(VERSION.encode())
        for i, (rel, path) in enumerate(assets):
            with open(path, 'rb') as f:
                data = f.read()
            store = rel.lower().endswith(('.ogg', '.c2v'))     # already compressed
            z.add('assets/game/' + rel, data, compress=not store)
            manifest.append(rel)
            stamp.update(('%s %d\n' % (rel, len(data))).encode())
            if i % 50 == 0:
                self.progress(0.88 + 0.08 * i / len(assets), 'packing game files')
        z.add('assets/game/manifest.txt', ('\n'.join(manifest) + '\n').encode())
        z.add('assets/game/stamp.txt', (stamp.hexdigest()[:16] + '\n').encode())
        z.close()

        self.progress(0.97, 'signing')
        keyfile = os.path.join(user_dir('config'), 'signing-key.txt')
        if os.path.isfile(keyfile):
            key = apk.load_key(keyfile)
        else:
            self.log('Creating your signing key (kept in %s, so later builds install as updates)' % keyfile)
            key = apk.make_key()
            apk.save_key(key, keyfile)
        os.makedirs(os.path.dirname(self.out) or '.', exist_ok=True)
        apk.sign_v2(unsigned, self.out, key)
        os.remove(unsigned)

    def build(self):
        t0 = time.time()
        os.makedirs(self.work, exist_ok=True)
        game = self.game_files()
        self.check_game(game)
        videos = self.recode_videos(game)
        gen = self.lift(game)
        libs = {}
        span = 0.6 / len(self.abis)
        for i, abi in enumerate(self.abis):
            libs[abi] = self.compile(gen, abi, 0.25 + i * span, 0.25 + (i + 1) * span)
        self.assemble(game, videos, libs)
        self.progress(1.0, 'done')
        self.log('Done in %d min: %s (%d MB)' % ((time.time() - t0 + 59) // 60, self.out,
                                                 os.path.getsize(self.out) >> 20))
        self.log('Install it with "adb install -r" or copy it to the phone and open it.')
        return self.out


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['--tool']:
        return run_tool(argv[1], argv[2:])
    if not argv:
        if WIN and FROZEN:
            # started by double-click: hide the console window this program got (only if it is ours alone)
            import ctypes
            k = ctypes.windll.kernel32
            if k.GetConsoleProcessList((ctypes.c_uint * 4)(), 4) <= 1:
                ctypes.windll.user32.ShowWindow(k.GetConsoleWindow(), 0)
        import gui
        return gui.main()
    ap = argparse.ArgumentParser(prog='carma2-apk-builder', description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source', help='setup_carmageddon2_2.1.0.28.exe or the installed game folder')
    ap.add_argument('-o', '--output', default='carmageddon2.apk')
    ap.add_argument('--abi', default='arm64-v8a', help='comma separated: arm64-v8a (phones), x86_64 (emulators)')
    ap.add_argument('--jobs', type=int, default=None)
    ap.add_argument('--template', help='template APK (default: the bundled one)')
    ap.add_argument('--version', action='version', version=VERSION)
    a = ap.parse_args(argv)
    abis = [x.strip() for x in a.abi.split(',') if x.strip()]
    for x in abis:
        if x not in ABIS:
            ap.error('unknown ABI ' + x)
    last = [None]

    def progress(frac, text):
        s = '[%3d%%] %s' % (frac * 100, text)
        if s != last[0]:
            last[0] = s
            print(s, flush=True)
    try:
        Builder(a.source, a.output, abis, a.jobs, template=a.template, progress=progress).build()
    except BuildError as e:
        print('error: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    if FROZEN:
        import multiprocessing
        multiprocessing.freeze_support()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.exit(main())
