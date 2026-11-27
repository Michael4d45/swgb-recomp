"""The tools' own tests, no game needed: tools/soak.py's scripts and log reading,
tools/lift_check.py, tools/smooth_check.py, tools/test.py's image and --fps-log readers, perf_ab.py
and mp_pair.py's sync compare.

    py -3 -m unittest -v tests.unit.test_tools        (tools/test.py --level unit runs it)
"""
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

import lift_check  # noqa: E402
import res_check  # noqa: E402
import smooth_check  # noqa: E402
import mp_pair  # noqa: E402
import perf_ab  # noqa: E402
import soak  # noqa: E402
import test as runner  # noqa: E402

LOG = r"""  registry: HKCU is private to A:\Games\star\games\testing-base-0
entering 0x00632881
[host] open data\genie.dat -> ok
SetDisplayMode(800, 600, 8)
[keys] 20.0s click 345,250
[keys] 21.0s click 600,168
[host] open sound\stream\won1.mp3 -> ok
[keys] 30.0s key 48
ICALL: unresolved VA 0x005648A0 from 0x00406E20
  nearest below: sub_0056480C (+0x94)
[watchdog] 60 s: stopping
"""


class SoakTools(unittest.TestCase):
    def test_script_sorted(self):
        s = soak.Script()
        s.at(5, 'b')
        s.click(1, (10, 20))
        s.at(3, 'shot=x.bmp')
        self.assertEqual(s.text().splitlines(), ['1:click:10:20', '3:shot=x.bmp', '5:b'])

    def test_analyse(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'run.log')
            with open(p, 'w') as f:
                f.write(LOG)
            m = soak.analyse(p, '')
        self.assertEqual(len(m['events']), 1)
        self.assertIn('0x005648A0', m['events'][0]['line'])
        self.assertEqual(len(m['events'][0]['keys_before']), 3)
        self.assertEqual(m['modes'], ['800x600'])
        self.assertTrue(m['watchdog'])
        self.assertEqual(m['endings'], [['21', 'victory']])

    def test_pick(self):
        s = soak.Script()
        t = soak.pick(soak.DD_SIZE, 2, s, 10)
        self.assertEqual(t, 12.5)
        (ax, ay), (ix, iy), step = soak.DD_SIZE
        self.assertEqual(s.steps, [(10, 'click:%d:%d' % (ax, ay)), (11.2, 'click:%d:%d' % (ix, iy + 2 * step))])


class SmoothCheck(unittest.TestCase):
    def log(self, smooth):
        """A unit that walks 6 px a step (50 ms), drawn every 10 ms; a tree that stands."""
        lines = []
        for frame in range(60):
            ms = 1000 + 10 * frame
            step = frame // 5
            sx = 100 + 6 * step
            f = (frame % 5) / 5.0
            ox = round((f - 1) * 6) if smooth and step else 0
            lines.append('%d %d %d 0A000000 7 %.4f 10.0000 %d 50 %d 0' % (ms, step, 1000 + 50 * step, 1 + 0.1 * step, sx, ox))
            lines.append('%d %d %d 0B000000 8 5.0000 5.0000 300 80 0 0' % (ms, step, 1000 + 50 * step))
        return '\n'.join(lines) + '\nnot a draw line\n'

    def analyse(self, smooth):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'draw.log')
            open(p, 'w').write(self.log(smooth))
            return smooth_check.analyse(p)

    def test_plain_and_smooth(self):
        plain = self.analyse(False)
        self.assertEqual(len(plain), 1)          # the tree never moved
        m = plain[0]
        self.assertEqual((m['id'], m['steps']), (7, 11))
        self.assertEqual(m['plain_max'], 6.0)
        self.assertGreater(m['plain_still'], 0.7)
        self.assertFalse(m['smoothed'])
        m = self.analyse(True)[0]
        self.assertTrue(m['smoothed'])
        self.assertLessEqual(m['smooth_max'], 2.0)
        self.assertLess(m['smooth_still'], m['plain_still'])


class LiftCheck(unittest.TestCase):
    def write_gen(self, d, bodies):
        os.makedirs(d, exist_ok=True)
        src = '#include "recomp_types.h"\n\n' + ''.join('void %s(void) {\n%s\n}\n\n' % (n, b) for n, b in bodies)
        open(os.path.join(d, 'recomp_0000.c'), 'w').write(src)
        open(os.path.join(d, 'recomp_dispatch.c'), 'w').write(
            'const x[] = {\n' + ''.join('    { 0x%su, %s },\n' % (n[4:], n) for n, _ in bodies) + '};\n')

    def test_identical_changed_new(self):
        with tempfile.TemporaryDirectory() as d:
            gen, base, stats = os.path.join(d, 'gen'), os.path.join(d, 'b.json'), os.path.join(d, 's.json')
            json.dump({'lifted': 2, 'errors': 0, 'stubs': 0}, open(stats, 'w'))
            self.write_gen(gen, [('sub_00401000', '    eax = 1;'), ('sub_00401010', '    eax = 2;')])
            rep = lift_check.check(gen, stats, base)
            self.assertTrue(rep['ok'])
            self.assertTrue(os.path.exists(base))                 # recorded on the first run
            rep = lift_check.check(gen, stats, base)
            self.assertTrue(rep['ok'])
            self.assertEqual(rep['identical'], 2)
            self.write_gen(gen, [('sub_00401000', '    eax = 1;'), ('sub_00401010', '    eax = 3;'),
                                 ('sub_00401020', '    eax = 4;')])
            rep = lift_check.check(gen, stats, base)
            self.assertFalse(rep['ok'])
            self.assertEqual((rep['identical'], rep['changed'], rep['new'], rep['gone']), (1, 1, 1, 0))
            self.write_gen(gen, [('sub_00401000', '    eax = 1;')])
            rep = lift_check.check(gen, stats, base)
            self.assertFalse(rep['ok'])
            self.assertEqual(rep['gone'], 1)

    def test_errors_fail(self):
        with tempfile.TemporaryDirectory() as d:
            gen, stats = os.path.join(d, 'gen'), os.path.join(d, 's.json')
            self.write_gen(gen, [('sub_00401000', '    eax = 1;')])
            json.dump({'lifted': 1, 'errors': 3, 'stubs': 0}, open(stats, 'w'))
            rep = lift_check.check(gen, stats, None)
            self.assertFalse(rep['ok'])

    def test_one_line_body(self):
        """the host's functions (run_lift.py HLE) are one line: void sub_X(void) { swgb_inflate(); }"""
        with tempfile.TemporaryDirectory() as d:
            open(os.path.join(d, 'recomp_0001.c'), 'w').write('void sub_0062343B(void) { swgb_inflate(); }\n')
            open(os.path.join(d, 'recomp_dispatch.c'), 'w').write('{ 0x0062343Bu, sub_0062343B },\n')
            funcs, dispatch, _ = lift_check.scan(d)
            self.assertEqual(list(funcs), ['sub_0062343B'])
            self.assertEqual(dispatch, ['sub_0062343B'])


class ResCheck(unittest.TestCase):
    def test_fit(self):
        self.assertEqual(res_check.fit(1920, 1080), (1280, 1024))
        self.assertEqual(res_check.fit(2560, 1440), (1280, 1024))
        self.assertEqual(res_check.fit(1364, 768), (1024, 768))
        self.assertEqual(res_check.fit(1280, 720), (800, 600))
        self.assertEqual(res_check.fit(1024, 768), (1024, 768))

    def test_hud_button(self):
        # the game's own sizes: the art's places; WxH: the 1280 layout at the bottom, centred
        self.assertEqual(res_check.hud_button(800, 600, 0), (768, 480))
        self.assertEqual(res_check.hud_button(1024, 768, 4), (983, 732))
        self.assertEqual(res_check.hud_button(1920, 1080, 0), (1230 + 320, 852 + 56))
        self.assertEqual(res_check.hud_button(1364, 768, 0), (1230 + 42, 852 - 256))

    def _shots(self, w, h):
        import numpy as np
        before = np.full((h, w), 90, dtype=np.int16)       # the map
        before[h - 150:, :] = 60                            # the HUD
        return before

    def test_popup_box(self):
        before = self._shots(1920, 1080)
        after = before.copy()
        after[390:690, 810:1110] = 10                       # a dialog in the middle
        after[1000:1020, 1540:1560] = 200                   # the button's highlight
        after[100:104, 300:304] = 0                         # a unit walking
        self.assertEqual(res_check.popup_box(before, after), (810, 390, 1109, 689))
        self.assertIsNone(res_check.popup_box(before, before))
        rep = res_check.Report(quiet=True)
        self.assertTrue(res_check.check_centred(rep, before, after, 'dialog'))
        off = before.copy()
        off[150:450, 250:550] = 10                          # where an 800x600 layout put it
        self.assertFalse(res_check.check_centred(rep, before, off, 'dialog at 800x600'))
        self.assertFalse(res_check.check_centred(rep, before, before, 'nothing opened'))
        self.assertEqual(rep.fails, 2)

    def test_full_screen(self):
        import numpy as np
        rep = res_check.Report(quiet=True)
        good = np.zeros((1080, 1920), dtype=np.int16)
        good[28:1052, 320:1600] = 40                        # 1280x1024, centred
        self.assertTrue(res_check.check_full_screen(rep, good, 'save'))
        bad = np.zeros((1080, 1920), dtype=np.int16)
        bad[0:1024, 0:1280] = 40                            # the art at 0,0
        self.assertFalse(res_check.check_full_screen(rep, bad, 'save at 0,0'))
        own = np.full((768, 1024), 40, dtype=np.int16)      # the game's own size: no margins
        self.assertTrue(res_check.check_full_screen(rep, own, 'save at 1024x768'))
        self.assertEqual(rep.fails, 1)


class Runner(unittest.TestCase):
    def test_fps_figures(self):
        rows = ['[fps] %6.1fs mode %d cpu %3.0f%% presents %.0f/s changed %.0f/s | world 1 ms' % (t, 4 if t > 5 else 0, c, p, 30)
                for t, c, p in [(1, 90, 5), (3, 80, 5), (6, 12, 60), (30, 8, 61), (31, 9, 59), (32, 10, 60), (33, 30, 60)]]
        f = runner.fps_figures('\n'.join(rows), settle=20)
        self.assertEqual(f['seconds'], 4)
        self.assertEqual(f['presents_median'], 60)
        self.assertEqual(f['cpu_median'], 9.5)
        self.assertIsNone(runner.fps_figures('nothing'))

    def test_compare(self):
        from PIL import Image

        class A:
            update_baselines = False

        class C:
            a = A()

        with tempfile.TemporaryDirectory() as d:
            ctx = C()
            ctx.baselines = os.path.join(d, 'base')
            png = os.path.join(d, 'x.png')
            Image.new('L', (100, 100), 50).save(png)

            class R(runner.Result):
                def __init__(self):
                    self.lines, self.metrics, self.images, self.status, self.root = [], {}, [], 'PASS', d

            r = R()
            self.assertEqual(runner.compare(ctx, r, png, 'k/menu'), 0.0)   # recorded
            self.assertTrue(os.path.exists(os.path.join(d, 'base', 'k', 'menu.png')))
            im = Image.new('L', (100, 100), 50)
            for x in range(10):                                            # 1% differs
                im.putpixel((x, 0), 200)
            im.save(png)
            self.assertAlmostEqual(runner.compare(ctx, r, png, 'k/menu', tol=0.02), 0.001)
            self.assertEqual(r.status, 'PASS')
            for x in range(100):
                for y in range(5):
                    im.putpixel((x, y), 200)
            im.save(png)
            runner.compare(ctx, r, png, 'k/menu', tol=0.02)
            self.assertEqual(r.status, 'FAIL')
            r.status = 'PASS'
            self.assertEqual(runner.compare(ctx, r, png, 'k/menu', tol=0.02, region=(0, 50, 100, 100)), 0.0)
            self.assertEqual(r.status, 'PASS')

    def test_retry_keeps_both_tries(self):
        """--retry: the retry runs in <check>-retry1, so the first try's run.log and shots stay;
        the report has both tries' lines and shots, each under its folder."""
        with tempfile.TemporaryDirectory() as d:
            class A:
                jobs = 2

            class C:
                a, cancelled, results = A(), False, d

            def fake(passes_on):
                def run(c, attempt):
                    r = runner.Result(c, d, attempt)
                    os.makedirs(r.dir, exist_ok=True)
                    with open(os.path.join(r.dir, 'run.log'), 'w') as f:
                        f.write('try %d' % (attempt + 1))
                    with open(os.path.join(r.dir, 'map.png'), 'wb') as f:
                        f.write(b'png')
                    r.image(os.path.join(r.dir, 'map.png'), 'map')
                    if attempt < passes_on:
                        r.fail('no shot: map')
                    return r
                return run

            def text(*p):
                with open(os.path.join(d, *p)) as f:
                    return f.read()

            flaky = runner.Check('skirmish/base', 'functional', None, game='base', doc='x')
            solid = runner.Check('films/base', 'functional', None, game='base', doc='x')
            run = fake(passes_on=1)
            results = [run(flaky, 0), fake(0)(solid, 0)]
            runner.retry_failed(C(), results, 1, run)
            r = results[0]
            self.assertEqual((r.status, results[1].status), ('FLAKY', 'PASS'))
            self.assertEqual(text('skirmish_base', 'run.log'), 'try 1')
            self.assertEqual(text('skirmish_base-retry1', 'run.log'), 'try 2')
            self.assertEqual(r.lines[0], 'try 1: FAIL, in skirmish_base/')
            self.assertIn('try 2: PASS, in skirmish_base-retry1/', r.lines)
            self.assertEqual(r.images, [('skirmish_base/map.png', 'try 1: map'),
                                        ('skirmish_base-retry1/map.png', 'try 2: map')])
            self.assertEqual([t['dir'] for t in r.as_dict()['tries']], ['skirmish_base', 'skirmish_base-retry1'])
            self.assertFalse(os.path.exists(os.path.join(d, 'films_base-retry1')))   # a pass is not retried

            # failing every time: --retry 2 keeps three folders, and it stays a FAIL
            run = fake(passes_on=9)
            results = [run(runner.Check('res/1024', 'functional', None, game='base', doc='x'), 0)]
            runner.retry_failed(C(), results, 2, run)
            r = results[0]
            self.assertEqual(r.status, 'FAIL')
            self.assertEqual([t['try'] for t in r.as_dict()['tries']], [1, 2, 3])
            self.assertEqual(sum(l.startswith('try ') for l in r.lines), 3)
            for k, sub in enumerate(['res_1024', 'res_1024-retry1', 'res_1024-retry2']):
                self.assertEqual(text(sub, 'run.log'), 'try %d' % (k + 1))
    def test_hud_sides(self):
        import numpy as np
        from PIL import Image
        self.assertEqual([runner.side_column(d, 3) for d in range(8)], [0, 1, 2, 2, 1, 0, 0, 1])
        self.assertEqual(runner.hud_base(1920, 1080), (1280, 250))
        self.assertEqual(runner.hud_base(1152, 864), (1024, 200))
        self.assertEqual(runner.hud_base(1024, 600), (800, 170))
        W, H, bw, bh = 1920, 1080, 1280, 250
        dx, band = (W - bw) // 2, 12
        rng = np.random.default_rng(1)
        a = np.zeros((H, W, 3), np.uint8)
        a[H - bh:, dx:dx + bw] = rng.integers(30, 255, (bh, bw, 3), dtype=np.uint8)   # the art
        with tempfile.TemporaryDirectory() as d:
            png = os.path.join(d, 'm.png')
            Image.fromarray(a).save(png)
            f = runner.hud_sides(png)                       # black sides
            self.assertEqual(f['sides_black'], 1.0)
            self.assertLess(f['sides_mirror'], 0.01)
            for k in range(dx):                             # as widescreen.c fills them
                a[H - bh:, dx - 1 - k] = a[H - bh:, dx + runner.side_column(k, band)]
                a[H - bh:, dx + bw + k] = a[H - bh:, dx + bw - 1 - runner.side_column(k, band)]
            Image.fromarray(a).save(png)
            f = runner.hud_sides(png)
            self.assertEqual(f['sides_mirror'], 1.0)
            self.assertEqual(f['sides_black'], 0.0)
            self.assertGreater(f['sides_mean'], 20)
            a[H - bh:, :dx] = np.roll(a[H - bh:, :dx], 5, axis=1)   # the art 5 columns off
            Image.fromarray(a).save(png)
            self.assertLess(runner.hud_sides(png)['sides_mirror'], 0.9)
            with Image.open(runner.sides_crop(png, os.path.join(d, 's.png'))) as c:
                self.assertEqual(c.size, (W - bw, bh))

    def test_view_marker_moved(self):
        from PIL import Image, ImageDraw
        with tempfile.TemporaryDirectory() as d:
            shots = []
            for i, x in enumerate((100, 300)):
                im = Image.new('RGB', (400, 200), (20, 20, 20))
                dr = ImageDraw.Draw(im)
                dr.rectangle((5, 5, 30, 30), outline=(255, 255, 255))      # the art's own white: in both
                dr.rectangle((x - 25, 80, x + 25, 110), outline=(255, 255, 255))
                shots.append(os.path.join(d, '%d.png' % i))
                im.save(shots[-1])
            xa, xb = runner.view_marker_moved(shots[0], shots[1], 0, 0, 400, 200)
            self.assertAlmostEqual(xa, 100, delta=1)
            self.assertAlmostEqual(xb, 300, delta=1)
            self.assertEqual(runner.view_marker_moved(shots[0], shots[0], 0, 0, 400, 200), (None, None))

    def test_layout_text(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'dump.txt')
            with open(p, 'w') as f:
                f.write('08A50A88 vt 0065CE24  0,0 1920x1080\n  08A8F070 vt 00665088  320,830 240x250\n')
            self.assertEqual(runner.layout_text(p), 'vt 0065CE24  0,0 1920x1080\n  vt 00665088  320,830 240x250\n')
            self.assertIsNone(runner.layout_text(os.path.join(d, 'none.txt')))

    def test_levels_cumulative(self):
        class A:
            just = False
            with_lower = False
            cc = None
            only = []

        a = A()
        a.level = 'unit'
        unit = runner.select(a)
        self.assertTrue(unit and all(c.level == 'unit' for c in unit))
        a.level = 'functional'
        names = {c.name for c in runner.select(a)}
        self.assertTrue({'unit/scaler', 'conformance/base', 'present/A'} <= names)
        self.assertFalse(any(n.startswith(('soak/', 'real/')) for n in names))
        a.level = 'real'
        self.assertEqual([c.name for c in runner.select(a)], ['real/present'])
        a.level, a.cc = 'smoke', False
        self.assertFalse(any(c.game == 'cc' for c in runner.select(a)))


class PerfAB(unittest.TestCase):
    """tools/perf_ab.py (two builds, the same games) and mp_pair.py's sync-record compare."""

    def test_figures(self):
        rows = ['[fps] %6.1fs mode 4 cpu %3.0f%% presents %.0f/s changed 9/s | world 1 ms +1500/s speed 1.50 steps %.1f/s of 77.0 ms (max 80)'
                % (t, c, p, 19.0 + (t % 2)) for t, c, p in [(5, 99, 10), (20, 50, 100), (21, 60, 200), (22, 40, 100), (90, 99, 1)]]
        rows.append('[present] 1.0 s: 100 presents, 60 shown (180 Hz, sharp); by the timer: 0 held')
        f = perf_ab.figures('\n'.join(rows), 20, 30)
        self.assertEqual(f['seconds'], 3)
        self.assertEqual(f['cpu'], 50)
        self.assertEqual(f['drawn'], 100)
        self.assertAlmostEqual(f['ms_frame'], 150 * 10 / 400)   # CPU over the window / frames drawn
        self.assertEqual(f['steps'], 19)        # 19, 20, 19
        self.assertEqual(f['shown'], 60)
        self.assertIsNone(perf_ab.figures('nothing', 0, 10))

    def test_ratio_by_round(self):
        run = lambda ms, rc=4: {'ms_frame': ms, 'cpu': 50, 'exit': rc, 'events': 0}   # noqa: E731
        res = {('O1', 'late'): [run(4.0), run(3.0), run(2.0)],
               ('O2', 'late'): [run(3.6), run(3.0), run(1.0, rc=3)]}
        t = perf_ab.summarize(res)['late']
        self.assertEqual(t['O2']['ratio'][:2], [0.95, 0.9])     # rounds 0, 1; the fault's round left out
        self.assertEqual(t['O2']['bad'], 1)
        self.assertNotIn('ratio', t['O1'])
        self.assertIn('ms/frame vs O1', perf_ab.report(perf_ab.summarize(res), ['O1', 'O2']))

    def test_scenarios_set_their_screen_size(self):
        with tempfile.TemporaryDirectory() as d:
            for name, (plan, extra, size) in perf_ab.SCENARIOS.items():
                s, t0, t1 = plan('base', d, size)
                steps = dict((t, w) for t, w in s.steps)
                self.assertLess(t0, t1, name)
                if size is None:
                    continue
                # a game folder remembers Screen Size: each game picks its own before Single Player
                self.assertEqual(steps[12], 'click:%d:%d' % perf_ab.MAIN_OPTIONS, name)
                self.assertEqual(steps[19], 'click:%d:%d' % perf_ab.SIZE_RADIO[size], name)
                self.assertEqual(size == 'res', '--res' in extra, name)
                self.assertGreater(min(t for t, w in s.steps if w.startswith('click') and t > 19), 19, name)

    def test_mp_sync_compare(self):
        rec = lambda p, turn, ck: (0, (p, 1000 * turn, turn, 1000 * turn, ck, 7, 0, 5, 9))   # noqa: E731
        a = [rec(1, 40, 11), rec(2, 40, 11), rec(1, 80, 12), rec(2, 80, 12)]
        shared, bad = mp_pair.compare(a, list(a))
        self.assertEqual((len(shared), bad), (4, []))
        b = [rec(1, 40, 11), rec(2, 40, 11), rec(1, 80, 13), rec(2, 80, 13)]   # B's builds drift at turn 80
        shared, bad = mp_pair.compare(a, b)
        self.assertEqual(sorted(k for k, _, _ in bad), [(1, 80), (2, 80)])


MP_LOG = """[dplay] CreatePlayer "michael" -> 0001D2A1 (flags 0, event 00000000)
[dplay] player 0001D2A2 (michael) joined (45.9 s)
[sync]  1: 00000002 00001167 0000000A 00001167 0A51D3CA 00009882 00000000 00060015 00005A41
[sync]  2: 00000001 00001167 0000000A 00001167 0A51D3CA 00009882 00000000 00060015 00005A41
[mp] chat from player 3 (0001D2A2) to 0001D2A1 (YYYNNNNN): "mpchatc1"
[dplay] client 1 (127.0.0.1) gone (153.4 s)
[dplay] player 0001D2A2 (michael) left (153.4 s)
ICALL: unresolved VA 0x005648A0 from 0x00406E20
[dplay] DestroyPlayer 0001D2A1 (200.5 s)
[dplay] Close (client) (200.6 s)
[dplay] the host is gone: session lost (201.0 s)
"""


class MpPair(unittest.TestCase):
    def setUp(self):
        import mp_pair
        self.mp = mp_pair

    def test_check(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'run.log')
            with open(p, 'w') as f:
                f.write(MP_LOG)
            ev, chats, me, deps, left, started = self.mp.check(p)
        self.assertEqual(len(ev), 1)
        self.assertEqual(chats, {'mpchatc1': 3})
        self.assertEqual(me, '0001D2A1')
        self.assertEqual([(t, d_) for t, _, d_ in deps], [(153.4, '0001D2A2'), (201.0, 'host')])
        self.assertEqual(left, 200.5)
        self.assertTrue(started)

    def test_compare_all(self):
        r = [(1, (2, 4455, 10, 4455, 5, 6, 0, 7, 8)), (2, (1, 4455, 10, 4455, 5, 6, 0, 7, 8))]
        bad_c = [(1, (2, 4455, 10, 4455, 5, 6, 0, 7, 9))]
        shared, bad = self.mp.compare_all({'a': r, 'b': r, 'c': bad_c})
        self.assertEqual(shared, [(1, 10), (2, 10)])
        self.assertEqual(len(bad), 2)        # c against a, and against b
        shared, bad = self.mp.compare_all({'a': r, 'b': r})
        self.assertEqual(bad, [])
        _, bad = self.mp.compare_all({'a': r + [(3, (3, 4455, 10, 4455, 5, 6, 0, 7, 1))]})
        self.assertEqual(len(bad), 1)        # within one log: the players' records for a turn

    def test_plans(self):
        import argparse
        import random
        a = argparse.Namespace(players=3, minutes=4, activity='busy', chat=True, save=1.0, quit=None, resign=None,
                               drop=('c', 2.0))
        chats = self.mp.chat_times(3, 4)
        self.assertEqual(len(chats), 6)
        self.assertEqual([w for _, w, _ in chats], list('abcabc'))
        plan = self.mp.game_plan(random.Random(1), 'b', 1, 3, a, 'out')
        self.assertEqual(plan, sorted(plan, key=lambda s: s[0]))
        steps = [s for _, s in plan]
        self.assertTrue(any('drop_after' in st for st in steps))
        self.assertEqual([s for t, s in plan if 118 <= t <= 195 and s.startswith(('click', 'rclick'))], [])   # c drops: b only votes
        t_chat = next(t for t, s in plan if s == 'enter')
        near = [s for t, s in plan if t_chat - 2 <= t <= t_chat + 5 and s.startswith(('click', 'rclick'))]
        self.assertEqual(near, [])          # no clicks while typing
        plan_c = self.mp.game_plan(random.Random(1), 'c', 2, 3, a, 'out')
        self.assertFalse(any('drop_after' in s for _, s in plan_c))
        text, wd = self.mp.script('b', 1, 3, a, 'out')
        self.assertTrue(text.startswith('on:%s\n' % self.mp.ON_MENU))
        _, wd_a = self.mp.script('a', 0, 3, a, 'out')
        self.assertEqual(wd_a - wd, self.mp.B_DELAY + self.mp.STEP)

    def test_chat_shown(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('no PIL')
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'x.png')
            im = Image.new('RGB', (800, 600), (175, 126, 36))
            for x in range(10, 100):
                for y in range(160, 165):
                    im.putpixel((x, y), self.mp.CHAT_RGB[2])
            im.save(p)
            self.assertTrue(self.mp.chat_shown(p, 2))
            self.assertFalse(self.mp.chat_shown(p, 3))
            self.assertFalse(self.mp.chat_shown(os.path.join(d, 'none.png'), 2))


if __name__ == '__main__':
    unittest.main()
