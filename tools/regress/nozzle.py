# Run under gdb with a fresh memory card and SMS_WARP=2,7,1 (Bianco Hills)
# or 3,7,1 (Ricco Harbor). See docs/DEVELOPMENT.md for the invocation.
# Break a valid Turbo box with its ordinary trample message, then place Mario
# at the released item until collision collects it. Check the equipped nozzle,
# unlock flag and save prompt after ten seconds. No game code is skipped.
import gdb


def fail(message):
    print('nozzle: error: ' + message, flush=True)
    gdb.execute('bt 20')
    gdb.execute('quit 1')


class Ready(gdb.Breakpoint):
    box = None

    def stop(self):
        # Let the level load and the opening camera finish before breaking a box.
        if int(gdb.parse_and_eval('gpMarDirector->mState')) != 4:
            return False
        box = gdb.parse_and_eval('this')
        if int(box['mContainedNozzleType']) != 5 or not bool(box['unk15C']):
            return False
        self.box = box
        return True


class Observe(gdb.Breakpoint):
    def __init__(self, nozzle):
        super().__init__('TMarDirector::direct')
        self.nozzle = nozzle
        self.frames = 0
        self.equipped = False
        self.limit = 10 * int(gdb.parse_and_eval('port_frame_rate'))
        self.save_prompt = False
        self.guard_address = None
        self.guard = None

    def stop(self):
        self.frames += 1
        self.equipped = int(gdb.parse_and_eval(
            'gpMarioOriginal->mWaterGun->mCurrentNozzle')) == 5
        if self.equipped and self.guard_address is None:
            return True # run the controller checks outside breakpoint callbacks
        if self.guard_address is not None:
            if bytes(gdb.selected_inferior().read_memory(self.guard_address, 4)) != self.guard:
                fail('Turbo controller wrote past the real nozzle')
        self.save_prompt |= (int(gdb.parse_and_eval('gpMarDirector->mState')) == 11
                             and int(gdb.parse_and_eval('gpMarDirector->unk261')) == 4)
        if not self.equipped:
            for axis in ('x', 'y', 'z'):
                gdb.execute('set var gpMarioOriginal->mPosition.%s = %.9g' % (
                    axis, float(self.nozzle['mPosition'][axis])))
        return self.frames == self.limit


gdb.execute('set confirm off')
ready = Ready('TNozzleBox::control')
gdb.execute('continue' if gdb.selected_inferior().pid else 'run')
if ready.box is None:
    fail('stopped before reaching a valid Turbo box')
box = ready.box
ready.delete()
area = int(gdb.parse_and_eval('gpApplication.mCurrArea.unk0'))
if area not in (2, 3):
    fail('expected Bianco Hills or Ricco Harbor, got stage %d' % area)
right = 'TFlagManager::smInstance->getNozzleRight(%d, 1)' % area
if bool(gdb.parse_and_eval(right)):
    fail('Turbo is already unlocked; use a fresh SMS_SAVE_DIR')
nozzle = box['mContainedNozzleItem']
gdb.execute('call ((TNozzleBox*)%d)->receiveMessage((THitActor*)gpMarioOriginal, 0)' % int(box))
if int(nozzle['mLiveFlag']) & 1:
    fail('trampling the box did not release its nozzle')
print('nozzle: Turbo box broken in stage %d' % area, flush=True)
observe = Observe(nozzle)
gdb.execute('continue')
if not observe.equipped:
    fail('stopped before Mario equipped Turbo')
# The shipped Turbo is a TNozzleTrigger, while the obsolete store addresses
# a field beyond it as if it were the larger abandoned TNozzleTurbo.
observe.guard_address = int(gdb.parse_and_eval(
    '&((TNozzleTurbo*)&gpMarioOriginal->mWaterGun->mNozzleTurbo)->unk714'))
observe.guard = bytes(gdb.selected_inferior().read_memory(observe.guard_address, 4))
status = int(gdb.parse_and_eval('gpMarioOriginal->mStatus'))
dash = float(gdb.parse_and_eval('gpMarioOriginal->mDashSpeed'))
for test_status in (0x0c400201, 0x04000440):
    gdb.execute('set var gpMarioOriginal->mStatus = %d' % test_status)
    gdb.execute('set var gpMarioOriginal->mDashSpeed = 32.0f')
    gdb.execute('call gpMarioOriginal->checkController((JDrama::TGraphics*)0)')
    if bytes(gdb.selected_inferior().read_memory(observe.guard_address, 4)) != observe.guard:
        fail('Turbo controller wrote past the real nozzle')
gdb.execute('set var gpMarioOriginal->mStatus = %d' % status)
gdb.execute('set var gpMarioOriginal->mDashSpeed = %.9g' % dash)
print('nozzle: ground and swim controller preserve adjacent memory', flush=True)
gdb.execute('continue')
if observe.frames != observe.limit:
    fail('stopped during the pickup or unlock prompt')
observe.delete()
if not observe.equipped:
    fail('Mario did not equip Turbo')
if not bool(gdb.parse_and_eval(right)):
    fail('the pickup did not unlock Turbo')
if not observe.save_prompt:
    fail('the unlock save prompt did not open (director=%d, flags=%#x, Mario=%#x)' % (
        int(gdb.parse_and_eval('gpMarDirector->mState')),
        int(gdb.parse_and_eval('gpMarDirector->unk4C')),
        int(gdb.parse_and_eval('gpMarioOriginal->mStatus'))))
if int(gdb.parse_and_eval('gpMarDirector->unk261')) != 4:
    fail('the save prompt is not for the Turbo unlock')
print('nozzle: PASS stage=%d turbo=5 unlock=1 save=4 frames=%d' % (
    area, observe.frames), flush=True)
gdb.execute('quit 0')
