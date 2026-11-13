# Input state regressions

```sh
python3 -m unittest discover -s tests -v
```

The host test compiles the actual keyboard-leave and motion-modifier functions
against event-recording stubs with AddressSanitizer/UndefinedBehaviorSanitizer.
It checks stale-owner leaves, balanced releases, retained locks, missing client
resources, stale modifier repair and genuinely held modifiers.

Compile AndroidKeyCode.java and tests/android/AndroidKeyCodeProbe.java against
the Android SDK, then convert their classes with d8. Run the resulting jar in
an isolated Android app_process to check the mappings against Android KeyEvent
constants. No Linux application or daemon needs to be restarted for that test.
Real focus transitions and hardware input need separate integrated validation.
