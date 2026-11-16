package com.anland.consumer.hud;

/** Tracks one standard-button gesture; each primary action is a complete tap. */
final class HudButtonPress {
    private boolean active;
    private boolean sendOnRelease;

    void down(boolean releaseMode, Runnable tap) {
        active = true;
        sendOnRelease = releaseMode;
        if (!releaseMode) tap.run();
    }

    void repeat(Runnable tap) {
        if (active && !sendOnRelease) tap.run();
    }

    void cancel() {
        active = false;
    }

    void up(Runnable tap) {
        boolean send = active && sendOnRelease;
        active = false;
        if (send) tap.run();
    }
}
