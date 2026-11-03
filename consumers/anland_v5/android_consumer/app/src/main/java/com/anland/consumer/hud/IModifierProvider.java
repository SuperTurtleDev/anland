package com.anland.consumer.hud;

/**
 * Hardware modifier keys (Ctrl, Alt, Shift, Super) owned by the HUD: a modifier
 * can be latched on the overlay and the soft keyboard still has to see it, so
 * SystemIME injects its combos through this interface.
 */
public interface IModifierProvider {

    /**
     * @return True if any latched or locked modifier key is currently active.
     */
    boolean hasActiveModifier();

    /**
     * Emits the modifier(s) down, the target character down/up, and the modifier(s) up,
     * then resets unlocked modifiers to inactive state.
     *
     * @param evdevScancode The target character scan code to combo with the active modifiers.
     */
    void sendKeyComboFromExternal(int evdevScancode);

    /**
     * Clears all modifier states.
     */
    void reset();
}
