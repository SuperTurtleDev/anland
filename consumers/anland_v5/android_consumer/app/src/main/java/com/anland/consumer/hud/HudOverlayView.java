package com.anland.consumer.hud;

import com.anland.consumer.R;
import com.anland.consumer.theme.M3;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.DashPathEffect;
import android.graphics.Insets;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Root Overlay View managing Execution Mode and Fullscreen HUD Editor Mode (PUBG/FreeFire style).
 * Handles separate Portrait/Landscape layouts, dynamic collision snapping, guidelines, and property inspector.
 */
public final class HudOverlayView extends FrameLayout implements IModifierProvider {

    public interface HudHost {
        void sendKey(int action, int evdevCode); // 0=down, 1=up
        void sendTextInput(byte[] utf8);
        void sendMouseMotion(float dx, float dy);
        void sendMouseButton(int button, boolean pressed);
        void sendMouseScroll(int axis, float value);
        void toggleSystemKeyboard();
        void toggleVirtualKeyboard();
        void openSettings();
        void toggleSteamDeckMode();
        void exitSteamDeckMode();
        void onLayoutSaved();
        // True when the daemon socket is live and the native pipeline is
        // connected. False during editor-only mode when the daemon is offline.
        boolean isNativeReady();
        // Notified when the user finishes HUD edit mode (via [EXIT EDIT] or
        // back-press). The host can use this to clear transient flags that
        // were set when the editor was opened, so a stray daemon-down event
        // fired right after exit does not bounce the user out of the app.
        void onEditModeExited();
    }

    /* Shared with MainActivity / SettingsActivity / HudEditorActivity. Kept
     * here too so this view can read and write the profile on its own. */
    private static final String PREF_KEY_PROFILE = "hud_layout_profile_v2";
    private static final String PREF_KEY_USE_HUD = "use_hud_overlay";
    private static final String PREF_KEY_STEAM_DECK = "steam_deck_mode";
    /** Long-pressing the KB key hides every HUD control but the KB key itself. */
    private static final String PREF_KEY_HUD_HIDDEN = "hud_hidden_mode";

    private final HudHost mHost;
    private final SharedPreferences mPrefs;
    private HudLayoutProfile mProfile;

    /** The main HUD layer, shown while the keyboard is hidden. */
    public static final int LAYER_HUD = 0;
    /** Second HUD layer, shown together with the virtual keyboard. */
    public static final int LAYER_KEYBOARD = 1;

    private int mLayer = LAYER_HUD;

    private boolean mIsEditMode = false;
    private boolean mHasUnsavedChanges = false;
    // Runtime "clean screen" mode: everything is hidden except the KB key, so
    // the way back is always on screen. Persisted across restarts.
    private boolean mHudHidden = false;
    private HudButton mSelectedButton = null;
    private View mSelectedView = null;
    // Group container currently highlighted as the drop target while a key is
    // dragged over it. Null when nothing is being hovered.
    private HudGroupView mDropTargetGroup = null;

    private final SnapGeometryEngine mSnapEngine;
    private boolean mSnapEnabled = true;
    private final Paint mGuidePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    // Self-edge guides are drawn in a different color so the user can
    // distinguish "the canvas edge my button snapped to" (green, sibling)
    // from "the edge of the button I am dragging" (cyan, self).
    private final Paint mSelfGuidePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mSelectBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mGridPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mBannerFillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mBannerStrokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mBannerTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final List<Float> mActiveVerticalGuides = new ArrayList<>();
    private final List<Float> mActiveHorizontalGuides = new ArrayList<>();
    // Self-edge guides (cạnh nút đang kéo). Tách riêng để vẽ khác màu với
    // sibling guides (cạnh của nút khác trên màn hình).
    private final List<Float> mActiveSelfVerticalGuides = new ArrayList<>();
    private final List<Float> mActiveSelfHorizontalGuides = new ArrayList<>();
    private boolean mWasSnapped = false;

    // UI Sub-containers
    private FrameLayout mFloatingContainer;
    private LinearLayout mTopToolbar;
    private HudPropertyInspectorView mInspectorView;

    /* ---- Layout lifecycle bookkeeping -----------------------------------
     * A rebuild removes and re-adds every widget view. Doing that from inside
     * a layout pass is fatal: the children get added *after* the measure phase
     * that would have sized them, and their requestLayout() cannot bubble past
     * the parents that still carry PFLAG_FORCE_LAYOUT, so no further traversal
     * is scheduled and the fresh views keep their 0x0 measured size.
     * That is the "HUD disappears / lands in the wrong place after rotating"
     * bug: onSizeChanged() is called mid-layout, and both the standalone
     * HudEditorActivity and the live desktop reach it on every rotation (and
     * on IME / freeform resizes).
     *
     * The bookkeeping below lets a pure geometry change reposition the views
     * that already exist, and defers a real rebuild to the next frame.
     */
    private boolean mRebuildPosted = false;
    // The layout object the currently attached child views were built from. A
    // different object means the children belong to the other orientation (or
    // layer) and have to be recreated rather than just moved.
    private HudLayout mBuiltLayout = null;
    private final Runnable mDeferredRebuild = new Runnable() {
        @Override public void run() {
            mRebuildPosted = false;
            rebuildActiveLayout();
        }
    };

    private int mInsetTop = 0;
    private int mInsetBottom = 0;
    private int mInsetLeft = 0;
    private int mInsetRight = 0;

    private float mTouchDownX;
    private float mTouchDownY;
    private float mInitialBtnLeft;
    private float mInitialBtnTop;

    // Modifier latch: tap toggles, long press locks. SystemIME reads it through
    // IModifierProvider so soft-keyboard combos carry the same modifiers.
    private static final class ModState {
        boolean active;
        boolean locked;
        int evdev;
    }
    private final Map<Integer, ModState> mModifiers = new LinkedHashMap<>();

    /**
     * Action dispatch shared by plain key widgets and by the keys inside a
     * group container, so a child key behaves exactly like a free key.
     */
    private final HudFreeformButtonView.ButtonActionListener mKeyButtonListener =
            new HudFreeformButtonView.ButtonActionListener() {
        @Override
        public void onButtonPress(HudButton btn, boolean isDown) {
            if (btn == null || (mIsEditMode && isDown)) return;
            if (HudAction.TYPE_MODIFIER.equals(btn.action.type)) {
                if (isDown) toggleModifier(btn.action.code);
            } else if (HudAction.TYPE_KEY.equals(btn.action.type)) {
                if (mHost != null && btn.action.code > 0) mHost.sendKey(isDown ? 0 : 1, btn.action.code);
            } else if (isDown) {
                dispatchHudAction(btn.action);
            }
        }
        @Override
        public void onPopupTrigger(HudButton btn) {
            if (!mIsEditMode && btn != null && btn.popupAction != null) {
                dispatchHudAction(btn.popupAction);
            }
        }
        @Override
        public void onButtonLongPress(HudButton btn) {
            if (mIsEditMode || btn == null) return;
            if (btn.isVirtualKeyboardToggle()) toggleHudHidden();
        }
    };

    // ---- Widget callbacks -------------------------------------------------
    // None of these depends on the button being built, so they are created once
    // instead of once per layout rebuild.
    private final TrackpointNubView.MotionDispatcher mTrackpointDispatcher =
                new TrackpointNubView.MotionDispatcher() {
                    @Override
                    public void onPointerMove(float dx, float dy) {
                        if (!mIsEditMode && mHost != null) mHost.sendMouseMotion(dx, dy);
                    }
                    @Override
                    public void onPointerClick(int button) {
                        if (!mIsEditMode && mHost != null) {
                            int code = (button == 2) ? 0x111 : (button == 3 ? 0x112 : 0x110);
                            mHost.sendMouseButton(code, true);
                            postDelayed(() -> mHost.sendMouseButton(code, false), 50);
                        }
                    }
                    @Override
                    public void onPointerScroll(float dx, float dy) {
                        // The dispatchContinuousMotion() loop already multiplies
                        // the offset by trackpointSensitivity, so the deltas
                        // arrive scaled. We only need to translate them into
                        // vertical / horizontal wheel ticks. dx maps to
                        // horizontal scroll (axis 1), dy maps to vertical
                        // (axis 0). Sign is inverted so the nub feels like
                        // a real touchpad: drag up -> page scrolls up.
                        if (!mIsEditMode && mHost != null) {
                            if (Math.abs(dy) > 0.01f) mHost.sendMouseScroll(0, -dy);
                            if (Math.abs(dx) > 0.01f) mHost.sendMouseScroll(1, dx);
                        }
                    }
                };

    private final SuperGestureButtonView.GestureListener mSuperGestureListener =
                new SuperGestureButtonView.GestureListener() {
                    @Override
                    public void onSingleTap(HudButton btn) {
                        if (!mIsEditMode) dispatchHudAction(btn.action);
                    }
                    @Override
                    public void onLongPress(HudButton btn) {
                        if (!mIsEditMode) dispatchHudAction(btn.action);
                    }
                    @Override
                    public void onSwipe(HudButton btn, int direction) {
                        if (mIsEditMode) return;
                        if (direction == SuperGestureButtonView.DIR_LEFT) dispatchHudAction(btn.swipeLeftAction);
                        else if (direction == SuperGestureButtonView.DIR_RIGHT) dispatchHudAction(btn.swipeRightAction);
                        else if (direction == SuperGestureButtonView.DIR_UP) dispatchHudAction(btn.swipeUpAction);
                        else if (direction == SuperGestureButtonView.DIR_DOWN) dispatchHudAction(btn.swipeDownAction);
                    }
                };

    private final HudTouchpadView.TouchpadDispatcher mTouchpadDispatcher =
                new HudTouchpadView.TouchpadDispatcher() {
                    @Override
                    public void onPointerMove(float dx, float dy) {
                        if (!mIsEditMode && mHost != null) mHost.sendMouseMotion(dx, dy);
                    }
                    @Override
                    public void onPointerClick(int button) {
                        if (!mIsEditMode && mHost != null) {
                            int code = (button == 2) ? 0x111 : (button == 3 ? 0x112 : 0x110);
                            mHost.sendMouseButton(code, true);
                            postDelayed(() -> mHost.sendMouseButton(code, false), 50);
                        }
                    }
                    @Override
                    public void onPointerButton(int button, boolean pressed) {
                        if (!mIsEditMode && mHost != null) {
                            int code = (button == 2) ? 0x111 : (button == 3 ? 0x112 : 0x110);
                            mHost.sendMouseButton(code, pressed);
                        }
                    }
                    @Override
                    public void onPointerScroll(float dx, float dy) {
                        if (!mIsEditMode && mHost != null) {
                            if (Math.abs(dy) > 0.01f) mHost.sendMouseScroll(0, -dy);
                            if (Math.abs(dx) > 0.01f) mHost.sendMouseScroll(1, dx);
                        }
                    }
                };

    private final HudScrollBarView.ScrollDispatcher mScrollBarDispatcher =
                new HudScrollBarView.ScrollDispatcher() {
                    @Override
                    public void onPointerScroll(float dx, float dy) {
                        if (!mIsEditMode && mHost != null) {
                            if (Math.abs(dy) > 0.01f) mHost.sendMouseScroll(0, dy);
                            if (Math.abs(dx) > 0.01f) mHost.sendMouseScroll(1, dx);
                        }
                    }
                };

    public HudOverlayView(Context context, SharedPreferences prefs, HudHost host) {
        super(context);
        this.mPrefs = prefs;
        setVisibility(prefs.getBoolean(PREF_KEY_USE_HUD, true) ? VISIBLE : GONE);
        this.mHost = host;
        this.mSnapEngine = new SnapGeometryEngine(getResources().getDisplayMetrics().density);

        this.mInsetTop = getSystemStatusBarHeight(context);
        this.mInsetBottom = getSystemNavigationBarHeight(context);

        mGuidePaint.setColor(0xFF4CAF50); // Vibrant Green magnetic line
        mGuidePaint.setStrokeWidth(2f);
        mGuidePaint.setStyle(Paint.Style.STROKE);
        mGuidePaint.setPathEffect(new DashPathEffect(new float[]{8, 8}, 0));

        mSelfGuidePaint.setColor(0xFF80DEEA); // Cyan: cạnh của nút đang kéo
        mSelfGuidePaint.setStrokeWidth(2.5f);
        mSelfGuidePaint.setStyle(Paint.Style.STROKE);
        mSelfGuidePaint.setPathEffect(new DashPathEffect(new float[]{4, 4}, 0));

        mSelectBorderPaint.setColor(0xFF80DEEA);
        mSelectBorderPaint.setStrokeWidth(3f);
        mSelectBorderPaint.setStyle(Paint.Style.STROKE);

        mGridPaint.setColor(0x22FFFFFF);
        mGridPaint.setStrokeWidth(1f);
        mGridPaint.setStyle(Paint.Style.STROKE);

        mBannerTextPaint.setColor(Color.WHITE);
        mBannerTextPaint.setFakeBoldText(true);
        mBannerTextPaint.setTextAlign(Paint.Align.CENTER);

        mBannerStrokePaint.setStyle(Paint.Style.STROKE);
        mBannerStrokePaint.setStrokeWidth(1.5f);

        setWillNotDraw(false);
        loadProfile();
        initViews();
    }

    public void loadProfile() {
        String json = mPrefs.getString(PREF_KEY_PROFILE, null);
        mProfile = HudLayoutProfile.fromJSON(json);
        if (mProfile == null) {
            mProfile = new HudLayoutProfile();
        }
        mHudHidden = mPrefs.getBoolean(PREF_KEY_HUD_HIDDEN, false);
        boolean isSteamDeck = mPrefs.getBoolean(PREF_KEY_STEAM_DECK, false);
        if (isSteamDeck) {
            ensureSteamDeckLayout();
        }
        if (mFloatingContainer != null) {
            rebuildActiveLayout();
        }
    }

    public void ensureSteamDeckLayout() {
        if (mProfile != null && !isKeyboardLayer()) {
            if (mProfile.landscapeLayout == null || mProfile.landscapeLayout.floatingButtons == null || mProfile.landscapeLayout.floatingButtons.isEmpty()) {
                mProfile.landscapeLayout = HudLayoutProfile.createSteamDeckLayout();
                try {
                    mPrefs.edit().putString(PREF_KEY_PROFILE, mProfile.toJSON().toString()).apply();
                } catch (Exception ignored) {}
            }
        }
    }

    public boolean saveProfile() {
        try {
            mPrefs.edit().putString(PREF_KEY_PROFILE, mProfile.toJSON().toString()).apply();
            mHasUnsavedChanges = false;
            if (mHost != null) mHost.onLayoutSaved();
            Toast.makeText(getContext(), R.string.hud_overlay_saved, Toast.LENGTH_SHORT).show();
            return true;
        } catch (Exception e) {
            Toast.makeText(getContext(), R.string.hud_overlay_save_error, Toast.LENGTH_SHORT).show();
            return false;
        }
    }

    /**
     * Flip clean-screen mode: hide every HUD control except the KB key and
     * remember the choice, so it survives a restart until the user holds the KB
     * key again.
     */
    private void toggleHudHidden() {
        mHudHidden = !mHudHidden;
        mPrefs.edit().putBoolean(PREF_KEY_HUD_HIDDEN, mHudHidden).apply();
        selectButton(null, null);
        rebuildActiveLayout();
        Toast.makeText(getContext(),
                mHudHidden ? R.string.hud_overlay_hidden_on : R.string.hud_overlay_hidden_off,
                Toast.LENGTH_SHORT).show();
    }

    public boolean isEditMode() {
        return mIsEditMode;
    }

    public void cancelEditMode() {
        if (!mIsEditMode) return;
        mHasUnsavedChanges = false;
        // Reload saved profile from disk to discard all uncommitted in-memory edits
        loadProfile();
        selectButton(null, null);
        setEditMode(false);
        Toast.makeText(getContext(), R.string.hud_overlay_changes_discarded, Toast.LENGTH_SHORT).show();
    }

    public void requestExit() {
        if (!mIsEditMode) return;
        if (!mHasUnsavedChanges) {
            selectButton(null, null);
            setEditMode(false);
            return;
        }
        mInspectorView.setVisibility(GONE);

        AlertDialog dialog = styleDialog(darkDialog()
                .setTitle(R.string.hud_overlay_exit_title)
                .setMessage(R.string.hud_overlay_exit_message)
                .setPositiveButton(R.string.hud_overlay_save_and_exit, (d, w) -> {
                    if (!saveProfile()) return;
                    selectButton(null, null);
                    setEditMode(false);
                })
                .setNegativeButton(R.string.hud_overlay_discard, (d, w) -> {
                    cancelEditMode();
                })
                .setNeutralButton(R.string.hud_overlay_cancel, (d, w) -> {
                    d.dismiss();
                })
                .setOnDismissListener(d -> {
                    if (mIsEditMode && mSelectedButton != null) {
                        mInspectorView.setVisibility(VISIBLE);
                    }
                })
                .create());
        dialog.show();

        Button p = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
        if (p != null) p.setTextColor(M3.COLOR_PRIMARY);
        Button n = dialog.getButton(AlertDialog.BUTTON_NEGATIVE);
        if (n != null) n.setTextColor(M3.COLOR_ERROR);
        Button c = dialog.getButton(AlertDialog.BUTTON_NEUTRAL);
        if (c != null) c.setTextColor(M3.COLOR_TEXT_MUTED);
    }

    public void setEditMode(boolean editMode) {
        this.mIsEditMode = editMode;
        if (editMode) {
            mHasUnsavedChanges = false;
            setVisibility(VISIBLE);
            updateToolbarMargins();
        }
        if (editMode && !mProfile.firstTimeNoticeShown) {
            showFirstTimeNoticeDialog();
        }
        mTopToolbar.setVisibility(editMode ? VISIBLE : GONE);
        if (!editMode) {
            selectButton(null, null);
            // Tell the host we are leaving the editor so it can clear the
            // editor-only flags. If we do not, a deferred-finish timer
            // armed for the editor path can fire after the user is already
            // back in the production view, killing the activity and
            // creating the "ghost tab" the user reported.
            if (mHost != null) mHost.onEditModeExited();
        }
        rebuildActiveLayout();
        invalidate();
    }

    private void showFirstTimeNoticeDialog() {
        AlertDialog dialog = styleDialog(darkDialog()
                .setTitle(R.string.hud_overlay_orientation_title)
                .setMessage(R.string.hud_overlay_orientation_message)
                .setPositiveButton(R.string.hud_overlay_got_it, (d, w) -> {
                    mProfile.firstTimeNoticeShown = true;
                    saveProfile();
                })
                .setCancelable(false)
                .create());
        dialog.show();
    }

    /** Switches between the HUD layer and the keyboard layer. */
    public void setLayer(int layer) {
        if (mLayer == layer) return;
        mLayer = layer;
        selectButton(null, null);
        rebuildActiveLayout();
        invalidate();
    }

    public int getLayer() {
        return mLayer;
    }

    private boolean isKeyboardLayer() {
        return mLayer == LAYER_KEYBOARD;
    }

    public HudLayout getActiveLayout() {
        if (mProfile == null) {
            String json = mPrefs.getString(PREF_KEY_PROFILE, null);
            mProfile = HudLayoutProfile.fromJSON(json);
            if (mProfile == null) {
                mProfile = new HudLayoutProfile();
            }
        }
        boolean isSteamDeck = mPrefs.getBoolean(PREF_KEY_STEAM_DECK, false);
        int orientation = getResources().getConfiguration().orientation;
        // In Steam Deck Mode, ALWAYS lock to landscape layout regardless of transient system orientation.
        boolean wantLandscape = isSteamDeck || (orientation == Configuration.ORIENTATION_LANDSCAPE);
        if (isKeyboardLayer()) {
            HudLayout kb = wantLandscape
                    ? mProfile.keyboardLandscapeLayout : mProfile.keyboardPortraitLayout;
            if (kb == null) {
                // Empty by design: the keyboard layer starts with nothing on it.
                kb = new HudLayout();
                if (wantLandscape) {
                    mProfile.keyboardLandscapeLayout = kb;
                } else {
                    mProfile.keyboardPortraitLayout = kb;
                }
            }
            return kb;
        }
        HudLayout layout = wantLandscape
                ? mProfile.landscapeLayout : mProfile.portraitLayout;
        if (layout == null) {
            layout = wantLandscape
                    ? HudLayoutProfile.createSteamDeckLayout()
                    : HudLayoutProfile.createDefaultPortraitLayout();
            if (wantLandscape) {
                mProfile.landscapeLayout = layout;
            } else {
                mProfile.portraitLayout = layout;
            }
        }
        return layout;
    }

    public static int getSystemStatusBarHeight(Context ctx) {
        int resId = ctx.getResources().getIdentifier("status_bar_height", "dimen", "android");
        int res = (resId > 0) ? ctx.getResources().getDimensionPixelSize(resId) : 0;
        return Math.max(res, Math.round(34 * ctx.getResources().getDisplayMetrics().density));
    }

    public static int getSystemNavigationBarHeight(Context ctx) {
        int resId = ctx.getResources().getIdentifier("navigation_bar_height", "dimen", "android");
        int res = (resId > 0) ? ctx.getResources().getDimensionPixelSize(resId) : 0;
        return Math.max(res, Math.round(20 * ctx.getResources().getDisplayMetrics().density));
    }

    public void applySystemInsets(WindowInsets insets) {
        if (insets != null) {
            Insets statusCutout = insets.getInsetsIgnoringVisibility(
                    WindowInsets.Type.statusBars() | WindowInsets.Type.displayCutout());
            Insets nav = insets.getInsetsIgnoringVisibility(
                    WindowInsets.Type.navigationBars());
            DisplayCutout cutout = insets.getDisplayCutout();
            int cutoutTop = cutout != null ? cutout.getSafeInsetTop() : 0;

            mInsetTop = Math.max(getSystemStatusBarHeight(getContext()), Math.max(statusCutout.top, cutoutTop));
            mInsetLeft = statusCutout.left;
            mInsetRight = statusCutout.right;
            mInsetBottom = Math.max(getSystemNavigationBarHeight(getContext()), nav.bottom);
            updateToolbarMargins();
            invalidate();
        }
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        applySystemInsets(insets);
        return super.onApplyWindowInsets(insets);
    }

    private void updateToolbarMargins() {
        if (mTopToolbar != null) {
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) mTopToolbar.getLayoutParams();
            if (lp != null) {
                int orientation = getResources().getConfiguration().orientation;
                boolean isLandscape = (orientation == Configuration.ORIENTATION_LANDSCAPE);
                if (isLandscape) {
                    lp.width = ViewGroup.LayoutParams.WRAP_CONTENT;
                    lp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
                    lp.setMargins(0, Math.max(dp(8), mInsetTop), 0, 0);
                } else {
                    lp.width = ViewGroup.LayoutParams.MATCH_PARENT;
                    lp.gravity = Gravity.TOP;
                    int topMargin = Math.max(dp(36), mInsetTop + dp(6));
                    lp.setMargins(dp(10) + mInsetLeft, topMargin, dp(10) + mInsetRight, 0);
                }
                mTopToolbar.setLayoutParams(lp);
            }
            applyToolButtonLayoutParams();
        }
    }

    /**
     * Re-apply the per-button layout params of the editor toolbar.
     *
     * The cells are "weighted, equal width" in portrait and "wrap content" in
     * landscape. Those LinearLayout.LayoutParams are baked in when a button is
     * added, so after a rotation the bar kept the previous orientation's cell
     * sizing: in landscape the weighted cells collapsed to a thin sliver, and
     * in portrait the wrap-content cells overflowed the row and pushed the
     * trailing buttons (EXIT, the keyboard-layer switch) off screen.
     */
    private void applyToolButtonLayoutParams() {
        if (mTopToolbar == null) return;
        for (int i = 0; i < mTopToolbar.getChildCount(); i++) {
            View child = mTopToolbar.getChildAt(i);
            if (child == null) continue;
            ViewGroup.LayoutParams current = child.getLayoutParams();
            LinearLayout.LayoutParams next =
                    createToolButtonLayoutParams(i > 0);
            if (current instanceof LinearLayout.LayoutParams) {
                LinearLayout.LayoutParams cur = (LinearLayout.LayoutParams) current;
                if (cur.width == next.width && cur.height == next.height
                        && cur.weight == next.weight
                        && cur.leftMargin == next.leftMargin
                        && cur.gravity == next.gravity) {
                    continue;
                }
            }
            child.setLayoutParams(next);
        }
    }

    /**
     * Everything the toolbar needs when the orientation changed: the bar's own
     * margins/size plus the cell sizing of its buttons.
     */
    private void applyToolbarLayoutParams() {
        updateToolbarMargins();
        applyToolButtonLayoutParams();
    }

    private void initViews() {
        removeAllViews();
        setClipChildren(false);
        setClipToPadding(false);

        mFloatingContainer = new FrameLayout(getContext());
        mFloatingContainer.setClipChildren(false);
        mFloatingContainer.setClipToPadding(false);
        addView(mFloatingContainer, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // Top Toolbar (Visible only in Edit Mode)
        mTopToolbar = createTopToolbar();
        mTopToolbar.setVisibility(GONE);
        FrameLayout.LayoutParams tbLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        addView(mTopToolbar, tbLp);
        updateToolbarMargins();

        // Property Inspector Panel (Floating, initially hidden)
        mInspectorView = new HudPropertyInspectorView(getContext(), new HudPropertyInspectorView.InspectorCallback() {
            @Override
            public void onModelChanged(HudButton button) {
                mHasUnsavedChanges = true;
                rebuildActiveLayout();
            }
            @Override
            public void onDeleteRequested(HudButton button) {
                mHasUnsavedChanges = true;
                getActiveLayout().floatingButtons.remove(button);
                selectButton(null, null);
                rebuildActiveLayout();
            }
            @Override
            public void onDuplicateRequested(HudButton button) {
                mHasUnsavedChanges = true;
                HudButton dup = button.duplicate();
                getActiveLayout().floatingButtons.add(dup);
                rebuildActiveLayout();
            }
            @Override
            public void onPickActionRequested(HudButton button, int targetSlot) {
                // All five action slots (main / popup / 4 swipes) share a
                // single picker dialog. Temporarily hide the inspector view
                // while the modal picker is open to eliminate background clutter.
                mInspectorView.setVisibility(GONE);
                HudKeyPickerDialog.show(getContext(), (action, displayLabel) -> {
                    mHasUnsavedChanges = true;
                    switch (targetSlot) {
                        case 0:
                            button.action = action;
                            button.label = displayLabel;
                            break;
                        case 1:
                            button.popupAction = action;
                            break;
                        case 2:
                            button.setDirectionalAction(2, action);
                            break;
                        case 3:
                            button.setDirectionalAction(3, action);
                            break;
                        case 4:
                            button.setDirectionalAction(4, action);
                            break;
                        case 5:
                            button.setDirectionalAction(5, action);
                            break;
                    }
                    mInspectorView.bindButton(button);
                    rebuildActiveLayout();
                }, () -> {
                    if (mSelectedButton != null) {
                        mInspectorView.setVisibility(VISIBLE);
                    }
                });
            }
            @Override
            public void onGroupAddKeyRequested(HudButton group) {
                if (group == null) return;
                mInspectorView.setVisibility(GONE);
                HudKeyPickerDialog.show(getContext(), (action, displayLabel) -> {
                    HudButton child = new HudButton();
                    child.widgetType = HudButton.WIDGET_STANDARD;
                    child.action = action;
                    child.label = displayLabel != null ? displayLabel : "";
                    child.widthDp = 44;
                    child.heightDp = 36;
                    child.cornerRadiusDp = 8;
                    group.children.add(child);
                    // Children are arranged automatically by HudGroupView; the
                    // container only has to be large enough for the new grid.
                    ensureGroupFits(group);
                    mHasUnsavedChanges = true;
                    mInspectorView.setVisibility(VISIBLE);
                    selectButton(group, null);
                    rebuildActiveLayout();
                    mInspectorView.bindButton(group);
                });
            }

            @Override
            public void onGroupAddExistingRequested(HudButton group) {
                if (group == null) return;
                showMoveIntoGroupChooser(group);
            }

            @Override
            public void onGroupChildDetachRequested(HudButton group, HudButton child) {
                if (group == null || child == null) return;
                if (!group.children.remove(child)) return;
                // Drop it back on the canvas just below-right of the group so it
                // does not end up hidden underneath its former container.
                child.posXPercent = Math.min(0.95f, group.posXPercent + 0.08f);
                child.posYPercent = Math.min(0.95f, group.posYPercent + 0.10f);
                getActiveLayout().floatingButtons.add(child);
                mHasUnsavedChanges = true;
                selectButton(group, null);
                rebuildActiveLayout();
                mInspectorView.bindButton(group);
                Toast.makeText(getContext(), R.string.hud_group_detached, Toast.LENGTH_SHORT).show();
            }

            @Override
            public void onGroupChildRebindRequested(HudButton group, HudButton child) {
                if (group == null || child == null) return;
                mInspectorView.setVisibility(GONE);
                HudKeyPickerDialog.show(getContext(), (action, displayLabel) -> {
                    child.action = action;
                    if (displayLabel != null) child.label = displayLabel;
                    mHasUnsavedChanges = true;
                    mInspectorView.setVisibility(VISIBLE);
                    bindGroupView(group);
                    mInspectorView.bindButton(group);
                });
            }

            @Override
            public void onGroupChildRemoveRequested(HudButton group, HudButton child) {
                if (group == null || child == null) return;
                group.children.remove(child);
                mHasUnsavedChanges = true;
                bindGroupView(group);
                mInspectorView.bindButton(group);
            }

            public void onCloseRequested() {
                selectButton(null, null);
            }
        });
        mInspectorView.setVisibility(GONE);
        addView(mInspectorView, new LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        rebuildActiveLayout();
    }

    private LinearLayout createTopToolbar() {
        LinearLayout bar = new LinearLayout(getContext());
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackground(M3.shape(getContext(), M3.RADIUS_PANEL, 0xF0181825, M3.COLOR_BORDER_STRONG, 1.0f));
        bar.setPadding(dp(8), dp(6), dp(8), dp(6));
        bar.setElevation(dp(8));

        addTool(bar, getContext().getString(R.string.hud_overlay_toolbar_save),
                M3.COLOR_PRIMARY, false, this::saveProfile);
        addTool(bar, getContext().getString(R.string.hud_overlay_toolbar_add),
                Color.WHITE, true, this::showAddWidgetChooser);

        Button btnSnap = addTool(bar, getContext().getString(R.string.hud_overlay_toolbar_snap),
                mSnapEnabled ? M3.COLOR_PRIMARY : M3.COLOR_TEXT_MUTED, true, null);
        btnSnap.setOnClickListener(v -> {
            mSnapEnabled = !mSnapEnabled;
            btnSnap.setTextColor(mSnapEnabled ? M3.COLOR_PRIMARY : M3.COLOR_TEXT_MUTED);
            Toast.makeText(getContext(),
                    mSnapEnabled ? R.string.hud_overlay_snap_on : R.string.hud_overlay_snap_off,
                    Toast.LENGTH_SHORT).show();
        });

        addTool(bar, getContext().getString(R.string.hud_overlay_toolbar_layout),
                Color.WHITE, true, this::showLayoutOptionsChooser);
        // EXIT prompts for saving first.
        addTool(bar, getContext().getString(R.string.hud_overlay_toolbar_exit),
                M3.COLOR_ERROR, true, this::requestExit);

        return bar;
    }

    private LinearLayout.LayoutParams createToolButtonLayoutParams(boolean hasLeftMargin) {
        int orientation = getResources().getConfiguration().orientation;
        boolean isLandscape = (orientation == Configuration.ORIENTATION_LANDSCAPE);
        LinearLayout.LayoutParams lp = isLandscape
                ? new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(32))
                : new LinearLayout.LayoutParams(0, dp(34), 1f);
        if (hasLeftMargin) lp.leftMargin = isLandscape ? dp(6) : dp(4);
        return lp;
    }

    private void showLayoutOptionsChooser() {
        int orientation = getResources().getConfiguration().orientation;
        boolean isLandscape = (orientation == Configuration.ORIENTATION_LANDSCAPE);
        String copyOption = getContext().getString(isLandscape
                ? R.string.hud_overlay_copy_portrait_here
                : R.string.hud_overlay_copy_landscape_here);

        final String[] options = new String[] {
                copyOption,
                getContext().getString(R.string.hud_overlay_productivity_preset),
                getContext().getString(R.string.hud_overlay_user_portrait_hotkeys_layout),
                getContext().getString(R.string.hud_overlay_clear_active_layout)
        };
        mInspectorView.setVisibility(GONE);
        AlertDialog dialog = styleDialog(darkDialog()
                .setTitle(R.string.hud_overlay_layout_options_title)
                .setItems(options, (d, which) -> {
                    if (!mIsEditMode) return;
                    mHasUnsavedChanges = true;
                    HudLayout dst = getActiveLayout();
                    if (which == 0) {
                        // Copy from opposite layout
                        HudLayout src = isLandscape ? mProfile.portraitLayout : mProfile.landscapeLayout;
                        dst.floatingButtons.clear();
                        for (HudButton b : src.floatingButtons) {
                            dst.floatingButtons.add(b.duplicate());
                        }
                        selectButton(null, null);
                        rebuildActiveLayout();
                        Toast.makeText(getContext(), R.string.hud_overlay_copied, Toast.LENGTH_SHORT).show();
                    } else if (which == 1) {
                        // Load Desktop / Productivity Preset (10% Inset)
                        dst.floatingButtons.clear();
                        HudLayout preset = HudLayoutProfile.createProductivityLayout(isLandscape);
                        for (HudButton b : preset.floatingButtons) {
                            dst.floatingButtons.add(b.duplicate());
                        }
                        selectButton(null, null);
                        rebuildActiveLayout();
                        Toast.makeText(getContext(), R.string.hud_overlay_productivity_preset_loaded, Toast.LENGTH_SHORT).show();
                    } else if (which == 2) {
                        // Load User Portrait Hotkeys Layout
                        dst.floatingButtons.clear();
                        HudLayout preset = HudLayoutProfile.createDefaultPortraitLayout();
                        for (HudButton b : preset.floatingButtons) {
                            dst.floatingButtons.add(b.duplicate());
                        }
                        selectButton(null, null);
                        rebuildActiveLayout();
                        Toast.makeText(getContext(), R.string.hud_overlay_user_portrait_layout_loaded, Toast.LENGTH_SHORT).show();
                    } else if (which == 3) {
                        // Clear
                        dst.floatingButtons.clear();
                        selectButton(null, null);
                        rebuildActiveLayout();
                        Toast.makeText(getContext(), R.string.hud_overlay_layout_cleared, Toast.LENGTH_SHORT).show();
                    }
                    d.dismiss();
                })
                .setOnDismissListener(d -> {
                    if (mSelectedButton != null) mInspectorView.setVisibility(VISIBLE);
                })
                .setNegativeButton(R.string.hud_overlay_cancel, null)
                .create());
        dialog.show();
    }

    // Modal chooser for the "what kind of widget do you want to add?"
    // prompt. Replaces the old trio of separate [+] buttons which were
    // squeezed off the right edge of the bar on portrait phones.
    private void showAddWidgetChooser() {
        final String[] labels = new String[] {
                getContext().getString(R.string.hud_overlay_add_standard_key_button),
                getContext().getString(R.string.hud_overlay_add_super_gesture_nub),
                getContext().getString(R.string.hud_overlay_add_trackpoint),
                getContext().getString(R.string.hud_overlay_add_steam_deck_trackpad),
                getContext().getString(R.string.hud_overlay_add_scroll_bar),
                getContext().getString(R.string.hud_overlay_add_dpad),
                getContext().getString(R.string.hud_overlay_add_group)
        };
        final HudButton.WidgetKind[] kinds = new HudButton.WidgetKind[] {
                HudButton.WidgetKind.KEY,
                HudButton.WidgetKind.SUPER_GESTURE,
                HudButton.WidgetKind.TRACKPOINT,
                HudButton.WidgetKind.TOUCHPAD,
                HudButton.WidgetKind.SCROLLBAR,
                HudButton.WidgetKind.DPAD,
                HudButton.WidgetKind.GROUP
        };
        showItemChooser(R.string.hud_overlay_add_widget_title, labels, which -> {
            if (mIsEditMode) addWidgetOfKind(kinds[which]);
        });
    }

    /**
     * Lists the standalone keys currently on the canvas so they can be moved
     * into a group container. Dragging a key onto the container does the same
     * thing, but the list is the reliable path when the target is small.
     */
    private void showMoveIntoGroupChooser(HudButton group) {
        if (group == null) return;
        final List<HudButton> candidates = new ArrayList<>();
        final List<CharSequence> labels = new ArrayList<>();
        for (HudButton b : getActiveLayout().floatingButtons) {
            if (b == group || !HudButton.WIDGET_STANDARD.equals(b.widgetType)) continue;
            candidates.add(b);
            boolean hasLabel = b.label != null && !b.label.isEmpty();
            labels.add(hasLabel ? b.label : getContext().getString(R.string.hud_widget_button));
        }
        if (candidates.isEmpty()) {
            Toast.makeText(getContext(), R.string.hud_group_no_candidates,
                    Toast.LENGTH_SHORT).show();
            return;
        }
        showItemChooser(R.string.hud_group_move_in_title,
                labels.toArray(new CharSequence[0]),
                which -> moveWidgetIntoGroup(group, candidates.get(which)));
    }

    private void addWidgetOfKind(HudButton.WidgetKind kind) {
        HudButton b = new HudButton();
        switch (kind) {
            case KEY:
                b.label = getContext().getString(R.string.hud_overlay_label_key);
                b.action = HudAction.key(1);
                b.posXPercent = 0.5f;
                b.posYPercent = 0.4f;
                break;
            case SUPER_GESTURE:
                b.widgetType = HudButton.WIDGET_SUPER_GESTURE;
                b.label = getContext().getString(R.string.hud_overlay_label_super);
                b.widthDp = 58;
                b.heightDp = 58;
                b.cornerRadiusDp = 29;
                b.posXPercent = 0.5f;
                b.posYPercent = 0.5f;
                break;
            case TRACKPOINT:
                b.widgetType = HudButton.WIDGET_TRACKPOINT;
                b.label = getContext().getString(R.string.hud_overlay_label_mouse);
                b.widthDp = 60;
                b.heightDp = 60;
                b.cornerRadiusDp = 30;
                b.posXPercent = 0.2f;
                b.posYPercent = 0.5f;
                break;
            case TOUCHPAD:
                b.widgetType = HudButton.WIDGET_TOUCHPAD;
                b.label = getContext().getString(R.string.hud_overlay_label_touchpad);
                b.widthDp = 92;
                b.heightDp = 92;
                b.cornerRadiusDp = 18;
                b.posXPercent = 0.85f;
                b.posYPercent = 0.65f;
                break;
            case SCROLLBAR:
                b.widgetType = HudButton.WIDGET_SCROLLBAR;
                b.label = getContext().getString(R.string.hud_overlay_label_scroll);
                b.trackpointMode = "vertical";
                b.widthDp = 34;
                b.heightDp = 130;
                b.cornerRadiusDp = 17;
                b.posXPercent = 0.95f;
                b.posYPercent = 0.5f;
                break;
            case DPAD:
                b.widgetType = HudButton.WIDGET_DPAD;
                b.label = getContext().getString(R.string.hud_overlay_label_dpad);
                b.widthDp = 130;
                b.heightDp = 130;
                b.cornerRadiusDp = 65;
                b.posXPercent = 0.15f;
                b.posYPercent = 0.6f;
                break;
            case GROUP:
                b.widgetType = HudButton.WIDGET_GROUP;
                b.label = getContext().getString(R.string.hud_widget_group);
                b.widthDp = 180;
                b.heightDp = 120;
                b.cornerRadiusDp = 14;
                b.padDp = 8;
                b.opacity = 0.25f;
                b.posXPercent = 0.5f;
                b.posYPercent = 0.5f;
                break;
        }
        avoidOverlap(b);
        getActiveLayout().floatingButtons.add(b);
        mHasUnsavedChanges = true;
        rebuildActiveLayout();
        selectButton(b, null);
    }

    /**
     * Nudge a freshly added widget so it does not land exactly on top of one
     * that is already on the canvas. Without this every ADD stacks widgets at
     * the same spot, and only the topmost one can be grabbed or dropped into a
     * group container.
     */
    private void avoidOverlap(HudButton widget) {
        if (widget == null) return;
        HudLayout layout = getActiveLayout();
        final float step = 0.09f;
        for (int attempt = 0; attempt < 30; attempt++) {
            boolean clash = false;
            for (HudButton other : layout.floatingButtons) {
                if (other == widget) continue;
                if (Math.abs(other.posXPercent - widget.posXPercent) < 0.02f
                        && Math.abs(other.posYPercent - widget.posYPercent) < 0.02f) {
                    clash = true;
                    break;
                }
            }
            if (!clash) return;
            widget.posXPercent += step;
            if (widget.posXPercent > 0.94f) {
                widget.posXPercent = 0.14f;
                widget.posYPercent = Math.min(0.92f, widget.posYPercent + step);
            }
        }
    }

    /**
     * Append a button to the editor toolbar. Used by the floating-control editor
     * to expose the keyboard layer next to SAVE / ADD / SNAP / LAYOUT / EXIT.
     */
    public Button addToolbarButton(CharSequence label, Runnable onClick) {
        if (mTopToolbar == null) return null;
        return addTool(mTopToolbar, label, Color.WHITE, true, onClick);
    }

    /** One toolbar button; every button in the editor bar is built like this. */
    private Button addTool(LinearLayout bar, CharSequence label, int textColor,
                           boolean hasLeftMargin, Runnable onClick) {
        Button b = createToolButton(label.toString());
        b.setTextColor(textColor);
        if (onClick != null) b.setOnClickListener(v -> onClick.run());
        bar.addView(b, createToolButtonLayoutParams(hasLeftMargin));
        return b;
    }

    /**
     * True when an on-screen control would take a touch at this window-space
     * point. Immersive mode grabs the panel away from Android, so it has to
     * decide per pointer which of them belong to the HUD and which keep going
     * to the desktop; empty space inside a group container is *not* a control,
     * it must fall through to the desktop like any other hole.
     */
    public boolean isControlAt(float x, float y) {
        if (getVisibility() != VISIBLE) return false;
        return findControlAt(this, x, y) != null;
    }

    private View findControlAt(ViewGroup parent, float x, float y) {
        for (int i = parent.getChildCount() - 1; i >= 0; i--) {
            View child = parent.getChildAt(i);
            if (child.getVisibility() != VISIBLE) continue;
            float lx = x - child.getX();
            float ly = y - child.getY();
            if (lx < 0f || ly < 0f || lx >= child.getWidth() || ly >= child.getHeight()) continue;
            if (child instanceof ViewGroup) {
                View inner = findControlAt((ViewGroup) child, lx, ly);
                if (inner != null) return inner;
                // Containers are only a frame: a miss inside them falls through.
                continue;
            }
            return child;
        }
        return null;
    }

    // Material-style flat button. All top-bar buttons share the rounded
    // 8dp Material surface, the same corner radius the rest of the
    // editor UI uses. Background is a subtle translucent white so the
    // chrome blends with the dark scrim above Niri.
    private Button createToolButton(String label) {
        Button b = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
        b.setText(label);
        b.setAllCaps(true);
        b.setTextSize(11f);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setTextColor(Color.WHITE);
        b.setBackground(M3.createRippleDrawable(getContext(), M3.RADIUS_CHIP, 0x22FFFFFF, 0x44FFFFFF, M3.COLOR_BORDER_SUBTLE));
        b.setPadding(dp(10), dp(4), dp(10), dp(4));
        b.setGravity(Gravity.CENTER);
        b.setSingleLine(true);
        b.setMinWidth(dp(44));
        b.setMinHeight(dp(30));
        return b;
    }

    @Override
    protected void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // The orientation decides which HudLayout is active, so this is a real
        // rebuild (new children), not just a reposition. It must not run from
        // the middle of a layout pass -- see scheduleRebuild().
        scheduleRebuild();
        // The toolbar switches between "full-width row of equal cells" in
        // portrait and "wrap-content, centred" in landscape, so its layout
        // parameters have to be recomputed for the new orientation once the new
        // size is known.
        post(() -> {
            applyToolbarLayoutParams();
            if (mSelectedButton != null) placeInspector(mSelectedButton);
            else if (mInspectorView != null) mInspectorView.adaptSize(getWidth(), getHeight());
        });
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        if (w <= 0 || h <= 0 || (w == oldw && h == oldh)) return;
        // Called from inside the layout pass. Touching the child list here is
        // what left every HUD widget at 0x0 (and therefore invisible) after a
        // rotation: the container had already been measured with the previous
        // children, so the freshly added ones were never measured and their
        // requestLayout() could not force another pass.
        if (getActiveLayout() != mBuiltLayout) {
            // A different layout object means the other orientation (or the
            // other layer): the children must be recreated. Do it after the
            // layout pass, never inside it.
            scheduleRebuild();
        } else {
            // Same layout, new canvas size (IME, freeform resize, a rotation
            // between two same-shaped windows): the existing children only
            // have to be moved to their new percentage positions.
            repositionExistingChildren();
        }
        invalidate();
    }

    /**
     * Move the already-attached widget views to their percentage positions for
     * the current canvas size. Safe to call at any point of the layout pass:
     * it never adds or removes children, so it cannot leave the container with
     * unmeasured views.
     */
    private void repositionExistingChildren() {
        if (mFloatingContainer == null) return;
        // The children were built from mBuiltLayout, which after a rotation is
        // still the *previous* orientation's layout. Use it, not the freshly
        // selected one: the percentages only exist for the layout the views
        // were actually created from.
        HudLayout layout = mBuiltLayout;
        if (layout == null) return;
        int pw = getWidth();
        int ph = getHeight();
        if (pw <= 0 || ph <= 0) return;
        int count = Math.min(mFloatingContainer.getChildCount(), layout.floatingButtons.size());
        float density = getResources().getDisplayMetrics().density;
        for (int i = 0; i < count; i++) {
            View child = mFloatingContainer.getChildAt(i);
            if (child == null) continue;
            HudButton model = layout.floatingButtons.get(i);
            int w = Math.round(model.widthDp * density);
            int h = Math.round(model.heightDp * density);
            ViewGroup.LayoutParams lp = child.getLayoutParams();
            if (lp != null && (lp.width != w || lp.height != h)) {
                lp.width = w;
                lp.height = h;
                child.setLayoutParams(lp);
            }
            child.setX(model.posXPercent * pw - w * 0.5f);
            child.setY(model.posYPercent * ph - h * 0.5f);
        }
    }

    /**
     * Recreate the widget views on the next frame. Used instead of calling
     * {@link #rebuildActiveLayout()} directly whenever the caller may be
     * inside a layout pass; the rebuild then happens with a clean layout state
     * so the new children are actually measured.
     */
    private void scheduleRebuild() {
        if (mRebuildPosted) return;
        mRebuildPosted = true;
        postOnAnimation(mDeferredRebuild);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (mIsEditMode) {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                selectButton(null, null);
            }
            return true;
        }
        return super.onTouchEvent(event);
    }

    // ---- IModifierProvider: modifier latch ----

    @Override
    public boolean hasActiveModifier() {
        for (ModState m : mModifiers.values())
            if (m.active) return true;
        return false;
    }

    @Override
    public void sendKeyComboFromExternal(int evdevScancode) {
        sendWithModifiers(() -> {
            if (mHost != null) {
                mHost.sendKey(0, evdevScancode);
                mHost.sendKey(1, evdevScancode);
            }
        });
    }

    @Override
    public void reset() {
        for (ModState m : mModifiers.values()) {
            if (m.active && mHost != null) mHost.sendKey(1, m.evdev);
            m.active = false;
            m.locked = false;
        }
    }

    private void toggleModifier(int evdev) {
        ModState state = mModifiers.get(evdev);
        if (state == null) {
            state = new ModState();
            state.evdev = evdev;
            mModifiers.put(evdev, state);
        }
        setModifierActive(state, !state.active);
        if (!state.active) state.locked = false;
    }

    private void setModifierActive(ModState state, boolean active) {
        state.active = active;
        if (!active) state.locked = false;
        // Dock strip was removed; floating TrackPoint / SuperGesture buttons
        // do not surface modifier latch state visually today. A future
        // floating modifier indicator widget could subscribe to a
        // listener here if we ever add one.
    }

    private void sendWithModifiers(Runnable emit) {
        List<ModState> held = new ArrayList<>();
        for (ModState m : mModifiers.values())
            if (m.active) held.add(m);

        for (ModState m : held) {
            if (mHost != null) mHost.sendKey(0, m.evdev);
        }
        emit.run();
        for (int i = held.size() - 1; i >= 0; i--) {
            if (mHost != null) mHost.sendKey(1, held.get(i).evdev);
        }

        // Auto-release modifiers that aren't locked
        for (ModState m : held) {
            if (!m.locked) setModifierActive(m, false);
        }
    }

    public void rebuildActiveLayout() {
        if (mFloatingContainer == null) return;
        // Belt and braces: a rebuild removes and re-adds children, which must
        // never happen while this view is measuring/laying out -- the fresh
        // children would never be measured (0x0, i.e. invisible). Every caller
        // is expected to use scheduleRebuild() in that situation, but deferring
        // here means a future caller cannot reintroduce the bug.
        if (isInLayout()) {
            scheduleRebuild();
            return;
        }
        mFloatingContainer.removeAllViews();
        HudLayout layout = getActiveLayout();
        mBuiltLayout = layout;
        // A rebuild supersedes any rebuild that was already queued: this call
        // is about to recreate the very views that one would have touched.
        mRebuildPosted = false;
        removeCallbacks(mDeferredRebuild);

        for (HudButton b : layout.floatingButtons) {
            View widgetView;
            if (HudButton.WIDGET_TRACKPOINT.equals(b.widgetType)) {
                widgetView = new TrackpointNubView(getContext(), b, mTrackpointDispatcher);
                ((TrackpointNubView) widgetView).setInEditMode(mIsEditMode);
            } else if (HudButton.WIDGET_SUPER_GESTURE.equals(b.widgetType)) {
                widgetView = new SuperGestureButtonView(getContext(), b, mSuperGestureListener);
                ((SuperGestureButtonView) widgetView).setInEditMode(mIsEditMode);
            } else if (HudButton.WIDGET_TOUCHPAD.equals(b.widgetType)) {
                widgetView = new HudTouchpadView(getContext(), b, mTouchpadDispatcher);
                ((HudTouchpadView) widgetView).setInEditMode(mIsEditMode);
            } else if (HudButton.WIDGET_SCROLLBAR.equals(b.widgetType)) {
                widgetView = new HudScrollBarView(getContext(), b, mScrollBarDispatcher);
                ((HudScrollBarView) widgetView).setInEditMode(mIsEditMode);
            } else if (HudButton.WIDGET_DPAD.equals(b.widgetType)) {
                HudDpadInput input = new HudDpadInput((keyAction, code) -> {
                    if (mHost != null) mHost.sendKey(keyAction, code);
                });
                widgetView = new HudDpadView(getContext(), b, (direction, pressed) -> {
                    if (mIsEditMode && pressed) return;
                    int slot;
                    switch (direction) {
                        case HudDpadView.DIR_UP: slot = 4; break;
                        case HudDpadView.DIR_DOWN: slot = 5; break;
                        case HudDpadView.DIR_LEFT: slot = 2; break;
                        case HudDpadView.DIR_RIGHT: slot = 3; break;
                        default: return;
                    }
                    HudAction action = b.directionalAction(slot);
                    input.update(direction, action, pressed);
                    if (pressed && action != null && !HudDpadInput.isHeldAction(action))
                        dispatchHudAction(action);
                });
                ((HudDpadView) widgetView).setInEditMode(mIsEditMode);
            } else if (HudButton.WIDGET_GROUP.equals(b.widgetType)) {
                HudGroupView groupView = new HudGroupView(getContext(), b, mKeyButtonListener);
                groupView.setInEditMode(mIsEditMode);
                widgetView = groupView;
            } else {
                HudFreeformButtonView keyView = new HudFreeformButtonView(
                        getContext(), b, mKeyButtonListener);
                keyView.setInEditMode(mIsEditMode);
                widgetView = keyView;
            }

            if (b == mSelectedButton) {
                mSelectedView = widgetView;
            }
            // Clean-screen mode keeps only the KB key, which is also the way
            // back. Editing always shows everything so the layout stays workable.
            if (mHudHidden && !mIsEditMode && !isKeyboardLayer()) {
                if (b.isVirtualKeyboardToggle()) {
                    widgetView.setAlpha(0.6f);
                } else {
                    widgetView.setVisibility(GONE);
                }
            }
            attachTouchAndLayout(widgetView, b);
            mFloatingContainer.addView(widgetView);
        }
        invalidate();
    }

    /** Re-render a group's children after the model changed. */
    private void bindGroupView(HudButton group) {
        if (mFloatingContainer == null || group == null) return;
        for (int i = 0; i < mFloatingContainer.getChildCount(); i++) {
            View child = mFloatingContainer.getChildAt(i);
            if (child instanceof HudGroupView) {
                HudGroupView gv = (HudGroupView) child;
                if (gv.getModel() == group) {
                    gv.rebuildChildren();
                    return;
                }
            }
        }
        rebuildActiveLayout();
    }

    private void attachTouchAndLayout(View view, HudButton model) {
        float density = getResources().getDisplayMetrics().density;
        int w = Math.round(model.widthDp * density);
        int h = Math.round(model.heightDp * density);

        view.setLayoutParams(new LayoutParams(w, h));
        int curPw = getWidth();
        int curPh = getHeight();
        boolean isLandscape = getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE;
        boolean dimensionsMatchOrientation = (isLandscape && curPw >= curPh) || (!isLandscape && curPw <= curPh);

        if (curPw > 0 && curPh > 0 && dimensionsMatchOrientation) {
            view.setX(model.posXPercent * curPw - w * 0.5f);
            view.setY(model.posYPercent * curPh - h * 0.5f);
        } else {
            post(() -> {
                int pw = getWidth();
                int ph = getHeight();
                if (pw > 0 && ph > 0) {
                    view.setX(model.posXPercent * pw - w * 0.5f);
                    view.setY(model.posYPercent * ph - h * 0.5f);
                }
            });
        }

        view.setOnTouchListener((v, event) -> {
            if (!mIsEditMode) return false;

            float rawX = event.getRawX();
            float rawY = event.getRawY();

            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: {
                    selectButton(model, view);
                    // Raise the dragged widget above its siblings. translationZ
                    // changes drawing order without touching the child index,
                    // which getSiblingBounds() relies on.
                    view.setTranslationZ(dp(12));
                    mTouchDownX = rawX;
                    mTouchDownY = rawY;
                    // Recompute initial position from the percentage model every
                    // time the user starts dragging. View.getX() can be stale
                    // after orientation changes or after rebuildActiveLayout
                    // queued a post() that has not run yet, so trusting the
                    // model is the only way to keep the view and the touch
                    // delta in lock-step.
                    int parentW = getWidth();
                    int parentH = getHeight();
                    if (parentW > 0 && parentH > 0) {
                        mInitialBtnLeft = model.posXPercent * parentW - view.getWidth() * 0.5f;
                        mInitialBtnTop = model.posYPercent * parentH - view.getHeight() * 0.5f;
                        // Snap the view itself to the recomputed position so
                        // that subsequent getX()/getY() calls also return this
                        // value (the system View.setX() is a no-op when the
                        // new value is identical to the current one).
                        view.setX(mInitialBtnLeft);
                        view.setY(mInitialBtnTop);
                    } else {
                        mInitialBtnLeft = view.getX();
                        mInitialBtnTop = view.getY();
                    }
                    clearGuides();
                    mWasSnapped = false;
                    invalidate();
                    return true;
                }

                case MotionEvent.ACTION_MOVE: {
                    mHasUnsavedChanges = true;
                    float targetX = mInitialBtnLeft + (rawX - mTouchDownX);
                    float targetY = mInitialBtnTop + (rawY - mTouchDownY);
                    float snappedX = targetX;
                    float snappedY = targetY;

                    clearGuides();

                    // Keys can be dropped into a group container: highlight the
                    // container under the finger and suspend magnetic snapping
                    // while hovering it, so the drop aim wins over alignment.
                    if (canJoinGroup(model)) {
                        setDropTargetGroup(findGroupUnder(targetX, targetY,
                                view.getWidth(), view.getHeight()));
                    }

                    if (mSnapEnabled && mDropTargetGroup == null) {
                        RectF candidate = new RectF(targetX, targetY,
                                targetX + view.getWidth(), targetY + view.getHeight());
                        List<RectF> siblings = getSiblingBounds(model);
                        float dockTop = getHeight();
                        SnapGeometryEngine.SnapResult snap = mSnapEngine.computeSnap(
                                candidate, siblings, getWidth(), getHeight(), dockTop);

                        snappedX = snap.snappedX;
                        snappedY = snap.snappedY;

                        // Only draw exact alignment guidelines (Zalith Launcher style)
                        mActiveVerticalGuides.addAll(snap.verticalGuidelines);
                        mActiveHorizontalGuides.addAll(snap.horizontalGuidelines);

                        // Crisp haptic feedback on magnetic latch
                        boolean isSnapped = snap.hasSnapX || snap.hasSnapY;
                        if (isSnapped && !mWasSnapped) {
                            view.performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK);
                        }
                        mWasSnapped = isSnapped;
                    }

                    view.setX(snappedX);
                    view.setY(snappedY);

                    model.posXPercent = (snappedX + view.getWidth() * 0.5f) / getWidth();
                    model.posYPercent = (snappedY + view.getHeight() * 0.5f) / getHeight();
                    invalidate();
                    return true;
                }

                case MotionEvent.ACTION_UP: {
                    view.setTranslationZ(0);
                    HudGroupView target = mDropTargetGroup;
                    setDropTargetGroup(null);
                    clearGuides();
                    mWasSnapped = false;
                    invalidate();
                    if (target != null) {
                        view.performHapticFeedback(android.view.HapticFeedbackConstants.CONFIRM);
                        moveWidgetIntoGroup(target.getModel(), model);
                    }
                    return true;
                }

                case MotionEvent.ACTION_CANCEL: {
                    view.setTranslationZ(0);
                    setDropTargetGroup(null);
                    clearGuides();
                    mWasSnapped = false;
                    invalidate();
                    return true;
                }
            }
            return false;
        });
    }

    /** Every editor dialog is the same dark Material card. */
    private AlertDialog.Builder darkDialog() {
        return new AlertDialog.Builder(getContext(), AlertDialog.THEME_DEVICE_DEFAULT_DARK);
    }

    private AlertDialog styleDialog(AlertDialog dialog) {
        if (dialog.getWindow() != null) {
            dialog.getWindow().setBackgroundDrawable(M3.shape(getContext(),
                    M3.RADIUS_CARD, M3.COLOR_SURFACE, M3.COLOR_BORDER_STRONG, 1.0f));
        }
        return dialog;
    }

    /**
     * List chooser with the inspector hidden behind it. The inspector comes
     * back only if a widget is still selected, which is the behaviour every
     * chooser in this editor wants.
     */
    private void showItemChooser(int titleRes, CharSequence[] items,
                                 java.util.function.IntConsumer onPick) {
        mInspectorView.setVisibility(GONE);
        AlertDialog dialog = styleDialog(darkDialog()
                .setTitle(titleRes)
                .setItems(items, (d, which) -> onPick.accept(which))
                .setOnDismissListener(d -> {
                    if (mSelectedButton != null) mInspectorView.setVisibility(VISIBLE);
                })
                .setNegativeButton(R.string.hud_overlay_cancel, null)
                .create());
        dialog.show();
    }

    private void clearGuides() {
        mActiveVerticalGuides.clear();
        mActiveHorizontalGuides.clear();
        mActiveSelfVerticalGuides.clear();
        mActiveSelfHorizontalGuides.clear();
    }

    /** Only plain key widgets can live inside a group container. */
    private static boolean canJoinGroup(HudButton model) {
        return model != null && HudButton.WIDGET_STANDARD.equals(model.widgetType);
    }

    /**
     * Hit-test the group containers against the dragged widget's centre point.
     * The rect is passed in overlay coordinates, which is exactly what the
     * group views use because mFloatingContainer fills the overlay at (0,0).
     */
    private HudGroupView findGroupUnder(float left, float top, int width, int height) {
        if (mFloatingContainer == null) return null;
        float cx = left + width * 0.5f;
        float cy = top + height * 0.5f;
        for (int i = mFloatingContainer.getChildCount() - 1; i >= 0; i--) {
            View child = mFloatingContainer.getChildAt(i);
            if (!(child instanceof HudGroupView)) continue;
            if (cx >= child.getX() && cx <= child.getX() + child.getWidth()
                    && cy >= child.getY() && cy <= child.getY() + child.getHeight()) {
                return (HudGroupView) child;
            }
        }
        return null;
    }

    private void setDropTargetGroup(HudGroupView target) {
        if (mDropTargetGroup == target) return;
        if (mDropTargetGroup != null) mDropTargetGroup.setDropTarget(false);
        mDropTargetGroup = target;
        if (target != null) target.setDropTarget(true);
    }

    /** Move a standalone key widget into a group container. */
    private void moveWidgetIntoGroup(HudButton group, HudButton widget) {
        if (group == null || widget == null || group == widget) return;
        if (!HudButton.WIDGET_GROUP.equals(group.widgetType)) return;
        if (!HudButton.WIDGET_STANDARD.equals(widget.widgetType)) return;
        HudLayout layout = getActiveLayout();
        if (!layout.floatingButtons.remove(widget)) return;
        group.children.add(widget);
        ensureGroupFits(group);
        mHasUnsavedChanges = true;
        selectButton(group, null);
        rebuildActiveLayout();
        Toast.makeText(getContext(), R.string.hud_group_moved_in, Toast.LENGTH_SHORT).show();
    }

    /**
     * Grow a group container so the keys it just received fit inside it. Both
     * the width and the height follow the same grid HudGroupView lays out, so
     * the container never clips its own children.
     */
    private void ensureGroupFits(HudButton group) {
        if (group == null || group.children == null || group.children.isEmpty()) return;
        final int gap = 4;
        // Group containers are allowed to grow as large as the inspector lets
        // the user size them (see HudPropertyInspectorView.GROUP_SIZE_MAX_DP).
        final int maxSizeDp = 480;
        int pad = Math.max(0, group.padDp);
        int maxW = 0;
        int maxH = 0;
        for (HudButton child : group.children) {
            maxW = Math.max(maxW, child.widthDp);
            maxH = Math.max(maxH, child.heightDp);
        }
        if (maxW <= 0 || maxH <= 0) return;
        if (maxW + pad * 2 > group.widthDp)
            group.widthDp = Math.min(maxSizeDp, maxW + pad * 2);
        int contentW = Math.max(1, group.widthDp - pad * 2);
        int cols = Math.max(1, (contentW + gap) / (maxW + gap));
        cols = Math.min(cols, group.children.size());
        int rows = (group.children.size() + cols - 1) / cols;
        int neededH = pad * 2 + rows * maxH + Math.max(0, rows - 1) * gap;
        if (neededH > group.heightDp)
            group.heightDp = Math.min(maxSizeDp, neededH);
    }

    private List<RectF> getSiblingBounds(HudButton exclude) {
        List<RectF> list = new ArrayList<>();
        HudLayout layout = getActiveLayout();
        for (int i = 0; i < mFloatingContainer.getChildCount(); i++) {
            View child = mFloatingContainer.getChildAt(i);
            if (i < layout.floatingButtons.size()) {
                HudButton b = layout.floatingButtons.get(i);
                if (b != exclude) {
                    list.add(new RectF(child.getX(), child.getY(), child.getX() + child.getWidth(), child.getY() + child.getHeight()));
                }
            }
        }
        return list;
    }

    private void selectButton(HudButton b, View v) {
        this.mSelectedButton = b;
        this.mSelectedView = v;
        boolean wasHidden = mInspectorView.getVisibility() != VISIBLE;
        mInspectorView.bindButton(b);
        if (b != null && wasHidden) {
            post(() -> placeInspector(b));
        }
        invalidate();
    }

    /**
     * Park the property inspector in the corner diagonally opposite the widget
     * being edited, clamped so it is always fully on screen. Also used after a
     * rotation: the panel used to keep the translation it had for the previous
     * canvas size and ended up outside the window.
     */
    private void placeInspector(HudButton b) {
        if (b == null || mInspectorView == null) return;
        if (mInspectorView.getVisibility() != VISIBLE) return;
        int pw = getWidth();
        int ph = getHeight();
        if (pw <= 0 || ph <= 0) return;
        mInspectorView.adaptSize(pw, ph);
        int iw = mInspectorView.getWidth() > 0 ? mInspectorView.getWidth() : dp(pw > ph ? 290 : 260);
        int ih = mInspectorView.getHeight() > 0 ? mInspectorView.getHeight() : dp(pw > ph ? 240 : 350);
        float targetX = (b.posXPercent < 0.5f) ? (pw - iw - dp(16) - mInsetRight) : (dp(16) + mInsetLeft);
        float targetY = (b.posYPercent < 0.5f) ? (ph - ih - dp(16) - mInsetBottom) : (dp(56) + mInsetTop);
        targetX = Math.max(dp(8) + mInsetLeft, Math.min(pw - iw - dp(8) - mInsetRight, targetX));
        targetY = Math.max(dp(56) + mInsetTop, Math.min(ph - ih - dp(8) - mInsetBottom, targetY));
        mInspectorView.setTranslationX(Math.max(0f, targetX));
        mInspectorView.setTranslationY(Math.max(0f, targetY));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (mIsEditMode) {
            float density = getResources().getDisplayMetrics().density;
            float w = getWidth();
            float h = getHeight();

            // 1. Semi-transparent dark scrim overlay (backdrop)
            canvas.drawColor(0xBB11111B);

            // 2. Blueprint subtle grid (32dp steps)
            float step = 32f * density;
            for (float gx = step; gx < w; gx += step) {
                canvas.drawLine(gx, 0, gx, h, mGridPaint);
            }
            for (float gy = step; gy < h; gy += step) {
                canvas.drawLine(0, gy, w, gy, mGridPaint);
            }

            // 3. Draw magnetic guidelines
            // 3a. Sibling guides (xanh lá) - snap với nút khác / mép canvas
            for (float x : mActiveVerticalGuides) {
                canvas.drawLine(x, 0, x, h, mGuidePaint);
            }
            for (float y : mActiveHorizontalGuides) {
                canvas.drawLine(0, y, w, y, mGuidePaint);
            }
            // 3b. Self guides (cyan) - các cạnh của chính nút đang kéo
            for (float x : mActiveSelfVerticalGuides) {
                canvas.drawLine(x, 0, x, h, mSelfGuidePaint);
            }
            for (float y : mActiveSelfHorizontalGuides) {
                canvas.drawLine(0, y, w, y, mSelfGuidePaint);
            }

            // 4. Draw bounding border around selected view
            if (mSelectedView != null && mSelectedButton != null) {
                float bw = mSelectedView.getWidth();
                float bh = mSelectedView.getHeight();
                if (bw <= 0 || bh <= 0) {
                    bw = mSelectedButton.widthDp * density;
                    bh = mSelectedButton.heightDp * density;
                }
                RectF r = new RectF(mSelectedView.getX() - 3 * density,
                        mSelectedView.getY() - 3 * density,
                        mSelectedView.getX() + bw + 3 * density,
                        mSelectedView.getY() + bh + 3 * density);
                canvas.drawRoundRect(r, 6 * density, 6 * density, mSelectBorderPaint);
            }

            // 5. Bottom-center daemon status banner (only when we know the host)
            if (mHost != null) {
                drawDaemonBanner(canvas, w, h, density);
            }
        }
    }

    private void drawDaemonBanner(Canvas canvas, float w, float h, float density) {
        boolean nativeReady = mHost.isNativeReady();
        float bannerH = 26f * density;
        float bannerW = Math.min(w - 60f * density, 240f * density);
        float left = (w - bannerW) / 2f;
        float bottomMargin = Math.max(mInsetBottom, getSystemNavigationBarHeight(getContext())) + 8f * density;
        float top = h - bannerH - bottomMargin;
        if (top < 16f * density) top = 16f * density;

        mBannerFillPaint.setStyle(Paint.Style.FILL);
        mBannerFillPaint.setColor(nativeReady ? 0xEE14281A : 0xEE301616);
        float radius = bannerH / 2f;
        canvas.drawRoundRect(left, top, left + bannerW, top + bannerH, radius, radius, mBannerFillPaint);

        mBannerStrokePaint.setColor(nativeReady ? 0x88A6E3A1 : 0x88F38BA8);
        canvas.drawRoundRect(left, top, left + bannerW, top + bannerH, radius, radius, mBannerStrokePaint);

        // Status indicator dot
        mBannerFillPaint.setColor(nativeReady ? M3.COLOR_SUCCESS : M3.COLOR_ERROR);
        float dotX = left + 14f * density;
        float dotY = top + bannerH / 2f;
        canvas.drawCircle(dotX, dotY, 3.5f * density, mBannerFillPaint);

        // Text
        mBannerTextPaint.setTextSize(11f * density);
        float textY = top + bannerH / 2f - ((mBannerTextPaint.descent() + mBannerTextPaint.ascent()) * 0.5f);
        String msg = getContext().getString(nativeReady
                ? R.string.hud_overlay_daemon_connected
                : R.string.hud_overlay_daemon_offline);
        canvas.drawText(msg, left + bannerW / 2f + 4f * density, textY, mBannerTextPaint);
    }

    private void dispatchHudAction(HudAction action) {
        if (action == null || mHost == null) return;
        // In edit mode, all action dispatch is suspended. The dock strip and
        // freeform buttons only serve as drag-to-move handles; the user picks
        // a button to rebind it via the property inspector. Without this guard
        // tapping ESC/Tab/Super in edit mode would still send real keys to
        // the desktop, surprising the user mid-design.
        if (mIsEditMode) return;
        if (HudAction.TYPE_KEY.equals(action.type)) {
            if (action.code <= 0) return;
            sendWithModifiers(() -> {
                if (mHost != null) {
                    mHost.sendKey(0, action.code);
                    mHost.sendKey(1, action.code);
                }
            });
        } else if (HudAction.TYPE_MODIFIER.equals(action.type)) {
            toggleModifier(action.code);
        } else if (HudAction.TYPE_COMBO.equals(action.type)) {
            for (int k : action.comboKeys) mHost.sendKey(0, k);
            postDelayed(() -> {
                for (int i = action.comboKeys.size() - 1; i >= 0; i--) {
                    mHost.sendKey(1, action.comboKeys.get(i));
                }
            }, 50);
        } else if (HudAction.TYPE_TEXT.equals(action.type)) {
            sendWithModifiers(() -> {
                if (mHost != null) {
                    mHost.sendTextInput(action.text.getBytes());
                }
            });
        } else if (HudAction.TYPE_SYSTEM.equals(action.type)) {
            if ("toggle_ime".equals(action.systemCommand)) mHost.toggleSystemKeyboard();
            else if ("toggle_vk".equals(action.systemCommand)) mHost.toggleVirtualKeyboard();
            else if ("open_settings".equals(action.systemCommand)) mHost.openSettings();
            else if ("toggle_steam_deck".equals(action.systemCommand)) mHost.toggleSteamDeckMode();
            else if ("exit_steam_deck".equals(action.systemCommand)) mHost.exitSteamDeckMode();
            else if ("mouse_left".equals(action.systemCommand)) {
                mHost.sendMouseButton(0x110, true);
                postDelayed(() -> mHost.sendMouseButton(0x110, false), 50);
            } else if ("mouse_right".equals(action.systemCommand)) {
                mHost.sendMouseButton(0x111, true);
                postDelayed(() -> mHost.sendMouseButton(0x111, false), 50);
            } else if ("mouse_middle".equals(action.systemCommand)) {
                mHost.sendMouseButton(0x112, true);
                postDelayed(() -> mHost.sendMouseButton(0x112, false), 50);
            }
        }
    }

    private int dp(int val) {
        return Math.round(val * getResources().getDisplayMetrics().density);
    }
}
