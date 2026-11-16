package com.anland.consumer;

import android.content.SharedPreferences;

import java.util.LinkedHashSet;
import java.util.Set;

/**
 * All sources use the root helper's exclusive evdev stream. Gold sources take
 * Gold's remapped output; Gold keeps its physical devices and mapping profiles.
 */
enum ImmersiveInputSource {
    DIRECT_EVENT_NODES("direct_event_nodes"),
    EXISTING_UINPUT_BUS("existing_uinput_bus"),
    /** Physical pointers/touch/controllers plus Gold's remapped keyboard output. */
    DIRECT_PLUS_GOLD_KEYBOARD("direct_plus_gold_keyboard");

    /**
     * Whether this source includes physical nodes in addition to Gold output.
     */
    boolean takesDevices() {
        return this != EXISTING_UINPUT_BUS;
    }

    /**
     * Whether the helper exclusively reads Gold's output nodes.
     */
    boolean listensToGoldKeyboard() {
        return this != DIRECT_EVENT_NODES;
    }

    /**
     * Whether this source can run at all right now.
     *
     * <p>The two sources that forward Gold's keyboard have nothing to forward
     * without it, and Gold is a separate Magisk module that may simply not be
     * installed. Asking here rather than at the point of use is what lets the
     * picker say so before the user chooses, instead of accepting the choice
     * and then doing nothing with it.
     */
    boolean isAvailable(android.content.Context context) {
        return !listensToGoldKeyboard() || GoldKeyboard.goldKeyboardPresent(context);
    }

    String helperArgument() {
        switch (this) {
            case EXISTING_UINPUT_BUS: return "source=gold";
            case DIRECT_PLUS_GOLD_KEYBOARD: return "source=combined";
            default: return "source=physical";
        }
    }

    boolean allowsUnboundToggle() {
        return this == EXISTING_UINPUT_BUS;
    }

    static final String KEY_PREFERENCE = "immersive_input_source";

    /**
     * The order the picker lists them in, and the order the names in
     * {@code R.array.immersive_source_options} are written in. One list, so a
     * fourth source cannot be added to the menu and forgotten by the code that
     * has to name it in a message.
     */
    static final ImmersiveInputSource[] ORDER = {
        DIRECT_EVENT_NODES, EXISTING_UINPUT_BUS, DIRECT_PLUS_GOLD_KEYBOARD,
    };

    /** Index into {@link #ORDER} and the parallel string array. */
    int menuIndex() {
        for (int i = 0; i < ORDER.length; i++) {
            if (ORDER[i] == this)
                return i;
        }
        return 0;
    }

    final String preferenceValue;

    ImmersiveInputSource(String preferenceValue) {
        this.preferenceValue = preferenceValue;
    }

    /**
     * Anything unrecognised — missing, empty, or a value written by a future
     * version — falls back to direct, so an existing install keeps behaving
     * exactly as it did before this preference existed.
     */
    static ImmersiveInputSource fromPreference(String value) {
        for (ImmersiveInputSource source : values()) {
            if (source.preferenceValue.equals(value))
                return source;
        }
        return DIRECT_EVENT_NODES;
    }

    static ImmersiveInputSource read(SharedPreferences preferences) {
        return fromPreference(preferences == null ? null
                : preferences.getString(KEY_PREFERENCE, null));
    }

    // ---- direct-source node selection -------------------------------------

    /** Whether the direct source picks nodes itself instead of using the list. */
    static final String KEY_AUTO = "immersive_input_auto";

    /** The saved node list, comma separated, in the legacy grammar. */
    static final String KEY_NODES = "immersive_input_nodes";

    /**
     * Missing means automatic, so an install that predates this preference keeps
     * taking whatever the helper finds — the behaviour it already had.
     */
    static boolean isAutomatic(SharedPreferences preferences) {
        return preferences == null || preferences.getBoolean(KEY_AUTO, true);
    }

    /**
     * The saved node list as written. Not validated here: the resolver decides
     * what the list means and whether it can be honoured, and it never rewrites
     * what the user saved.
     */
    static Set<String> savedNodes(SharedPreferences preferences) {
        Set<String> nodes = new LinkedHashSet<>();
        if (preferences == null)
            return nodes;
        String raw = preferences.getString(KEY_NODES, "");
        if (raw == null)
            return nodes;
        for (String part : raw.split(",")) {
            String node = part.trim();
            if (!node.isEmpty())
                nodes.add(node);
        }
        return nodes;
    }
}
