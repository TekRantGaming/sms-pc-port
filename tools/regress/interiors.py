#!/usr/bin/env python3
"""Check plaza interior entry and exit in both word sizes at 30/60 fps.

Run with Python from the port checkout; --disc supplies the GMSE01 image.
Use --fps 30,60,120 to include 120 fps on builds that support it.
The same file is sourced inside gdb to place Mario on the real doorway
triangles. The game performs each warp itself. Each building is entered and
exited twice, checking the active model, Mario's height and his ground plane.
"""
import os

try:
    import gdb
except ImportError:
    gdb = None


def install_breakpoint():
    # Coordinates come from dolpic5's map.col and building01.col. The
    # interiors are 9000 units above their exterior doorway triangles.
    buildings = (
        ('boathouse', 2, (-3150, 728.021301, -5500), (-2800, 9671.978516, -5500)),
        ('lighthouse', 1, (-10500, 335, 2400), (-10500, 9325, 2750)),
    )
    actions = []
    for name, model, entrance, exit_point in buildings:
        for trip in (1, 2):
            actions.extend(((name, trip, 'entry', model, entrance),
                            (name, trip, 'exit', 0, exit_point)))

    class Doorways(gdb.Breakpoint):
        def __init__(self):
            super().__init__('TMapWarp::watchToWarp')
            self.frames = 0
            self.action = 0
            self.placed = False
            self.wait = 0

        def finish(self, message):
            print('interiors: ' + message, flush=True)
            gdb.execute('kill')
            return True

        def stop(self):
            try:
                if int(gdb.parse_and_eval('gpMarDirector->mMap')) != 1:
                    return False
                self.frames += 1
                if self.frames < 120:
                    return False
                requested_rate = int(os.environ['SMS_FRAME_RATE'])
                actual_rate = int(gdb.parse_and_eval('port_frame_rate'))
                if actual_rate != requested_rate:
                    return self.finish('FAIL requested %d fps, configured %d' %
                                       (requested_rate, actual_rate))
                name, trip, direction, expected_model, position = actions[self.action]
                if not self.placed:
                    for axis, value in zip('xyz', position):
                        gdb.execute('set var gpMarioPos->%s = %.9f' % (axis, value))
                        gdb.execute('set var gpMarioOriginal->mVel.%s = 0' % axis)
                    self.placed = True
                    self.wait = 0
                    return False
                self.wait += 1
                if self.wait < 20:
                    return False
                mario = gdb.parse_and_eval('gpMarioOriginal')
                y = float(mario['mPosition']['y'])
                model = int(gdb.parse_and_eval('gpMap->mWarp->unk8'))
                if model != expected_model or ((y > 9000) != (direction == 'entry')):
                    return self.finish('FAIL %s trip %d %s: model=%d y=%.3f' %
                                       (name, trip, direction, model, y))
                if int(mario['mGroundPlane']['mFlags']) & 0x10:
                    return self.finish('FAIL %s trip %d %s: missing ground' %
                                       (name, trip, direction))
                print('interiors: %s trip %d %s model=%d y=%.3f' %
                      (name, trip, direction, model, y), flush=True)
                self.action += 1
                self.placed = False
                if self.action == len(actions):
                    return self.finish('PASS boathouse=2 lighthouse=2')
            except gdb.error as error:
                return self.finish('FAIL debugger: %s' % error)
            return False

    Doorways()


def main():
    import argparse
    import subprocess
    from concurrent.futures import ThreadPoolExecutor
    import regress

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--disc', help='GMSE01 disc image')
    parser.add_argument('--build32', default='build/linux-32')
    parser.add_argument('--build64', default='build/linux-64')
    parser.add_argument('--arch', default='32,64')
    parser.add_argument('--fps', default='30,60', help='rates to check; add 120 on builds that support it')
    parser.add_argument('--work', default='build/interiors-regress')
    parser.add_argument('--timeout', type=int, default=600)
    args = parser.parse_args()
    disc = args.disc or regress.find_disc()
    if not disc or not os.path.isfile(disc):
        parser.error('supply a GMSE01 image with --disc')
    disc = os.path.abspath(disc)
    archs, rates = args.arch.split(','), args.fps.split(',')
    if any(a not in ('32', '64') for a in archs) or any(f not in ('30', '60', '120') for f in rates):
        parser.error('--arch takes 32,64; --fps takes 30,60,120')
    executables = {a: os.path.abspath(getattr(args, 'build' + a) + '/sms') for a in archs}
    if any(not os.path.isfile(exe) for exe in executables.values()):
        parser.error('build each requested executable first')
    script = os.path.abspath(__file__)

    def run(arch, fps):
        directory = os.path.abspath(os.path.join(args.work, '%s-%s' % (arch, fps)))
        os.makedirs(directory, exist_ok=True)
        save = os.path.join(directory, 'save')
        os.makedirs(save, exist_ok=True)
        env = regress.base_env(save)
        env.update(SMS_WARP='1,5,2', SMS_FRAME_RATE=fps, SMS_AUTOPRESS=regress.GATE_AP)
        command = regress.GDB + ['-x', script, '-ex', 'run', '--args', executables[arch], disc]
        log_path = os.path.join(directory, 'run.log')
        with open(log_path, 'wb') as log:
            process = subprocess.Popen(command, cwd=regress.ROOT, env=env,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL, start_new_session=True)
            try:
                process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                regress.kill_group(process)
        with open(log_path, errors='replace') as log:
            contents = log.read()
        # gdb can return nonzero after the breakpoint deliberately kills the
        # inferior. Only the completed gameplay assertions establish success.
        passed = ('interiors: PASS boathouse=2 lighthouse=2' in contents
                  and 'interiors: FAIL' not in contents)
        details = next((line for line in contents.splitlines() if line.startswith('interiors: FAIL')),
                       'see ' + log_path)
        print('%s interiors %s-bit %s fps: %s' %
              ('PASS' if passed else 'FAIL', arch, fps,
               'two round trips per building' if passed else details), flush=True)
        return passed

    results = []
    with ThreadPoolExecutor(max_workers=2) as pool:
        for fps in rates:
            results.extend(pool.map(lambda arch: run(arch, fps), archs))
    return 0 if all(results) else 1


if gdb is not None:
    install_breakpoint()
elif __name__ == '__main__':
    raise SystemExit(main())
