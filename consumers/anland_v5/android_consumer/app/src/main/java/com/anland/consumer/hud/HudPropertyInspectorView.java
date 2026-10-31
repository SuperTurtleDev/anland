package com.anland.consumer.hud;

import com.anland.consumer.R;
import com.anland.consumer.theme.M3;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

/**
 * Draggable Property Inspector Panel (PUBG/FreeFire style).
 * Allows live fine-tuning of Width, Height, Corner Radius, and Opacity via both smooth SeekBars
 * and exact numeric EditTexts, with draggable header to prevent blocking button views.
 */
public final class HudPropertyInspectorView extends LinearLayout {

    public interface InspectorCallback {
        void onModelChanged(HudButton button);
        void onDeleteRequested(HudButton button);
        void onDuplicateRequested(HudButton button);
        void onPickActionRequested(HudButton button, int targetSlot); // 0=main, 1=popup, 2=left, 3=right, 4=up, 5=down
        /** Append a freshly picked key to a group container. */
        void onGroupAddKeyRequested(HudButton group);
        /** Move a key that already lives on the canvas into a group container. */
        void onGroupAddExistingRequested(HudButton group);
        /** Rebind one existing child key of a group. */
        void onGroupChildRebindRequested(HudButton group, HudButton child);
        /** Take a child key out of the group and back onto the canvas. */
        void onGroupChildDetachRequested(HudButton group, HudButton child);
        /** Delete one child key from a group. */
        void onGroupChildRemoveRequested(HudButton group, HudButton child);
        void onCloseRequested();
    }

    /** Max width / height a normal widget can have, in dp. */
    private static final int SIZE_MAX_DP = 240;
    /** Group containers hold a grid of keys, so they may grow much bigger. */
    private static final int GROUP_SIZE_MAX_DP = 480;
    private static final int SIZE_MIN_DP = 20;

    private HudButton mActiveButton;
    private final InspectorCallback mCallback;

    private static int sizeMaxDp(HudButton b) {
        return (b != null && HudButton.WIDGET_GROUP.equals(b.widgetType))
                ? GROUP_SIZE_MAX_DP : SIZE_MAX_DP;
    }
    // Guard flag: true while we are programmatically setting EditText / SeekBar
    // values inside bindButton(). The TextWatcher on mLabelInput and the seek
    // listeners otherwise fire on every setText(), which would call back into
    // mCallback.onModelChanged() and round-trip through HudOverlayView's
    // rebuildActiveLayout(), ultimately resulting in a re-entrant bind and
    // either a stack overflow or a runaway model update.
    private boolean mIsProgrammaticChange = false;

    private TextView mTitleText;
    private EditText mLabelInput;

    // Sliders + Exact Numeric Inputs
    private SeekBar mWidthSeekBar;
    private EditText mWidthInput;

    private SeekBar mHeightSeekBar;
    private EditText mHeightInput;

    private SeekBar mCornerSeekBar;
    private EditText mCornerInput;

    private SeekBar mOpacitySeekBar;
    private EditText mOpacityInput;

    // Container that holds the four size/opacity slider rows. Visible only for
    // floating buttons; hidden for dock items (their size is fixed by the
    // dock strip's grid policy).
    private LinearLayout mSizeSection;

    private Button mBtnPickMainAction;
    private LinearLayout mMainActionRow;
    private Switch mRepeatSwitch;
    private LinearLayout mGroupOptions;
    private LinearLayout mGroupChildList;
    private Button mBtnPickPopupAction;
    private LinearLayout mPopupActionRow;
    private LinearLayout mSuperGestureOptions;
    // TrackPoint mode toggle row: visible only for the trackpoint widget.
    // Two buttons (MOUSE / SCROLL) plus a small status text underneath.
    private LinearLayout mTrackPointOptions;
    private Button mBtnTrackPointMode;
    private SeekBar mTrackSensitivitySeekBar;
    private EditText mTrackSensitivityInput;
    private SeekBar mTrackDeadzoneSeekBar;
    private EditText mTrackDeadzoneInput;
    // Per-button haptic toggle
    private android.widget.Switch mHapticSwitch;
    private Button mBtnDelete;
    private Button mBtnDuplicate;
    private boolean mConfirmDeleteActive = false;
    private final Handler mDeleteHandler = new Handler(Looper.getMainLooper());
    private final Runnable mResetDeleteRunnable = () -> {
        mConfirmDeleteActive = false;
        if (mBtnDelete != null) {
            mBtnDelete.setText(getContext().getString(R.string.hud_delete_short));
            mBtnDelete.setBackground(M3.shape(getContext(), M3.RADIUS_INPUT, 0x33FF0000, 0x55F38BA8, 1.0f));
            mBtnDelete.setTextColor(M3.COLOR_ERROR);
        }
    };
    private LinearLayout mColorThemeRow;
    private Button mBtnPickSwipeLeft;
    private Button mBtnPickSwipeRight;
    private Button mBtnPickSwipeUp;
    private Button mBtnPickSwipeDown;

    private float mDragStartX;
    private float mDragStartY;
    private ScrollView mScrollView;

    public HudPropertyInspectorView(Context context, InspectorCallback callback) {
        super(context);
        this.mCallback = callback;
        initView();
    }

    private void initView() {
        setOrientation(VERTICAL);
        android.graphics.drawable.GradientDrawable panelBg = new android.graphics.drawable.GradientDrawable();
        panelBg.setCornerRadius(dp(14));
        panelBg.setColor(0xF0181825);
        panelBg.setStroke(dp(1), 0x33FFFFFF);
        setBackground(panelBg);
        setPadding(0, 0, 0, dp(10));
        setElevation(dp(8));

        // 1. Full-width Draggable Header Bar (flush with top, left, right edges)
        LinearLayout header = new LinearLayout(getContext());
        header.setOrientation(HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        android.graphics.drawable.GradientDrawable headerBg = new android.graphics.drawable.GradientDrawable();
        float hr = dp(14);
        headerBg.setCornerRadii(new float[]{hr, hr, hr, hr, 0, 0, 0, 0});
        headerBg.setColor(0x2AFFFFFF);
        header.setBackground(headerBg);
        header.setPadding(dp(14), dp(8), dp(10), dp(8));
        LinearLayout.LayoutParams headerLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        header.setLayoutParams(headerLp);

        M3.DragGripView grip = new M3.DragGripView(getContext());
        header.addView(grip);

        View headerSpacer = new View(getContext());
        header.addView(headerSpacer, new LinearLayout.LayoutParams(0, 1, 1f));

        Button btnDuplicate = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
        btnDuplicate.setText(getContext().getString(R.string.hud_duplicate_short));
        btnDuplicate.setAllCaps(true);
        btnDuplicate.setTextColor(M3.COLOR_TEXT_PRIMARY);
        btnDuplicate.setTextSize(11);
        btnDuplicate.setTypeface(Typeface.DEFAULT_BOLD);
        btnDuplicate.setBackground(M3.createRippleDrawable(getContext(), M3.RADIUS_INPUT, M3.COLOR_SURFACE_HIGHEST, 0x44FFFFFF, M3.COLOR_BORDER_SUBTLE));
        btnDuplicate.setPadding(dp(10), dp(4), dp(10), dp(4));
        btnDuplicate.setMinHeight(dp(28));
        btnDuplicate.setMinWidth(0);
        btnDuplicate.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) mCallback.onDuplicateRequested(mActiveButton);
        });
        header.addView(btnDuplicate);
        mBtnDuplicate = btnDuplicate;

        mBtnDelete = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
        mBtnDelete.setText(getContext().getString(R.string.hud_delete_short));
        mBtnDelete.setAllCaps(true);
        mBtnDelete.setTextColor(M3.COLOR_ERROR);
        mBtnDelete.setTextSize(11);
        mBtnDelete.setTypeface(Typeface.DEFAULT_BOLD);
        mBtnDelete.setBackground(M3.shape(getContext(), M3.RADIUS_INPUT, 0x33FF0000, 0x55F38BA8, 1.0f));
        mBtnDelete.setPadding(dp(10), dp(4), dp(10), dp(4));
        mBtnDelete.setMinHeight(dp(28));
        mBtnDelete.setMinWidth(0);
        LinearLayout.LayoutParams delLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        delLp.leftMargin = dp(6);
        delLp.rightMargin = dp(8);
        mBtnDelete.setLayoutParams(delLp);
        mBtnDelete.setOnClickListener(v -> {
            if (!mConfirmDeleteActive) {
                mConfirmDeleteActive = true;
                mBtnDelete.setText(getContext().getString(R.string.hud_delete_confirm));
                mBtnDelete.setBackground(M3.shape(getContext(), M3.RADIUS_INPUT, 0xCCFF1744, 0xFFFF5252, 1.0f));
                mBtnDelete.setTextColor(Color.WHITE);
                mDeleteHandler.postDelayed(mResetDeleteRunnable, 3000);
            } else {
                mDeleteHandler.removeCallbacks(mResetDeleteRunnable);
                mConfirmDeleteActive = false;
                if (mCallback != null && mActiveButton != null) {
                    mCallback.onDeleteRequested(mActiveButton);
                }
            }
        });
        header.addView(mBtnDelete);

        // Round close button.
        Button btnClose = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
        btnClose.setText("X");
        btnClose.setTextColor(Color.WHITE);
        btnClose.setTextSize(13);
        btnClose.setBackground(getRoundCloseBackground());
        btnClose.setMinWidth(0);
        btnClose.setMinHeight(0);
        btnClose.setPadding(0, 0, 0, 0);
        LinearLayout.LayoutParams closeLp = new LinearLayout.LayoutParams(dp(28), dp(28));
        btnClose.setLayoutParams(closeLp);
        btnClose.setOnClickListener(v -> {
            if (mCallback != null) mCallback.onCloseRequested();
        });
        header.addView(btnClose);

        // Header drag listener with screen bounds clamping
        header.setOnTouchListener((v, event) -> {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    mDragStartX = event.getRawX() - getTranslationX();
                    mDragStartY = event.getRawY() - getTranslationY();
                    return true;
                case MotionEvent.ACTION_MOVE:
                    float targetX = event.getRawX() - mDragStartX;
                    float targetY = event.getRawY() - mDragStartY;
                    View p = (View) getParent();
                    if (p != null && p.getWidth() > 0 && p.getHeight() > 0) {
                        float maxX = Math.max(0, p.getWidth() - getWidth());
                        float maxY = Math.max(0, p.getHeight() - getHeight());
                        targetX = Math.max(0, Math.min(maxX, targetX));
                        targetY = Math.max(0, Math.min(maxY, targetY));
                    }
                    setTranslationX(targetX);
                    setTranslationY(targetY);
                    return true;
            }
            return false;
        });
        addView(header);

        mScrollView = new ScrollView(getContext());
        mScrollView.setVerticalScrollBarEnabled(true);
        mScrollView.setScrollbarFadingEnabled(true);
        mScrollView.setScrollBarStyle(View.SCROLLBARS_INSIDE_OVERLAY);
        LinearLayout content = new LinearLayout(getContext());
        content.setOrientation(VERTICAL);
        content.setPadding(dp(16), dp(8), dp(16), dp(4));

        mTitleText = new TextView(getContext());
        mTitleText.setText(getContext().getString(R.string.hud_title_button_properties));
        mTitleText.setTextColor(Color.WHITE);
        mTitleText.setTextSize(14);
        mTitleText.setTypeface(Typeface.DEFAULT_BOLD);
        content.addView(mTitleText);

        // Label input
        content.addView(createLabelValueRow(getString(R.string.hud_display_label),
                null, mLabelInput = createLabelEditText()));
        mLabelInput.setTextColor(Color.WHITE);
        mLabelInput.setTextSize(13);
        mLabelInput.setBackgroundColor(0x22FFFFFF);
        mLabelInput.setPadding(dp(8), dp(6), dp(8), dp(6));
        mLabelInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) {
                if (mIsProgrammaticChange) return;
                if (mActiveButton != null && !s.toString().equals(mActiveButton.label)) {
                    mActiveButton.label = s.toString();
                    if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                }
            }
        });
        content.addView(createQuickIconSelector());

        // Sliders with exact Numeric Edit Fields. All four live inside a single
        // container so we can hide the whole block for dock items (where
        // size/corner/opacity are fixed by the strip layout) without tearing
        // down the views underneath.
        mSizeSection = new LinearLayout(getContext());
        mSizeSection.setOrientation(VERTICAL);

        mWidthSeekBar = new SeekBar(getContext());
        mWidthInput = createExactNumberInput();
        mSizeSection.addView(createSliderRow(getString(R.string.hud_width_dp), mWidthSeekBar, mWidthInput, SIZE_MIN_DP, SIZE_MAX_DP, val -> {
            if (mActiveButton != null) {
                mActiveButton.widthDp = val;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        }));

        mHeightSeekBar = new SeekBar(getContext());
        mHeightInput = createExactNumberInput();
        mSizeSection.addView(createSliderRow(getString(R.string.hud_height_dp), mHeightSeekBar, mHeightInput, SIZE_MIN_DP, SIZE_MAX_DP, val -> {
            if (mActiveButton != null) {
                mActiveButton.heightDp = val;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        }));

        mCornerSeekBar = new SeekBar(getContext());
        mCornerInput = createExactNumberInput();
        mSizeSection.addView(createSliderRow(getString(R.string.hud_corner_dp), mCornerSeekBar, mCornerInput, 0, 50, val -> {
            if (mActiveButton != null) {
                mActiveButton.cornerRadiusDp = val;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        }));

        mOpacitySeekBar = new SeekBar(getContext());
        mOpacityInput = createExactNumberInput();
        mSizeSection.addView(createSliderRow(getString(R.string.hud_opacity_percent), mOpacitySeekBar, mOpacityInput, 10, 100, val -> {
            if (mActiveButton != null) {
                mActiveButton.opacity = val / 100f;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        }));
        mSizeSection.addView(createNudgeControls());
        content.addView(mSizeSection);

        // Action Assignment rendered as a left-right row: the "Action" label
        // on the left, a badge button on the right showing the currently
        // bound key / combo. Long-press clears the slot.
        content.addView(createSectionLabel(getString(R.string.hud_action_mapping)));
        mBtnPickMainAction = createBadgeButton(formatMainActionLabel(null));
        mBtnPickMainAction.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) mCallback.onPickActionRequested(mActiveButton, 0);
        });
        mBtnPickMainAction.setOnLongClickListener(v -> {
            if (mActiveButton != null) {
                mActiveButton.action = new HudAction();
                mBtnPickMainAction.setText(unassignedLabel());
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                return true;
            }
            return false;
        });
        mMainActionRow = createLabelValueRow(getString(R.string.hud_main_action), null, mBtnPickMainAction);
        content.addView(mMainActionRow);

        // Hold-to-repeat: replaces the legacy extra-keys bar behaviour where
        // arrows / backspace / page keys kept firing while held.
        mRepeatSwitch = new Switch(getContext());
        mRepeatSwitch.setText(R.string.hud_hold_to_repeat);
        mRepeatSwitch.setTextSize(13);
        mRepeatSwitch.setTextColor(Color.WHITE);
        mRepeatSwitch.setOnCheckedChangeListener((v, checked) -> {
            if (mIsProgrammaticChange || mActiveButton == null) return;
            if (mActiveButton.action == null) mActiveButton.action = new HudAction();
            mActiveButton.action.repeat = checked;
            if (mCallback != null) mCallback.onModelChanged(mActiveButton);
        });
        content.addView(mRepeatSwitch);

        mBtnPickPopupAction = createBadgeButton(unassignedLabel());
        mBtnPickPopupAction.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) mCallback.onPickActionRequested(mActiveButton, 1);
        });
        mBtnPickPopupAction.setOnLongClickListener(v -> {
            if (mActiveButton != null) {
                mActiveButton.popupAction = null;
                mBtnPickPopupAction.setText(unassignedLabel());
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                return true;
            }
            return false;
        });
        mPopupActionRow = createLabelValueRow(getString(R.string.hud_popup_swipe_up), null, mBtnPickPopupAction);
        content.addView(mPopupActionRow);

        // Super Gesture Specific Options
        mSuperGestureOptions = new LinearLayout(getContext());
        mSuperGestureOptions.setOrientation(VERTICAL);
        mSuperGestureOptions.setPadding(0, dp(6), 0, 0);

        mSuperGestureOptions.addView(createSectionLabel(getString(R.string.hud_swipe_actions)));
        mBtnPickSwipeLeft = createBadgeButton(unassignedLabel());
        mBtnPickSwipeLeft.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) mCallback.onPickActionRequested(mActiveButton, 2);
        });
        mBtnPickSwipeLeft.setOnLongClickListener(v -> {
            if (mActiveButton != null) {
                mActiveButton.setDirectionalAction(2, null);
                mBtnPickSwipeLeft.setText(unassignedLabel());
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                return true;
            }
            return false;
        });
        mSuperGestureOptions.addView(createLabelValueRow(getString(R.string.hud_direction_left), null, mBtnPickSwipeLeft));

        mBtnPickSwipeRight = createBadgeButton(unassignedLabel());
        mBtnPickSwipeRight.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) mCallback.onPickActionRequested(mActiveButton, 3);
        });
        mBtnPickSwipeRight.setOnLongClickListener(v -> {
            if (mActiveButton != null) {
                mActiveButton.setDirectionalAction(3, null);
                mBtnPickSwipeRight.setText(unassignedLabel());
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                return true;
            }
            return false;
        });
        mSuperGestureOptions.addView(createLabelValueRow(getString(R.string.hud_direction_right), null, mBtnPickSwipeRight));

        mBtnPickSwipeUp = createBadgeButton(unassignedLabel());
        mBtnPickSwipeUp.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) mCallback.onPickActionRequested(mActiveButton, 4);
        });
        mBtnPickSwipeUp.setOnLongClickListener(v -> {
            if (mActiveButton != null) {
                mActiveButton.setDirectionalAction(4, null);
                mBtnPickSwipeUp.setText(unassignedLabel());
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                return true;
            }
            return false;
        });
        mSuperGestureOptions.addView(createLabelValueRow(getString(R.string.hud_direction_up), null, mBtnPickSwipeUp));

        mBtnPickSwipeDown = createBadgeButton(unassignedLabel());
        mBtnPickSwipeDown.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) mCallback.onPickActionRequested(mActiveButton, 5);
        });
        mBtnPickSwipeDown.setOnLongClickListener(v -> {
            if (mActiveButton != null) {
                mActiveButton.setDirectionalAction(5, null);
                mBtnPickSwipeDown.setText(unassignedLabel());
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                return true;
            }
            return false;
        });
        mSuperGestureOptions.addView(createLabelValueRow(getString(R.string.hud_direction_down), null, mBtnPickSwipeDown));
        content.addView(mSuperGestureOptions);

        // TrackPoint specific options: Mode toggle + Sensitivity & Deadzone sliders
        mTrackPointOptions = new LinearLayout(getContext());
        mTrackPointOptions.setOrientation(VERTICAL);
        mTrackPointOptions.setPadding(0, dp(6), 0, 0);
        mTrackPointOptions.addView(createSectionLabel(getString(R.string.hud_trackpoint_mode)));
        mBtnTrackPointMode = createBadgeButton(getString(R.string.hud_mode_mouse));
        mBtnTrackPointMode.setOnClickListener(v -> {
            if (mActiveButton == null) return;
            if (HudButton.WIDGET_SCROLLBAR.equals(mActiveButton.widgetType)) {
                boolean wasVert = !"horizontal".equalsIgnoreCase(mActiveButton.trackpointMode);
                mActiveButton.trackpointMode = wasVert ? "horizontal" : "vertical";
                int temp = mActiveButton.widthDp;
                mActiveButton.widthDp = mActiveButton.heightDp;
                mActiveButton.heightDp = temp;
                mBtnTrackPointMode.setText(trackpointModeLabel(mActiveButton.trackpointMode));
                syncSliderAndInput(mWidthSeekBar, mWidthInput, mActiveButton.widthDp, 20, 300);
                syncSliderAndInput(mHeightSeekBar, mHeightInput, mActiveButton.heightDp, 20, 300);
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                return;
            }
            String cur = mActiveButton.trackpointMode != null ? mActiveButton.trackpointMode : HudButton.MODE_MOUSE;
            String next;
            if (HudButton.MODE_MOUSE.equals(cur)) next = HudButton.MODE_SCROLL;
            else next = HudButton.MODE_MOUSE;
            mActiveButton.trackpointMode = next;
            mBtnTrackPointMode.setText(trackpointModeLabel(next));
            if (mCallback != null) mCallback.onModelChanged(mActiveButton);
        });
        mTrackPointOptions.addView(createLabelValueRow(getString(R.string.hud_mode), null, mBtnTrackPointMode));

        mTrackSensitivitySeekBar = new SeekBar(getContext());
        mTrackSensitivityInput = createExactNumberInput();
        mTrackPointOptions.addView(createSliderRow(getString(R.string.hud_sensitivity_x10), mTrackSensitivitySeekBar, mTrackSensitivityInput, 5, 30, val -> {
            if (mActiveButton != null) {
                mActiveButton.trackpointSensitivity = val / 10f;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        }));

        mTrackDeadzoneSeekBar = new SeekBar(getContext());
        mTrackDeadzoneInput = createExactNumberInput();
        mTrackPointOptions.addView(createSliderRow(getString(R.string.hud_deadzone_dp), mTrackDeadzoneSeekBar, mTrackDeadzoneInput, 0, 16, val -> {
            if (mActiveButton != null) {
                mActiveButton.trackpointDeadzoneDp = val;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        }));
        content.addView(mTrackPointOptions);

        // Group container options: add / rebind / remove child keys.
        mGroupOptions = new LinearLayout(getContext());
        mGroupOptions.setOrientation(VERTICAL);
        mGroupOptions.setPadding(0, dp(6), 0, 0);
        mGroupOptions.addView(createSectionLabel(getString(R.string.hud_group_children)));

        Button addChild = createBadgeButton(getString(R.string.hud_group_add_key));
        addChild.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) mCallback.onGroupAddKeyRequested(mActiveButton);
        });
        // createBadgeButton() is sized for horizontal rows (width 0 + weight 1),
        // so it needs explicit full-width params inside this vertical container.
        mGroupOptions.addView(addChild, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        // Second way to fill a group: move a key that is already on the canvas
        // into it, without having to aim a drag at the container.
        Button addExisting = createBadgeButton(getString(R.string.hud_group_add_existing));
        addExisting.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) mCallback.onGroupAddExistingRequested(mActiveButton);
        });
        LinearLayout.LayoutParams existingLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        existingLp.topMargin = dp(4);
        mGroupOptions.addView(addExisting, existingLp);

        mGroupChildList = new LinearLayout(getContext());
        mGroupChildList.setOrientation(VERTICAL);
        mGroupOptions.addView(mGroupChildList);
        content.addView(mGroupOptions);

        // Color Theme Presets
        content.addView(createSectionLabel(getString(R.string.hud_color_theme)));
        mColorThemeRow = (LinearLayout) createColorPresetsRow();
        content.addView(mColorThemeRow);

        // Per-button Haptic Feedback toggle
        mHapticSwitch = new android.widget.Switch(getContext());
        M3.styleSwitch(mHapticSwitch);
        mHapticSwitch.setText(getString(R.string.hud_haptic_feedback));
        mHapticSwitch.setTextColor(0xFFCCCCCC);
        mHapticSwitch.setTextSize(android.util.TypedValue.COMPLEX_UNIT_DIP, 12);
        mHapticSwitch.setPadding(0, dp(6), 0, dp(6));
        mHapticSwitch.setOnCheckedChangeListener((v, checked) -> {
            if (mIsProgrammaticChange) return;
            if (mActiveButton != null) {
                mActiveButton.hapticEnabled = checked;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        });
        content.addView(mHapticSwitch);

        mScrollView.addView(content);
        addView(mScrollView, new LayoutParams(dp(280), dp(320)));
    }

    public void adaptSize(int parentW, int parentH) {
        if (parentW <= 0 || parentH <= 0 || mScrollView == null) return;
        boolean isLandscape = parentW > parentH;
        int targetH = isLandscape ? Math.min(dp(220), parentH - dp(90)) : dp(320);
        int targetW = isLandscape ? dp(310) : dp(280);

        ViewGroup.LayoutParams lp = mScrollView.getLayoutParams();
        if (lp != null) {
            lp.width = targetW;
            lp.height = targetH;
            mScrollView.setLayoutParams(lp);
        }
    }

    public void bindButton(HudButton b) {
        this.mActiveButton = b;
        mResetDeleteRunnable.run();
        if (b == null) {
            setVisibility(GONE);
            return;
        }
        setVisibility(VISIBLE);
        View p = (View) getParent();
        if (p != null && p.getWidth() > 0 && p.getHeight() > 0) {
            adaptSize(p.getWidth(), p.getHeight());
        }
        mTitleText.setText(getString(R.string.hud_properties_title, widgetTitle(b.widgetType)));
        // Setting label text would otherwise re-trigger the TextWatcher and
        // call back into onModelChanged -> rebuildActiveLayout -> bindButton
        // in a tight loop. The flag is reset by syncSliderAndInput too, so
        // both blocks share the same suppression window.
        mIsProgrammaticChange = true;
        try {
            mLabelInput.setText(b.label != null ? b.label : "");
        } finally {
            mIsProgrammaticChange = false;
        }

        // The width / height range depends on the widget: a group container
        // may grow to GROUP_SIZE_MAX_DP so a big key grid still fits.
        int sizeMax = sizeMaxDp(b);
        mWidthSeekBar.setMax(sizeMax - SIZE_MIN_DP);
        mHeightSeekBar.setMax(sizeMax - SIZE_MIN_DP);
        syncSliderAndInput(mWidthSeekBar, mWidthInput, b.widthDp, SIZE_MIN_DP, sizeMax);
        syncSliderAndInput(mHeightSeekBar, mHeightInput, b.heightDp, SIZE_MIN_DP, sizeMax);
        syncSliderAndInput(mCornerSeekBar, mCornerInput, b.cornerRadiusDp, 0, 50);
        syncSliderAndInput(mOpacitySeekBar, mOpacityInput, Math.round(b.opacity * 100), 10, 100);

        // Update the badges with the live values from the model.
        mMainActionRow.setVisibility(HudButton.WIDGET_DPAD.equals(b.widgetType) ? GONE : VISIBLE);
        mRepeatSwitch.setVisibility(HudButton.WIDGET_DPAD.equals(b.widgetType) ? GONE : VISIBLE);
        mBtnPickMainAction.setText(formatMainActionLabel(b));

        if (mRepeatSwitch != null) {
            mIsProgrammaticChange = true;
            try {
                boolean canRepeat = b.action != null
                        && !HudAction.TYPE_MODIFIER.equals(b.action.type);
                mRepeatSwitch.setEnabled(canRepeat);
                mRepeatSwitch.setChecked(canRepeat && b.action.repeat);
            } finally {
                mIsProgrammaticChange = false;
            }
        }

        boolean isStandardBtn = HudButton.WIDGET_STANDARD.equals(b.widgetType);
        if (mPopupActionRow != null) {
            mPopupActionRow.setVisibility(isStandardBtn ? VISIBLE : GONE);
            if (isStandardBtn) {
                mBtnPickPopupAction.setText(formatActionBadge(b.popupAction));
            }
        }

        if (HudButton.WIDGET_SUPER_GESTURE.equals(b.widgetType)
                || HudButton.WIDGET_DPAD.equals(b.widgetType)) {
            mSuperGestureOptions.setVisibility(VISIBLE);
            mBtnPickSwipeLeft.setText(formatActionBadge(b.directionalAction(2)));
            mBtnPickSwipeRight.setText(formatActionBadge(b.directionalAction(3)));
            mBtnPickSwipeUp.setText(formatActionBadge(b.directionalAction(4)));
            mBtnPickSwipeDown.setText(formatActionBadge(b.directionalAction(5)));
        } else {
            mSuperGestureOptions.setVisibility(GONE);
        }
        if (mGroupOptions != null) {
            boolean isGroup = HudButton.WIDGET_GROUP.equals(b.widgetType);
            mGroupOptions.setVisibility(isGroup ? VISIBLE : GONE);
            if (isGroup) populateGroupChildren(b);
        }

        boolean isTrackpoint = HudButton.WIDGET_TRACKPOINT.equals(b.widgetType);
        boolean isTouchpad = HudButton.WIDGET_TOUCHPAD.equals(b.widgetType);
        boolean isScrollBar = HudButton.WIDGET_SCROLLBAR.equals(b.widgetType);
        if (mTrackPointOptions != null) {
            mTrackPointOptions.setVisibility((isTrackpoint || isTouchpad || isScrollBar) ? VISIBLE : GONE);
            if (isTrackpoint || isTouchpad || isScrollBar) {
                mBtnTrackPointMode.setVisibility(isTouchpad ? GONE : VISIBLE);
                if (isScrollBar) {
                    boolean isVert = !"horizontal".equalsIgnoreCase(b.trackpointMode);
                    mBtnTrackPointMode.setText(getString(isVert ? R.string.hud_orientation_vertical : R.string.hud_orientation_horizontal));
                } else {
                    String mode = b.trackpointMode != null ? b.trackpointMode : HudButton.MODE_MOUSE;
                    mBtnTrackPointMode.setText(trackpointModeLabel(mode));
                }
                syncSliderAndInput(mTrackSensitivitySeekBar, mTrackSensitivityInput, Math.round(b.trackpointSensitivity * 10), 5, 30);
                syncSliderAndInput(mTrackDeadzoneSeekBar, mTrackDeadzoneInput, b.trackpointDeadzoneDp, 0, 16);
            }
        }
        // Floating buttons show size sliders, color presets, and header action buttons.
        if (mSizeSection != null) mSizeSection.setVisibility(VISIBLE);
        if (mColorThemeRow != null) mColorThemeRow.setVisibility(VISIBLE);
        if (mBtnDelete != null) mBtnDelete.setVisibility(VISIBLE);
        if (mBtnDuplicate != null) mBtnDuplicate.setVisibility(VISIBLE);
        // Sync haptic toggle
        if (mHapticSwitch != null) {
            mIsProgrammaticChange = true;
            try { mHapticSwitch.setChecked(b.hapticEnabled); } finally { mIsProgrammaticChange = false; }
            mHapticSwitch.setVisibility(VISIBLE);
        }
    }

    // Slim inspector for a dock strip button: only the display label and the
    // action mapping matter, because dock items share a fixed size/opacity set
    // by the strip layout and cannot be deleted/duplicated from the strip.
    public void bindDockItem(HudButton b) {
        this.mActiveButton = b;
        mResetDeleteRunnable.run();
        if (b == null) {
            setVisibility(GONE);
            return;
        }
        setVisibility(VISIBLE);
        mTitleText.setText(getString(R.string.hud_dock_button_properties));
        mIsProgrammaticChange = true;
        try {
            mLabelInput.setText(b.label != null ? b.label : "");
        } finally {
            mIsProgrammaticChange = false;
        }
        mMainActionRow.setVisibility(HudButton.WIDGET_DPAD.equals(b.widgetType) ? GONE : VISIBLE);
        mRepeatSwitch.setVisibility(HudButton.WIDGET_DPAD.equals(b.widgetType) ? GONE : VISIBLE);
        mBtnPickMainAction.setText(formatMainActionLabel(b));
        if (mPopupActionRow != null) mPopupActionRow.setVisibility(GONE);
        mSuperGestureOptions.setVisibility(GONE);
        if (mTrackPointOptions != null) mTrackPointOptions.setVisibility(GONE);
        if (mSizeSection != null) mSizeSection.setVisibility(GONE);
        if (mColorThemeRow != null) mColorThemeRow.setVisibility(GONE);
        if (mBtnDelete != null) mBtnDelete.setVisibility(GONE);
        if (mBtnDuplicate != null) mBtnDuplicate.setVisibility(GONE);
    }

    private View createColorPresetsRow() {
        LinearLayout container = new LinearLayout(getContext());
        container.setOrientation(VERTICAL);
        container.setPadding(0, dp(4), 0, dp(4));

        int[][] themes = {
            {0xE62A2B3D, 0xFF80DEEA, 0xFF80DEEA}, // Slate / Cyan
            {0xE61A3B4D, 0xFF00E5FF, 0xFF00E5FF}, // Cyber Cyan
            {0xE64A202A, 0xFFFF5252, 0xFFFF5252}, // Crimson Alert
            {0xE64A3820, 0xFFFFAB40, 0xFFFFAB40}, // Amber Warning
            {0xE61E3D2F, 0xFF69F0AE, 0xFF69F0AE}, // Emerald
            {0xE614141E, 0xFFCCCCCC, 0xFF888888}, // Stealth Dark
        };
        String[] themeNames = getResources().getStringArray(R.array.hud_color_theme_names);

        for (int rowIdx = 0; rowIdx < 2; rowIdx++) {
            LinearLayout row = new LinearLayout(getContext());
            row.setOrientation(HORIZONTAL);
            LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            if (rowIdx > 0) rowLp.topMargin = dp(6);
            row.setLayoutParams(rowLp);

            for (int col = 0; col < 3; col++) {
                int i = rowIdx * 3 + col;
                final int bg = themes[i][0];
                final int active = themes[i][1];
                final int accent = themes[i][2];
                Button chip = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
                chip.setText(themeNames[i]);
                chip.setTextSize(11);
                chip.setAllCaps(true);
                chip.setTextColor(accent);
                android.graphics.drawable.GradientDrawable chipBg = new android.graphics.drawable.GradientDrawable();
                chipBg.setCornerRadius(dp(8));
                chipBg.setColor(bg);
                chipBg.setStroke(dp(1), accent);
                chip.setBackground(chipBg);
                chip.setPadding(dp(4), dp(6), dp(4), dp(6));
                chip.setMinHeight(dp(34));
                chip.setMinWidth(0);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
                if (col > 0) lp.leftMargin = dp(6);
                chip.setLayoutParams(lp);
                chip.setOnClickListener(v -> {
                    if (mActiveButton != null) {
                        mActiveButton.bgColor = bg;
                        mActiveButton.activeColor = active;
                        if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                    }
                });
                row.addView(chip);
            }
            container.addView(row);
        }
        return container;
    }

    private interface ValueConsumer {
        void accept(int val);
    }

    public static void styleSeekBar(SeekBar bar, int activeColor) {
        M3.styleSeekBar(bar, activeColor);
    }

    private View createSliderRow(String title, SeekBar bar, EditText input, int min, int max, ValueConsumer consumer) {
        styleSeekBar(bar, 0xFF80DEEA);
        bar.setPadding(dp(12), dp(6), dp(12), dp(6));
        bar.setThumbOffset(dp(0));
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(VERTICAL);
        row.setPadding(0, dp(4), 0, dp(4));

        LinearLayout top = new LinearLayout(getContext());
        top.setOrientation(HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);

        TextView t = new TextView(getContext());
        t.setText(title);
        t.setTextColor(0xFFCCCCCC);
        t.setTextSize(12);
        t.setLayoutParams(new LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        top.addView(t);
        top.addView(input);
        row.addView(top);

        bar.setMax(max - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (fromUser) {
                    int val = min + progress;
                    input.setText(String.valueOf(val));
                    consumer.accept(val);
                }
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        input.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) {
                if (mIsProgrammaticChange) return;
                try {
                    int val = Integer.parseInt(s.toString());
                    // Clamp against the seek bar's current ceiling instead of
                    // the one captured at creation time, so widgets with a
                    // larger range (group containers) accept bigger numbers.
                    val = Math.max(min, Math.min(bar.getMax() + min, val));
                    bar.setProgress(val - min);
                    consumer.accept(val);
                } catch (Exception ignored) {}
            }
        });

        row.addView(bar);
        return row;
    }

    private void syncSliderAndInput(SeekBar bar, EditText input, int value, int min, int max) {
        int clamped = Math.max(min, Math.min(max, value));
        // Suppress the per-EditText / per-SeekBar listeners so they do not
        // re-enter the callback chain (the model already carries the same
        // value we are writing back to the views).
        mIsProgrammaticChange = true;
        try {
            bar.setProgress(clamped - min);
            input.setText(String.valueOf(clamped));
        } finally {
            mIsProgrammaticChange = false;
        }
    }

    private EditText createExactNumberInput() {
        EditText et = new EditText(getContext());
        et.setInputType(InputType.TYPE_CLASS_NUMBER);
        et.setTextColor(0xFF80DEEA);
        et.setTextSize(12);
        et.setTypeface(Typeface.DEFAULT_BOLD);
        android.graphics.drawable.GradientDrawable numBg = new android.graphics.drawable.GradientDrawable();
        numBg.setCornerRadius(dp(6));
        numBg.setColor(0x22FFFFFF);
        numBg.setStroke(dp(1), 0x3380DEEA);
        et.setBackground(numBg);
        et.setGravity(Gravity.CENTER);
        et.setPadding(dp(4), dp(4), dp(4), dp(4));
        et.setLayoutParams(new LayoutParams(dp(44), ViewGroup.LayoutParams.WRAP_CONTENT));
        return et;
    }

    // Rounded badge-style button used for the action assignment rows.
    // Reads as "Key: SUPER" / "Type: COMBO" with a rounded background
    // that stands out against the dark panel. Text is left-aligned with
    // a 12dp padding-left so the label lines up with the title text
    // above it (Material Design body-text rhythm).
    private Button createBadgeButton(String label) {
        Button b = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
        b.setText(label);
        b.setTextColor(Color.WHITE);
        b.setTextSize(12);
        b.setAllCaps(false);
        android.graphics.drawable.GradientDrawable badgeBg = new android.graphics.drawable.GradientDrawable();
        badgeBg.setCornerRadius(dp(8));
        badgeBg.setColor(0xFF2A2B3D);
        badgeBg.setStroke(dp(1), 0x22FFFFFF);
        b.setBackground(badgeBg);
        // 12dp left padding + 6dp top/bottom + 12dp right padding keeps the
        // text left-aligned and the badge readable; gravity START so the
        // text always starts at the padding-left edge.
        b.setPadding(dp(12), dp(6), dp(12), dp(6));
        b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        b.setSingleLine(true);
        b.setEllipsize(android.text.TextUtils.TruncateAt.END);
        LayoutParams lp = new LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.weight = 1f;
        lp.setMargins(dp(8), dp(3), 0, dp(3));
        b.setLayoutParams(lp);
        return b;
    }

    private String unassignedLabel() {
        return getString(R.string.hud_unassigned);
    }

    // Build a left-aligned "Label - Value (Badge/EditText)" row. The label
    // sits in a fixed 110dp column on the left, so the right-hand value
    // column always starts at the same X position regardless of the label
    // text width. The right child (or the single EditText) stretches to
    // fill the rest of the row width.
    private LinearLayout createLabelValueRow(String label, View leftItem, View rightItem) {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(4), 0, dp(4));

        // Fixed 92dp label column so the right-hand value / edit text has ample width
        TextView l = new TextView(getContext());
        l.setText(label);
        l.setTextColor(0xFFCCCCCC);
        l.setTextSize(12);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                dp(92), LinearLayout.LayoutParams.WRAP_CONTENT);
        l.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        row.addView(l, lp);

        if (leftItem != null) {
            LinearLayout.LayoutParams li = new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
            li.leftMargin = dp(8);
            row.addView(leftItem, li);
        }
        if (rightItem != null) {
            LinearLayout.LayoutParams ri = new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
            ri.leftMargin = dp(8);
            row.addView(rightItem, ri);
        }
        return row;
    }

    private EditText createLabelEditText() {
        EditText et = new EditText(getContext());
        et.setTextColor(Color.WHITE);
        et.setTextSize(13);
        android.graphics.drawable.GradientDrawable lblBg = new android.graphics.drawable.GradientDrawable();
        lblBg.setCornerRadius(dp(8));
        lblBg.setColor(0x22FFFFFF);
        lblBg.setStroke(dp(1), 0x33FFFFFF);
        et.setBackground(lblBg);
        et.setPadding(dp(10), dp(6), dp(10), dp(6));
        et.setSingleLine(true);
        et.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        return et;
    }

    // Programmatic round dark background used by the panel's close button.
    // Keeps the look consistent with the no-emoji rule (no Unicode glyphs
    // or material icon dependency).
    private android.graphics.drawable.Drawable getRoundCloseBackground() {
        android.graphics.drawable.GradientDrawable d = new android.graphics.drawable.GradientDrawable();
        d.setShape(android.graphics.drawable.GradientDrawable.OVAL);
        d.setColor(0x33FFFFFF);
        return d;
    }

    // Format a HudAction as a short human-readable string for the action badge.
    private String formatMainActionLabel(HudButton b) {
        if (b == null || b.action == null) return unassignedLabel();
        return formatActionBadge(b.action);
    }

    // Render the swipe-action badge as a pure value string: "SUPER + LEFT",
    // "Overview", "Unassigned". The direction token is implied by the
    // row's own left-hand label ("Left", "Right", "Up", "Down") so we do
    // not duplicate it inside the badge.
    private String formatActionBadge(HudAction a) {
        if (a == null) return unassignedLabel();
        switch (a.type) {
            case HudAction.TYPE_KEY:
                return a.code > 0 ? labelForEvdev(getContext(), a.code) : unassignedLabel();
            case HudAction.TYPE_MODIFIER:
                return labelForEvdev(getContext(), a.code);
            case HudAction.TYPE_COMBO:
                if (a.comboKeys == null || a.comboKeys.isEmpty())
                    return unassignedLabel();
                StringBuilder sb = new StringBuilder();
                for (int i = 0; i < a.comboKeys.size(); i++) {
                    if (i > 0) sb.append(" + ");
                    sb.append(labelForEvdev(getContext(), a.comboKeys.get(i)));
                }
                return sb.toString();
            case HudAction.TYPE_TEXT:
                return a.text != null && !a.text.isEmpty()
                        ? getString(R.string.hud_text_action, a.text) : unassignedLabel();
            case HudAction.TYPE_SYSTEM:
                return getString(R.string.hud_system_action, systemActionLabel(a.systemCommand));
            default:
                return unassignedLabel();
        }
    }

    /**
     * Map a Linux evdev scancode to a short human label. The
     * ComboBuilderView produces keycodes from this same map so the
     * badge text and the live preview stay consistent. Unknown codes
     * fall back to "Key 125" so the user is never left wondering what
     * the numeric value is.
     */
    public static String labelForEvdev(Context context, int code) {
        switch (code) {
            case 29: return "CTRL";
            case 56: return "ALT";
            case 125: return "SUPER";
            case 42: return "SHIFT";
            case 1: return "ESC";
            case 15: return "TAB";
            case 28: return "ENTER";
            case 14: return "BKSP";
            case 111: return "DEL";
            case 57: return "SPACE";
            case 102: return "HOME";
            case 107: return "END";
            case 103: return "UP";
            case 108: return "DOWN";
            case 105: return "LEFT";
            case 106: return "RIGHT";
            case 104: return "PGUP";
            case 109: return "PGDN";
            case 30: return "A";
            case 48: return "B";
            case 46: return "C";
            case 32: return "D";
            case 18: return "E";
            case 33: return "F";
            case 34: return "G";
            case 35: return "H";
            case 23: return "I";
            case 36: return "J";
            case 37: return "K";
            case 38: return "L";
            case 50: return "M";
            case 49: return "N";
            case 24: return "O";
            case 25: return "P";
            case 16: return "Q";
            case 19: return "R";
            case 31: return "S";
            case 20: return "T";
            case 22: return "U";
            case 47: return "V";
            case 17: return "W";
            case 45: return "X";
            case 21: return "Y";
            case 44: return "Z";
            case 11: return "0";
            case 2: return "1";
            case 3: return "2";
            case 4: return "3";
            case 5: return "4";
            case 6: return "5";
            case 7: return "6";
            case 8: return "7";
            case 9: return "8";
            case 10: return "9";
            case 59: return "F1";
            case 60: return "F2";
            case 61: return "F3";
            case 62: return "F4";
            case 63: return "F5";
            case 64: return "F6";
            case 65: return "F7";
            case 66: return "F8";
            case 67: return "F9";
            case 68: return "F10";
            case 87: return "F11";
            case 88: return "F12";
            case 12: return "-";
            case 13: return "=";
            case 26: return "[";
            case 27: return "]";
            case 43: return "\\";
            case 39: return ";";
            case 40: return "'";
            case 51: return ",";
            case 52: return ".";
            case 53: return "/";
            case 41: return "`";
            default: return context.getString(R.string.hud_key_code, code);
        }
    }

    private String trackpointModeLabel(String mode) {
        if (HudButton.MODE_MOUSE.equals(mode)) return getString(R.string.hud_mode_mouse);
        if (HudButton.MODE_SCROLL.equals(mode)) return getString(R.string.hud_mode_scroll);
        return mode != null ? mode.toUpperCase().replace('_', ' ') : getString(R.string.hud_mode_mouse);
    }

    /** Rebuild the "child keys" rows for a group container. */
    private void populateGroupChildren(HudButton group) {
        if (mGroupChildList == null) return;
        mGroupChildList.removeAllViews();
        if (group.children == null || group.children.isEmpty()) {
            TextView empty = new TextView(getContext());
            empty.setText(R.string.hud_group_empty);
            empty.setTextColor(0xFF888888);
            empty.setTextSize(11);
            empty.setPadding(0, dp(4), 0, dp(4));
            mGroupChildList.addView(empty);
            return;
        }
        for (HudButton child : group.children) {
            LinearLayout row = new LinearLayout(getContext());
            row.setOrientation(HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);

            Button label = createBadgeButton(formatActionBadge(child.action));
            label.setOnClickListener(v -> {
                if (mCallback != null) mCallback.onGroupChildRebindRequested(group, child);
            });
            row.addView(label, new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

            Button detach = createBadgeButton(getString(R.string.hud_group_detach));
            detach.setOnClickListener(v -> {
                if (mCallback != null) mCallback.onGroupChildDetachRequested(group, child);
            });
            LinearLayout.LayoutParams dlp =
                    new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                            LinearLayout.LayoutParams.WRAP_CONTENT);
            dlp.leftMargin = dp(6);
            row.addView(detach, dlp);

            Button remove = createBadgeButton(getString(R.string.hud_delete_short));
            remove.setOnClickListener(v -> {
                if (mCallback != null) mCallback.onGroupChildRemoveRequested(group, child);
            });
            LinearLayout.LayoutParams rlp =
                    new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                            LinearLayout.LayoutParams.WRAP_CONTENT);
            rlp.leftMargin = dp(4);
            row.addView(remove, rlp);

            mGroupChildList.addView(row);
        }
    }

    private String widgetTitle(String widgetType) {
        if (HudButton.WIDGET_STANDARD.equals(widgetType)) return getString(R.string.hud_widget_button);
        if (HudButton.WIDGET_SUPER_GESTURE.equals(widgetType)) return getString(R.string.hud_widget_super_gesture);
        if (HudButton.WIDGET_TRACKPOINT.equals(widgetType)) return getString(R.string.hud_widget_trackpoint);
        if (HudButton.WIDGET_TOUCHPAD.equals(widgetType)) return getString(R.string.hud_widget_touchpad);
        if (HudButton.WIDGET_SCROLLBAR.equals(widgetType)) return getString(R.string.hud_widget_scrollbar);
        if (HudButton.WIDGET_DPAD.equals(widgetType)) return getString(R.string.hud_widget_dpad);
        if (HudButton.WIDGET_GROUP.equals(widgetType)) return getString(R.string.hud_widget_group);
        return widgetType != null ? widgetType.replace('_', ' ').toUpperCase() : getString(R.string.hud_widget_button);
    }

    private String systemActionLabel(String command) {
        if ("toggle_ime".equals(command)) return getString(R.string.hud_system_soft_keyboard);
        if ("toggle_vk".equals(command)) return getString(R.string.hud_system_virtual_keyboard);
        if ("open_settings".equals(command)) return getString(R.string.hud_system_settings);
        if ("mouse_left".equals(command)) return getString(R.string.hud_system_mouse_left);
        if ("mouse_right".equals(command)) return getString(R.string.hud_system_mouse_right);
        if ("mouse_middle".equals(command)) return getString(R.string.hud_system_mouse_middle);
        if ("mouse_scroll_up".equals(command)) return getString(R.string.hud_system_scroll_up);
        if ("mouse_scroll_down".equals(command)) return getString(R.string.hud_system_scroll_down);
        if ("toggle_steam_deck".equals(command)) return getString(R.string.hud_system_steam_deck_mode);
        if ("exit_steam_deck".equals(command)) return getString(R.string.hud_system_exit_deck_mode);
        return command != null && !command.isEmpty() ? command : unassignedLabel();
    }

    private String getString(int resourceId) {
        return getContext().getString(resourceId);
    }

    private String getString(int resourceId, Object arg) {
        return getContext().getString(resourceId, arg);
    }

    private View createQuickIconSelector() {
        HorizontalScrollView hsv = new HorizontalScrollView(getContext());
        hsv.setHorizontalScrollBarEnabled(false);
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(HORIZONTAL);
        row.setPadding(0, dp(2), 0, dp(6));

        final String[] icons = new String[]{
                "▲", "▼", "◀", "▶", "⧉", "☰", "⚙", "🖱",
                "↵", "⌫", "⇥", "⎋", "⌥", "⌘", "⇧", "␣", "ESC", "TAB", "CTRL", "ALT"
        };
        for (String ic : icons) {
            Button btn = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
            btn.setText(ic);
            btn.setTextSize(11f);
            btn.setTextColor(Color.WHITE);
            btn.setBackground(M3.createRippleDrawable(getContext(), M3.RADIUS_CHIP, 0x22FFFFFF, 0x44FFFFFF, M3.COLOR_BORDER_SUBTLE));
            btn.setPadding(dp(6), dp(2), dp(6), dp(2));
            btn.setMinWidth(dp(30));
            btn.setMinHeight(dp(26));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(28));
            lp.rightMargin = dp(4);
            btn.setLayoutParams(lp);
            btn.setOnClickListener(v -> {
                if (mActiveButton != null) {
                    mActiveButton.label = ic;
                    mIsProgrammaticChange = true;
                    try { mLabelInput.setText(ic); } finally { mIsProgrammaticChange = false; }
                    if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                }
            });
            row.addView(btn);
        }
        hsv.addView(row);
        return hsv;
    }

    private View createNudgeControls() {
        LinearLayout container = new LinearLayout(getContext());
        container.setOrientation(VERTICAL);
        container.setPadding(0, dp(4), 0, dp(4));

        TextView lbl = createSectionLabel(getString(R.string.hud_nudge_position));
        container.addView(lbl);

        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        String[] arrows = new String[]{"◀", "▲", "▼", "▶"};
        float[] dx = new float[]{-0.005f, 0f, 0f, 0.005f};
        float[] dy = new float[]{0f, -0.005f, 0.005f, 0f};

        for (int i = 0; i < 4; i++) {
            final float dX = dx[i];
            final float dY = dy[i];
            Button b = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
            b.setText(arrows[i]);
            b.setTextSize(12f);
            b.setTextColor(M3.COLOR_PRIMARY);
            b.setBackground(M3.createRippleDrawable(getContext(), M3.RADIUS_CHIP, 0x22FFFFFF, 0x44FFFFFF, M3.COLOR_BORDER_SUBTLE));
            b.setMinWidth(dp(44));
            b.setMinHeight(dp(30));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(30), 1f);
            if (i > 0) lp.leftMargin = dp(4);
            b.setLayoutParams(lp);
            b.setOnClickListener(v -> {
                if (mActiveButton != null) {
                    mActiveButton.posXPercent = Math.max(0.01f, Math.min(0.99f, mActiveButton.posXPercent + dX));
                    mActiveButton.posYPercent = Math.max(0.01f, Math.min(0.99f, mActiveButton.posYPercent + dY));
                    if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                }
            });
            row.addView(b);
        }
        container.addView(row);
        return container;
    }

    private TextView createSectionLabel(String text) {
        TextView t = new TextView(getContext());
        t.setText(text);
        t.setTextColor(0xFFAAAAAA);
        t.setTextSize(11);
        t.setPadding(0, dp(4), 0, dp(2));
        return t;
    }

    private int dp(int dpVal) {
        return Math.round(dpVal * getResources().getDisplayMetrics().density);
    }
}
