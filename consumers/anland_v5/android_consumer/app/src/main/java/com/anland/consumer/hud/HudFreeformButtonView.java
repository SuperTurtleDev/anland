package com.anland.consumer.hud;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * Visual Freeform Button View with touch down/up states and swipe-up popup support.
 */
public final class HudFreeformButtonView extends View {

    public interface ButtonActionListener {
        void onButtonPress(HudButton button, boolean isDown);
        void onPopupTrigger(HudButton button);
        /**
         * The press was held for {@link #LONG_PRESS_MS} without moving. The
         * click that a short tap would have produced is suppressed.
         */
        default void onButtonLongPress(HudButton button) {}
    }

    /** How long the KB key has to be held to toggle the whole HUD. */
    private static final long LONG_PRESS_MS = 1000L;
    /** Movement that cancels a pending long press, in dp. */
    private static final float LONG_PRESS_SLOP_DP = 12f;

    private final HudButton mModel;
    private final ButtonActionListener mListener;
    private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF mBounds = new RectF();

    private boolean mIsPressed;
    private float mDownY;
    private boolean mPopupTriggered;
    // Long press: only the KB key uses it, and while it is pending the click is
    // held back so a hold never fires the tap action as well.
    private boolean mLongPressArmed = false;
    private boolean mLongPressFired = false;
    // In edit mode the parent HudOverlayView already owns the touch stream
    // (it uses an OnTouchListener to drag-to-move). We must NOT respond to
    // touch ourselves, or the parent would think we have a competing gesture
    // and we would also accidentally trigger button press on every tap.
    private boolean mInEditMode = false;

    // Hold-to-repeat: the legacy extra-keys bar repeated directional and
    // editing keys while held. HudAction.repeat carries that intent.
    private static final long REPEAT_INITIAL_DELAY_MS = 320L;
    private static final long REPEAT_INTERVAL_MS = 60L;
    private final Handler mRepeatHandler = new Handler(Looper.getMainLooper());
    private final Runnable mRepeatTick = new Runnable() {
        @Override public void run() {
            if (!mIsPressed || mListener == null) return;
            // Close the previous press and open a new one so the desktop sees
            // discrete key events, exactly like a hardware auto-repeat.
            mListener.onButtonPress(mModel, false);
            mListener.onButtonPress(mModel, true);
            mRepeatHandler.postDelayed(this, REPEAT_INTERVAL_MS);
        }
    };

    private final Runnable mLongPressTick = new Runnable() {
        @Override public void run() {
            if (!mIsPressed || !mLongPressArmed) return;
            mLongPressFired = true;
            if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
            if (mListener != null) mListener.onButtonLongPress(mModel);
        }
    };

    /** The KB key is the only control with a hold gesture. */
    private boolean hasLongPress() {
        return mModel != null && mModel.isVirtualKeyboardToggle();
    }

    private void dropLongPress() {
        mLongPressArmed = false;
        mRepeatHandler.removeCallbacks(mLongPressTick);
    }

    public HudFreeformButtonView(Context context, HudButton model, ButtonActionListener listener) {
        super(context);
        this.mModel = model;
        this.mListener = listener;

        mTextPaint.setColor(Color.WHITE);
        mTextPaint.setTextAlign(Paint.Align.CENTER);
        mTextPaint.setFakeBoldText(true);
    }

    public void setInEditMode(boolean editMode) {
        mInEditMode = editMode;
        if (editMode) stopRepeat();
    }

    private boolean mHeldAction;

    private void releaseHeldAction() {
        if (mHeldAction && mListener != null) mListener.onButtonPress(mModel, false);
        mHeldAction = false;
    }

    @Override
    protected void onDetachedFromWindow() {
        releaseHeldAction();
        stopRepeat();
        dropLongPress();
        super.onDetachedFromWindow();
    }

    private boolean isRepeatable() {
        if (mModel == null || mModel.action == null || !mModel.action.repeat) return false;
        // A latched modifier must never repeat; it is a toggle, not a key.
        return !HudAction.TYPE_MODIFIER.equals(mModel.action.type);
    }

    private void startRepeat() {
        mRepeatHandler.removeCallbacks(mRepeatTick);
        if (!isRepeatable()) return;
        mRepeatHandler.postDelayed(mRepeatTick, REPEAT_INITIAL_DELAY_MS);
    }

    private void stopRepeat() {
        mRepeatHandler.removeCallbacks(mRepeatTick);
    }

    public HudButton getModel() {
        return mModel;
    }

    private boolean isHapticEnabled() {
        return mModel.hapticEnabled;
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        float density = getResources().getDisplayMetrics().density;
        int w = Math.round(mModel.widthDp * density);
        int h = Math.round(mModel.heightDp * density);
        setMeasuredDimension(w, h);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float density = getResources().getDisplayMetrics().density;
        float w = getWidth();
        float h = getHeight();
        mBounds.set(2, 2, w - 2, h - 2);

        // Fill
        mPaint.setStyle(Paint.Style.FILL);
        mPaint.setColor(mIsPressed ? mModel.activeColor : mModel.bgColor);
        mPaint.setAlpha(Math.round(mModel.opacity * 255));
        float corner = mModel.cornerRadiusDp * density;
        canvas.drawRoundRect(mBounds, corner, corner, mPaint);

        // Stroke
        mPaint.setStyle(Paint.Style.STROKE);
        mPaint.setStrokeWidth(Math.max(1, Math.round(1.0f * density)));
        mPaint.setColor(0x66FFFFFF);
        canvas.drawRoundRect(mBounds, corner, corner, mPaint);

        // Text with auto font scaling
        mTextPaint.setColor(mIsPressed ? 0xFF11111B : mModel.textColor);
        String label = mModel.label != null ? mModel.label : "BTN";
        float maxTextWidth = w - 12 * density;
        float textSize = Math.min(w, h) * 0.35f;
        mTextPaint.setTextSize(textSize);
        float textW = mTextPaint.measureText(label);
        if (textW > maxTextWidth && textW > 0) {
            mTextPaint.setTextSize(Math.max(8 * density, textSize * (maxTextWidth / textW)));
        }
        float textY = h * 0.5f - ((mTextPaint.descent() + mTextPaint.ascent()) * 0.5f);
        canvas.drawText(label, w * 0.5f, textY, mTextPaint);

        // Secondary popup glyph dot if configured
        if (mModel.popupAction != null) {
            mPaint.setStyle(Paint.Style.FILL);
            mPaint.setColor(0xCC80DEEA);
            canvas.drawCircle(w - 7 * density, 7 * density, 2.5f * density, mPaint);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        // Edit mode: the parent's OnTouchListener already owns this widget for
        // drag-to-move. Returning false here is the Android-idiomatic way of
        // saying "I don't want to handle this; my parent will".
        if (mInEditMode) return false;

        float y = event.getY();
        float density = getResources().getDisplayMetrics().density;

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                mDownY = y;
                mIsPressed = true;
                mPopupTriggered = false;
                mLongPressFired = false;
                if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                invalidate();
                if (hasLongPress()) {
                    // Wait: a tap still toggles the keyboard, a hold hides the HUD.
                    mLongPressArmed = true;
                    mRepeatHandler.postDelayed(mLongPressTick, LONG_PRESS_MS);
                } else {
                    if (mListener != null) {
                        mListener.onButtonPress(mModel, true);
                        mHeldAction = mModel.action != null && (HudAction.TYPE_KEY.equals(mModel.action.type));
                    }
                    startRepeat();
                }
                return true;

            case MotionEvent.ACTION_MOVE:
                if (mLongPressArmed && Math.abs(y - mDownY) > LONG_PRESS_SLOP_DP * density) {
                    dropLongPress();
                }
                if (mModel.popupAction != null && !mPopupTriggered && (mDownY - y) > 30 * density) {
                    mPopupTriggered = true;
                    mIsPressed = false;
                    stopRepeat();
                    if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.CONFIRM);
                    invalidate();
                    if (mListener != null) {
                        mListener.onButtonPress(mModel, false);
                        mHeldAction = false;
                        mListener.onPopupTrigger(mModel);
                    }
                }
                return true;

            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                boolean canceled = event.getActionMasked() == MotionEvent.ACTION_CANCEL;
                mIsPressed = false;
                stopRepeat();
                dropLongPress();
                invalidate();
                if (mLongPressFired) {
                    mLongPressFired = false;
                    return true;   // the hold already did its job
                }
                // A cancel is not a tap: the action that ran on touch-down may
                // itself have hidden this view (the KB key does exactly that),
                // and replaying the click here would undo it straight away.
                if (canceled) {
                    releaseHeldAction();
                    return true;
                }
                if (hasLongPress()) {
                    // A short tap on the KB key: replay it as a plain click.
                    if (mListener != null) {
                        mListener.onButtonPress(mModel, true);
                        mListener.onButtonPress(mModel, false);
                    }
                } else if (mListener != null && !mPopupTriggered) {
                    mListener.onButtonPress(mModel, false);
                    mHeldAction = false;
                }
                return true;
        }
        return super.onTouchEvent(event);
    }
}
