package com.anland.consumer;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.view.animation.AccelerateInterpolator;
import android.view.animation.DecelerateInterpolator;

import com.anland.consumer.hud.HudAction;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Virtual Keyboard supporting:
 *   1. Portrait Floating QWERTY Keyboard (movable via drag handle).
 *   2. Landscape Ergonomic Split Arc Keyboard (concentric dual-thumb radial layout matching Gboard QWERTY).
 *   3. Long-press numbers on top row (Q->1, W->2 ... P->0) with corner subscripts.
 *   4. Centered bottom Space Bar for dual-thumb typing.
 *   5. Slim, non-bulky Beveled Top Wings contoured to arc curve.
 *   6. Corner fan-shaped dismiss keyboard button.
 *   7. Dedicated ?123 Symbol layer.
 *   8. Snappy 180ms Fan Open and Corner Zoom animations.
 */
public class VirtualKeyboardView extends View {

    private static final String TAG = "VirtualKeyboard";
    private static final String PREFS_NAME = "anland_settings";
    public static final String KEY_LANDSCAPE_SPLIT_ARC = "landscape_split_arc";
    /** Fractions of the view: where the user parked each half of the split keyboard. */
    private static final String KEY_SPLIT_LEFT_X = "keyboard_split_left_x_pct";
    private static final String KEY_SPLIT_LEFT_Y = "keyboard_split_left_y_pct";
    private static final String KEY_SPLIT_RIGHT_X = "keyboard_split_right_x_pct";
    private static final String KEY_SPLIT_RIGHT_Y = "keyboard_split_right_y_pct";
    public static final String KEY_SPLIT_ARC_TOP_WINGS = "split_arc_top_wings";

    // ---------- Portrait Standard QWERTY Layout ----------
    private final String[][] keyboardRows = {
            {"ESC", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"},
            {"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "=", "BKSP"},
            {"Tab", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "[", "]", "\\"},
            {"Caps", "A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'", "Enter"},
            {"Shift", "Z", "X", "C", "V", "B", "N", "M", ",", "UP", ".", "/", "Shift"},
            {"Ctrl", "Alt", "Space", "Alt", "Home", "LT", "DN", "RT", "End", "Ctrl", "HIDE"}
    };

    public static final String KEY_HAPTIC_FEEDBACK = "haptic_feedback_enabled";
    public static final String KEY_KEYBOARD_HANDEDNESS = "keyboard_handedness"; // "left" or "right"
    public static final String KEY_KEYBOARD_THEME = "keyboard_theme"; // 0: filled, 1: border_only, 2: transparent_full

    public static final int THEME_FILLED = 0;
    public static final int THEME_BORDER_ONLY = 1;
    public static final int THEME_TRANSPARENT_FULL = 2;

    // Left-Handed (Default: G & V on the left)
    private static final String[][] LEFT_LETTERS_LH = {
            {"Z", "X", "C", "V"},
            {"A", "S", "D", "F", "G"},
            {"Q", "W", "E", "R", "T"}
    };
    private static final String[][] RIGHT_LETTERS_LH = {
            {"B", "N", "M"},
            {"H", "J", "K", "L"},
            {"Y", "U", "I", "O", "P"}
    };

    // Right-Handed (G & V on the right)
    private static final String[][] LEFT_LETTERS_RH = {
            {"Z", "X", "C"},
            {"A", "S", "D", "F"},
            {"Q", "W", "E", "R", "T"}
    };
    private static final String[][] RIGHT_LETTERS_RH = {
            {"V", "B", "N", "M"},
            {"G", "H", "J", "K", "L"},
            {"Y", "U", "I", "O", "P"}
    };

    // Symbols Layer 1 (?123) - Left Handed
    private static final String[][] LEFT_SYMBOLS_1_LH = {
            {"+", "(", ")", "/"},
            {"@", "#", "$", "_", "&"},
            {"1", "2", "3", "4", "5"}
    };
    private static final String[][] RIGHT_SYMBOLS_1_LH = {
            {";", "!", "?"},
            {"*", "\"", "'", ":"},
            {"6", "7", "8", "9", "0"}
    };

    // Symbols Layer 1 (?123) - Right Handed
    private static final String[][] LEFT_SYMBOLS_1_RH = {
            {"+", "(", ")"},
            {"@", "#", "$", "_"},
            {"1", "2", "3", "4", "5"}
    };
    private static final String[][] RIGHT_SYMBOLS_1_RH = {
            {"/", ";", "!", "?"},
            {"&", "*", "\"", "'", ":"},
            {"6", "7", "8", "9", "0"}
    };

    // Symbols Layer 2 (=\<) - Left Handed
    private static final String[][] LEFT_SYMBOLS_2_LH = {
            {"^", "°", "=", "§"},
            {"[", "]", "{", "}", "%"},
            {"~", "\\", "|", "<", ">"}
    };
    private static final String[][] RIGHT_SYMBOLS_2_LH = {
            {"_", "+", "*"},
            {"©", "®", "™", "•"},
            {"€", "£", "¥", "¢", "₹"}
    };

    // Symbols Layer 2 (=\<) - Right Handed
    private static final String[][] LEFT_SYMBOLS_2_RH = {
            {"^", "°", "="},
            {"[", "]", "{", "}"},
            {"~", "\\", "|", "<", ">"}
    };
    private static final String[][] RIGHT_SYMBOLS_2_RH = {
            {"§", "_", "+", "*"},
            {"%", "©", "®", "™", "•"},
            {"€", "£", "¥", "¢", "₹"}
    };

    // Map QWERTY top row to long-press numbers like Gboard
    private static final Map<String, String> NUMBER_SUBSCRIPTS = new HashMap<>();
    static {
        NUMBER_SUBSCRIPTS.put("Q", "1");
        NUMBER_SUBSCRIPTS.put("W", "2");
        NUMBER_SUBSCRIPTS.put("E", "3");
        NUMBER_SUBSCRIPTS.put("R", "4");
        NUMBER_SUBSCRIPTS.put("T", "5");
        NUMBER_SUBSCRIPTS.put("Y", "6");
        NUMBER_SUBSCRIPTS.put("U", "7");
        NUMBER_SUBSCRIPTS.put("I", "8");
        NUMBER_SUBSCRIPTS.put("O", "9");
        NUMBER_SUBSCRIPTS.put("P", "0");
    }

    private static class SymbolInfo {
        final int baseKeyCode;
        final boolean needsShift;
        SymbolInfo(int baseKeyCode, boolean needsShift) {
            this.baseKeyCode = baseKeyCode;
            this.needsShift = needsShift;
        }
    }

    private static final Map<String, SymbolInfo> SYMBOL_MAP = new HashMap<>();
    static {
        // Shifted number row
        SYMBOL_MAP.put("!", new SymbolInfo(KeyEvent.KEYCODE_1, true));
        SYMBOL_MAP.put("@", new SymbolInfo(KeyEvent.KEYCODE_2, true));
        SYMBOL_MAP.put("#", new SymbolInfo(KeyEvent.KEYCODE_3, true));
        SYMBOL_MAP.put("$", new SymbolInfo(KeyEvent.KEYCODE_4, true));
        SYMBOL_MAP.put("%", new SymbolInfo(KeyEvent.KEYCODE_5, true));
        SYMBOL_MAP.put("^", new SymbolInfo(KeyEvent.KEYCODE_6, true));
        SYMBOL_MAP.put("&", new SymbolInfo(KeyEvent.KEYCODE_7, true));
        SYMBOL_MAP.put("*", new SymbolInfo(KeyEvent.KEYCODE_8, true));
        SYMBOL_MAP.put("(", new SymbolInfo(KeyEvent.KEYCODE_9, true));
        SYMBOL_MAP.put(")", new SymbolInfo(KeyEvent.KEYCODE_0, true));

        // Shifted punctuation
        SYMBOL_MAP.put("_", new SymbolInfo(KeyEvent.KEYCODE_MINUS, true));
        SYMBOL_MAP.put("+", new SymbolInfo(KeyEvent.KEYCODE_EQUALS, true));
        SYMBOL_MAP.put("{", new SymbolInfo(KeyEvent.KEYCODE_LEFT_BRACKET, true));
        SYMBOL_MAP.put("}", new SymbolInfo(KeyEvent.KEYCODE_RIGHT_BRACKET, true));
        SYMBOL_MAP.put("|", new SymbolInfo(KeyEvent.KEYCODE_BACKSLASH, true));
        SYMBOL_MAP.put(":", new SymbolInfo(KeyEvent.KEYCODE_SEMICOLON, true));
        SYMBOL_MAP.put("\"", new SymbolInfo(KeyEvent.KEYCODE_APOSTROPHE, true));
        SYMBOL_MAP.put("<", new SymbolInfo(KeyEvent.KEYCODE_COMMA, true));
        SYMBOL_MAP.put(">", new SymbolInfo(KeyEvent.KEYCODE_PERIOD, true));
        SYMBOL_MAP.put("?", new SymbolInfo(KeyEvent.KEYCODE_SLASH, true));
        SYMBOL_MAP.put("~", new SymbolInfo(KeyEvent.KEYCODE_GRAVE, true));

        // Unshifted symbols
        SYMBOL_MAP.put("-", new SymbolInfo(KeyEvent.KEYCODE_MINUS, false));
        SYMBOL_MAP.put("=", new SymbolInfo(KeyEvent.KEYCODE_EQUALS, false));
        SYMBOL_MAP.put("[", new SymbolInfo(KeyEvent.KEYCODE_LEFT_BRACKET, false));
        SYMBOL_MAP.put("]", new SymbolInfo(KeyEvent.KEYCODE_RIGHT_BRACKET, false));
        SYMBOL_MAP.put("\\", new SymbolInfo(KeyEvent.KEYCODE_BACKSLASH, false));
        SYMBOL_MAP.put(";", new SymbolInfo(KeyEvent.KEYCODE_SEMICOLON, false));
        SYMBOL_MAP.put("'", new SymbolInfo(KeyEvent.KEYCODE_APOSTROPHE, false));
        SYMBOL_MAP.put(",", new SymbolInfo(KeyEvent.KEYCODE_COMMA, false));
        SYMBOL_MAP.put(".", new SymbolInfo(KeyEvent.KEYCODE_PERIOD, false));
        SYMBOL_MAP.put("/", new SymbolInfo(KeyEvent.KEYCODE_SLASH, false));
        SYMBOL_MAP.put("`", new SymbolInfo(KeyEvent.KEYCODE_GRAVE, false));
    }

    // ---------- Symbols Map (Portrait) ----------
    private static final Map<String, String> SYMBOL_CHAR_MAP = new HashMap<>();
    static {
        String[] digits = {"1","2","3","4","5","6","7","8","9","0"};
        String[] digitSymbols = {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")"};
        for (int i = 0; i < digits.length; i++) {
            SYMBOL_CHAR_MAP.put(digits[i], digitSymbols[i]);
        }
        SYMBOL_CHAR_MAP.put("`", "~");
        SYMBOL_CHAR_MAP.put("-", "_");
        SYMBOL_CHAR_MAP.put("=", "+");
        SYMBOL_CHAR_MAP.put("[", "{");
        SYMBOL_CHAR_MAP.put("]", "}");
        SYMBOL_CHAR_MAP.put("\\", "|");
        SYMBOL_CHAR_MAP.put(";", ":");
        SYMBOL_CHAR_MAP.put("'", "\"");
        SYMBOL_CHAR_MAP.put(",", "<");
        SYMBOL_CHAR_MAP.put(".", ">");
        SYMBOL_CHAR_MAP.put("/", "?");
    }

    private boolean hasSymbol(String label) {
        return SYMBOL_CHAR_MAP.containsKey(label);
    }

    // ---------- Keyboard State ----------
    private boolean leftShiftOn = false;
    private boolean leftCtrlOn  = false;
    private boolean leftAltOn   = false;
    private boolean leftMetaOn  = false;
    // Caps kieu Gboard: OFF -> ONCE (hoa 1 ky tu) -> LOCK (khoa) -> OFF.
    private static final int CAPS_OFF = 0;
    private static final int CAPS_ONCE = 1;
    private static final int CAPS_LOCK = 2;
    private static final long CAPS_DOUBLE_TAP_MS = 350L;
    private int mCapsState = CAPS_OFF;
    private long mLastCapsTapTime = 0L;
    private boolean symbolLayerActive = false;
    private int mSymbolLayer = 0; // 0: ABC, 1: ?123, 2: =\<
    private boolean mAutoReturnToAbcOnSpace = false;

    // Keys
    private final List<KeyData> keys = new ArrayList<>();
    private final List<KeyData> mArcKeys = new ArrayList<>();
    private final List<KeyData> mWingKeys = new ArrayList<>();
    private KeyData mLeftSpaceKey;
    private KeyData mRightSpaceKey;
    private KeyData mCommaKey;
    private KeyData mPeriodKey;
    private KeyData mMinusKey;
    private KeyData mCapsKey;
    private KeyData mEnterKey;
    private KeyData mBkspKey;
    private KeyData mSymbolToggleKey;
    private KeyData mCornerDismissKey;
    private KeyData mTouchScrollPadKey;

    // ---- Split-arc: the two halves are independent, movable clusters ----
    /** Offset of each half, as a fraction of the view, so it survives rotation. */
    private float mSplitLeftPctX = 0f, mSplitLeftPctY = 0f;
    private float mSplitRightPctX = 0f, mSplitRightPctY = 0f;
    /** Moving the two halves is an editor-only gesture, never a gameplay one. */
    private boolean mSplitDragEnabled = false;
    /** Set while a drag is moving one of the halves. */
    private boolean mSplitDragActive = false;
    private boolean mSplitDragRight = false;
    private float mSplitDragStartX = 0f, mSplitDragStartY = 0f;
    private float mSplitDragStartPctX = 0f, mSplitDragStartPctY = 0f;
    /** On-screen bounds of each half, refreshed by layoutSplitArc(). */
    private final RectF mLeftClusterRect = new RectF();
    private final RectF mRightClusterRect = new RectF();
    private final RectF mClusterTmp = new RectF();

    private boolean mIsTouchScrolling = false;
    private int mTouchScrollPointerId = -1;
    private float mTouchScrollLastY = 0f;

    private boolean mSpaceSwipeActive = false;
    private float mSpaceDragStartX = 0f;
    private float mSpaceLastMoveX = 0f;

        public interface OnDismissListener {
        void onDismiss();
    }
    private OnDismissListener mDismissListener;
    public void setOnDismissListener(OnDismissListener l) {
        mDismissListener = l;
    }

    private final SparseArray<KeyData> activePointers = new SparseArray<>();

    private float lastRawX, lastRawY;
    private boolean isDragging = false;
    private final int dragHandleHeight;

    private int screenWidth, screenHeight;
    private OnKeyEventListener listener;

    private boolean mIsSplitArcMode = false;
    private float mAnimProgress = 1.0f;
    private boolean mIsAnimating = false;
    private ValueAnimator mAnimator = null;

    private final Handler mLongPressHandler = new Handler(Looper.getMainLooper());
    private KeyData mPendingLongPressKey = null;

    private final Paint bgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint keyBgPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint subTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint modActivePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint handlePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint keyStrokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint iconPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private int keyHeight;
    private int padding = 4;

    // ---- Material 3 tokens (dark scheme, matches theme/M3.java) ----
    // Keys are tonal surfaces with a state layer; the old build stroked every
    // key with a 1px white outline, which is what read as "old Android".
    private static final int MD_SURFACE                  = 0xF21B1B27; // keyboard backdrop
    private static final int MD_SURFACE_CONTAINER_HIGH    = 0xFF34364C; // letter key
    private static final int MD_SURFACE_CONTAINER         = 0xFF2A2B3B; // action key
    private static final int MD_PRIMARY_CONTAINER         = 0xFF004F58; // pressed / latched
    private static final int MD_ON_SURFACE                = 0xFFE6E0E9;
    private static final int MD_ON_SURFACE_VARIANT        = 0xFFA9AFC4; // secondary glyphs
    private static final int MD_ON_PRIMARY_CONTAINER      = 0xFF97F0FF;
    private static final int MD_OUTLINE_VARIANT           = 0xFF494A5C;

    // MD3 shape scale: medium key radius, pill for wide keys, large backdrop.
    private static final float KEY_CORNER_DP      = 12f;
    private static final float WING_CORNER_DP     = 10f;
    private static final float BACKDROP_CORNER_DP = 24f;
    private static final float HANDLE_WIDTH_DP    = 34f;
    private static final float HANDLE_HEIGHT_DP   = 4f;

    private static final int COLOR_TEXT_MUTED = MD_ON_SURFACE_VARIANT;
    private static final int COLOR_PRIMARY = 0xFF80DEEA;
    private static final int COLOR_SECONDARY = 0xFFB4BEFE;

    public interface OnKeyEventListener {
        void onKeyDown(int scanCode);
        void onKeyUp(int scanCode);
        void onTextInput(String text);
        void onScroll(float dy);
    }

    public VirtualKeyboardView(Context context) {
        super(context);
        dragHandleHeight = dpToPx(20);
        updateScreenSize();
        initKeys();
        initPaints();
        setFocusable(true);
        setFocusableInTouchMode(true);
        setClipToOutline(false);
        bringToFront();
    }

    public void updateScreenSize() {
        try {
            ViewParent pp = getParent();
            if (pp instanceof View) {
                int pw = ((View) pp).getWidth();
                int ph = ((View) pp).getHeight();
                if (pw > 0 && ph > 0) {
                    screenWidth = pw;
                    screenHeight = ph;
                    checkOrientationMode();
                    return;
                }
            }
            Resources res = getResources();
            if (res != null) {
                screenWidth = res.getDisplayMetrics().widthPixels;
                screenHeight = res.getDisplayMetrics().heightPixels;
                checkOrientationMode();
            }
        } catch (Exception e) {
            Log.e(TAG, "updateScreenSize error", e);
            screenWidth = 1920;
            screenHeight = 1080;
        }
    }

    private void checkOrientationMode() {
        int orientation = getResources().getConfiguration().orientation;
        boolean isLandscape = (orientation == Configuration.ORIENTATION_LANDSCAPE);
        boolean splitPref = getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .getBoolean(KEY_LANDSCAPE_SPLIT_ARC, true);
        mIsSplitArcMode = isLandscape && splitPref;
        // Never reload while the user is dragging a half: the stored values are
        // the ones from before the drag started and would make it jump back.
        if (mIsSplitArcMode && !mSplitDragActive) loadSplitOffsets();
    }

    /** Restore where the user parked each half of the split keyboard. */
    private void loadSplitOffsets() {
        SharedPreferences p = getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        mSplitLeftPctX = p.getFloat(KEY_SPLIT_LEFT_X, 0f);
        mSplitLeftPctY = p.getFloat(KEY_SPLIT_LEFT_Y, 0f);
        mSplitRightPctX = p.getFloat(KEY_SPLIT_RIGHT_X, 0f);
        mSplitRightPctY = p.getFloat(KEY_SPLIT_RIGHT_Y, 0f);
    }

    /** Persist the half positions after a drag. */
    private void saveSplitOffsets() {
        getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit()
                .putFloat(KEY_SPLIT_LEFT_X, mSplitLeftPctX)
                .putFloat(KEY_SPLIT_LEFT_Y, mSplitLeftPctY)
                .putFloat(KEY_SPLIT_RIGHT_X, mSplitRightPctX)
                .putFloat(KEY_SPLIT_RIGHT_Y, mSplitRightPctY)
                .apply();
    }

    public boolean isSplitArcMode() {
        return mIsSplitArcMode;
    }

    /**
     * Allows dragging the two halves of the split keyboard. Only the layout
     * editor turns this on: while the keyboard is in use the empty space around
     * the keys has to keep falling through to the desktop.
     */
    public void setSplitDragEnabled(boolean enabled) {
        mSplitDragEnabled = enabled;
        if (!enabled) mSplitDragActive = false;
    }

    public void refreshKeyboardPreferences() {
        updateScreenSize();
        initArcKeys();
        initTopWings();
        if (getWidth() > 0 && getHeight() > 0) {
            if (mIsSplitArcMode) {
                layoutSplitArc(getWidth(), getHeight());
            } else {
                layoutKeys(getWidth(), getHeight());
            }
        }
        applySymbolLayer();
        requestLayout();
        invalidate();
    }

    public void showWithAnimation() {
        cancelAnimation();
        checkOrientationMode();
        setRotation(0f);
        setVisibility(VISIBLE);
        bringToFront();
        mIsAnimating = true;
        mAnimProgress = 0f;

        mAnimator = ValueAnimator.ofFloat(0f, 1f);
        mAnimator.setDuration(180); // Snappy power-user 180ms
        mAnimator.setInterpolator(new DecelerateInterpolator(1.8f));
        mAnimator.addUpdateListener(a -> {
            mAnimProgress = (float) a.getAnimatedValue();
            invalidate();
        });
        mAnimator.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(Animator animation) {
                mIsAnimating = false;
                mAnimProgress = 1f;
                invalidate();
            }
        });
        mAnimator.start();
    }

    public void hideWithAnimation(Runnable onComplete) {
        cancelAnimation();
        mIsAnimating = true;
        mAnimator = ValueAnimator.ofFloat(mAnimProgress, 0f);
        mAnimator.setDuration(150);
        mAnimator.setInterpolator(new AccelerateInterpolator(1.5f));
        mAnimator.addUpdateListener(a -> {
            mAnimProgress = (float) a.getAnimatedValue();
            invalidate();
        });
        mAnimator.addListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(Animator animation) {
                mIsAnimating = false;
                mAnimProgress = 0f;
                setVisibility(GONE);
                if (onComplete != null) onComplete.run();
                if (mDismissListener != null) mDismissListener.onDismiss();
            }
        });
        mAnimator.start();
    }

    private void cancelAnimation() {
        if (mAnimator != null) {
            mAnimator.cancel();
            mAnimator = null;
        }
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        try {
            ViewParent parent = getParent();
            while (parent instanceof ViewGroup) {
                ViewGroup vg = (ViewGroup) parent;
                vg.setClipChildren(false);
                vg.setClipToPadding(false);
                parent = vg.getParent();
            }
            View root = getRootView();
            if (root instanceof ViewGroup) {
                ((ViewGroup) root).setClipChildren(false);
                ((ViewGroup) root).setClipToPadding(false);
            }
            bringToFront();
        } catch (Exception e) {
            Log.e(TAG, "onAttachedToWindow error", e);
        }
    }

    @Override
    public void setVisibility(int visibility) {
        try {
            super.setVisibility(visibility);
            if (visibility == VISIBLE) {
                checkOrientationMode();
                post(() -> {
                    try {
                        setInitialPosition();
                        applySymbolLayer();
                    } catch (Exception e) {
                        Log.e(TAG, "setVisibility VISIBLE init error", e);
                    }
                });
            } else if (visibility == GONE) {
                leftShiftOn = false;
                leftCtrlOn = false;
                leftAltOn = false;
                leftMetaOn = false;
                mCapsState = CAPS_OFF;
                symbolLayerActive = false;
                mSymbolLayer = 0;
                mAutoReturnToAbcOnSpace = false;
                for (KeyData k : keys) {
                    k.modActive = false;
                    k.pressed = false;
                }
                for (KeyData k : mArcKeys) {
                    k.modActive = false;
                    k.pressed = false;
                }
                for (KeyData k : mWingKeys) {
                    k.pressed = false;
                    k.modActive = false;
                }
                if (mLeftSpaceKey != null) mLeftSpaceKey.pressed = false;
                if (mRightSpaceKey != null) mRightSpaceKey.pressed = false;
                if (mCommaKey != null) mCommaKey.pressed = false;
                if (mPeriodKey != null) mPeriodKey.pressed = false;
                if (mCapsKey != null) mCapsKey.pressed = false;
                if (mEnterKey != null) mEnterKey.pressed = false;
                if (mBkspKey != null) mBkspKey.pressed = false;
                if (mSymbolToggleKey != null) mSymbolToggleKey.pressed = false;
                if (mCornerDismissKey != null) mCornerDismissKey.pressed = false;

                applySymbolLayer();
                invalidate();
            }
        } catch (Exception e) {
            Log.e(TAG, "setVisibility error", e);
        }
    }

    private void initPaints() {
        // MD3 text is flat: no drop shadow, no synthetic bold.
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setAntiAlias(true);
        textPaint.setColor(MD_ON_SURFACE);
        textPaint.clearShadowLayer();

        subTextPaint.setTextAlign(Paint.Align.CENTER);
        subTextPaint.setAntiAlias(true);
        subTextPaint.setColor(MD_ON_SURFACE_VARIANT);

        keyBgPaint.setAntiAlias(true);
        keyBgPaint.setStyle(Paint.Style.FILL);

        modActivePaint.setAntiAlias(true);
        modActivePaint.setColor(MD_PRIMARY_CONTAINER);

        handlePaint.setColor(MD_ON_SURFACE_VARIANT);
        handlePaint.setAntiAlias(true);

        keyStrokePaint.setStyle(Paint.Style.STROKE);
        keyStrokePaint.setStrokeWidth(1);
        keyStrokePaint.setColor(MD_OUTLINE_VARIANT);
    }

    // ========== Initialize Keys ==========
    private void initKeys() {
        // 1. Standard QWERTY portrait keys
        keys.clear();
        for (int r = 0; r < keyboardRows.length; r++) {
            String[] row = keyboardRows[r];
            for (int c = 0; c < row.length; c++) {
                String rawLabel = row[c];
                String internalLabel = resolveInternalLabel(rawLabel, r, c, row.length);
                String displayLabel = resolveDisplayLabel(rawLabel, internalLabel);
                int keyCode = getKeyCodeForLabel(internalLabel);
                float weight = getWeightForLabel(internalLabel);
                boolean hasSym = hasSymbol(rawLabel);

                KeyData k = new KeyData(displayLabel, keyCode, weight, hasSym);
                k.internalLabel = internalLabel;
                // The bottom-right key closes the keyboard. It is the only way
                // back to the desktop on the full layout, so it must never be
                // treated as a plain key.
                if ("HIDE".equals(internalLabel)) k.isCornerDismiss = true;
                k.symbolChar = hasSym ? SYMBOL_CHAR_MAP.get(rawLabel) : null;
                k.currentLabel = displayLabel;
                k.currentKeyCode = keyCode;
                keys.add(k);
            }
        }

        // 2. Arc keys (Left & Right)
        initArcKeys();

        // 3. Slim Beveled Top Wings
        initTopWings();

        // 4. Day Space che doi + , . 2 ben + Caps tren ?123 + Enter/BKSP ben phai
        mLeftSpaceKey = new KeyData("", KeyEvent.KEYCODE_SPACE, 1f, false);
        mLeftSpaceKey.isSpace = true;
        mLeftSpaceKey.internalLabel = "SpaceL";
        mRightSpaceKey = new KeyData("", KeyEvent.KEYCODE_SPACE, 1f, false);
        mRightSpaceKey.isSpace = true;
        mRightSpaceKey.internalLabel = "SpaceR";
        mCommaKey = new KeyData(",", KeyEvent.KEYCODE_COMMA, 1f, false);
        mCommaKey.internalLabel = ",";
        mPeriodKey = new KeyData(".", KeyEvent.KEYCODE_PERIOD, 1f, false);
        mPeriodKey.internalLabel = ".";
        mMinusKey = new KeyData("-", KeyEvent.KEYCODE_MINUS, 1f, false);
        mMinusKey.internalLabel = "-";
        mCapsKey = new KeyData("Caps", KeyEvent.KEYCODE_CAPS_LOCK, 1f, false);
        mCapsKey.internalLabel = "Caps";
        mEnterKey = new KeyData("Enter", KeyEvent.KEYCODE_ENTER, 1f, false);
        mEnterKey.internalLabel = "Enter";
        mBkspKey = new KeyData("BKSP", KeyEvent.KEYCODE_DEL, 1f, false);
        mBkspKey.internalLabel = "BKSP";

        // 5. Symbol Toggle Key (?123 / ABC)
        mSymbolToggleKey = new KeyData("?123", KeyEvent.KEYCODE_UNKNOWN, 1f, false);
        mSymbolToggleKey.isSymbolToggle = true;

        // 6. Corner Dismiss Key (Fan-shaped in bottom-right corner)
        mCornerDismissKey = new KeyData("DOWN", KeyEvent.KEYCODE_UNKNOWN, 1f, false);
        mCornerDismissKey.isCornerDismiss = true;

        // 7. Touch Scroll Pad Key
        mTouchScrollPadKey = new KeyData("SCROLL", KeyEvent.KEYCODE_UNKNOWN, 1f, false);
        mTouchScrollPadKey.isTouchScrollPad = true;
    }

    private String resolveInternalLabel(String rawLabel, int r, int c, int rowLen) {
        if (rawLabel.equals("Shift")) {
            if (r == 4) return (c == 0) ? "ShiftL" : "ShiftR";
        } else if (rawLabel.equals("Ctrl")) {
            if (r == 5) return (c == 0) ? "CtrlL" : "CtrlR";
        } else if (rawLabel.equals("Alt")) {
            if (r == 5) return (c == 1) ? "AltL" : "AltR";
        }
        return rawLabel;
    }

    private String resolveDisplayLabel(String rawLabel, String internalLabel) {
        if (internalLabel.startsWith("Shift")) return "Shift";
        if (internalLabel.startsWith("Ctrl")) return "Ctrl";
        if (internalLabel.startsWith("Alt")) return "Alt";
        return rawLabel;
    }

    public boolean isRightHanded() {
        return "right".equals(getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .getString(KEY_KEYBOARD_HANDEDNESS, "left"));
    }

    public boolean isTouchScrollEnabled() {
        return getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .getBoolean("enable_touch_scroll_pad", true);
    }

    public int getKeyboardTheme() {
        return getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .getInt(KEY_KEYBOARD_THEME, THEME_FILLED);
    }

    private void initArcKeys() {
        mArcKeys.clear();
        String[][] leftSource;
        String[][] rightSource;
        boolean rh = isRightHanded();
        if (mSymbolLayer == 1) {
            leftSource = rh ? LEFT_SYMBOLS_1_RH : LEFT_SYMBOLS_1_LH;
            rightSource = rh ? RIGHT_SYMBOLS_1_RH : RIGHT_SYMBOLS_1_LH;
        } else if (mSymbolLayer == 2) {
            leftSource = rh ? LEFT_SYMBOLS_2_RH : LEFT_SYMBOLS_2_LH;
            rightSource = rh ? RIGHT_SYMBOLS_2_RH : RIGHT_SYMBOLS_2_LH;
        } else {
            leftSource = rh ? LEFT_LETTERS_RH : LEFT_LETTERS_LH;
            rightSource = rh ? RIGHT_LETTERS_RH : RIGHT_LETTERS_LH;
        }

        // Left Arc
        for (int r = 0; r < leftSource.length; r++) {
            String[] row = leftSource[r];
            for (int c = 0; c < row.length; c++) {
                String raw = row[c];
                String internal = raw;
                String display = resolveDisplayLabel(raw, internal);
                int keyCode = getKeyCodeForLabel(internal);

                KeyData k = new KeyData(display, keyCode, 1f, false);
                k.internalLabel = internal;
                k.currentLabel = display;
                k.currentKeyCode = keyCode;
                k.isArc = true;
                k.isRight = false;
                k.ringIndex = r;
                k.colIndex = c;
                k.ringTotalKeys = row.length;
                if (mSymbolLayer == 0 && NUMBER_SUBSCRIPTS.containsKey(display.toUpperCase())) {
                    k.longPressNumber = NUMBER_SUBSCRIPTS.get(display.toUpperCase());
                }
                mArcKeys.add(k);
            }
        }
        // Right Arc
        for (int r = 0; r < rightSource.length; r++) {
            String[] row = rightSource[r];
            for (int c = 0; c < row.length; c++) {
                String raw = row[c];
                String internal = raw;
                String display = resolveDisplayLabel(raw, internal);
                int keyCode = getKeyCodeForLabel(internal);

                KeyData k = new KeyData(display, keyCode, 1f, false);
                k.internalLabel = internal;
                k.currentLabel = display;
                k.currentKeyCode = keyCode;
                k.isArc = true;
                k.isRight = true;
                k.ringIndex = r;
                k.colIndex = c;
                k.ringTotalKeys = row.length;
                if (mSymbolLayer == 0 && NUMBER_SUBSCRIPTS.containsKey(display.toUpperCase())) {
                    k.longPressNumber = NUMBER_SUBSCRIPTS.get(display.toUpperCase());
                }
                mArcKeys.add(k);
            }
        }
    }

    private void initTopWings() {
        mWingKeys.clear();
        // Nut phu nho gon 3x2 moi ben (12 nut), ha thap gan phim chu.
        // Cánh trái: các phím bổ trợ & thao tác nhanh
        String[] leftDefLabels = {"ESC", "TAB", "CTRL", "ALT", "SHIFT", "SUPER"};
        int[] leftDefCodes = {KeyEvent.KEYCODE_ESCAPE, KeyEvent.KEYCODE_TAB,
                KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_ALT_LEFT,
                KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.KEYCODE_META_LEFT};

        // Cánh phải: cụm phím điều hướng chữ T ngược chuẩn (Inverted-T) + Del/PgUp
        String[] rightDefLabels = {"DEL", "▲", "PGUP", "◀", "▼", "▶"};
        int[] rightDefCodes = {KeyEvent.KEYCODE_FORWARD_DEL, KeyEvent.KEYCODE_DPAD_UP,
                KeyEvent.KEYCODE_PAGE_UP, KeyEvent.KEYCODE_DPAD_LEFT,
                KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT};

        SharedPreferences prefs = getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
        String customJson = prefs.getString(KEY_SPLIT_ARC_TOP_WINGS, null);
        Map<Integer, String> customLabels = new HashMap<>();
        Map<Integer, HudAction> customActions = new HashMap<>();
        if (customJson != null && !customJson.isEmpty()) {
            try {
                JSONArray arr = new JSONArray(customJson);
                for (int i = 0; i < arr.length(); i++) {
                    JSONObject obj = arr.getJSONObject(i);
                    int idx = obj.optInt("index", -1);
                    if (idx >= 0) {
                        customLabels.put(idx, obj.optString("label", ""));
                        if (obj.has("action")) {
                            customActions.put(idx, HudAction.fromJSON(obj.optJSONObject("action")));
                        }
                    }
                }
            } catch (Exception ignored) {}
        }

        // Tuong thich layout cu 4+4 nut: map index phai cu 4..7 sang 6..9 moi.
        if (customLabels.containsKey(4) || customLabels.containsKey(5)
                || customLabels.containsKey(6) || customLabels.containsKey(7)) {
            boolean hasNew = customLabels.containsKey(8) || customLabels.containsKey(9)
                    || customLabels.containsKey(10) || customLabels.containsKey(11)
                    || customLabels.containsKey(6);
            if (!hasNew) {
                for (int old = 4; old <= 7; old++) {
                    if (customLabels.containsKey(old)) {
                        int nw = 6 + (old - 4);
                        customLabels.put(nw, customLabels.get(old));
                        if (customActions.containsKey(old)) customActions.put(nw, customActions.get(old));
                    }
                }
            }
        }

        // Left Top Wing (indices 0..5: 3 cot x 2 dong)
        for (int i = 0; i < 6; i++) {
            String lbl = customLabels.containsKey(i) ? customLabels.get(i) : leftDefLabels[i];
            HudAction act = customActions.get(i);
            int code = (act != null && act.code > 0) ? act.code : leftDefCodes[i];

            KeyData k = new KeyData(lbl, code, 1f, false);
            k.isWing = true;
            k.isRight = false;
            k.wingIndex = i;
            k.customAction = act;
            k.internalLabel = wingInternalLabel(lbl, code, false);
            mWingKeys.add(k);
        }

        // Right Top Wing (indices 6..11: 3 cot x 2 dong)
        for (int i = 0; i < 6; i++) {
            int slotIdx = 6 + i;
            String lbl = customLabels.containsKey(slotIdx) ? customLabels.get(slotIdx) : rightDefLabels[i];
            HudAction act = customActions.get(slotIdx);
            int code = (act != null && act.code > 0) ? act.code : rightDefCodes[i];

            KeyData k = new KeyData(lbl, code, 1f, false);
            k.isWing = true;
            k.isRight = true;
            k.wingIndex = slotIdx;
            k.customAction = act;
            k.internalLabel = wingInternalLabel(lbl, code, true);
            mWingKeys.add(k);
        }
    }

    // Map wing nut phu sang internal modifier de combo duoc voi phim chu.
    // Uu tien code (rebind), fallback sang label khi code la UNKNOWN.
    private String wingInternalLabel(String lbl, int code, boolean right) {
        if (code == KeyEvent.KEYCODE_CTRL_LEFT) return "CtrlL";
        if (code == KeyEvent.KEYCODE_ALT_LEFT) return "AltL";
        if (code == KeyEvent.KEYCODE_SHIFT_LEFT) return "ShiftL";
        if (code == KeyEvent.KEYCODE_META_LEFT) return "MetaL";
        if (code == KeyEvent.KEYCODE_CTRL_RIGHT) return "CtrlR";
        if (code == KeyEvent.KEYCODE_ALT_RIGHT) return "AltR";
        if (code == KeyEvent.KEYCODE_SHIFT_RIGHT) return "ShiftR";
        if (code == KeyEvent.KEYCODE_META_RIGHT) return "MetaL";
        if (code == KeyEvent.KEYCODE_CAPS_LOCK) return "Caps";
        if (lbl == null) return "Wing";
        String u = lbl.trim().toUpperCase();
        if (u.equals("CTRL")) return right ? "CtrlR" : "CtrlL";
        if (u.equals("ALT")) return right ? "AltR" : "AltL";
        if (u.equals("SHIFT")) return right ? "ShiftR" : "ShiftL";
        if (u.equals("SUPER") || u.equals("META")) return "MetaL";
        if (u.equals("CAPS")) return "Caps";
        return lbl;
    }

    private int getKeyCodeForLabel(String label) {
        switch (label) {
            case "ESC": return KeyEvent.KEYCODE_ESCAPE;
            case "F1": return KeyEvent.KEYCODE_F1;
            case "F2": return KeyEvent.KEYCODE_F2;
            case "F3": return KeyEvent.KEYCODE_F3;
            case "F4": return KeyEvent.KEYCODE_F4;
            case "F5": return KeyEvent.KEYCODE_F5;
            case "F6": return KeyEvent.KEYCODE_F6;
            case "F7": return KeyEvent.KEYCODE_F7;
            case "F8": return KeyEvent.KEYCODE_F8;
            case "F9": return KeyEvent.KEYCODE_F9;
            case "F10": return KeyEvent.KEYCODE_F10;
            case "F11": return KeyEvent.KEYCODE_F11;
            case "F12": return KeyEvent.KEYCODE_F12;
            case "`": return KeyEvent.KEYCODE_GRAVE;
            case "1": return KeyEvent.KEYCODE_1;
            case "2": return KeyEvent.KEYCODE_2;
            case "3": return KeyEvent.KEYCODE_3;
            case "4": return KeyEvent.KEYCODE_4;
            case "5": return KeyEvent.KEYCODE_5;
            case "6": return KeyEvent.KEYCODE_6;
            case "7": return KeyEvent.KEYCODE_7;
            case "8": return KeyEvent.KEYCODE_8;
            case "9": return KeyEvent.KEYCODE_9;
            case "0": return KeyEvent.KEYCODE_0;
            case "-": return KeyEvent.KEYCODE_MINUS;
            case "=": return KeyEvent.KEYCODE_EQUALS;
            case "+": return KeyEvent.KEYCODE_PLUS;
            case "@": return KeyEvent.KEYCODE_AT;
            case "#": return KeyEvent.KEYCODE_POUND;
            case "$": return KeyEvent.KEYCODE_4; // Shift+4
            case "_": return KeyEvent.KEYCODE_MINUS;
            case "/": return KeyEvent.KEYCODE_SLASH;
            case "%": return KeyEvent.KEYCODE_5;
            case "&": return KeyEvent.KEYCODE_7;
            case "*": return KeyEvent.KEYCODE_STAR;
            case "(": return KeyEvent.KEYCODE_NUMPAD_LEFT_PAREN;
            case ")": return KeyEvent.KEYCODE_NUMPAD_RIGHT_PAREN;
            case "\"": return KeyEvent.KEYCODE_APOSTROPHE;
            case "'": return KeyEvent.KEYCODE_APOSTROPHE;
            case ":": return KeyEvent.KEYCODE_SEMICOLON;
            case ";": return KeyEvent.KEYCODE_SEMICOLON;
            case "!": return KeyEvent.KEYCODE_1;
            case "?": return KeyEvent.KEYCODE_SLASH;
            case "BKSP": return KeyEvent.KEYCODE_DEL;
            case "Tab": return KeyEvent.KEYCODE_TAB;
            case "Q": return KeyEvent.KEYCODE_Q;
            case "W": return KeyEvent.KEYCODE_W;
            case "E": return KeyEvent.KEYCODE_E;
            case "R": return KeyEvent.KEYCODE_R;
            case "T": return KeyEvent.KEYCODE_T;
            case "Y": return KeyEvent.KEYCODE_Y;
            case "U": return KeyEvent.KEYCODE_U;
            case "I": return KeyEvent.KEYCODE_I;
            case "O": return KeyEvent.KEYCODE_O;
            case "P": return KeyEvent.KEYCODE_P;
            case "[": return KeyEvent.KEYCODE_LEFT_BRACKET;
            case "]": return KeyEvent.KEYCODE_RIGHT_BRACKET;
            case "\\": return KeyEvent.KEYCODE_BACKSLASH;
            case "Caps": return KeyEvent.KEYCODE_CAPS_LOCK;
            case "A": return KeyEvent.KEYCODE_A;
            case "S": return KeyEvent.KEYCODE_S;
            case "D": return KeyEvent.KEYCODE_D;
            case "F": return KeyEvent.KEYCODE_F;
            case "G": return KeyEvent.KEYCODE_G;
            case "H": return KeyEvent.KEYCODE_H;
            case "J": return KeyEvent.KEYCODE_J;
            case "K": return KeyEvent.KEYCODE_K;
            case "L": return KeyEvent.KEYCODE_L;
            case "Enter": return KeyEvent.KEYCODE_ENTER;
            case "Shift":
            case "ShiftL": return KeyEvent.KEYCODE_SHIFT_LEFT;
            case "ShiftR": return KeyEvent.KEYCODE_SHIFT_RIGHT;
            case "Z": return KeyEvent.KEYCODE_Z;
            case "X": return KeyEvent.KEYCODE_X;
            case "C": return KeyEvent.KEYCODE_C;
            case "V": return KeyEvent.KEYCODE_V;
            case "B": return KeyEvent.KEYCODE_B;
            case "N": return KeyEvent.KEYCODE_N;
            case "M": return KeyEvent.KEYCODE_M;
            case ",": return KeyEvent.KEYCODE_COMMA;
            case "UP":
            case "▲":
            case "↑": return KeyEvent.KEYCODE_DPAD_UP;
            case "DN":
            case "DOWN":
            case "▼":
            case "↓": return KeyEvent.KEYCODE_DPAD_DOWN;
            case "LT":
            case "LEFT":
            case "◀":
            case "←": return KeyEvent.KEYCODE_DPAD_LEFT;
            case "RT":
            case "RIGHT":
            case "▶":
            case "→": return KeyEvent.KEYCODE_DPAD_RIGHT;
            case "PGUP": return KeyEvent.KEYCODE_PAGE_UP;
            case "PGDN": return KeyEvent.KEYCODE_PAGE_DOWN;
            case "DEL": return KeyEvent.KEYCODE_FORWARD_DEL;
            case ".": return KeyEvent.KEYCODE_PERIOD;
            case "Ctrl":
            case "CtrlL": return KeyEvent.KEYCODE_CTRL_LEFT;
            case "CtrlR": return KeyEvent.KEYCODE_CTRL_RIGHT;
            case "Alt":
            case "AltL": return KeyEvent.KEYCODE_ALT_LEFT;
            case "AltR": return KeyEvent.KEYCODE_ALT_RIGHT;
            case "Home": return KeyEvent.KEYCODE_MOVE_HOME;
            case "End": return KeyEvent.KEYCODE_MOVE_END;
            case "SUPER":
            case "Meta":
            case "MetaL": return KeyEvent.KEYCODE_META_LEFT;
            case "Space": return KeyEvent.KEYCODE_SPACE;
            default: return KeyEvent.KEYCODE_UNKNOWN;
        }
    }

    private float getWeightForLabel(String label) {
        switch (label) {
            case "ESC":
            case "F1": case "F2": case "F3": case "F4": case "F5":
            case "F6": case "F7": case "F8": case "F9": case "F10":
            case "F11": case "F12":
            case "Home": case "End":
                return 1.0f;
            case "BKSP": return 2.0f;
            case "Tab": return 1.5f;
            case "Caps": return 1.5f;
            case "Enter": return 1.5f;
            case "ShiftL": return 1.75f;
            case "ShiftR": return 1.75f;
            case "CtrlL": return 1.5f;
            case "CtrlR": return 1.5f;
            case "AltL": return 1.5f;
            case "AltR": return 1.5f;
            case "Space": return 3.0f;
            default: return 1.0f;
        }
    }

    public void setOnKeyEventListener(OnKeyEventListener l) {
        this.listener = l;
    }

    // ========== Measure and Layout ==========
    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        try {
            updateScreenSize();
            if (mIsSplitArcMode) {
                int pw = MeasureSpec.getSize(widthMeasureSpec);
                int ph = MeasureSpec.getSize(heightMeasureSpec);
                if (pw <= 0) pw = screenWidth;
                if (ph <= 0) ph = screenHeight;
                setMeasuredDimension(pw, ph);
                return;
            }

            int desiredWidth = (int) (screenWidth * 0.45f);
            if (desiredWidth < 400) desiredWidth = 400;
            int maxWidth = MeasureSpec.getSize(widthMeasureSpec);
            if (maxWidth > 0 && desiredWidth > maxWidth) desiredWidth = maxWidth;
            int rowCount = keyboardRows.length;
            int keyH = dpToPx(32);
            int totalHeight = dragHandleHeight + padding + rowCount * (keyH + padding) + padding;
            if (totalHeight <= 0) totalHeight = 350;
            setMeasuredDimension(desiredWidth, totalHeight);
        } catch (Exception e) {
            Log.e(TAG, "onMeasure error", e);
            setMeasuredDimension(600, 350);
        }
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        updateScreenSize();
        try {
            if (mIsSplitArcMode) {
                layoutSplitArc(w, h);
                setRotation(0f);
            } else {
                setRotation(0f);
                layoutKeys(w, h);
                post(this::setInitialPosition);
            }
        } catch (Exception e) {
            Log.e(TAG, "onSizeChanged error", e);
        }
    }

    private void layoutKeys(int viewW, int viewH) {
        int totalRows = keyboardRows.length;
        if (totalRows == 0) return;

        int availH = viewH - dragHandleHeight - padding * (totalRows + 1);
        if (availH < 20) availH = 20;
        keyHeight = availH / totalRows;
        if (keyHeight < 20) keyHeight = 20;

        int keyIndex = 0;
        for (int row = 0; row < totalRows; row++) {
            String[] rowLabels = keyboardRows[row];
            int numKeys = rowLabels.length;

            float totalWeight = 0;
            for (int c = 0; c < numKeys; c++) {
                KeyData k = keys.get(keyIndex + c);
                totalWeight += k.weight;
            }
            if (totalWeight <= 0) totalWeight = 1.0f;

            int rowWidth = viewW - padding * (numKeys + 1);
            if (rowWidth < 10) rowWidth = viewW;
            float unit = rowWidth / totalWeight;

            int rowY = dragHandleHeight + padding + row * (keyHeight + padding);
            if (rowY < 0) rowY = 0;

            int x = padding;
            for (int c = 0; c < numKeys; c++) {
                KeyData k = keys.get(keyIndex + c);
                int w = (int) (unit * k.weight);
                if (w < 25) w = 25;
                if (c == numKeys - 1) {
                    w = viewW - x - padding;
                    if (w < 25) w = 25;
                }
                k.rect.set(x, rowY, x + w, rowY + keyHeight);
                x += w + padding;
            }
            keyIndex += numKeys;
        }
    }

    private void layoutSplitArc(int viewW, int viewH) {
        if (viewW <= 0 || viewH <= 0) return;
        float density = getResources().getDisplayMetrics().density;

        float bottomMarginDp = getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .getFloat("keyboard_bottom_margin_dp", 8f);
        float customKeyHDp = getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .getFloat("keyboard_key_height_dp", 42f);

        float u = 38f * density;
        float keyH = customKeyHDp * density;
        float gap = 5f * density;
        float indent = 0.5f * u;
        float capsW = 1.5f * u;
        float bkspW = 1.5f * u;
        float enterW = 1.5f * u;
        float margin = 8f * density;
        float rightMargin = viewW - margin;
        boolean rh = isRightHanded();

        // Vertical positions of the 4 rows (bottom-up):
        float row3Bottom = viewH - bottomMarginDp * density;
        float row3Top = row3Bottom - keyH;

        float row2Bottom = row3Top - gap;
        float row2Top = row2Bottom - keyH;

        float row1Bottom = row2Top - gap;
        float row1Top = row1Bottom - keyH;

        float row0Bottom = row1Top - gap;
        float row0Top = row0Bottom - keyH;

        // 1. Left Cluster Letters
        // Row 0: starts at margin
        // Row 1: indented 0.5u -> W center is halfway between A & S!
        // Row 2: starts after Caps (1.5u + gap) -> Z is directly under S!
        for (KeyData k : mArcKeys) {
            if (k.isRight) continue;
            k.keyHeight = keyH;
            k.keyWidth = u;

            float kLeft, kTop;
            if (k.ringIndex == 2) {
                // Row 0: Q, W, E, R, T
                kLeft = margin + k.colIndex * (u + gap);
                kTop = row0Top;
            } else if (k.ringIndex == 1) {
                // Row 1: A, S, D, F (, G if !rh)
                kLeft = margin + indent + k.colIndex * (u + gap);
                kTop = row1Top;
            } else {
                // Row 2: Z, X, C (, V if !rh)
                kLeft = margin + capsW + gap + k.colIndex * (u + gap);
                kTop = row2Top;
            }
            k.rect.set(Math.round(kLeft), Math.round(kTop), Math.round(kLeft + u), Math.round(kTop + keyH));
            k.currentCx = k.rect.centerX();
            k.currentCy = k.rect.centerY();
        }

        // Left Caps / Symbol layer key (Row 2, far left, width = 1.5u)
        if (mCapsKey != null) {
            mCapsKey.keyHeight = keyH;
            mCapsKey.keyWidth = capsW;
            mCapsKey.rect.set(Math.round(margin), Math.round(row2Top), Math.round(margin + capsW), Math.round(row2Bottom));
            mCapsKey.currentCx = mCapsKey.rect.centerX();
            mCapsKey.currentCy = mCapsKey.rect.centerY();
            if (mSymbolLayer == 1) {
                mCapsKey.currentLabel = "=\\<";
            } else if (mSymbolLayer == 2) {
                mCapsKey.currentLabel = "?123";
            } else {
                mCapsKey.currentLabel = "Caps";
            }
        }

        // Left Row 3 keys:
        // ?123 / ABC directly below Caps (width = 1.5u)
        if (mSymbolToggleKey != null) {
            mSymbolToggleKey.keyHeight = keyH;
            mSymbolToggleKey.keyWidth = capsW;
            mSymbolToggleKey.rect.set(Math.round(margin), Math.round(row3Top), Math.round(margin + capsW), Math.round(row3Bottom));
            mSymbolToggleKey.currentLabel = (mSymbolLayer == 0) ? "?123" : "ABC";
        }
        // Comma directly below Z
        float commaLeft = margin + capsW + gap;
        if (mCommaKey != null) {
            mCommaKey.keyHeight = keyH;
            mCommaKey.keyWidth = u;
            mCommaKey.rect.set(Math.round(commaLeft), Math.round(row3Top), Math.round(commaLeft + u), Math.round(row3Bottom));
            mCommaKey.currentLabel = ",";
        }
        // SpaceL to the right of comma
        float spaceLeft = commaLeft + u + gap;
        float spaceW = Math.max(76f * density, 2.0f * u);
        if (mLeftSpaceKey != null) {
            mLeftSpaceKey.keyHeight = keyH;
            mLeftSpaceKey.keyWidth = spaceW;
            mLeftSpaceKey.rect.set(Math.round(spaceLeft), Math.round(row3Top), Math.round(spaceLeft + spaceW), Math.round(row3Bottom));
            mLeftSpaceKey.currentLabel = "";
        }
        // Minus key to the right of SpaceL
        float minusLeft = spaceLeft + spaceW + gap;
        if (mMinusKey != null) {
            mMinusKey.keyHeight = keyH;
            mMinusKey.keyWidth = u;
            mMinusKey.rect.set(Math.round(minusLeft), Math.round(row3Top), Math.round(minusLeft + u), Math.round(row3Bottom));
            mMinusKey.currentLabel = "-";
        }

        // 2. Right Cluster Letters
        // Row 0: rightmost key at rightMargin
        // Row 1: indented 0.5u from rightMargin -> O center is halfway between K & L!
        // Row 2: left of Backspace 1.5u -> M is directly under K!
        for (KeyData k : mArcKeys) {
            if (!k.isRight) continue;
            k.keyHeight = keyH;
            k.keyWidth = u;

            float kRight, kTop;
            if (k.ringIndex == 2) {
                // Row 0: Y, U, I, O, P
                kRight = rightMargin - (k.ringTotalKeys - 1 - k.colIndex) * (u + gap);
                kTop = row0Top;
            } else if (k.ringIndex == 1) {
                // Row 1: (G if rh, ) H, J, K, L
                kRight = (rightMargin - indent) - (k.ringTotalKeys - 1 - k.colIndex) * (u + gap);
                kTop = row1Top;
            } else {
                // Row 2: (V if rh, ) B, N, M
                kRight = (rightMargin - bkspW - gap) - (k.ringTotalKeys - 1 - k.colIndex) * (u + gap);
                kTop = row2Top;
            }
            float kLeft = kRight - u;
            k.rect.set(Math.round(kLeft), Math.round(kTop), Math.round(kRight), Math.round(kTop + keyH));
            k.currentCx = k.rect.centerX();
            k.currentCy = k.rect.centerY();
        }

        // Right Backspace key (Row 2, far right, width = 1.5u)
        if (mBkspKey != null) {
            mBkspKey.keyHeight = keyH;
            mBkspKey.keyWidth = bkspW;
            mBkspKey.rect.set(Math.round(rightMargin - bkspW), Math.round(row2Top), Math.round(rightMargin), Math.round(row2Bottom));
            mBkspKey.currentCx = mBkspKey.rect.centerX();
            mBkspKey.currentCy = mBkspKey.rect.centerY();
        }

        // Right Row 3 keys:
        // Enter directly below Backspace (width = 1.5u)
        if (mEnterKey != null) {
            mEnterKey.keyHeight = keyH;
            mEnterKey.keyWidth = enterW;
            mEnterKey.rect.set(Math.round(rightMargin - enterW), Math.round(row3Top), Math.round(rightMargin), Math.round(row3Bottom));
            mEnterKey.currentCx = mEnterKey.rect.centerX();
            mEnterKey.currentCy = mEnterKey.rect.centerY();
        }
        // Period directly below M
        float periodRight = rightMargin - enterW - gap;
        if (mPeriodKey != null) {
            mPeriodKey.keyHeight = keyH;
            mPeriodKey.keyWidth = u;
            mPeriodKey.rect.set(Math.round(periodRight - u), Math.round(row3Top), Math.round(periodRight), Math.round(row3Bottom));
            mPeriodKey.currentLabel = ".";
        }
        // SpaceR to the left of period
        float spaceRRight = periodRight - u - gap;
        if (mRightSpaceKey != null) {
            mRightSpaceKey.keyHeight = keyH;
            mRightSpaceKey.keyWidth = spaceW;
            mRightSpaceKey.rect.set(Math.round(spaceRRight - spaceW), Math.round(row3Top), Math.round(spaceRRight), Math.round(row3Bottom));
            mRightSpaceKey.currentLabel = "";
        }
        // Dismiss to the left of SpaceR. It is the only way back to the
        // desktop, so it gets a full special-key width and a clear icon.
        float disW = 52f * density;
        float disRight = spaceRRight - spaceW - gap;
        if (mCornerDismissKey != null) {
            mCornerDismissKey.keyHeight = keyH;
            mCornerDismissKey.keyWidth = disW;
            mCornerDismissKey.rect.set(Math.round(disRight - disW), Math.round(row3Top), Math.round(disRight), Math.round(row3Bottom));
        }

        // 3. Touch Scroll Pad in the cluster gap (width = 1u, height = 2 keys + gap)
        if (mTouchScrollPadKey != null) {
            if (isTouchScrollEnabled()) {
                mTouchScrollPadKey.keyHeight = (2 * keyH) + gap;
                mTouchScrollPadKey.keyWidth = u;
                // The pad lives in whichever cluster owns the gap.
                mTouchScrollPadKey.isRight = !rh;
                if (rh) {
                    // Gap on left cluster (to the right of F / C)
                    float fRight = margin + indent + 3 * (u + gap) + u;
                    float padLeft = fRight + gap;
                    mTouchScrollPadKey.rect.set(Math.round(padLeft), Math.round(row1Top), Math.round(padLeft + u), Math.round(row2Bottom));
                } else {
                    // Gap on right cluster (to the left of H / B)
                    float hLeft = (rightMargin - indent) - 3 * (u + gap) - u;
                    float padRight = hLeft - gap;
                    mTouchScrollPadKey.rect.set(Math.round(padRight - u), Math.round(row1Top), Math.round(padRight), Math.round(row2Bottom));
                }
                mTouchScrollPadKey.currentCx = mTouchScrollPadKey.rect.centerX();
                mTouchScrollPadKey.currentCy = mTouchScrollPadKey.rect.centerY();
            } else {
                mTouchScrollPadKey.rect.set(0, 0, 0, 0);
                mTouchScrollPadKey.currentCx = -1000f;
                mTouchScrollPadKey.currentCy = -1000f;
            }
        }

        // 4. Wings (2 rows x 3 cols, enlarged: 44dp x 32dp each)
        float wingColW = 44f * density;
        float wingBtnH = 32f * density;
        float wingSpacing = 4f * density;
        float wingBottom = row0Top - 10f * density;
        float wingRow1Top = wingBottom - wingBtnH;
        float wingRow0Top = wingRow1Top - wingSpacing - wingBtnH;
        float wingTop = Math.max(8f * density, wingRow0Top);

        // Left Wing
        for (KeyData k : mWingKeys) {
            if (k.isRight) continue;
            int col = k.wingIndex % 3;
            int row = k.wingIndex / 3;
            float kLeft = margin + col * (wingColW + wingSpacing);
            float kTop = wingTop + row * (wingBtnH + wingSpacing);
            float kBottom = kTop + wingBtnH;
            k.beveledPath = null;
            k.wingBounds.set(kLeft, kTop, kLeft + wingColW, kBottom);
        }

        // Right Wing
        for (KeyData k : mWingKeys) {
            if (!k.isRight) continue;
            int idx = k.wingIndex - 6;
            int col = idx % 3;
            int row = idx / 3;
            float kRight = rightMargin - (2 - col) * (wingColW + wingSpacing);
            float kLeft = kRight - wingColW;
            float kTop = wingTop + row * (wingBtnH + wingSpacing);
            float kBottom = kTop + wingBtnH;
            k.beveledPath = null;
            k.wingBounds.set(kLeft, kTop, kRight, kBottom);
        }

        // Each half is its own block: shift it by the user's offset and record
        // where it ended up, so dragging and clamping have real bounds.
        applyHalfOffset(false, mSplitLeftPctX * viewW, mSplitLeftPctY * viewH);
        applyHalfOffset(true, mSplitRightPctX * viewW, mSplitRightPctY * viewH);
    }

    /** Faint rounded container that ties one half's keys together. */
    private void drawHalfBackdrop(Canvas canvas, RectF bounds, float slideOffset, int alpha) {
        if (bounds == null || bounds.isEmpty()) return;
        bgPaint.setColor(MD_SURFACE);
        bgPaint.setAlpha(Math.round(0.42f * alpha));
        float radius = dpToPx(BACKDROP_CORNER_DP);
        canvas.drawRoundRect(bounds.left, bounds.top + slideOffset,
                bounds.right, bounds.bottom + slideOffset, radius, radius, bgPaint);
    }

    /** Shift one half of the split keyboard and recompute its bounds. */
    private void applyHalfOffset(boolean right, float dx, float dy) {
        RectF bounds = right ? mRightClusterRect : mLeftClusterRect;
        bounds.setEmpty();
        int idx = Math.round(dx);
        int idy = Math.round(dy);

        for (KeyData k : mArcKeys) {
            if (k.isRight != right) continue;
            offsetKey(k, idx, idy);
            unionRect(bounds, k.rect);
        }
        for (KeyData k : mWingKeys) {
            if (k.isRight != right) continue;
            k.wingBounds.offset(dx, dy);
            if (!k.wingBounds.isEmpty()) {
                if (bounds.isEmpty()) bounds.set(k.wingBounds);
                else bounds.union(k.wingBounds);
            }
        }
        if (right) {
            addHalfKey(bounds, mBkspKey, idx, idy);
            addHalfKey(bounds, mEnterKey, idx, idy);
            addHalfKey(bounds, mPeriodKey, idx, idy);
            addHalfKey(bounds, mRightSpaceKey, idx, idy);
            addHalfKey(bounds, mCornerDismissKey, idx, idy);
        } else {
            addHalfKey(bounds, mCapsKey, idx, idy);
            addHalfKey(bounds, mSymbolToggleKey, idx, idy);
            addHalfKey(bounds, mCommaKey, idx, idy);
            addHalfKey(bounds, mLeftSpaceKey, idx, idy);
            addHalfKey(bounds, mMinusKey, idx, idy);
        }
        if (mTouchScrollPadKey != null && mTouchScrollPadKey.isRight == right)
            addHalfKey(bounds, mTouchScrollPadKey, idx, idy);

        // Leave a little air around the keys so the block reads as one unit and
        // the backdrop does not clip the outer keys.
        if (!bounds.isEmpty()) bounds.inset(-dpToPx(6), -dpToPx(6));
    }

    private void addHalfKey(RectF bounds, KeyData k, int dx, int dy) {
        if (k == null || k.rect.isEmpty()) return;
        offsetKey(k, dx, dy);
        unionRect(bounds, k.rect);
    }

    private void offsetKey(KeyData k, int dx, int dy) {
        if (k == null || k.rect.isEmpty()) return;
        k.rect.offset(dx, dy);
        if (k.isArc || k == mTouchScrollPadKey) {
            k.currentCx += dx;
            k.currentCy += dy;
        }
    }

    private static void unionRect(RectF bounds, Rect r) {
        if (r.isEmpty()) return;
        if (bounds.isEmpty()) {
            bounds.set(r.left, r.top, r.right, r.bottom);
            return;
        }
        if (r.left < bounds.left) bounds.left = r.left;
        if (r.top < bounds.top) bounds.top = r.top;
        if (r.right > bounds.right) bounds.right = r.right;
        if (r.bottom > bounds.bottom) bounds.bottom = r.bottom;
    }

    /** Persist the dragged position as a fraction of the parent, independent of rotation. */
    private void saveKeyboardPosition() {
        ViewParent pp = getParent();
        if (!(pp instanceof View)) return;
        int pw = ((View) pp).getWidth();
        int ph = ((View) pp).getHeight();
        if (pw <= 0 || ph <= 0) return;
        float px = (getTranslationX() + getWidth() * 0.5f) / pw;
        float py = (getTranslationY() + getHeight() * 0.5f) / ph;
        getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit()
                .putFloat("keyboard_pos_x_pct", px)
                .putFloat("keyboard_pos_y_pct", py)
                .apply();
    }

    public void setInitialPosition() {
        if (mIsSplitArcMode) {
            setTranslationX(0);
            setTranslationY(0);
            bringToFront();
            return;
        }

        try {
            if (getWidth() == 0 || getHeight() == 0) {
                post(this::setInitialPosition);
                return;
            }
            int w = getWidth();
            int h = getHeight();
            int pw = 0, ph = 0;
            ViewParent pp = getParent();
            if (pp instanceof View) {
                pw = ((View) pp).getWidth();
                ph = ((View) pp).getHeight();
            }
            if (pw <= 0 || ph <= 0) {
                post(this::setInitialPosition);
                return;
            }
            if (w > 0 && h > 0) {
                android.content.SharedPreferences prefs =
                        getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
                float savedX = prefs.getFloat("keyboard_pos_x_pct", -1f);
                float savedY = prefs.getFloat("keyboard_pos_y_pct", -1f);
                float x, y;
                if (savedX >= 0f && savedY >= 0f) {
                    // Restore the user's dragged position; the centre point is
                    // stored so the same fraction works after a rotation.
                    x = savedX * pw - w * 0.5f;
                    y = savedY * ph - h * 0.5f;
                } else {
                    x = (pw - w) / 2f;
                    y = ph - h - dpToPx(50);
                }
                x = Math.max(0, Math.min(x, pw - w));
                y = Math.max(0, Math.min(y, ph - h));
                setTranslationX(x);
                setTranslationY(y);
                bringToFront();
            }
        } catch (Exception e) {
            Log.e(TAG, "setInitialPosition error", e);
        }
    }

    // ========== Core State Management ==========
    private void updateSymbolLayer() {
        symbolLayerActive = leftShiftOn;
        applySymbolLayer();
    }

    private void applySymbolLayer() {
        try {
            List<KeyData> all = new ArrayList<>(keys);
            for (KeyData k : all) {
                if (k.hasSymbol && k.symbolChar != null) {
                    if (symbolLayerActive) {
                        k.currentLabel = k.symbolChar;
                        k.currentKeyCode = getKeyCodeForSymbol(k.symbolChar);
                    } else {
                        k.currentLabel = k.defaultLabel;
                        k.currentKeyCode = k.defaultKeyCode;
                    }
                }
            }
            for (KeyData k : all) {
                if ("ShiftL".equals(k.internalLabel) || "ShiftR".equals(k.internalLabel)) {
                    k.modActive = leftShiftOn;
                } else if ("CtrlL".equals(k.internalLabel)) {
                    k.modActive = leftCtrlOn;
                } else if ("AltL".equals(k.internalLabel)) {
                    k.modActive = leftAltOn;
                } else if ("Caps".equals(k.internalLabel)) {
                    k.modActive = mCapsState != CAPS_OFF;
                }
            }
            for (KeyData k : mWingKeys) {
                if ("Caps".equals(k.internalLabel)) {
                    k.modActive = mCapsState != CAPS_OFF;
                }
            }
            invalidate();
        } catch (Exception e) {
            Log.e(TAG, "applySymbolLayer error", e);
        }
    }

    private int getKeyCodeForSymbol(String symbol) {
        for (Map.Entry<String, String> entry : SYMBOL_CHAR_MAP.entrySet()) {
            if (entry.getValue().equals(symbol)) {
                return getKeyCodeForLabel(entry.getKey());
            }
        }
        return KeyEvent.KEYCODE_UNKNOWN;
    }

    // ========== Send Keys ==========
    private void sendKey(int keyCode, boolean down) {
        if (listener == null) return;
        int scan = KeyCodeMapper.getScanCode(keyCode);
        if (scan != -1) {
            if (down) listener.onKeyDown(scan);
            else listener.onKeyUp(scan);
        }
    }

    private void sendKeyWithCaps(int keyCode, boolean down) {
        if (listener == null) return;
        boolean isLetter = keyCode >= KeyEvent.KEYCODE_A && keyCode <= KeyEvent.KEYCODE_Z;
        boolean shouldApplyCaps = isLetter && mCapsState != CAPS_OFF;

        if (shouldApplyCaps) {
            if (down) {
                sendKey(KeyEvent.KEYCODE_SHIFT_LEFT, true);
                sendKey(keyCode, true);
            } else {
                sendKey(keyCode, false);
                sendKey(KeyEvent.KEYCODE_SHIFT_LEFT, false);
            }
        } else {
            sendKey(keyCode, down);
        }
    }

    private void executeCombination(KeyData targetKey) {
        List<KeyData> mods = new ArrayList<>();
        List<KeyData> all = new ArrayList<>(keys);
        all.addAll(mArcKeys);
        all.addAll(mWingKeys);
        for (KeyData k : all) {
            if (isLeftModifier(k) && getLeftModifierState(k)) {
                if (!mods.contains(k)) mods.add(k);
            }
        }
        for (int i = 0; i < activePointers.size(); i++) {
            KeyData held = activePointers.valueAt(i);
            if (held != null && held.isWing && isLeftModifier(held) && !mods.contains(held)) {
                mods.add(held);
            }
        }
        if (mods.isEmpty()) return;

        // Send all active modifiers down
        for (KeyData k : mods) {
            sendKeyData(k, true);
        }

        // Send target key
        String lbl = targetKey.currentLabel != null ? targetKey.currentLabel : targetKey.defaultLabel;
        if (SYMBOL_MAP.containsKey(lbl)) {
            SymbolInfo info = SYMBOL_MAP.get(lbl);
            if (info.needsShift) sendKey(KeyEvent.KEYCODE_SHIFT_LEFT, true);
            sendKey(info.baseKeyCode, true);
            sendKey(info.baseKeyCode, false);
            if (info.needsShift) sendKey(KeyEvent.KEYCODE_SHIFT_LEFT, false);
        } else {
            if (targetKey.isWing && targetKey.customAction != null) {
                dispatchWingAction(targetKey.customAction, true);
                dispatchWingAction(targetKey.customAction, false);
            } else {
                int code = targetKey.currentKeyCode;
                sendKey(code, true);
                sendKey(code, false);
            }
        }

        // Release modifiers in reverse order
        for (int i = mods.size() - 1; i >= 0; i--) {
            sendKeyData(mods.get(i), false);
        }

        // Reset all latched modifiers
        leftShiftOn = false;
        leftCtrlOn = false;
        leftAltOn = false;
        leftMetaOn = false;
        for (KeyData k : all) {
            if (isLeftModifier(k)) {
                k.modActive = false;
            }
        }
        consumeCapsOnce(targetKey.currentKeyCode);
        updateSymbolLayer();
        invalidate();
    }

    private void dispatchNormalOrSymbolKey(KeyData key, boolean isDown) {
        String lbl = key.currentLabel != null ? key.currentLabel : key.defaultLabel;
        if (SYMBOL_MAP.containsKey(lbl)) {
            SymbolInfo info = SYMBOL_MAP.get(lbl);
            if (isDown) {
                if (info.needsShift) {
                    sendKey(KeyEvent.KEYCODE_SHIFT_LEFT, true);
                    sendKey(info.baseKeyCode, true);
                    sendKey(info.baseKeyCode, false);
                    sendKey(KeyEvent.KEYCODE_SHIFT_LEFT, false);
                } else {
                    sendKey(info.baseKeyCode, true);
                    sendKey(info.baseKeyCode, false);
                }
                if (mSymbolLayer > 0 && !isSingleDigit(lbl)) {
                    mAutoReturnToAbcOnSpace = true;
                }
            }
            return;
        }

        // Text fallback for non-ASCII or extra Unicode symbols
        if (lbl != null && lbl.length() == 1 && !Character.isLetterOrDigit(lbl.charAt(0))) {
            if (isDown) {
                if (listener != null) {
                    listener.onTextInput(lbl);
                }
                if (mSymbolLayer > 0) {
                    mAutoReturnToAbcOnSpace = true;
                }
            }
            return;
        }

        int code = key.currentKeyCode;
        if (isDown) {
            sendKeyWithCaps(code, true);
        } else {
            sendKeyWithCaps(code, false);
            consumeCapsOnce(code);
        }
    }

    private boolean isSingleDigit(String s) {
        return s != null && s.length() == 1 && Character.isDigit(s.charAt(0));
    }

    private boolean isLeftModifier(KeyData k) {
        if (k == null) return false;
        String in = k.internalLabel;
        return "ShiftL".equals(in) || "CtrlL".equals(in) || "AltL".equals(in) || "MetaL".equals(in)
                || "ShiftR".equals(in) || "CtrlR".equals(in) || "AltR".equals(in);
    }

    private boolean getLeftModifierState(KeyData k) {
        if (k == null) return false;
        String in = k.internalLabel;
        if ("ShiftL".equals(in) || "ShiftR".equals(in)) return leftShiftOn;
        if ("CtrlL".equals(in) || "CtrlR".equals(in)) return leftCtrlOn;
        if ("AltL".equals(in) || "AltR".equals(in)) return leftAltOn;
        if ("MetaL".equals(in)) return leftMetaOn;
        return false;
    }

    // Caps kieu Gboard:
    // - Tu OFF: cham 1 lan -> ONCE (viet hoa 1 ky tu)
    // - Tu ONCE: cham tiep nhanh (<=350ms) -> LOCK (khoa CapsLock); neu cham cham (>350ms) -> OFF (tat)
    // - Tu LOCK: cham lan nua -> OFF (tat)
    private void tapCapsKey(KeyData k) {
        long now = android.os.SystemClock.uptimeMillis();
        if (mCapsState == CAPS_LOCK) {
            mCapsState = CAPS_OFF;
        } else if (mCapsState == CAPS_ONCE) {
            if ((now - mLastCapsTapTime) <= CAPS_DOUBLE_TAP_MS) {
                mCapsState = CAPS_LOCK;
            } else {
                mCapsState = CAPS_OFF;
            }
        } else {
            mCapsState = CAPS_ONCE;
        }
        mLastCapsTapTime = now;
        applySymbolLayer();
        invalidate();
    }

    // One-shot chi bi phim ky tu thuong tieu thu; space/dieu huong giu nguyen.
    private void consumeCapsOnce(int keyCode) {
        if (mCapsState == CAPS_ONCE && isPrintableKeyCode(keyCode)) {
            mCapsState = CAPS_OFF;
            applySymbolLayer();
        }
    }

    private boolean isPrintableKeyCode(int keyCode) {
        if (keyCode == KeyEvent.KEYCODE_SPACE || keyCode == KeyEvent.KEYCODE_UNKNOWN) return false;
        if (isDirectionKey(keyCode)) return false;
        if (keyCode == KeyEvent.KEYCODE_MOVE_HOME || keyCode == KeyEvent.KEYCODE_MOVE_END) return false;
        if (keyCode == KeyEvent.KEYCODE_DEL || keyCode == KeyEvent.KEYCODE_FORWARD_DEL) return false;
        if (keyCode == KeyEvent.KEYCODE_ENTER) return false;
        return keyCode > 0;
    }

    private void toggleLeftModifier(KeyData k) {
        if (k == null) return;
        String in = k.internalLabel;
        boolean newState;
        if ("ShiftL".equals(in) || "ShiftR".equals(in)) {
            newState = !leftShiftOn;
            leftShiftOn = newState;
        } else if ("CtrlL".equals(in) || "CtrlR".equals(in)) {
            newState = !leftCtrlOn;
            leftCtrlOn = newState;
        } else if ("AltL".equals(in) || "AltR".equals(in)) {
            newState = !leftAltOn;
            leftAltOn = newState;
        } else if ("MetaL".equals(in)) {
            newState = !leftMetaOn;
            leftMetaOn = newState;
        } else {
            return;
        }
        k.modActive = newState;
        updateSymbolLayer();
        invalidate();
    }

    private boolean hasAnyActiveModifier() {
        if (leftShiftOn || leftCtrlOn || leftAltOn || leftMetaOn) return true;
        for (KeyData w : mWingKeys) {
            if (w.modActive && isLeftModifier(w)) return true;
        }
        return false;
    }

    private boolean isHapticEnabled() {
        return getContext().getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
                .getBoolean(KEY_HAPTIC_FEEDBACK, true);
    }

    // ========== onTouchEvent ==========
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        try {
            int action = event.getActionMasked();
            float x = event.getX();
            float y = event.getY();

            // Portrait drag handle
            if (!mIsSplitArcMode && action == MotionEvent.ACTION_DOWN && y < dragHandleHeight) {
                isDragging = true;
                lastRawX = x;
                lastRawY = y;
                bringToFront();
                return true;
            }

            if (!mIsSplitArcMode && isDragging) {
                switch (action) {
                    case MotionEvent.ACTION_MOVE:
                        float dx = event.getX() - lastRawX;
                        float dy = event.getY() - lastRawY;
                        setTranslationX(getTranslationX() + dx);
                        setTranslationY(getTranslationY() + dy);
                        lastRawX = event.getX();
                        lastRawY = event.getY();
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        isDragging = false;
                        saveKeyboardPosition();
                        return true;
                }
                return true;
            }

            // Split arc: each half is a movable block. A press that misses every
            // key grabs the half it is closest to and drags it, so the two thumb
            // clusters can be parked anywhere on screen.
            if (mIsSplitArcMode && handleSplitHalfDrag(action, x, y)) {
                return true;
            }

            int pointerIndex = event.getActionIndex();
            int pointerId = event.getPointerId(pointerIndex);
            float touchX = event.getX(pointerIndex);
            float touchY = event.getY(pointerIndex);
            KeyData hitKey = findKeyAt(touchX, touchY);

            switch (action) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_POINTER_DOWN:
                    if (action == MotionEvent.ACTION_POINTER_DOWN || activePointers.size() > 0) {
                        mPendingLongPressKey = null;
                        mLongPressHandler.removeCallbacksAndMessages(null);
                    }
                    if (hitKey == null) {
                        return false;
                    }

                    if (isHapticEnabled()) {
                        performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    }

                    // 1. Touch Scroll Pad Key
                    if (hitKey == mTouchScrollPadKey) {
                        mIsTouchScrolling = true;
                        mTouchScrollPointerId = pointerId;
                        mTouchScrollLastY = touchY;
                        activePointers.put(pointerId, hitKey);
                        hitKey.pressed = true;
                        invalidate();
                        return true;
                    }

                    // 2. Corner Dismiss Key
                    if (hitKey == mCornerDismissKey || hitKey.isCornerDismiss) {
                        hideWithAnimation(null);
                        return true;
                    }

                    // 3. Symbol Toggle Key (?123 / ABC)
                    if (hitKey == mSymbolToggleKey || hitKey.isSymbolToggle) {
                        if (mSymbolLayer == 0) {
                            mSymbolLayer = 1;
                        } else {
                            mSymbolLayer = 0;
                        }
                        mAutoReturnToAbcOnSpace = false;
                        initArcKeys();
                        layoutSplitArc(getWidth(), getHeight());
                        invalidate();
                        return true;
                    }

                    // 4. Caps / =\< key
                    if (hitKey == mCapsKey || "Caps".equals(hitKey.internalLabel)) {
                        if (mSymbolLayer == 1) {
                            mSymbolLayer = 2;
                            initArcKeys();
                            layoutSplitArc(getWidth(), getHeight());
                            invalidate();
                            return true;
                        } else if (mSymbolLayer == 2) {
                            mSymbolLayer = 1;
                            initArcKeys();
                            layoutSplitArc(getWidth(), getHeight());
                            invalidate();
                            return true;
                        } else {
                            tapCapsKey(hitKey);
                            return true;
                        }
                    }

                    // 5. Modifier Wing key tap (latches the modifier, HUD-style)
                    if (isLeftModifier(hitKey)) {
                        toggleLeftModifier(hitKey);
                        if (isHapticEnabled()) {
                            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                        }
                        return true;
                    }

                    // 6. Top Wing non-modifier key (ESC, TAB, DEL, ▲, ◀, ▼, ▶, etc.)
                    // Instant response on ACTION_DOWN, smooth auto-repeat while held
                    if (hitKey.isWing) {
                        activePointers.put(pointerId, hitKey);
                        hitKey.pressed = true;
                        if (isHapticEnabled()) {
                            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                        }
                        if (hasAnyActiveModifier()) {
                            hitKey.comboFired = true;
                            executeCombination(hitKey);
                        } else {
                            if (hitKey.customAction != null) {
                                dispatchWingAction(hitKey.customAction, true);
                            } else {
                                sendKey(hitKey.currentKeyCode, true);
                            }
                            startWingRepeat(hitKey);
                        }
                        invalidate();
                        return true;
                    }

                    // 7. Space keys (touch down: initialize cursor swipe tracking)
                    if (hitKey == mLeftSpaceKey || hitKey == mRightSpaceKey) {
                        activePointers.put(pointerId, hitKey);
                        hitKey.pressed = true;
                        mSpaceSwipeActive = false;
                        mSpaceDragStartX = touchX;
                        mSpaceLastMoveX = touchX;
                        invalidate();
                        return true;
                    }

                    // 8. Long-press number on top row (Q->1, W->2 ... P->0)
                    if (hitKey.isArc && hitKey.longPressNumber != null) {
                        mPendingLongPressKey = hitKey;
                        final KeyData numKey = hitKey;
                        mLongPressHandler.postDelayed(() -> {
                            if (mPendingLongPressKey == numKey) {
                                mPendingLongPressKey = null;
                                numKey.longPressFired = true;
                                if (isHapticEnabled()) {
                                    performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
                                }
                                int numKeyCode = getKeyCodeForLabel(numKey.longPressNumber);
                                sendKey(numKeyCode, true);
                                postDelayed(() -> sendKey(numKeyCode, false), 40);
                                numKey.pressed = false;
                                invalidate();
                            }
                        }, 350);
                    }

                    activePointers.put(pointerId, hitKey);
                    hitKey.pressed = true;

                    // 9. All other keys (letters, symbols, punctuation, enter, bksp)
                    if (hasAnyActiveModifier()) {
                        hitKey.comboFired = true;
                        executeCombination(hitKey);
                    } else if (hitKey.longPressNumber == null) {
                        int code = hitKey.currentKeyCode;
                        if (isDirectionKey(code) || code == KeyEvent.KEYCODE_MOVE_HOME || code == KeyEvent.KEYCODE_MOVE_END) {
                            sendKey(code, true);
                        } else {
                            dispatchNormalOrSymbolKey(hitKey, true);
                        }
                    }
                    invalidate();
                    return true;

                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_POINTER_UP: {
                    mPendingLongPressKey = null;
                    mLongPressHandler.removeCallbacksAndMessages(null);

                    KeyData released = activePointers.get(pointerId);
                    if (released == null) {
                        if (!mIsTouchScrolling && activePointers.size() == 0) return false;
                        break;
                    }

                    if (released == mTouchScrollPadKey || pointerId == mTouchScrollPointerId) {
                        mIsTouchScrolling = false;
                        mTouchScrollPointerId = -1;
                        if (mTouchScrollPadKey != null) mTouchScrollPadKey.pressed = false;
                        activePointers.remove(pointerId);
                        invalidate();
                        if (released == mTouchScrollPadKey) break;
                    }

                    if (released.comboFired) {
                        released.comboFired = false;
                    } else if (released.isArc && released.longPressNumber != null) {
                        if (!released.longPressFired) {
                            int code = released.currentKeyCode;
                            if (hasAnyActiveModifier()) {
                                executeCombination(released);
                            } else {
                                dispatchNormalOrSymbolKey(released, true);
                                postDelayed(() -> dispatchNormalOrSymbolKey(released, false), 30);
                            }
                        }
                        released.longPressFired = false;
                    } else if (released.isWing && !isLeftModifier(released)) {
                        stopWingRepeat();
                        if (released.comboFired) {
                            released.comboFired = false;
                        } else {
                            if (released.customAction != null) {
                                dispatchWingAction(released.customAction, false);
                            } else {
                                sendKey(released.currentKeyCode, false);
                            }
                        }
                    } else if (released == mLeftSpaceKey || released == mRightSpaceKey) {
                        if (mSpaceSwipeActive) {
                            mSpaceSwipeActive = false;
                        } else if (hasAnyActiveModifier()) {
                            released.comboFired = true;
                            executeCombination(released);
                        } else {
                            sendKey(KeyEvent.KEYCODE_SPACE, true);
                            postDelayed(() -> sendKey(KeyEvent.KEYCODE_SPACE, false), 30);
                            if (mAutoReturnToAbcOnSpace && mSymbolLayer > 0) {
                                mAutoReturnToAbcOnSpace = false;
                                mSymbolLayer = 0;
                                initArcKeys();
                                layoutSplitArc(getWidth(), getHeight());
                            }
                        }
                    } else {
                        int relCode = released.currentKeyCode;
                        if (isDirectionKey(relCode) || relCode == KeyEvent.KEYCODE_MOVE_HOME || relCode == KeyEvent.KEYCODE_MOVE_END) {
                            sendKey(relCode, false);
                        } else if (!isLeftModifier(released) && !"Caps".equals(released.internalLabel)) {
                            dispatchNormalOrSymbolKey(released, false);
                        }
                    }
                    released.pressed = false;
                    activePointers.remove(pointerId);
                    invalidate();
                    break;
                }

                case MotionEvent.ACTION_MOVE: {
                    if (!mIsTouchScrolling && activePointers.size() == 0) {
                        return false;
                    }
                    if (mIsTouchScrolling && mTouchScrollPointerId >= 0) {
                        int idx = event.findPointerIndex(mTouchScrollPointerId);
                        if (idx >= 0) {
                            float py = event.getY(idx);
                            float dy = py - mTouchScrollLastY;
                            mTouchScrollLastY = py;
                            if (Math.abs(dy) > 0.5f && listener != null) {
                                listener.onScroll(dy);
                            }
                        }
                    }

                    for (int i = 0; i < activePointers.size(); i++) {
                        int pid = activePointers.keyAt(i);
                        KeyData key = activePointers.valueAt(i);
                        if (key == mTouchScrollPadKey) continue;
                        int idx = event.findPointerIndex(pid);
                        if (idx < 0) continue;
                        float px = event.getX(idx);
                        float py = event.getY(idx);

                        if (key == mLeftSpaceKey || key == mRightSpaceKey) {
                            float totalDx = px - mSpaceDragStartX;
                            float density = getResources().getDisplayMetrics().density;
                            if (!mSpaceSwipeActive && Math.abs(totalDx) > 10f * density) {
                                mSpaceSwipeActive = true;
                                mSpaceLastMoveX = px;
                            }
                            if (mSpaceSwipeActive) {
                                float delta = px - mSpaceLastMoveX;
                                float stepPx = 9f * density;
                                while (delta >= stepPx) {
                                    sendKey(KeyEvent.KEYCODE_DPAD_RIGHT, true);
                                    sendKey(KeyEvent.KEYCODE_DPAD_RIGHT, false);
                                    if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                                    delta -= stepPx;
                                    mSpaceLastMoveX += stepPx;
                                }
                                while (delta <= -stepPx) {
                                    sendKey(KeyEvent.KEYCODE_DPAD_LEFT, true);
                                    sendKey(KeyEvent.KEYCODE_DPAD_LEFT, false);
                                    if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                                    delta += stepPx;
                                    mSpaceLastMoveX -= stepPx;
                                }
                            }
                            continue;
                        }

                        boolean stillHit = false;
                        if (key.isWing) {
                            stillHit = key.wingBounds.contains(px, py);
                        } else {
                            stillHit = key.rect.contains((int) px, (int) py);
                        }

                        if (!stillHit) {
                            mPendingLongPressKey = null;
                            mLongPressHandler.removeCallbacksAndMessages(null);
                            key.pressed = false;
                            if (key.comboFired) {
                                key.comboFired = false;
                            } else if (key.isWing && key.customAction != null) {
                                if (!(isLeftModifier(key)
                                        && HudAction.TYPE_MODIFIER.equals(key.customAction.type))) {
                                    dispatchWingAction(key.customAction, false);
                                }
                            } else if (!key.longPressFired && key != mLeftSpaceKey && key != mRightSpaceKey) {
                                int kc = key.currentKeyCode;
                                if (isDirectionKey(kc) || kc == KeyEvent.KEYCODE_MOVE_HOME || kc == KeyEvent.KEYCODE_MOVE_END) {
                                    sendKey(kc, false);
                                } else if (!isLeftModifier(key) && !"Caps".equals(key.internalLabel)) {
                                    dispatchNormalOrSymbolKey(key, false);
                                }
                            }
                            key.longPressFired = false;
                            activePointers.remove(pid);
                            invalidate();
                        }
                    }
                    break;
                }

                case MotionEvent.ACTION_CANCEL: {
                    stopWingRepeat();
                    mPendingLongPressKey = null;
                    mLongPressHandler.removeCallbacksAndMessages(null);
                    mIsTouchScrolling = false;
                    mTouchScrollPointerId = -1;
                    if (mTouchScrollPadKey != null) mTouchScrollPadKey.pressed = false;
                    for (int i = 0; i < activePointers.size(); i++) {
                        KeyData key = activePointers.valueAt(i);
                        key.pressed = false;
                        key.longPressFired = false;
                        if (key == mTouchScrollPadKey) continue;
                        if (key.comboFired) {
                            key.comboFired = false;
                        } else if (key.isWing && key.customAction != null) {
                            if (!(isLeftModifier(key)
                                    && HudAction.TYPE_MODIFIER.equals(key.customAction.type))) {
                                dispatchWingAction(key.customAction, false);
                            }
                        } else if (key != mLeftSpaceKey && key != mRightSpaceKey) {
                            int kc = key.currentKeyCode;
                            if (isDirectionKey(kc) || kc == KeyEvent.KEYCODE_MOVE_HOME || kc == KeyEvent.KEYCODE_MOVE_END) {
                                sendKey(kc, false);
                            } else if (!isLeftModifier(key) && !"Caps".equals(key.internalLabel)) {
                                dispatchNormalOrSymbolKey(key, false);
                            }
                        }
                    }
                    activePointers.clear();
                    leftShiftOn = false;
                    leftCtrlOn = false;
                    leftAltOn = false;
                    leftMetaOn = false;
                    updateSymbolLayer();
                    invalidate();
                    break;
                }
            }
            return true;
        } catch (Exception e) {
            Log.e(TAG, "onTouchEvent error", e);
            return false;
        }
    }

    private void dispatchWingAction(HudAction action, boolean isDown) {
        if (action == null || listener == null) return;
        if (HudAction.TYPE_KEY.equals(action.type)) {
            if (isDown) listener.onKeyDown(action.code);
            else listener.onKeyUp(action.code);
        } else if (HudAction.TYPE_MODIFIER.equals(action.type)) {
            if (isDown) listener.onKeyDown(action.code);
            else listener.onKeyUp(action.code);
        } else if (HudAction.TYPE_COMBO.equals(action.type)) {
            if (isDown && action.comboKeys != null) {
                for (int k : action.comboKeys) {
                    listener.onKeyDown(k);
                }
                postDelayed(() -> {
                    for (int i = action.comboKeys.size() - 1; i >= 0; i--) {
                        listener.onKeyUp(action.comboKeys.get(i));
                    }
                }, 50);
            }
        } else if (HudAction.TYPE_TEXT.equals(action.type) && isDown
                && action.text != null && !action.text.isEmpty()) {
            listener.onTextInput(action.text);
        }
    }

    private void sendKeyData(KeyData key, boolean isDown) {
        if (key != null && key.isWing && key.customAction != null
                && (HudAction.TYPE_KEY.equals(key.customAction.type)
                || HudAction.TYPE_MODIFIER.equals(key.customAction.type))) {
            dispatchWingAction(key.customAction, isDown);
        } else if (key != null) {
            sendKey(key.currentKeyCode, isDown);
        }
    }

    private boolean isDirectionKey(int keyCode) {
        return keyCode == KeyEvent.KEYCODE_DPAD_UP ||
                keyCode == KeyEvent.KEYCODE_DPAD_DOWN ||
                keyCode == KeyEvent.KEYCODE_DPAD_LEFT ||
                keyCode == KeyEvent.KEYCODE_DPAD_RIGHT;
    }

    private Runnable mWingRepeatRunnable;
    private static final long WING_REPEAT_INITIAL_DELAY_MS = 320L;
    private static final long WING_REPEAT_INTERVAL_MS = 60L;

    private void startWingRepeat(final KeyData key) {
        stopWingRepeat();
        if (key == null) return;
        int code = key.currentKeyCode;
        boolean repeatable = isDirectionKey(code)
                || code == KeyEvent.KEYCODE_FORWARD_DEL
                || code == KeyEvent.KEYCODE_DEL
                || code == KeyEvent.KEYCODE_TAB
                || code == KeyEvent.KEYCODE_PAGE_UP
                || code == KeyEvent.KEYCODE_PAGE_DOWN;
        if (!repeatable && key.customAction != null && key.customAction.repeat) {
            repeatable = true;
        }
        if (!repeatable) return;

        mWingRepeatRunnable = new Runnable() {
            @Override
            public void run() {
                if (key.pressed) {
                    if (isHapticEnabled()) {
                        performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    }
                    if (key.customAction != null) {
                        dispatchWingAction(key.customAction, true);
                        postDelayed(() -> dispatchWingAction(key.customAction, false), 20);
                    } else {
                        sendKey(key.currentKeyCode, true);
                        postDelayed(() -> sendKey(key.currentKeyCode, false), 20);
                    }
                    mLongPressHandler.postDelayed(this, WING_REPEAT_INTERVAL_MS);
                }
            }
        };
        mLongPressHandler.postDelayed(mWingRepeatRunnable, WING_REPEAT_INITIAL_DELAY_MS);
    }

    private void stopWingRepeat() {
        if (mWingRepeatRunnable != null) {
            mLongPressHandler.removeCallbacks(mWingRepeatRunnable);
            mWingRepeatRunnable = null;
        }
    }

    private KeyData findKeyAt(float x, float y) {
        if (mIsSplitArcMode) {
            float density = getResources().getDisplayMetrics().density;
            // Check Touch Scroll Pad
            if (isTouchScrollEnabled() && mTouchScrollPadKey != null && mTouchScrollPadKey.rect.contains((int) x, (int) y)) {
                return mTouchScrollPadKey;
            }
            // Check bottom keys
            if (mCapsKey != null && mCapsKey.rect.contains((int) x, (int) y)) {
                return mCapsKey;
            }
            if (mBkspKey != null && mBkspKey.rect.contains((int) x, (int) y)) {
                return mBkspKey;
            }
            if (mEnterKey != null && mEnterKey.rect.contains((int) x, (int) y)) {
                return mEnterKey;
            }
            if (mLeftSpaceKey != null && mLeftSpaceKey.rect.contains((int) x, (int) y)) {
                return mLeftSpaceKey;
            }
            if (mRightSpaceKey != null && mRightSpaceKey.rect.contains((int) x, (int) y)) {
                return mRightSpaceKey;
            }
            if (mCommaKey != null && mCommaKey.rect.contains((int) x, (int) y)) {
                return mCommaKey;
            }
            if (mMinusKey != null && mMinusKey.rect.contains((int) x, (int) y)) {
                return mMinusKey;
            }
            if (mPeriodKey != null && mPeriodKey.rect.contains((int) x, (int) y)) {
                return mPeriodKey;
            }
            if (mSymbolToggleKey != null && mSymbolToggleKey.rect.contains((int) x, (int) y)) {
                return mSymbolToggleKey;
            }
            if (mCornerDismissKey != null && mCornerDismissKey.rect.contains((int) x, (int) y)) {
                return mCornerDismissKey;
            }
            // Check top wings
            for (KeyData k : mWingKeys) {
                if (k.wingBounds.contains(x, y)) {
                    return k;
                }
            }
            // Check cluster letter keys (direct rectangle hit)
            int ix = (int) x;
            int iy = (int) y;
            for (KeyData k : mArcKeys) {
                if (k.rect.contains(ix, iy)) {
                    return k;
                }
            }
            return null;
        }

        // Portrait floating mode
        for (KeyData k : keys) {
            if (k.rect.contains((int) x, (int) y)) return k;
        }
        return null;
    }

    /**
     * Drags one half of the split keyboard. Returns true when the gesture was
     * taken, false when it should keep going to the keys (a press on a key must
     * always type it, never move the block).
     */
    private boolean handleSplitHalfDrag(int action, float x, float y) {
        if (!mSplitDragEnabled) {
            mSplitDragActive = false;
            return false;
        }
        if (action == MotionEvent.ACTION_DOWN) {
            if (activePointers.size() > 0) return false;
            if (findKeyAt(x, y) != null) return false;
            // Only the two blocks grab the gesture. Everything else -- the
            // editor toolbar that sits above this full-screen view, for
            // instance -- has to keep falling through untouched.
            boolean onLeft = mLeftClusterRect.contains(x, y);
            boolean onRight = mRightClusterRect.contains(x, y);
            if (!onLeft && !onRight) return false;
            boolean right = onLeft && onRight ? isRightClusterNearest(x, y) : onRight;
            mSplitDragActive = true;
            mSplitDragRight = right;
            mSplitDragStartX = x;
            mSplitDragStartY = y;
            mSplitDragStartPctX = right ? mSplitRightPctX : mSplitLeftPctX;
            mSplitDragStartPctY = right ? mSplitRightPctY : mSplitLeftPctY;
            bringToFront();
            return true;
        }
        if (!mSplitDragActive) return false;

        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) return true;

        if (action == MotionEvent.ACTION_MOVE) {
            float pctX = mSplitDragStartPctX + (x - mSplitDragStartX) / w;
            float pctY = mSplitDragStartPctY + (y - mSplitDragStartY) / h;
            float[] clamped = clampHalfOffset(mSplitDragRight, pctX, pctY, w, h);
            if (mSplitDragRight) {
                mSplitRightPctX = clamped[0];
                mSplitRightPctY = clamped[1];
            } else {
                mSplitLeftPctX = clamped[0];
                mSplitLeftPctY = clamped[1];
            }
            layoutSplitArc(w, h);
            invalidate();
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            mSplitDragActive = false;
            saveSplitOffsets();
            return true;
        }
        return true;
    }

    /** True when the right half is the closer block to this point. */
    private boolean isRightClusterNearest(float x, float y) {
        return distanceToRect(mRightClusterRect, x, y) < distanceToRect(mLeftClusterRect, x, y);
    }

    private static float distanceToRect(RectF r, float x, float y) {
        if (r == null || r.isEmpty()) return Float.MAX_VALUE;
        float dx = Math.max(Math.max(r.left - x, 0f), x - r.right);
        float dy = Math.max(Math.max(r.top - y, 0f), y - r.bottom);
        return (float) Math.hypot(dx, dy);
    }

    /**
     * Keeps at least a sliver of the block on screen. Returns the clamped
     * {xPercent, yPercent} pair for the half being dragged.
     */
    private float[] clampHalfOffset(boolean right, float pctX, float pctY, int w, int h) {
        RectF current = right ? mRightClusterRect : mLeftClusterRect;
        if (current.isEmpty()) return new float[]{pctX, pctY};
        float oldPctX = right ? mSplitRightPctX : mSplitLeftPctX;
        float oldPctY = right ? mSplitRightPctY : mSplitLeftPctY;
        mClusterTmp.set(current);
        mClusterTmp.offset((pctX - oldPctX) * w, (pctY - oldPctY) * h);

        float keep = dpToPx(56);
        float shiftX = 0f;
        float shiftY = 0f;
        if (mClusterTmp.right < keep) shiftX = keep - mClusterTmp.right;
        else if (mClusterTmp.left > w - keep) shiftX = (w - keep) - mClusterTmp.left;
        if (mClusterTmp.bottom < keep) shiftY = keep - mClusterTmp.bottom;
        else if (mClusterTmp.top > h - keep) shiftY = (h - keep) - mClusterTmp.top;

        return new float[]{pctX + shiftX / w, pctY + shiftY / h};
    }

    @Override
    protected void onDraw(Canvas canvas) {
        try {
            if (mIsSplitArcMode) {
                drawSplitArcLayout(canvas);
                return;
            }

            // Standard Portrait Draw
            if (keys.isEmpty()) return;

            int theme = getKeyboardTheme();
            if (theme != THEME_TRANSPARENT_FULL) {
                bgPaint.setColor(MD_SURFACE);
                canvas.drawRoundRect(0, 0, getWidth(), getHeight(),
                        dpToPx(BACKDROP_CORNER_DP), dpToPx(BACKDROP_CORNER_DP), bgPaint);
            }

            // MD3 drag handle: a small centred pill, not a full-width bar.
            handlePaint.setColor(MD_ON_SURFACE_VARIANT);
            handlePaint.setAlpha(0x66);
            float hw = dpToPx(HANDLE_WIDTH_DP) / 2f;
            float hh = dpToPx(HANDLE_HEIGHT_DP) / 2f;
            float hcx = getWidth() / 2f;
            float hcy = dragHandleHeight / 2f;
            canvas.drawRoundRect(hcx - hw, hcy - hh, hcx + hw, hcy + hh,
                    hh, hh, handlePaint);

            float keyRadius = dpToPx(KEY_CORNER_DP);
            for (KeyData k : keys) {
                Rect r = k.rect;
                boolean wide = r.width() > r.height() * 1.8f;
                drawKeySurface(canvas, r.left, r.top, r.right, r.bottom,
                        wide ? r.height() * 0.5f : keyRadius, k, 255);

                float density = getResources().getDisplayMetrics().density;
                if (!drawKeyIcon(canvas, k, r.centerX(), r.centerY(),
                        Math.min(r.width(), keyHeight) * 0.32f, 255, density)) {
                    textPaint.setColor(resolveKeyTextColor(k));
                    float textSize = keyHeight * 0.4f;
                    if (textSize <= 0) textSize = 20;
                    textPaint.setTextSize(textSize);
                    textPaint.setFakeBoldText(false);
                    float cx = r.centerX();
                    float cy = r.centerY() - ((textPaint.descent() + textPaint.ascent()) / 2);
                    String display = k.currentLabel != null ? k.currentLabel : k.defaultLabel;
                    canvas.drawText(display, cx, cy, textPaint);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "onDraw error", e);
        }
    }

    /** Resting vs. active (pressed / latched) tonal fill, per MD3. */
    private int resolveKeyBg(KeyData k) {
        return resolveKeyBg(k, false);
    }

    private int resolveKeyBg(KeyData k, boolean actionKey) {
        if (k.pressed || k.modActive) return MD_PRIMARY_CONTAINER;
        return actionKey ? MD_SURFACE_CONTAINER : MD_SURFACE_CONTAINER_HIGH;
    }

    private int resolveKeyTextColor(KeyData k) {
        return (k.pressed || k.modActive) ? MD_ON_PRIMARY_CONTAINER : MD_ON_SURFACE;
    }

    private int resolveKeyMutedTextColor(KeyData k) {
        return (k.pressed || k.modActive) ? MD_ON_PRIMARY_CONTAINER : MD_ON_SURFACE_VARIANT;
    }

    /**
     * Draw one key surface. Filled theme = tonal fill, outlined theme = hairline
     * outline only, transparent = nothing until the key is active. Radii follow
     * the MD3 shape scale; wide keys (space) pass radius = height/2 for a pill.
     */
    private void drawKeySurface(Canvas canvas, float l, float t, float r, float b,
                                float radiusPx, KeyData k, int alpha) {
        drawKeySurface(canvas, l, t, r, b, radiusPx, k, alpha, false);
    }

    private void drawKeySurface(Canvas canvas, float l, float t, float r, float b,
                                float radiusPx, KeyData k, int alpha, boolean actionKey) {
        int theme = getKeyboardTheme();
        boolean highlighted = k.pressed || k.modActive;
        keyBgPaint.setAlpha(alpha);
        if (theme == THEME_FILLED || highlighted) {
            keyBgPaint.setStyle(Paint.Style.FILL);
            keyBgPaint.setColor(resolveKeyBg(k, actionKey));
            canvas.drawRoundRect(l, t, r, b, radiusPx, radiusPx, keyBgPaint);
        } else if (theme == THEME_BORDER_ONLY) {
            keyStrokePaint.setStyle(Paint.Style.STROKE);
            keyStrokePaint.setStrokeWidth(Math.max(1f, dpToPx(1)));
            keyStrokePaint.setColor(MD_OUTLINE_VARIANT);
            keyStrokePaint.setAlpha(alpha);
            canvas.drawRoundRect(l, t, r, b, radiusPx, radiusPx, keyStrokePaint);
        }
    }

    private void drawSpecialButton(Canvas canvas, KeyData k, int alpha, float slideOffset, float cr) {
        if (k == null) return;
        Rect r = k.rect;
        float top = r.top + slideOffset;
        float bottom = r.bottom + slideOffset;
        // Space and other wide keys become MD3 pills.
        boolean wide = k.isSpace || (r.width() > r.height() * 1.8f);
        float radius = wide ? (bottom - top) * 0.5f : cr;
        drawKeySurface(canvas, r.left, top, r.right, bottom, radius, k, alpha, true);
    }

    private void drawDismissIcon(Canvas canvas, float cx, float cy, int alpha, float density) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(COLOR_PRIMARY);
        p.setStyle(Paint.Style.STROKE);
        p.setStrokeWidth(2f * density);
        p.setStrokeCap(Paint.Cap.ROUND);
        p.setStrokeJoin(Paint.Join.ROUND);
        p.setAlpha(alpha);

        // A keyboard outline with a chevron under it: the platform glyph for
        // "hide the keyboard", drawn large enough to read at a glance, plus a
        // label so the only way back to the desktop is never a guessing game.
        float w = 12f * density;
        float h = 4.5f * density;
        float top = cy - h - 8f * density;
        float bottom = cy - 2f * density;

        canvas.drawRoundRect(cx - w, top, cx + w, bottom, 3f * density, 3f * density, p);
        // One row of "keys" inside the outline hints at a keyboard.
        canvas.drawLine(cx - w * 0.55f, top + 3f * density, cx + w * 0.55f, top + 3f * density, p);
        // Chevron pointing down: "put it away".
        canvas.drawLine(cx - 5f * density, cy + 1f * density, cx, cy + 5f * density, p);
        canvas.drawLine(cx, cy + 5f * density, cx + 5f * density, cy + 1f * density, p);

        Paint t = new Paint(Paint.ANTI_ALIAS_FLAG);
        t.setColor(COLOR_PRIMARY);
        t.setAlpha(alpha);
        t.setTextAlign(Paint.Align.CENTER);
        t.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        t.setTextSize(8.5f * density);
        canvas.drawText("HIDE", cx, cy + 15f * density, t);
    }

    private boolean isCapsOrShiftActive() {
        return mCapsState != CAPS_OFF || leftShiftOn;
    }

    private String getLetterDisplay(KeyData k) {
        if (k == null) return "";
        String label = k.currentLabel != null ? k.currentLabel : k.defaultLabel;
        if (mSymbolLayer > 0) return label;
        if (isSingleLetter(label)) {
            return isCapsOrShiftActive() ? label.toUpperCase() : label.toLowerCase();
        }
        return label;
    }

    private boolean isSingleLetter(String s) {
        return s != null && s.length() == 1 && Character.isLetter(s.charAt(0));
    }

    // Icon vector thay text cho Caps / BKSP / Enter. Tra ve true neu da ve.
    private boolean drawKeyIcon(Canvas canvas, KeyData k, float cx, float cy,
                                float size, int alpha, float density) {
        String in = k.internalLabel;
        if ("Caps".equals(in)) {
            drawShiftIcon(canvas, cx, cy, size, alpha, density, mCapsState);
            return true;
        } else if ("BKSP".equals(in)) {
            drawBackspaceIcon(canvas, cx, cy, size, alpha, density);
            return true;
        } else if ("Enter".equals(in)) {
            drawEnterIcon(canvas, cx, cy, size, alpha, density);
            return true;
        } else if ("HIDE".equals(in)) {
            drawDismissIcon(canvas, cx, cy, alpha, density);
            return true;
        }
        return false;
    }

    private void setupIconPaint(float density, int alpha) {
        iconPaint.setStyle(Paint.Style.STROKE);
        iconPaint.setStrokeWidth(Math.max(1.5f, 1.6f * density));
        iconPaint.setStrokeCap(Paint.Cap.ROUND);
        iconPaint.setStrokeJoin(Paint.Join.ROUND);
        iconPaint.setColor(MD_ON_SURFACE_VARIANT);
        iconPaint.setAlpha(alpha);
    }

    // Mui ten shift: net manh = OFF, to day = ONCE, to day + gach chan = LOCK.
    private void drawShiftIcon(Canvas canvas, float cx, float cy, float s,
                               int alpha, float density, int capsState) {
        setupIconPaint(density, alpha);
        Path p = new Path();
        p.moveTo(cx - 0.55f * s, cy + 0.35f * s);
        p.lineTo(cx, cy - 0.45f * s);
        p.lineTo(cx + 0.55f * s, cy + 0.35f * s);
        if (capsState == CAPS_OFF) {
            canvas.drawPath(p, iconPaint);
            canvas.drawLine(cx, cy - 0.45f * s, cx, cy + 0.45f * s, iconPaint);
        } else {
            Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
            fill.setStyle(Paint.Style.FILL);
            fill.setColor(iconPaint.getColor());
            fill.setAlpha(alpha);
            p.lineTo(cx + 0.28f * s, cy + 0.35f * s);
            p.lineTo(cx + 0.28f * s, cy + 0.55f * s);
            p.lineTo(cx - 0.28f * s, cy + 0.55f * s);
            p.lineTo(cx - 0.28f * s, cy + 0.35f * s);
            p.close();
            canvas.drawPath(p, fill);
            if (capsState == CAPS_LOCK) {
                canvas.drawLine(cx - 0.55f * s, cy + 0.75f * s,
                        cx + 0.55f * s, cy + 0.75f * s, iconPaint);
            }
        }
    }

    // Nut xoa: khung chu nhat + X ben trong.
    private void drawBackspaceIcon(Canvas canvas, float cx, float cy, float s,
                                   int alpha, float density) {
        setupIconPaint(density, alpha);
        float hw = 0.62f * s, hh = 0.42f * s, nose = 0.22f * s;
        Path p = new Path();
        p.moveTo(cx - hw + nose, cy - hh);
        p.lineTo(cx + hw - nose, cy - hh);
        p.lineTo(cx + hw, cy);
        p.lineTo(cx + hw - nose, cy + hh);
        p.lineTo(cx - hw + nose, cy + hh);
        p.lineTo(cx - hw, cy);
        p.close();
        canvas.drawPath(p, iconPaint);
        float ix = cx + 0.02f * s, ih = 0.16f * s;
        canvas.drawLine(ix - ih, cy - ih, ix + ih, cy + ih, iconPaint);
        canvas.drawLine(ix - ih, cy + ih, ix + ih, cy - ih, iconPaint);
    }

    // Nut Enter: net xuong roi re trai + mui ten.
    private void drawEnterIcon(Canvas canvas, float cx, float cy, float s,
                               int alpha, float density) {
        setupIconPaint(density, alpha);
        Path p = new Path();
        p.moveTo(cx + 0.45f * s, cy - 0.5f * s);
        p.lineTo(cx + 0.45f * s, cy + 0.25f * s);
        p.quadTo(cx + 0.45f * s, cy + 0.45f * s, cx + 0.2f * s, cy + 0.45f * s);
        p.lineTo(cx - 0.35f * s, cy + 0.45f * s);
        canvas.drawPath(p, iconPaint);
        canvas.drawLine(cx - 0.35f * s, cy + 0.45f * s, cx - 0.05f * s, cy + 0.15f * s, iconPaint);
        canvas.drawLine(cx - 0.35f * s, cy + 0.45f * s, cx - 0.05f * s, cy + 0.75f * s, iconPaint);
    }

    private void drawSplitArcLayout(Canvas canvas) {
        int alpha = Math.max(0, Math.min(255, Math.round(255 * mAnimProgress)));

        float density = getResources().getDisplayMetrics().density;
        float slideOffset = (1f - mAnimProgress) * 24f * density;
        float cr = KEY_CORNER_DP * density;

        // Each half is one block the user can park anywhere: a soft container
        // behind it makes that obvious and gives the drag a visible target.
        if (getKeyboardTheme() != THEME_TRANSPARENT_FULL) {
            drawHalfBackdrop(canvas, mLeftClusterRect, slideOffset, alpha);
            drawHalfBackdrop(canvas, mRightClusterRect, slideOffset, alpha);
        }

        // 1. Draw Left & Right Cluster Letters (Rounded rectangles matching Gboard)
        int theme = getKeyboardTheme();
        for (KeyData k : mArcKeys) {
            Rect r = k.rect;
            float top = r.top + slideOffset;
            float bottom = r.bottom + slideOffset;

            drawKeySurface(canvas, r.left, top, r.right, bottom, cr, k, alpha);

            // Text (displays lowercase when caps is OFF, uppercase when caps is active)
            textPaint.setColor(resolveKeyTextColor(k));
            textPaint.setAlpha(alpha);
            textPaint.setTextSize(16f * density);
            float cx = r.centerX();
            float cy = ((top + bottom) / 2f) - ((textPaint.descent() + textPaint.ascent()) / 2f);
            String display = getLetterDisplay(k);
            canvas.drawText(display, cx, cy, textPaint);

            // Subscript Number in top-right corner
            if (k.longPressNumber != null) {
                subTextPaint.setAlpha(Math.round(0xCC * (alpha / 255f)));
                subTextPaint.setTextSize(9f * density);
                canvas.drawText(k.longPressNumber, r.right - 6f * density, top + 11f * density, subTextPaint);
            }
        }

        // 2. Special Keys
        // Caps (Row 2, Left, 1.5u)
        if (mCapsKey != null) {
            drawSpecialButton(canvas, mCapsKey, alpha, slideOffset, cr);
            drawShiftIcon(canvas, mCapsKey.rect.centerX(), mCapsKey.rect.centerY() + slideOffset,
                    mCapsKey.rect.height() * 0.42f, alpha, density, mCapsState);
        }

        // Backspace (Row 2, Right, 1.5u)
        if (mBkspKey != null) {
            drawSpecialButton(canvas, mBkspKey, alpha, slideOffset, cr);
            drawBackspaceIcon(canvas, mBkspKey.rect.centerX(), mBkspKey.rect.centerY() + slideOffset,
                    mBkspKey.rect.height() * 0.40f, alpha, density);
        }

        // Enter (Row 3, Right, 1.5u)
        if (mEnterKey != null) {
            drawSpecialButton(canvas, mEnterKey, alpha, slideOffset, cr);
            drawEnterIcon(canvas, mEnterKey.rect.centerX(), mEnterKey.rect.centerY() + slideOffset,
                    mEnterKey.rect.height() * 0.40f, alpha, density);
        }

        // ?123 (Row 3, Left, 1.5u)
        if (mSymbolToggleKey != null) {
            drawSpecialButton(canvas, mSymbolToggleKey, alpha, slideOffset, cr);
            textPaint.setColor(COLOR_SECONDARY);
            textPaint.setAlpha(alpha);
            textPaint.setTextSize(12.5f * density);
            float cy = (mSymbolToggleKey.rect.centerY() + slideOffset) - ((textPaint.descent() + textPaint.ascent()) / 2f);
            canvas.drawText(mSymbolToggleKey.currentLabel, mSymbolToggleKey.rect.centerX(), cy, textPaint);
        }

        // Comma (Row 3, Left)
        if (mCommaKey != null) {
            drawSpecialButton(canvas, mCommaKey, alpha, slideOffset, cr);
            textPaint.setColor(COLOR_TEXT_MUTED);
            textPaint.setAlpha(alpha);
            textPaint.setTextSize(14f * density);
            float cy = (mCommaKey.rect.centerY() + slideOffset) - ((textPaint.descent() + textPaint.ascent()) / 2f);
            canvas.drawText(",", mCommaKey.rect.centerX(), cy, textPaint);
        }

        // Period (Row 3, Right)
        if (mPeriodKey != null) {
            drawSpecialButton(canvas, mPeriodKey, alpha, slideOffset, cr);
            textPaint.setColor(COLOR_TEXT_MUTED);
            textPaint.setAlpha(alpha);
            textPaint.setTextSize(14f * density);
            float cy = (mPeriodKey.rect.centerY() + slideOffset) - ((textPaint.descent() + textPaint.ascent()) / 2f);
            canvas.drawText(".", mPeriodKey.rect.centerX(), cy, textPaint);
        }

        // SpaceL (Row 3, Left)
        if (mLeftSpaceKey != null) {
            drawSpecialButton(canvas, mLeftSpaceKey, alpha, slideOffset, cr);
        }

        // Minus key (Row 3, Right of SpaceL)
        if (mMinusKey != null) {
            drawSpecialButton(canvas, mMinusKey, alpha, slideOffset, cr);
            textPaint.setColor(COLOR_TEXT_MUTED);
            textPaint.setAlpha(alpha);
            textPaint.setTextSize(14f * density);
            float cy = (mMinusKey.rect.centerY() + slideOffset) - ((textPaint.descent() + textPaint.ascent()) / 2f);
            canvas.drawText("-", mMinusKey.rect.centerX(), cy, textPaint);
        }

        // SpaceR (Row 3, Right)
        if (mRightSpaceKey != null) {
            drawSpecialButton(canvas, mRightSpaceKey, alpha, slideOffset, cr);
        }

        // Dismiss (Row 3, Right)
        if (mCornerDismissKey != null) {
            drawSpecialButton(canvas, mCornerDismissKey, alpha, slideOffset, cr);
            drawDismissIcon(canvas, mCornerDismissKey.rect.centerX(), mCornerDismissKey.rect.centerY() + slideOffset, alpha, density);
        }

        // Touch Scroll Pad in the cluster gap
        if (isTouchScrollEnabled() && mTouchScrollPadKey != null && mTouchScrollPadKey.rect.width() > 0) {
            drawSpecialButton(canvas, mTouchScrollPadKey, alpha, slideOffset, cr);
            float padCx = mTouchScrollPadKey.rect.centerX();
            float padCy = mTouchScrollPadKey.rect.centerY() + slideOffset;
            float padH = mTouchScrollPadKey.rect.height();

            Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
            p.setColor(COLOR_PRIMARY);
            p.setAlpha(Math.round(0xCC * (alpha / 255f)));
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(2f * density);
            p.setStrokeCap(Paint.Cap.ROUND);

            float trackH = padH * 0.30f;
            canvas.drawLine(padCx, padCy - trackH, padCx, padCy + trackH, p);

            p.setStrokeWidth(1.6f * density);
            canvas.drawLine(padCx - 4f * density, padCy - trackH + 5f * density, padCx, padCy - trackH, p);
            canvas.drawLine(padCx + 4f * density, padCy - trackH + 5f * density, padCx, padCy - trackH, p);

            canvas.drawLine(padCx - 4f * density, padCy + trackH - 5f * density, padCx, padCy + trackH, p);
            canvas.drawLine(padCx + 4f * density, padCy + trackH - 5f * density, padCx, padCy + trackH, p);
        }

        // 3. Wings (2 rows x 3 cols, enlarged: 44dp x 32dp each)
        float wingSlideY = (1f - mAnimProgress) * -16f * density;
        float wingCr = WING_CORNER_DP * density;
        for (KeyData k : mWingKeys) {
            canvas.save();
            canvas.translate(0, wingSlideY);

            RectF r = k.wingBounds;
            boolean highlighted = k.pressed || k.modActive;
            drawKeySurface(canvas, r.left, r.top, r.right, r.bottom, wingCr, k, alpha);

            textPaint.setColor(resolveKeyMutedTextColor(k));
            textPaint.setAlpha(alpha);
            textPaint.setTextSize(12f * density);
            textPaint.setFakeBoldText(true);
            float cx = k.wingBounds.centerX();
            float cy = k.wingBounds.centerY() - ((textPaint.descent() + textPaint.ascent()) / 2f);
            String display = k.currentLabel != null ? k.currentLabel : k.defaultLabel;
            canvas.drawText(display, cx, cy, textPaint);

            canvas.restore();
        }
    }

    private int dpToPx(int dp) {
        return dpToPx((float) dp);
    }

    private int dpToPx(float dp) {
        try {
            return Math.round(dp * getResources().getDisplayMetrics().density);
        } catch (Exception e) {
            return Math.round(dp);
        }
    }

    // ========== Internal Key Data Class ==========
    private static class KeyData {
        Rect rect = new Rect();
        String defaultLabel;
        int defaultKeyCode;
        String internalLabel;
        boolean hasSymbol;
        String symbolChar;
        String currentLabel;
        int currentKeyCode;
        float weight;
        boolean pressed = false;
        boolean modActive = false;
        boolean comboFired = false;

        // Gboard Long-press numbers
        String longPressNumber = null;
        boolean longPressFired = false;

        // Split Arc specific attributes
        boolean isArc = false;
        boolean isRight = false;
        int ringIndex = 0;
        int colIndex = 0;
        int ringTotalKeys = 1;
        float pivotX = 0f;
        float pivotY = 0f;
        float baseRadius = 0f;
        float baseAngleDeg = 0f;
        float keyWidth = 0f;
        float keyHeight = 0f;
        float currentCx = 0f;
        float currentCy = 0f;

        // Top Wing specific attributes
        boolean isWing = false;
        int wingIndex = -1;
        RectF wingBounds = new RectF();
        Path beveledPath = null;
        HudAction customAction = null;

        // Custom Special keys
        boolean isSpace = false;
        boolean isSymbolToggle = false;
        boolean isCornerDismiss = false;
        boolean isTouchScrollPad = false;

        KeyData(String label, int keyCode, float weight, boolean hasSymbol) {
            this.defaultLabel = label;
            this.defaultKeyCode = keyCode;
            this.currentLabel = label;
            this.currentKeyCode = keyCode;
            this.weight = weight;
            this.hasSymbol = hasSymbol;
        }
    }
}
