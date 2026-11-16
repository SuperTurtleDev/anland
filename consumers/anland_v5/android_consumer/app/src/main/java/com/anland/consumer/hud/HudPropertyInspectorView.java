package com.anland.consumer.hud;

import android.app.AlertDialog;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import com.anland.consumer.R;
import com.anland.consumer.theme.M3;

/**
 * Landscape Modal Property Inspector (Alight Motion Design Specification).
 * Sidebar tabs: Transform, Label, Event, Style.
 * Exact color palette: #484848 (body), #A9A9A9 (footer), #A39CB8 (active tab/pills).
 * Includes Zalith 8 Corner Shapes picker and real-time auto-ghosting (25% opacity on seek).
 */
public final class HudPropertyInspectorView extends LinearLayout {

    public interface InspectorCallback {
        void onModelChanged(HudButton button);
        void onDeleteRequested(HudButton button);
        void onDuplicateRequested(HudButton button);
        void onPickActionRequested(HudButton button, int targetSlot); // 0=main, 1=popup, 2..5=directions, 6=long press
        void onGroupAddKeyRequested(HudButton group);
        void onGroupAddExistingRequested(HudButton group);
        void onGroupChildRebindRequested(HudButton group, HudButton child);
        void onGroupChildDetachRequested(HudButton group, HudButton child);
        void onGroupChildRemoveRequested(HudButton group, HudButton child);
        void onCloseRequested();
        void onCancelRequested();
    }

    private static final int COLOR_BODY_BG = 0xFF484848;
    private static final int COLOR_FOOTER_BG = 0xFFA9A9A9;
    private static final int COLOR_TAB_ACTIVE = 0xFFA39CB8;
    private static final int COLOR_TEXT_MUTED = 0xFFC3C3CD;
    private static final int COLOR_BTN_GREY = 0xFF878787;
    private static final int COLOR_DIVIDER = 0xFF5F5F5F;

    private static final int SIZE_MAX_DP = 240;
    private static final int GROUP_SIZE_MAX_DP = 480;
    private static final int SIZE_MIN_DP = 20;

    private HudButton mActiveButton;
    private final InspectorCallback mCallback;

    private boolean mIsProgrammaticChange = false;
    private boolean mAspectRatioLocked = false;
    private boolean mIsPreviewing = false;

    // Sidebar Tab Buttons
    private LinearLayout mSidebarContainer;
    private TextView[] mTabButtons;
    private FrameLayout mTabContentContainer;
    private int mCurrentTab = 0; // 0=Transform, 1=Label, 2=Event, 3=Style

    // Tab Views
    private ScrollView mTabTransformView;
    private ScrollView mTabLabelView;
    private ScrollView mTabEventView;
    private ScrollView mTabStyleView;

    // Tab 1: Transform
    private View mWidthRow;
    private View mHeightRow;
    private SeekBar mWidthSeekBar;
    private TextView mWidthBadge;
    private SeekBar mHeightSeekBar;
    private TextView mHeightBadge;
    private View mCornerRow;
    private SeekBar mCornerSeekBar;
    private TextView mCornerBadge;
    private TextView mShapeTitle;
    private HudCornerShapePickerView mShapePickerView;

    // Tab 2: Label
    private View mLabelPillText;
    private View mLabelPillIcon;
    private LinearLayout mTextSectionContainer;
    private LinearLayout mIconSectionContainer;
    private EditText mLabelInput;
    private Button mBtnAlignLeft;
    private Button mBtnAlignCenter;
    private Button mBtnAlignRight;
    private LinearLayout mTextColorRow;

    // Tab 3: Event
    private View mSectionPrimaryAction;
    private View mSectionPopupAction;
    private View mSectionLongPressAction;
    private RadioGroup mSendModeGroup;
    private Button mBtnPickMainAction;
    private Button mBtnPickPopupAction;
    private Button mBtnPickLongPressAction;
    private RadioButton mSendImmediately;
    private RadioButton mSendOnRelease;
    private Switch mRepeatSwitch;
    private SeekBar mLongPressSeekBar;
    private TextView mLongPressBadge;
    private View mLongPressDelayRow;
    private LinearLayout mSuperGestureOptions;
    private Button mBtnPickSwipeLeft;
    private Button mBtnPickSwipeRight;
    private Button mBtnPickSwipeUp;
    private Button mBtnPickSwipeDown;
    private LinearLayout mTrackPointOptions;
    private Button mBtnTrackPointMode;
    private SeekBar mTrackSensitivitySeekBar;
    private TextView mTrackSensitivityBadge;
    private SeekBar mTrackDeadzoneSeekBar;
    private TextView mTrackDeadzoneBadge;
    private LinearLayout mGroupOptions;
    private LinearLayout mGroupChildList;

    // Tab 4: Style
    private SeekBar mOpacitySeekBar;
    private TextView mOpacityBadge;
    private SeekBar mBorderWidthSeekBar;
    private TextView mBorderWidthBadge;
    private Switch mHapticSwitch;
    private LinearLayout mColorThemeRow;
    private LinearLayout mStyleVariantSection;
    private LinearLayout mStyleVariantRow;

    // Drag tracking
    private float mDragStartX;
    private float mDragStartY;
    private int mUserPanelWidth;
    private int mUserPanelHeight;

    public HudPropertyInspectorView(Context context, InspectorCallback callback) {
        super(context);
        this.mCallback = callback;
        initView();
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static int sizeMaxDp(HudButton b) {
        return (b != null && HudButton.WIDGET_GROUP.equals(b.widgetType))
                ? GROUP_SIZE_MAX_DP : SIZE_MAX_DP;
    }

    private void initView() {
        setOrientation(VERTICAL);

        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(22));
        bg.setColor(COLOR_BODY_BG);
        setBackground(bg);
        setElevation(dp(12));
        setClickable(true);

        // Clamping to parent on layout
        addOnLayoutChangeListener((v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
            View parent = (View) getParent();
            if (parent == null) return;
            setTranslationX(Math.max(0, Math.min(getTranslationX(), parent.getWidth() - getWidth())));
            setTranslationY(Math.max(0, Math.min(getTranslationY(), parent.getHeight() - getHeight())));
        });

        // 1. Center Area: Horizontal Split (Sidebar 130dp | Divider | Tab Content)
        LinearLayout centerArea = new LinearLayout(getContext());
        centerArea.setOrientation(HORIZONTAL);
        centerArea.setLayoutParams(new LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f));

        // Drag to move on top-bar area
        centerArea.setOnTouchListener((v, event) -> {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    mDragStartX = event.getRawX() - getTranslationX();
                    mDragStartY = event.getRawY() - getTranslationY();
                    return false;
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
                    return false;
            }
            return false;
        });

        // Sidebar
        mSidebarContainer = new LinearLayout(getContext());
        mSidebarContainer.setOrientation(VERTICAL);
        mSidebarContainer.setPadding(dp(8), dp(16), dp(8), dp(14));
        mSidebarContainer.setLayoutParams(new LayoutParams(dp(106), LayoutParams.MATCH_PARENT));

        String[] tabs = new String[]{getContext().getString(R.string.hud_editor_transform), getContext().getString(R.string.hud_editor_label), getContext().getString(R.string.hud_editor_event), getContext().getString(R.string.hud_editor_style)};
        mTabButtons = new TextView[tabs.length];
        for (int i = 0; i < tabs.length; i++) {
            final int tabIndex = i;
            TextView tabBtn = new TextView(getContext());
            tabBtn.setText(tabs[i]);
            tabBtn.setTextSize(11.5f);
            tabBtn.setTypeface(Typeface.DEFAULT_BOLD);
            tabBtn.setGravity(Gravity.CENTER);
            tabBtn.setSingleLine(true);
            tabBtn.setIncludeFontPadding(false);
            tabBtn.setPadding(dp(4), dp(8), dp(4), dp(8));
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                    LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT);
            tlp.bottomMargin = dp(8);
            tabBtn.setLayoutParams(tlp);
            tabBtn.setOnClickListener(v -> selectTab(tabIndex));
            mSidebarContainer.addView(tabBtn);
            mTabButtons[i] = tabBtn;
        }
        centerArea.addView(mSidebarContainer);

        // Vertical Divider Line
        View divider = new View(getContext());
        divider.setBackgroundColor(COLOR_DIVIDER);
        centerArea.addView(divider, new LayoutParams(dp(1.5f), LayoutParams.MATCH_PARENT));

        // Tab Content Container
        mTabContentContainer = new FrameLayout(getContext());
        mTabContentContainer.setPadding(dp(10), dp(10), dp(10), dp(10));
        centerArea.addView(mTabContentContainer, new LayoutParams(0, LayoutParams.MATCH_PARENT, 1f));

        buildTabTransform();
        buildTabLabel();
        buildTabEvent();
        buildTabStyle();

        addView(centerArea);

        // 2. Bottom Footer Bar (#A9A9A9 background)
        LinearLayout footer = new LinearLayout(getContext());
        footer.setOrientation(HORIZONTAL);
        footer.setGravity(Gravity.CENTER_VERTICAL);
        footer.setPadding(dp(8), dp(8), dp(8), dp(8));
        GradientDrawable footerBg = new GradientDrawable();
        float r = dp(22);
        footerBg.setCornerRadii(new float[]{0, 0, 0, 0, r, r, r, r});
        footerBg.setColor(COLOR_FOOTER_BG);
        footer.setBackground(footerBg);

        // Left 2x2 Dots (Drag handle for the panel)
        Dots2x2View dotsView = new Dots2x2View(getContext());
        dotsView.setClickable(true);
        dotsView.setOnTouchListener(new OnTouchListener() {
            private float dX, dY;
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getAction()) {
                    case MotionEvent.ACTION_DOWN:
                        dX = getTranslationX() - event.getRawX();
                        dY = getTranslationY() - event.getRawY();
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        View p = (View) getParent();
                        if (p != null) {
                            float newX = event.getRawX() + dX;
                            float newY = event.getRawY() + dY;
                            newX = Math.max(0, Math.min(p.getWidth() - getWidth(), newX));
                            newY = Math.max(0, Math.min(p.getHeight() - getHeight(), newY));
                            setTranslationX(newX);
                            setTranslationY(newY);
                        }
                        return true;
                }
                return false;
            }
        });
        footer.addView(dotsView, new LayoutParams(dp(28), dp(28)));

        // Action Buttons: Preview (Purple, Hold to preview), Delete (Grey), Copy (Grey), Done (Purple)
        Button btnPreview = createFooterButton(getContext().getString(R.string.hud_editor_preview), COLOR_TAB_ACTIVE, Color.WHITE);
        btnPreview.setOnTouchListener((v, event) -> {
            switch (event.getAction()) {
                case MotionEvent.ACTION_DOWN:
                    showPreview(true);
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    showPreview(false);
                    return true;
            }
            return false;
        });
        footer.addView(btnPreview);

        Button btnDelete = createFooterButton(getContext().getString(R.string.hud_editor_delete), COLOR_BTN_GREY, Color.WHITE);
        btnDelete.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) {
                mCallback.onDeleteRequested(mActiveButton);
            }
        });
        footer.addView(btnDelete);

        Button btnCopy = createFooterButton(getContext().getString(R.string.hud_editor_copy), COLOR_BTN_GREY, Color.WHITE);
        btnCopy.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) {
                mCallback.onDuplicateRequested(mActiveButton);
            }
        });
        footer.addView(btnCopy);

        Button btnCancel = createFooterButton(getContext().getString(R.string.hud_overlay_cancel), COLOR_BTN_GREY, Color.WHITE);
        btnCancel.setOnClickListener(v -> {
            if (mCallback != null) mCallback.onCancelRequested();
        });
        footer.addView(btnCancel);

        Button btnDone = createFooterButton(getContext().getString(R.string.hud_editor_done), COLOR_TAB_ACTIVE, Color.WHITE);
        btnDone.setOnClickListener(v -> {
            if (mCallback != null) {
                mCallback.onCloseRequested();
            }
        });
        footer.addView(btnDone);

        TextView resizeHandle = new TextView(getContext());
        resizeHandle.setText("◢");
        resizeHandle.setTextSize(20);
        resizeHandle.setTextColor(Color.WHITE);
        resizeHandle.setGravity(Gravity.CENTER);
        resizeHandle.setContentDescription(getContext().getString(R.string.hud_resize_panel));
        resizeHandle.setOnTouchListener(new OnTouchListener() {
            private float startX, startY;
            private int startWidth, startHeight;

            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        startX = event.getRawX();
                        startY = event.getRawY();
                        startWidth = getWidth();
                        startHeight = getHeight();
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        View parent = (View) getParent();
                        if (parent == null || parent.getWidth() <= 0 || parent.getHeight() <= 0) return true;
                        mUserPanelWidth = Math.max(Math.min(dp(320), parent.getWidth()),
                                Math.min(parent.getWidth(), startWidth + Math.round(event.getRawX() - startX)));
                        mUserPanelHeight = Math.max(Math.min(dp(200), parent.getHeight()),
                                Math.min(parent.getHeight(), startHeight + Math.round(event.getRawY() - startY)));
                        adaptSize(parent.getWidth(), parent.getHeight());
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        return true;
                    default:
                        return true;
                }
            }
        });
        footer.addView(resizeHandle, new LayoutParams(dp(28), dp(32)));

        addView(footer, new LayoutParams(LayoutParams.MATCH_PARENT, dp(48)));

        selectTab(0);
    }

    public boolean isPreviewing() {
        return mIsPreviewing;
    }

    public void showPreview(boolean preview) {
        mIsPreviewing = preview;
        if (preview) {
            animate().alpha(0.0f).setDuration(120).withEndAction(() -> {
                if (mIsPreviewing) setVisibility(INVISIBLE);
            }).start();
        } else {
            setVisibility(VISIBLE);
            animate().alpha(1.0f).setDuration(120).start();
        }
    }

    public void exitPreview() {
        showPreview(false);
    }

    private Button createFooterButton(String text, int bgColor, int textColor) {
        Button b = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
        b.setText(text);
        b.setTextSize(11);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setTextColor(textColor);
        b.setAllCaps(false);
        b.setSingleLine(true);
        b.setAutoSizeTextTypeUniformWithConfiguration(8, 11, 1, android.util.TypedValue.COMPLEX_UNIT_SP);
        b.setMinWidth(0);
        b.setMinHeight(0);
        b.setPadding(dp(2), dp(6), dp(2), dp(6));

        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(14));
        bg.setColor(bgColor);
        b.setBackground(bg);

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, dp(32), 1f);
        lp.leftMargin = dp(2);
        b.setLayoutParams(lp);
        return b;
    }

    private void selectTab(int index) {
        mCurrentTab = index;
        for (int i = 0; i < mTabButtons.length; i++) {
            boolean active = (i == index);
            TextView tb = mTabButtons[i];
            GradientDrawable tabBg = new GradientDrawable();
            tabBg.setCornerRadius(dp(12));
            tabBg.setColor(active ? COLOR_TAB_ACTIVE : Color.TRANSPARENT);
            tb.setBackground(tabBg);
            tb.setTextColor(active ? Color.WHITE : COLOR_TEXT_MUTED);
        }

        mTabTransformView.setVisibility(index == 0 ? VISIBLE : GONE);
        mTabLabelView.setVisibility(index == 1 ? VISIBLE : GONE);
        mTabEventView.setVisibility(index == 2 ? VISIBLE : GONE);
        mTabStyleView.setVisibility(index == 3 ? VISIBLE : GONE);
    }

    // =========================================================================
    // Tab 1: Transform (Width, Height, Lock, Zalith 8 Corner Shapes)
    // =========================================================================
    private void buildTabTransform() {
        mTabTransformView = new ScrollView(getContext());
        styleScrollView(mTabTransformView);
        LinearLayout content = new LinearLayout(getContext());
        content.setOrientation(VERTICAL);
        content.setPadding(0, 0, dp(14), 0); // Shift content left to make room for scrollbar

        // Width Row
        mWidthSeekBar = new SeekBar(getContext());
        mWidthBadge = createBadgeTextView("75px ✎");
        mWidthRow = createSliderRow(getContext().getString(R.string.hud_editor_width), mWidthSeekBar, mWidthBadge, SIZE_MIN_DP, SIZE_MAX_DP, "px", val -> {
            if (mActiveButton != null) {
                mActiveButton.widthDp = val;
                if ((mAspectRatioLocked || HudButton.WIDGET_DPAD.equals(mActiveButton.widgetType)) && mHeightSeekBar != null) {
                    mActiveButton.heightDp = val;
                    syncSliderAndBadge(mHeightSeekBar, mHeightBadge, val, SIZE_MIN_DP, "px");
                }
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        });
        content.addView(mWidthRow);

        // Height Row
        mHeightSeekBar = new SeekBar(getContext());
        mHeightBadge = createBadgeTextView("75px ✎");
        mHeightRow = createSliderRow(getContext().getString(R.string.hud_editor_height), mHeightSeekBar, mHeightBadge, SIZE_MIN_DP, SIZE_MAX_DP, "px", val -> {
            if (mActiveButton != null) {
                mActiveButton.heightDp = val;
                if ((mAspectRatioLocked || HudButton.WIDGET_DPAD.equals(mActiveButton.widgetType)) && mWidthSeekBar != null) {
                    mActiveButton.widthDp = val;
                    syncSliderAndBadge(mWidthSeekBar, mWidthBadge, val, SIZE_MIN_DP, "px");
                }
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        });
        content.addView(mHeightRow);

        // Corner Radius Row
        mCornerSeekBar = new SeekBar(getContext());
        mCornerBadge = createBadgeTextView("8dp ✎");
        mCornerRow = createSliderRow(getContext().getString(R.string.hud_editor_corner_radius), mCornerSeekBar, mCornerBadge, 0, 50, "dp", val -> {
            if (mActiveButton != null) {
                mActiveButton.cornerRadiusDp = val;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        });
        content.addView(mCornerRow);

        // Zalith 8 Corner Shapes Matrix
        mShapeTitle = new TextView(getContext());
        mShapeTitle.setText(getContext().getString(R.string.hud_editor_corner_shape));
        mShapeTitle.setTextColor(Color.WHITE);
        mShapeTitle.setTextSize(12);
        mShapeTitle.setTypeface(Typeface.DEFAULT_BOLD);
        mShapeTitle.setPadding(0, dp(8), 0, dp(4));
        content.addView(mShapeTitle);

        mShapePickerView = new HudCornerShapePickerView(getContext());
        mShapePickerView.setOnShapeSelectedListener(shape -> {
            if (mActiveButton != null) {
                mActiveButton.cornerShape = shape;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        });
        content.addView(mShapePickerView);

        mTabTransformView.addView(content);
        mTabContentContainer.addView(mTabTransformView);
    }

    // =========================================================================
    // Tab 2: Label (Display Mode Switch, Text Options, Vector Icons)
    // =========================================================================
    private void buildTabLabel() {
        mTabLabelView = new ScrollView(getContext());
        styleScrollView(mTabLabelView);
        LinearLayout content = new LinearLayout(getContext());
        content.setOrientation(VERTICAL);
        content.setPadding(0, 0, dp(14), 0);

        // 1. Segmented Pill Switch: [ Aa Text ] | [ ❖ Icon ]
        LinearLayout pillSwitch = new LinearLayout(getContext());
        pillSwitch.setOrientation(HORIZONTAL);
        pillSwitch.setGravity(Gravity.CENTER);
        pillSwitch.setPadding(dp(4), dp(4), dp(4), dp(4));
        GradientDrawable swBg = new GradientDrawable();
        swBg.setCornerRadius(dp(16));
        swBg.setColor(0x33000000);
        swBg.setStroke(dp(1), 0x33FFFFFF);
        pillSwitch.setBackground(swBg);

        mLabelPillText = createSegmentedPill(getContext().getString(R.string.hud_editor_text_pill), true, () -> setLabelMode(false));
        mLabelPillIcon = createSegmentedPill(getContext().getString(R.string.hud_editor_icon_pill), false, () -> setLabelMode(true));
        pillSwitch.addView(mLabelPillText, new LinearLayout.LayoutParams(0, dp(30), 1f));
        pillSwitch.addView(mLabelPillIcon, new LinearLayout.LayoutParams(0, dp(30), 1f));
        content.addView(pillSwitch);

        // 2. TEXT SECTION CONTAINER
        mTextSectionContainer = new LinearLayout(getContext());
        mTextSectionContainer.setOrientation(VERTICAL);
        mTextSectionContainer.setPadding(0, dp(10), 0, 0);

        TextView labelTitle = new TextView(getContext());
        labelTitle.setText(R.string.hud_display_label);
        labelTitle.setTextColor(Color.WHITE);
        labelTitle.setTextSize(12);
        labelTitle.setTypeface(Typeface.DEFAULT_BOLD);
        mTextSectionContainer.addView(labelTitle);

        mLabelInput = new EditText(getContext());
        mLabelInput.setTextColor(Color.WHITE);
        mLabelInput.setTextSize(14);
        mLabelInput.setSingleLine(true);
        GradientDrawable lblBg = new GradientDrawable();
        lblBg.setCornerRadius(dp(8));
        lblBg.setColor(0x33000000);
        lblBg.setStroke(dp(1), 0x33FFFFFF);
        mLabelInput.setBackground(lblBg);
        mLabelInput.setPadding(dp(12), 0, dp(12), 0);
        LinearLayout.LayoutParams etlp = new LinearLayout.LayoutParams(
                LayoutParams.MATCH_PARENT, dp(36));
        etlp.topMargin = dp(6);
        etlp.bottomMargin = dp(10);
        mLabelInput.setLayoutParams(etlp);
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
        mTextSectionContainer.addView(mLabelInput);

        // Alignment row: Left | Center | Right
        mTextSectionContainer.addView(createSectionHeader(getContext().getString(R.string.hud_editor_text_alignment)));
        LinearLayout alignRow = new LinearLayout(getContext());
        alignRow.setOrientation(HORIZONTAL);
        alignRow.setPadding(0, dp(2), 0, dp(10));

        mBtnAlignLeft = createAlignButton(getContext().getString(R.string.hud_direction_left), HudButton.ALIGN_LEFT);
        mBtnAlignCenter = createAlignButton(getContext().getString(R.string.hud_editor_center), HudButton.ALIGN_CENTER);
        mBtnAlignRight = createAlignButton(getContext().getString(R.string.hud_direction_right), HudButton.ALIGN_RIGHT);
        alignRow.addView(mBtnAlignLeft, new LinearLayout.LayoutParams(0, dp(32), 1f));
        alignRow.addView(mBtnAlignCenter, new LinearLayout.LayoutParams(0, dp(32), 1f));
        alignRow.addView(mBtnAlignRight, new LinearLayout.LayoutParams(0, dp(32), 1f));
        mTextSectionContainer.addView(alignRow);

        // Text Color Presets
        mTextSectionContainer.addView(createSectionHeader(getContext().getString(R.string.hud_editor_text_color)));
        mTextColorRow = createTextColorPresetsRow();
        mTextSectionContainer.addView(mTextColorRow);

        content.addView(mTextSectionContainer);

        // 3. ICON SECTION CONTAINER
        mIconSectionContainer = new LinearLayout(getContext());
        mIconSectionContainer.setOrientation(VERTICAL);
        mIconSectionContainer.setPadding(0, dp(10), 0, 0);
        mIconSectionContainer.addView(createSectionHeader(getContext().getString(R.string.hud_editor_select_icon)));
        buildIconPresetsGrid(mIconSectionContainer);
        content.addView(mIconSectionContainer);

        mTabLabelView.addView(content);
        mTabContentContainer.addView(mTabLabelView);
    }

    private View createSegmentedPill(String title, boolean active, Runnable onClick) {
        TextView tv = new TextView(getContext());
        tv.setText(title);
        tv.setTextSize(11.5f);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setGravity(Gravity.CENTER);
        tv.setClickable(true);
        tv.setOnClickListener(v -> {
            if (onClick != null) onClick.run();
        });
        updateSegmentedPill(tv, active);
        return tv;
    }

    private void updateSegmentedPill(View view, boolean active) {
        if (!(view instanceof TextView)) return;
        TextView tv = (TextView) view;
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(13));
        bg.setColor(active ? COLOR_TAB_ACTIVE : Color.TRANSPARENT);
        tv.setBackground(bg);
        tv.setTextColor(active ? Color.WHITE : 0xAAFFFFFF);
    }

    private void setLabelMode(boolean isIcon) {
        if (mActiveButton == null) return;
        if (isIcon) {
            if (mActiveButton.iconId == null || HudVectorIcons.ICON_NONE.equals(mActiveButton.iconId) || mActiveButton.iconId.isEmpty()) {
                mActiveButton.iconId = HudVectorIcons.ALL_ICONS[0]; // Default to first icon
            }
        } else {
            mActiveButton.iconId = null;
        }
        updateSegmentedPill(mLabelPillText, !isIcon);
        updateSegmentedPill(mLabelPillIcon, isIcon);
        mTextSectionContainer.setVisibility(!isIcon ? VISIBLE : GONE);
        mIconSectionContainer.setVisibility(isIcon ? VISIBLE : GONE);
        if (mCallback != null) mCallback.onModelChanged(mActiveButton);
    }

    private Button createAlignButton(String label, int alignValue) {
        Button b = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
        b.setText(label);
        b.setTextSize(11f);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setAllCaps(false);
        b.setPadding(0, 0, 0, 0);
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        b.setOnClickListener(v -> {
            if (mActiveButton != null) {
                mActiveButton.textAlign = alignValue;
                updateAlignButtons();
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        });
        return b;
    }

    private void updateAlignButtons() {
        if (mActiveButton == null) return;
        updateAlignBtnStyle(mBtnAlignLeft, mActiveButton.textAlign == HudButton.ALIGN_LEFT);
        updateAlignBtnStyle(mBtnAlignCenter, mActiveButton.textAlign == HudButton.ALIGN_CENTER);
        updateAlignBtnStyle(mBtnAlignRight, mActiveButton.textAlign == HudButton.ALIGN_RIGHT);
    }

    private void updateAlignBtnStyle(Button b, boolean active) {
        if (b == null) return;
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(8));
        bg.setColor(active ? COLOR_TAB_ACTIVE : 0x33000000);
        bg.setStroke(dp(1), active ? Color.WHITE : 0x33FFFFFF);
        b.setBackground(bg);
        b.setTextColor(active ? Color.WHITE : 0xAAFFFFFF);
    }

    private LinearLayout createTextColorPresetsRow() {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(HORIZONTAL);
        row.setPadding(0, dp(4), 0, dp(4));

        final int[] colors = new int[]{0xFFFFFFFF, 0xFFA39CB8, 0xFF80DEEA, 0xFFF38BA8, 0xFFF9E2AF, 0xFFA6E3A1};
        for (int c : colors) {
            View dot = new View(getContext());
            GradientDrawable d = new GradientDrawable();
            d.setShape(GradientDrawable.OVAL);
            d.setColor(c);
            d.setStroke(dp(1), Color.WHITE);
            dot.setBackground(d);
            dot.setOnClickListener(v -> {
                if (mActiveButton != null) {
                    mActiveButton.textColor = c;
                    if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(28), dp(28));
            lp.rightMargin = dp(10);
            row.addView(dot, lp);
        }
        return row;
    }

    private void buildIconPresetsGrid(LinearLayout content) {
        // Remove old grid if recreating
        for (int i = content.getChildCount() - 1; i >= 0; i--) {
            View child = content.getChildAt(i);
            if (child instanceof GridLayout) {
                content.removeViewAt(i);
            }
        }

        Context ctx = getContext();
        boolean isLandscape = ctx.getResources().getConfiguration().orientation
                == Configuration.ORIENTATION_LANDSCAPE;
        GridLayout grid = new GridLayout(ctx);
        int cols = isLandscape ? 4 : 3;
        grid.setColumnCount(cols);
        grid.setAlignmentMode(GridLayout.ALIGN_BOUNDS);

        // Add None Option first
        View noneCard = createIconPresetCard(ctx, HudVectorIcons.ICON_NONE, getContext().getString(R.string.hud_editor_none_text));
        grid.addView(noneCard);

        for (int i = 0; i < HudVectorIcons.ALL_ICONS.length; i++) {
            final String id = HudVectorIcons.ALL_ICONS[i];
            final String lbl = getContext().getString(HudVectorIcons.ICON_LABELS[i]);
            View card = createIconPresetCard(ctx, id, lbl);
            grid.addView(card);
        }
        content.addView(grid, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
    }

    private View createIconPresetCard(Context ctx, String iconId, String label) {
        LinearLayout card = new LinearLayout(ctx);
        card.setOrientation(VERTICAL);
        card.setGravity(Gravity.CENTER);
        card.setClickable(true);

        boolean isSelected = false;
        if (mActiveButton != null) {
            if (HudVectorIcons.ICON_NONE.equals(iconId)) {
                isSelected = (mActiveButton.iconId == null || mActiveButton.iconId.isEmpty() || HudVectorIcons.ICON_NONE.equals(mActiveButton.iconId));
            } else {
                isSelected = iconId.equals(mActiveButton.iconId);
            }
        }

        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(8));
        bg.setColor(isSelected ? 0xFF504A60 : 0xFF3A3A3A);
        bg.setStroke(dp(isSelected ? 1.5f : 1), isSelected ? COLOR_TAB_ACTIVE : 0x33FFFFFF);
        card.setBackground(bg);
        card.setPadding(dp(4), dp(6), dp(4), dp(6));

        // Icon Preview Canvas
        View iconView = new View(ctx) {
            private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
            @Override
            protected void onDraw(Canvas canvas) {
                super.onDraw(canvas);
                p.setColor(Color.WHITE);
                HudVectorIcons.drawIcon(canvas, iconId, getWidth() * 0.5f, getHeight() * 0.5f, getHeight() * 0.75f, p);
            }
        };
        card.addView(iconView, new LayoutParams(dp(28), dp(24)));

        TextView tv = new TextView(ctx);
        tv.setText(label);
        tv.setTextSize(9.5f);
        tv.setTextColor(0xFFCCCCCC);
        tv.setSingleLine(true);
        tv.setGravity(Gravity.CENTER);
        card.addView(tv);

        card.setOnClickListener(v -> {
            if (mActiveButton != null) {
                mActiveButton.iconId = HudVectorIcons.ICON_NONE.equals(iconId) ? null : iconId;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                // Highlight active icon
                buildIconPresetsGrid(mIconSectionContainer);
            }
        });

        GridLayout.LayoutParams glp = new GridLayout.LayoutParams();
        glp.width = 0;
        glp.height = dp(52);
        glp.columnSpec = GridLayout.spec(GridLayout.UNDEFINED, 1f);
        glp.setMargins(dp(3), dp(3), dp(3), dp(3));
        card.setLayoutParams(glp);
        return card;
    }

    // =========================================================================
    // Tab 3: Event (Action Binding, Combos, Super Gestures, Triggers)
    // =========================================================================
    private void buildTabEvent() {
        mTabEventView = new ScrollView(getContext());
        styleScrollView(mTabEventView);
        LinearLayout content = new LinearLayout(getContext());
        content.setOrientation(VERTICAL);
        content.setPadding(0, 0, dp(14), 0);

        // Main Action Row
        mSectionPrimaryAction = createSectionHeader(getContext().getString(R.string.hud_editor_primary_action));
        content.addView(mSectionPrimaryAction);
        mBtnPickMainAction = createBadgeButton(getContext().getString(R.string.hud_unassigned));
        mBtnPickMainAction.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) {
                mCallback.onPickActionRequested(mActiveButton, 0);
            }
        });
        content.addView(mBtnPickMainAction);

        // Trigger Mode: Down vs Release
        boolean isLandscape = getContext().getResources().getConfiguration().orientation
                == Configuration.ORIENTATION_LANDSCAPE;
        mSendModeGroup = new RadioGroup(getContext());
        mSendModeGroup.setOrientation(isLandscape ? HORIZONTAL : VERTICAL);
        mSendImmediately = new RadioButton(getContext());
        mSendImmediately.setText(R.string.hud_send_immediately);
        mSendImmediately.setTextColor(0xFFCCCCCC);
        mSendImmediately.setTextSize(11);
        mSendOnRelease = new RadioButton(getContext());
        mSendOnRelease.setText(R.string.hud_send_on_release);
        mSendOnRelease.setTextColor(0xFFCCCCCC);
        mSendOnRelease.setTextSize(11);
        mSendModeGroup.addView(mSendImmediately);
        mSendModeGroup.addView(mSendOnRelease);
        mSendModeGroup.setOnCheckedChangeListener((group, checkedId) -> {
            if (mIsProgrammaticChange || mActiveButton == null) return;
            mActiveButton.sendOnRelease = (checkedId == mSendOnRelease.getId());
            if (mCallback != null) mCallback.onModelChanged(mActiveButton);
        });
        content.addView(mSendModeGroup);

        // Repeat Switch
        mRepeatSwitch = new Switch(getContext());
        mRepeatSwitch.setText(R.string.hud_hold_to_repeat);
        mRepeatSwitch.setTextColor(0xFFCCCCCC);
        mRepeatSwitch.setTextSize(11);
        mRepeatSwitch.setOnCheckedChangeListener((v, checked) -> {
            if (mIsProgrammaticChange || mActiveButton == null || mActiveButton.action == null) return;
            mActiveButton.action.repeat = checked;
            if (mCallback != null) mCallback.onModelChanged(mActiveButton);
        });
        content.addView(mRepeatSwitch);

        // Popup Action (Swipe Up)
        mSectionPopupAction = createSectionHeader(getContext().getString(R.string.hud_editor_popup_action));
        content.addView(mSectionPopupAction);
        mBtnPickPopupAction = createBadgeButton(getContext().getString(R.string.hud_none));
        mBtnPickPopupAction.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) {
                mCallback.onPickActionRequested(mActiveButton, 1);
            }
        });
        content.addView(mBtnPickPopupAction);

        // Long Press Action
        mSectionLongPressAction = createSectionHeader(getContext().getString(R.string.hud_long_press_action));
        content.addView(mSectionLongPressAction);
        mBtnPickLongPressAction = createBadgeButton(getContext().getString(R.string.hud_none));
        mBtnPickLongPressAction.setOnClickListener(v -> {
            if (mCallback != null && mActiveButton != null) {
                mCallback.onPickActionRequested(mActiveButton, 6);
            }
        });
        content.addView(mBtnPickLongPressAction);

        mLongPressSeekBar = new SeekBar(getContext());
        mLongPressBadge = createBadgeTextView("1000ms");
        mLongPressDelayRow = createSliderRow(getContext().getString(R.string.hud_long_press_delay), mLongPressSeekBar, mLongPressBadge,
                HudButton.LONG_PRESS_MIN_MS, HudButton.LONG_PRESS_MAX_MS, "ms", val -> {
            if (mActiveButton != null) {
                mActiveButton.longPressMs = val;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        });
        content.addView(mLongPressDelayRow);

        // Super Gesture / D-Pad Directions
        mSuperGestureOptions = new LinearLayout(getContext());
        mSuperGestureOptions.setOrientation(VERTICAL);
        mSuperGestureOptions.addView(createSectionHeader(getContext().getString(R.string.hud_editor_direction_actions)));
        mBtnPickSwipeLeft = createBadgeButton(getContext().getString(R.string.hud_direction_left));
        mBtnPickSwipeLeft.setOnClickListener(v -> pickDirection(2));
        mBtnPickSwipeRight = createBadgeButton(getContext().getString(R.string.hud_direction_right));
        mBtnPickSwipeRight.setOnClickListener(v -> pickDirection(3));
        mBtnPickSwipeUp = createBadgeButton(getContext().getString(R.string.hud_direction_up));
        mBtnPickSwipeUp.setOnClickListener(v -> pickDirection(4));
        mBtnPickSwipeDown = createBadgeButton(getContext().getString(R.string.hud_direction_down));
        mBtnPickSwipeDown.setOnClickListener(v -> pickDirection(5));
        mSuperGestureOptions.addView(mBtnPickSwipeUp);
        mSuperGestureOptions.addView(mBtnPickSwipeDown);
        mSuperGestureOptions.addView(mBtnPickSwipeLeft);
        mSuperGestureOptions.addView(mBtnPickSwipeRight);
        content.addView(mSuperGestureOptions);

        // TrackPoint / Touchpad Options
        mTrackPointOptions = new LinearLayout(getContext());
        mTrackPointOptions.setOrientation(VERTICAL);
        mTrackPointOptions.addView(createSectionHeader(getContext().getString(R.string.hud_editor_pointer_settings)));
        mBtnTrackPointMode = createBadgeButton(getContext().getString(R.string.hud_editor_mode_format, getContext().getString(R.string.hud_mode_mouse)));
        mBtnTrackPointMode.setOnClickListener(v -> toggleTrackPointMode());
        mTrackPointOptions.addView(mBtnTrackPointMode);

        mTrackSensitivitySeekBar = new SeekBar(getContext());
        mTrackSensitivityBadge = createBadgeTextView("1.5x");
        mTrackPointOptions.addView(createSliderRow(getContext().getString(R.string.hud_editor_sensitivity), mTrackSensitivitySeekBar, mTrackSensitivityBadge, 5, 30, "x", val -> {
            if (mActiveButton != null) {
                mActiveButton.trackpointSensitivity = val / 10f;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        }));
        content.addView(mTrackPointOptions);

        mTabEventView.addView(content);
        mTabContentContainer.addView(mTabEventView);
    }

    private void pickDirection(int slot) {
        if (mCallback != null && mActiveButton != null) {
            mCallback.onPickActionRequested(mActiveButton, slot);
        }
    }

    private void toggleTrackPointMode() {
        if (mActiveButton == null) return;
        boolean mouse = HudButton.MODE_MOUSE.equals(mActiveButton.trackpointMode);
        mActiveButton.trackpointMode = mouse ? HudButton.MODE_SCROLL : HudButton.MODE_MOUSE;
        mBtnTrackPointMode.setText(getContext().getString(R.string.hud_editor_mode_format, getContext().getString(HudButton.MODE_MOUSE.equals(mActiveButton.trackpointMode) ? R.string.hud_mode_mouse : R.string.hud_mode_scroll)));
        if (mCallback != null) mCallback.onModelChanged(mActiveButton);
    }

    // =========================================================================
    // Tab 4: Style (Opacity, Haptics, Colors)
    // =========================================================================
    private void buildTabStyle() {
        mTabStyleView = new ScrollView(getContext());
        styleScrollView(mTabStyleView);
        LinearLayout content = new LinearLayout(getContext());
        content.setOrientation(VERTICAL);
        content.setPadding(0, 0, dp(14), 0);

        // Opacity Slider (0% to 100%)
        mOpacitySeekBar = new SeekBar(getContext());
        mOpacityBadge = createBadgeTextView("85% ✎");
        content.addView(createSliderRow(getContext().getString(R.string.hud_editor_opacity), mOpacitySeekBar, mOpacityBadge, 0, 100, "%", val -> {
            if (mActiveButton != null) {
                mActiveButton.opacity = val / 100f;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        }));

        // Haptic Feedback Switch
        mHapticSwitch = new Switch(getContext());
        mHapticSwitch.setText(R.string.hud_haptic_feedback);
        mHapticSwitch.setTextColor(Color.WHITE);
        mHapticSwitch.setTextSize(12);
        mHapticSwitch.setTypeface(Typeface.DEFAULT_BOLD);
        mHapticSwitch.setPadding(0, dp(10), 0, dp(10));
        mHapticSwitch.setOnCheckedChangeListener((v, checked) -> {
            if (mIsProgrammaticChange || mActiveButton == null) return;
            mActiveButton.hapticEnabled = checked;
            if (mCallback != null) mCallback.onModelChanged(mActiveButton);
        });
        content.addView(mHapticSwitch);

        // Color Theme Presets
        content.addView(createSectionHeader(getContext().getString(R.string.hud_editor_background_color)));
        mColorThemeRow = createColorPresetsRow();
        content.addView(mColorThemeRow);

        // Border Width Slider
        mBorderWidthSeekBar = new SeekBar(getContext());
        mBorderWidthBadge = createBadgeTextView("1dp ✎");
        content.addView(createSliderRow(getContext().getString(R.string.hud_editor_border_stroke), mBorderWidthSeekBar, mBorderWidthBadge, 0, 10, "dp", val -> {
            if (mActiveButton != null) {
                mActiveButton.borderStrokeWidthDp = val;
                if (mCallback != null) mCallback.onModelChanged(mActiveButton);
            }
        }));

        // Border Color Presets
        content.addView(createSectionHeader(getContext().getString(R.string.hud_editor_border_color)));
        content.addView(createBorderColorPresetsRow());

        // Widget Style Variant (For D-Pad, Touchpad, Scrollbar)
        mStyleVariantSection = new LinearLayout(getContext());
        mStyleVariantSection.setOrientation(VERTICAL);
        mStyleVariantSection.addView(createSectionHeader(getContext().getString(R.string.hud_editor_control_style)));
        mStyleVariantRow = createStyleVariantsRow();
        mStyleVariantSection.addView(mStyleVariantRow);
        content.addView(mStyleVariantSection);

        mTabStyleView.addView(content);
        mTabContentContainer.addView(mTabStyleView);
    }

    private LinearLayout createStyleVariantsRow() {
        Context ctx = getContext();
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(HORIZONTAL);
        row.setPadding(0, dp(4), 0, dp(4));

        String[] variants = new String[]{getContext().getString(R.string.hud_editor_style_flat), getContext().getString(R.string.hud_editor_style_bevel), getContext().getString(R.string.hud_editor_style_glass)};
        for (int v = 0; v < variants.length; v++) {
            final int varIdx = v;
            Button b = new Button(ctx, null, android.R.attr.buttonBarButtonStyle);
            b.setText(variants[v]);
            b.setTextSize(10.5f);
            b.setTypeface(Typeface.DEFAULT_BOLD);
            b.setTextColor(Color.WHITE);
            b.setSingleLine(true);
            b.setEllipsize(android.text.TextUtils.TruncateAt.END);
            GradientDrawable bg = new GradientDrawable();
            bg.setCornerRadius(dp(8));
            bg.setColor(0xFF3A3A3A);
            bg.setStroke(dp(1), 0x33FFFFFF);
            b.setBackground(bg);
            b.setMinHeight(dp(32));
            b.setPadding(dp(6), dp(4), dp(6), dp(4));
            b.setOnClickListener(view -> {
                if (mActiveButton != null) {
                    mActiveButton.styleVariant = varIdx;
                    if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                    updateStyleVariantButtons();
                }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(34), 1f);
            if (v > 0) lp.leftMargin = dp(6);
            row.addView(b, lp);
        }
        return row;
    }

    private void updateStyleVariantButtons() {
        if (mStyleVariantRow == null || mActiveButton == null) return;
        int sel = mActiveButton.styleVariant;
        for (int i = 0; i < mStyleVariantRow.getChildCount(); i++) {
            View child = mStyleVariantRow.getChildAt(i);
            if (child instanceof Button) {
                GradientDrawable bg = new GradientDrawable();
                bg.setCornerRadius(dp(8));
                bg.setColor(i == sel ? COLOR_TAB_ACTIVE : 0xFF3A3A3A);
                bg.setStroke(dp(1), i == sel ? Color.WHITE : 0x33FFFFFF);
                child.setBackground(bg);
            }
        }
    }

    private LinearLayout createBorderColorPresetsRow() {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(HORIZONTAL);
        row.setPadding(0, dp(4), 0, dp(4));

        final int[] colors = new int[]{0xFFFFFFFF, 0xFFA39CB8, 0xFF80DEEA, 0xFFFFD54F, 0xFFF38BA8, 0x00000000};
        for (int c : colors) {
            View dot = new View(getContext());
            GradientDrawable d = new GradientDrawable();
            d.setShape(GradientDrawable.OVAL);
            d.setColor(c == 0 ? 0x22FFFFFF : c);
            d.setStroke(dp(1), Color.WHITE);
            dot.setBackground(d);
            dot.setOnClickListener(v -> {
                if (mActiveButton != null) {
                    mActiveButton.borderColor = c;
                    if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(26), dp(26));
            lp.rightMargin = dp(8);
            row.addView(dot, lp);
        }
        return row;
    }

    private void styleScrollView(ScrollView sv) {
        sv.setVerticalScrollBarEnabled(true);
        sv.setScrollbarFadingEnabled(false);
        sv.setScrollBarStyle(View.SCROLLBARS_INSIDE_INSET);

        // Round and tint scrollbar thumb
        GradientDrawable thumb = new GradientDrawable();
        thumb.setShape(GradientDrawable.RECTANGLE);
        thumb.setCornerRadius(dp(4));
        thumb.setColor(COLOR_TAB_ACTIVE); // Lilac pastel #A39CB8
        thumb.setSize(dp(4), dp(40));
        sv.setVerticalScrollbarThumbDrawable(thumb);
    }

    // =========================================================================
    // Helpers & UI Controls
    // =========================================================================
    private TextView createSectionHeader(String title) {
        TextView tv = new TextView(getContext());
        tv.setText(title);
        tv.setTextColor(COLOR_TEXT_MUTED);
        tv.setTextSize(10);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setPadding(0, dp(10), 0, dp(4));
        return tv;
    }

    private TextView createBadgeTextView(String defaultText) {
        TextView tv = new TextView(getContext());
        tv.setText(defaultText);
        tv.setTextSize(11);
        tv.setTypeface(Typeface.DEFAULT_BOLD);
        tv.setTextColor(Color.WHITE);
        tv.setGravity(Gravity.CENTER);

        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(12));
        bg.setColor(COLOR_TAB_ACTIVE);
        tv.setBackground(bg);
        tv.setPadding(dp(10), dp(4), dp(10), dp(4));
        tv.setLayoutParams(new LinearLayout.LayoutParams(dp(76), dp(26)));
        return tv;
    }

    private View createSliderRow(String label, SeekBar bar, TextView badge, int min, int max, String unit, ValueConsumer consumer) {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(VERTICAL);
        row.setPadding(0, dp(4), 0, dp(6));

        LinearLayout top = new LinearLayout(getContext());
        top.setOrientation(HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);

        TextView lbl = new TextView(getContext());
        lbl.setText(label);
        lbl.setTextColor(Color.WHITE);
        lbl.setTextSize(13);
        lbl.setTypeface(Typeface.DEFAULT_BOLD);
        top.addView(lbl, new LinearLayout.LayoutParams(0, LayoutParams.WRAP_CONTENT, 1f));
        LinearLayout.LayoutParams badgeLp = new LinearLayout.LayoutParams(dp(76), dp(26));
        badgeLp.rightMargin = dp(8); // Align right edge flush with slider
        top.addView(badge, badgeLp);
        row.addView(top);

        badge.setClickable(true);
        badge.setOnClickListener(v -> showNumberInputDialog(label, min, max, unit, bar, badge, consumer));

        styleAlightMotionSeekBar(bar);
        bar.setMax(max - min);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
                // Auto-Ghosting: Dim to 0% opacity (completely invisible) while dragging to see HUD
                animate().cancel();
                setAlpha(0.0f);
            }
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (fromUser) {
                    if (getAlpha() > 0.0f) {
                        animate().cancel();
                        setAlpha(0.0f);
                    }
                    int val = min + progress;
                    badge.setText(val + unit + " ✎");
                    consumer.accept(val);
                }
            }
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                // Restore to 100% opacity on release
                animate().cancel();
                animate().alpha(1.0f).setDuration(120).start();
            }
        });

        row.addView(bar);
        return row;
    }

    private void showNumberInputDialog(String label, int min, int max, String unit,
                                       SeekBar bar, TextView badge, ValueConsumer consumer) {
        Context ctx = getContext();
        AlertDialog.Builder builder = new AlertDialog.Builder(ctx);

        LinearLayout root = new LinearLayout(ctx);
        root.setOrientation(VERTICAL);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(16));
        bg.setColor(0xFF3A3A3A);
        bg.setStroke(dp(1), COLOR_TAB_ACTIVE);
        root.setBackground(bg);
        root.setPadding(dp(16), dp(14), dp(16), dp(16));

        TextView title = new TextView(ctx);
        title.setText(getContext().getString(R.string.hud_editor_set_value, label));
        title.setTextColor(Color.WHITE);
        title.setTextSize(13);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, 0, 0, dp(10));
        root.addView(title);

        EditText input = new EditText(ctx);
        input.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        int curVal = min + bar.getProgress();
        input.setText(String.valueOf(curVal));
        input.setSelectAllOnFocus(true);
        input.setTextColor(Color.WHITE);
        input.setTextSize(16);
        input.setTypeface(Typeface.DEFAULT_BOLD);
        input.setGravity(Gravity.CENTER);
        GradientDrawable inputBg = new GradientDrawable();
        inputBg.setCornerRadius(dp(8));
        inputBg.setColor(0xFF484848);
        inputBg.setStroke(dp(1), 0x33FFFFFF);
        input.setBackground(inputBg);
        input.setPadding(dp(10), dp(8), dp(10), dp(8));
        root.addView(input);

        TextView rangeHint = new TextView(ctx);
        rangeHint.setText(getContext().getString(R.string.hud_editor_range, min, max, unit));
        rangeHint.setTextColor(0xFF888888);
        rangeHint.setTextSize(10);
        rangeHint.setGravity(Gravity.CENTER);
        rangeHint.setPadding(0, dp(6), 0, dp(12));
        root.addView(rangeHint);

        LinearLayout btnRow = new LinearLayout(ctx);
        btnRow.setOrientation(HORIZONTAL);

        AlertDialog dialog = builder.setView(root).create();
        if (dialog.getWindow() != null) {
            dialog.getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));
        }

        Button btnCancel = new Button(ctx, null, android.R.attr.buttonBarButtonStyle);
        btnCancel.setText(R.string.hud_overlay_cancel);
        btnCancel.setTextColor(0xFF888888);
        btnCancel.setTextSize(11);
        btnCancel.setOnClickListener(v -> dialog.dismiss());
        btnRow.addView(btnCancel, new LinearLayout.LayoutParams(0, dp(34), 1f));

        Button btnOk = new Button(ctx, null, android.R.attr.buttonBarButtonStyle);
        btnOk.setText(R.string.hud_editor_apply);
        btnOk.setTextColor(Color.WHITE);
        btnOk.setTypeface(Typeface.DEFAULT_BOLD);
        btnOk.setTextSize(11);
        GradientDrawable okBg = new GradientDrawable();
        okBg.setCornerRadius(dp(8));
        okBg.setColor(COLOR_TAB_ACTIVE);
        btnOk.setBackground(okBg);
        btnOk.setOnClickListener(v -> {
            try {
                int val = Integer.parseInt(input.getText().toString().trim());
                val = Math.max(min, Math.min(max, val));
                bar.setProgress(val - min);
                badge.setText(val + unit + " ✎");
                consumer.accept(val);
            } catch (Exception ignored) {}
            dialog.dismiss();
        });
        LinearLayout.LayoutParams okLp = new LinearLayout.LayoutParams(0, dp(34), 1f);
        okLp.leftMargin = dp(8);
        btnRow.addView(btnOk, okLp);

        root.addView(btnRow);
        dialog.show();
        if (dialog.getWindow() != null) {
            dialog.getWindow().setLayout(dp(260), ViewGroup.LayoutParams.WRAP_CONTENT);
        }
    }

    private void styleAlightMotionSeekBar(SeekBar bar) {
        bar.setPadding(dp(8), dp(6), dp(8), dp(6));
        bar.setThumbOffset(dp(0));

        // Track Drawable (Lilac empty, White progress)
        GradientDrawable progress = new GradientDrawable();
        progress.setCornerRadius(dp(6));
        progress.setColor(Color.WHITE);

        GradientDrawable background = new GradientDrawable();
        background.setCornerRadius(dp(6));
        background.setColor(COLOR_TAB_ACTIVE);

        android.graphics.drawable.ClipDrawable clip = new android.graphics.drawable.ClipDrawable(
                progress, Gravity.LEFT, android.graphics.drawable.ClipDrawable.HORIZONTAL);

        android.graphics.drawable.LayerDrawable layer = new android.graphics.drawable.LayerDrawable(
                new android.graphics.drawable.Drawable[]{background, clip});
        layer.setId(0, android.R.id.background);
        layer.setId(1, android.R.id.progress);
        bar.setProgressDrawable(layer);

        // Vertical Pill Thumb
        GradientDrawable thumb = new GradientDrawable();
        thumb.setShape(GradientDrawable.RECTANGLE);
        thumb.setSize(dp(12), dp(22));
        thumb.setCornerRadius(dp(6));
        thumb.setColor(Color.WHITE);
        thumb.setStroke(dp(1.5f), 0xFF3A3A3A);
        bar.setThumb(thumb);
    }

    private Button createBadgeButton(String label) {
        Button b = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
        b.setText(label);
        b.setTextColor(Color.WHITE);
        b.setTextSize(11.5f);
        b.setAllCaps(false);
        GradientDrawable badgeBg = new GradientDrawable();
        badgeBg.setCornerRadius(dp(8));
        badgeBg.setColor(0x33000000);
        badgeBg.setStroke(dp(1), 0x33FFFFFF);
        b.setBackground(badgeBg);
        b.setPadding(dp(12), 0, dp(12), 0);
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        b.setSingleLine(true);
        b.setEllipsize(android.text.TextUtils.TruncateAt.END);
        LayoutParams lp = new LayoutParams(LayoutParams.MATCH_PARENT, dp(36));
        lp.setMargins(0, dp(2), 0, dp(6));
        b.setLayoutParams(lp);
        return b;
    }

    private LinearLayout createColorPresetsRow() {
        LinearLayout row = new LinearLayout(getContext());
        row.setOrientation(HORIZONTAL);
        row.setPadding(0, dp(4), 0, dp(4));

        final int[] colors = new int[]{0xE62A2B3D, 0xE61E1E2E, 0xE6313244, 0xE645475A, 0xE6585B70};
        for (int c : colors) {
            View dot = new View(getContext());
            GradientDrawable d = new GradientDrawable();
            d.setShape(GradientDrawable.OVAL);
            d.setColor(c);
            d.setStroke(dp(1), Color.WHITE);
            dot.setBackground(d);
            dot.setOnClickListener(v -> {
                if (mActiveButton != null) {
                    mActiveButton.bgColor = c;
                    if (mCallback != null) mCallback.onModelChanged(mActiveButton);
                }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(28), dp(28));
            lp.rightMargin = dp(10);
            row.addView(dot, lp);
        }
        return row;
    }

    private void syncSliderAndBadge(SeekBar bar, TextView badge, int value, int min, String unit) {
        mIsProgrammaticChange = true;
        try {
            bar.setProgress(value - min);
            badge.setText(value + unit + " ✎");
        } finally {
            mIsProgrammaticChange = false;
        }
    }

    public void adaptSize(int parentW, int parentH) {
        if (parentW <= 0 || parentH <= 0) return;
        boolean isLandscape = (parentW > parentH);
        int targetW = mUserPanelWidth > 0 ? mUserPanelWidth : dp(isLandscape ? 480 : 370);
        int targetH = mUserPanelHeight > 0 ? mUserPanelHeight : dp(isLandscape ? 280 : 380);
        targetW = Math.max(1, Math.min(parentW, targetW));
        targetH = Math.max(1, Math.min(parentH, targetH));
        ViewGroup.LayoutParams lp = getLayoutParams();
        if (lp != null && (lp.width != targetW || lp.height != targetH)) {
            lp.width = targetW;
            lp.height = targetH;
            setLayoutParams(lp);
        }

        // Adjust sidebar width in portrait
        if (mSidebarContainer != null) {
            ViewGroup.LayoutParams sLp = mSidebarContainer.getLayoutParams();
            int sw = dp(isLandscape ? 116 : 100);
            if (sLp != null && sLp.width != sw) {
                sLp.width = sw;
                mSidebarContainer.setLayoutParams(sLp);
            }
        }

        // Adjust Corner Shape Picker columns in portrait
        if (mShapePickerView != null) {
            mShapePickerView.setColumnCountForOrientation(targetW >= dp(440));
        }
    }

    public void bindButton(HudButton b) {
        this.mActiveButton = b;
        if (b == null) {
            setVisibility(GONE);
            return;
        }
        setVisibility(VISIBLE);

        View p = (View) getParent();
        if (p != null && p.getWidth() > 0 && p.getHeight() > 0) {
            adaptSize(p.getWidth(), p.getHeight());
        }

        // Hide Label tab for special control surfaces (D-Pad, Touchpad, Scrollbar, Group)
        // (Super Gesture CAN have label!)
        boolean isSpecialControl = b != null && (
                HudButton.WIDGET_DPAD.equals(b.widgetType) ||
                HudButton.WIDGET_TOUCHPAD.equals(b.widgetType) ||
                HudButton.WIDGET_SCROLLBAR.equals(b.widgetType) ||
                HudButton.WIDGET_GROUP.equals(b.widgetType)
        );
        if (mTabButtons != null && mTabButtons.length > 1) {
            mTabButtons[1].setVisibility(isSpecialControl ? GONE : VISIBLE);
            if (isSpecialControl && mCurrentTab == 1) {
                selectTab(0);
            }
        }

        // Hide Event tab entirely for Group (group is just a container)
        boolean isGroup = b != null && HudButton.WIDGET_GROUP.equals(b.widgetType);
        if (mTabButtons != null && mTabButtons.length > 2) {
            mTabButtons[2].setVisibility(isGroup ? GONE : VISIBLE);
            if (isGroup && mCurrentTab == 2) {
                selectTab(0);
            }
        }

        mIsProgrammaticChange = true;
        try {
            if (mLabelInput != null) mLabelInput.setText(b.label != null ? b.label : "");

            // Sync Label Segmented Pill Switch state (Text vs Vector Icon)
            boolean hasIcon = b.iconId != null && !HudVectorIcons.ICON_NONE.equals(b.iconId) && !b.iconId.isEmpty();
            if (mLabelPillText != null && mLabelPillIcon != null) {
                updateSegmentedPill(mLabelPillText, !hasIcon);
                updateSegmentedPill(mLabelPillIcon, hasIcon);
            }
            if (mTextSectionContainer != null) mTextSectionContainer.setVisibility(!hasIcon ? VISIBLE : GONE);
            if (mIconSectionContainer != null) mIconSectionContainer.setVisibility(hasIcon ? VISIBLE : GONE);
            updateAlignButtons();

            int sizeMax = sizeMaxDp(b);
            mWidthSeekBar.setMax(sizeMax - SIZE_MIN_DP);
            mHeightSeekBar.setMax(sizeMax - SIZE_MIN_DP);
            syncSliderAndBadge(mWidthSeekBar, mWidthBadge, b.widthDp, SIZE_MIN_DP, "px");
            syncSliderAndBadge(mHeightSeekBar, mHeightBadge, b.heightDp, SIZE_MIN_DP, "px");
            syncSliderAndBadge(mCornerSeekBar, mCornerBadge, b.cornerRadiusDp, 0, "dp");
            syncSliderAndBadge(mOpacitySeekBar, mOpacityBadge, Math.round(b.opacity * 100), 0, "%");
            if (mBorderWidthSeekBar != null && mBorderWidthBadge != null) {
                syncSliderAndBadge(mBorderWidthSeekBar, mBorderWidthBadge, b.borderStrokeWidthDp, 0, "dp");
            }
            if (mStyleVariantSection != null) {
                boolean hasStyleVariant = b != null && (
                        HudButton.WIDGET_DPAD.equals(b.widgetType) ||
                        HudButton.WIDGET_TOUCHPAD.equals(b.widgetType) ||
                        HudButton.WIDGET_SCROLLBAR.equals(b.widgetType)
                );
                mStyleVariantSection.setVisibility(hasStyleVariant ? VISIBLE : GONE);
                updateStyleVariantButtons();
            }

            // Transform adaptations:
            boolean isDpad = b != null && HudButton.WIDGET_DPAD.equals(b.widgetType);
            boolean isScrollbar = b != null && HudButton.WIDGET_SCROLLBAR.equals(b.widgetType);

            // D-Pad has fixed cross shape: hide corner radius & presets
            // (Scrollbar CAN adjust corner radius!)
            boolean hideCorners = isDpad;
            if (mCornerRow != null) mCornerRow.setVisibility(hideCorners ? GONE : VISIBLE);
            if (mShapeTitle != null) mShapeTitle.setVisibility(hideCorners ? GONE : VISIBLE);
            if (mShapePickerView != null) mShapePickerView.setVisibility(hideCorners ? GONE : VISIBLE);

            // D-Pad is always 1:1 square: hide height slider (width drives both)
            if (mHeightRow != null) mHeightRow.setVisibility(isDpad ? GONE : VISIBLE);

            if (mShapePickerView != null) {
                mShapePickerView.setSelectedShape(b.cornerShape > 0 ? b.cornerShape : HudButton.CORNER_ALL);
            }

            // Event adaptations:
            boolean isTouchpadOrScrollbar = b != null && (
                    HudButton.WIDGET_TOUCHPAD.equals(b.widgetType) ||
                    HudButton.WIDGET_SCROLLBAR.equals(b.widgetType)
            );
            boolean isSuperGesture = b != null && HudButton.WIDGET_SUPER_GESTURE.equals(b.widgetType);

            // Hide standard key binding for Touchpad, Scrollbar, D-Pad, Super Gesture
            boolean hideStandardKeys = isTouchpadOrScrollbar || isDpad || isSuperGesture;
            if (mSectionPrimaryAction != null) mSectionPrimaryAction.setVisibility(hideStandardKeys ? GONE : VISIBLE);
            if (mBtnPickMainAction != null) mBtnPickMainAction.setVisibility(hideStandardKeys ? GONE : VISIBLE);
            if (mSendModeGroup != null) mSendModeGroup.setVisibility(hideStandardKeys ? GONE : VISIBLE);
            if (mRepeatSwitch != null) mRepeatSwitch.setVisibility(hideStandardKeys ? GONE : VISIBLE);
            if (mSectionPopupAction != null) mSectionPopupAction.setVisibility(hideStandardKeys ? GONE : VISIBLE);
            if (mBtnPickPopupAction != null) mBtnPickPopupAction.setVisibility(hideStandardKeys ? GONE : VISIBLE);
            if (mSectionLongPressAction != null) mSectionLongPressAction.setVisibility(hideStandardKeys ? GONE : VISIBLE);
            if (mBtnPickLongPressAction != null) mBtnPickLongPressAction.setVisibility(hideStandardKeys ? GONE : VISIBLE);
            if (mLongPressDelayRow != null) mLongPressDelayRow.setVisibility(hideStandardKeys ? GONE : VISIBLE);

            if (mBtnPickMainAction != null) {
                mBtnPickMainAction.setText(formatActionBadge(b.action));
            }
            if (mBtnPickPopupAction != null) {
                mBtnPickPopupAction.setText(formatActionBadge(b.popupAction));
            }
            if (mBtnPickLongPressAction != null) {
                mBtnPickLongPressAction.setText(formatActionBadge(b.longPressAction));
            }

            if (mSendOnRelease != null) {
                mSendOnRelease.setChecked(b.sendOnRelease);
                mSendImmediately.setChecked(!b.sendOnRelease);
            }
            if (mRepeatSwitch != null && b.action != null) {
                mRepeatSwitch.setChecked(b.action.repeat);
            }
            if (mHapticSwitch != null) {
                mHapticSwitch.setChecked(b.hapticEnabled);
            }

            boolean isSuper = HudButton.WIDGET_SUPER_GESTURE.equals(b.widgetType)
                    || HudButton.WIDGET_DPAD.equals(b.widgetType);
            mSuperGestureOptions.setVisibility(isSuper ? VISIBLE : GONE);
            if (isSuper) {
                String prefix = getContext().getString(isDpad ? R.string.hud_editor_widget_dpad : R.string.hud_editor_swipe_prefix);
                mBtnPickSwipeUp.setText(getContext().getString(R.string.hud_editor_direction_action, prefix, getContext().getString(R.string.hud_direction_up), formatActionBadge(b.directionalAction(4))));
                mBtnPickSwipeDown.setText(getContext().getString(R.string.hud_editor_direction_action, prefix, getContext().getString(R.string.hud_direction_down), formatActionBadge(b.directionalAction(5))));
                mBtnPickSwipeLeft.setText(getContext().getString(R.string.hud_editor_direction_action, prefix, getContext().getString(R.string.hud_direction_left), formatActionBadge(b.directionalAction(2))));
                mBtnPickSwipeRight.setText(getContext().getString(R.string.hud_editor_direction_action, prefix, getContext().getString(R.string.hud_direction_right), formatActionBadge(b.directionalAction(3))));
            }

            boolean isPointer = HudButton.WIDGET_TOUCHPAD.equals(b.widgetType);
            mTrackPointOptions.setVisibility(isPointer ? VISIBLE : GONE);
        } finally {
            mIsProgrammaticChange = false;
        }
    }

    private String formatActionBadge(HudAction a) {
        if (a == null) return getContext().getString(R.string.hud_none);
        switch (a.type) {
            case HudAction.TYPE_KEY:
            case HudAction.TYPE_MODIFIER:
                return a.code > 0 ? labelForEvdev(getContext(), a.code) : getContext().getString(R.string.hud_unassigned);
            case HudAction.TYPE_COMBO:
                if (a.comboKeys == null || a.comboKeys.isEmpty()) return getContext().getString(R.string.hud_editor_empty_combo);
                StringBuilder sb = new StringBuilder();
                for (int i = 0; i < a.comboKeys.size(); i++) {
                    if (i > 0) sb.append(" + ");
                    sb.append(labelForEvdev(getContext(), a.comboKeys.get(i)));
                }
                return sb.toString();
            case HudAction.TYPE_TEXT:
                return a.text != null && !a.text.isEmpty() ? a.text : getContext().getString(R.string.hud_editor_text);
            case HudAction.TYPE_SYSTEM:
                return getContext().getString(R.string.hud_system_action, systemActionLabel(a.systemCommand));
            default:
                return a.code > 0 ? labelForEvdev(getContext(), a.code) : getContext().getString(R.string.hud_editor_configured);
        }
    }

    private String systemActionLabel(String command) {
        if (command == null) return getContext().getString(R.string.hud_none);
        int label;
        switch (command) {
            case "toggle_ime": label = R.string.hud_system_soft_keyboard; break;
            case "toggle_vk": label = R.string.hud_system_virtual_keyboard; break;
            case "toggle_hud_hidden": label = R.string.hud_system_toggle_hud_hidden; break;
            case "open_settings": label = R.string.hud_system_settings; break;
            case "mouse_left": label = R.string.hud_system_mouse_left; break;
            case "mouse_right": label = R.string.hud_system_mouse_right; break;
            case "mouse_middle": label = R.string.hud_system_mouse_middle; break;
            case "mouse_scroll_up": label = R.string.hud_system_scroll_up; break;
            case "mouse_scroll_down": label = R.string.hud_system_scroll_down; break;
            case "switch_layout_1": label = R.string.hud_layout_1; break;
            case "switch_layout_2": label = R.string.hud_layout_2; break;
            case "switch_layout_3": label = R.string.hud_layout_3; break;
            default: return command;
        }
        return getContext().getString(label);
    }

    public static String labelForEvdev(Context context, int keycode) {
        if (keycode <= 0) return context.getString(R.string.hud_none);
        switch (keycode) {
            case 1: return "ESC";
            case 14: return "BKSP";
            case 15: return "TAB";
            case 28: return "ENTER";
            case 29: case 97: return "CTRL";
            case 42: case 54: return "SHIFT";
            case 56: case 100: return "ALT";
            case 57: return "SPACE";
            case 125: case 126: return "SUPER";
            case 102: return "HOME";
            case 103: return "UP";
            case 104: return "PGUP";
            case 105: return "LEFT";
            case 106: return "RIGHT";
            case 107: return "END";
            case 108: return "DOWN";
            case 109: return "PGDN";
            case 110: return "INS";
            case 111: return "DEL";
            case 58: return "CAPS";
            // Numbers 1-9, 0
            case 2: return "1"; case 3: return "2"; case 4: return "3";
            case 5: return "4"; case 6: return "5"; case 7: return "6";
            case 8: return "7"; case 9: return "8"; case 10: return "9"; case 11: return "0";
            // F1 - F12
            case 59: return "F1"; case 60: return "F2"; case 61: return "F3";
            case 62: return "F4"; case 63: return "F5"; case 64: return "F6";
            case 65: return "F7"; case 66: return "F8"; case 67: return "F9";
            case 68: return "F10"; case 87: return "F11"; case 88: return "F12";
            // Symbols
            case 12: return "-"; case 13: return "=";
            case 26: return "["; case 27: return "]";
            case 43: return "\\"; case 39: return ";";
            case 40: return "'"; case 41: return "`";
            case 51: return ","; case 52: return "."; case 53: return "/";
            default:
                if (keycode >= 16 && keycode <= 25) {
                    String[] row1 = {"Q","W","E","R","T","Y","U","I","O","P"};
                    return row1[keycode - 16];
                }
                if (keycode >= 30 && keycode <= 38) {
                    String[] row2 = {"A","S","D","F","G","H","J","K","L"};
                    return row2[keycode - 30];
                }
                if (keycode >= 44 && keycode <= 50) {
                    String[] row3 = {"Z","X","C","V","B","N","M"};
                    return row3[keycode - 44];
                }
                return "KEY_" + keycode;
        }
    }

    private interface ValueConsumer {
        void accept(int val);
    }

    private static final class Dots2x2View extends View {
        private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

        Dots2x2View(Context context) {
            super(context);
            mPaint.setColor(Color.WHITE);
            mPaint.setStyle(Paint.Style.FILL);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float w = getWidth();
            float h = getHeight();
            float r = Math.min(w, h) * 0.12f;
            float c1 = w * 0.3f;
            float c2 = w * 0.7f;
            float r1 = h * 0.3f;
            float r2 = h * 0.7f;
            canvas.drawCircle(c1, r1, r, mPaint);
            canvas.drawCircle(c2, r1, r, mPaint);
            canvas.drawCircle(c1, r2, r, mPaint);
            canvas.drawCircle(c2, r2, r, mPaint);
        }
    }
}
