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
        /** A configured long-press action was triggered. */
        default void onButtonLongPress(HudButton button) {}
    }

    /** Movement that cancels a pending long press, in dp. */
    private static final float LONG_PRESS_SLOP_DP = 12f;

    private final HudButton mModel;
    private final ButtonActionListener mListener;
    private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF mBounds = new RectF();
    private final HudButtonPress mPress = new HudButtonPress();

    private boolean mIsPressed;
    private float mDownX;
    private float mDownY;
    private boolean mPopupTriggered;
    // A configured long-press action competes with the release and swipe actions.
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
            mPress.repeat(HudFreeformButtonView.this::sendPrimaryTap);
            mRepeatHandler.postDelayed(this, REPEAT_INTERVAL_MS);
        }
    };

    private final Runnable mLongPressTick = new Runnable() {
        @Override public void run() {
            if (!mIsPressed || !mLongPressArmed) return;
            mLongPressFired = true;
            mPress.cancel();
            stopRepeat();
            if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
            if (mListener != null) mListener.onButtonLongPress(mModel);
        }
    };

    private boolean hasLongPress() {
        return mModel != null && mModel.longPressAction != null;
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

    private void sendPrimaryTap() {
        if (mListener == null) return;
        mListener.onButtonPress(mModel, true);
        mListener.onButtonPress(mModel, false);
    }

    @Override
    protected void onDetachedFromWindow() {
        mIsPressed = false;
        mPress.cancel();
        stopRepeat();
        dropLongPress();
        super.onDetachedFromWindow();
    }

    private boolean isRepeatable() {
        return mModel != null && !mModel.sendOnRelease && mModel.action != null
                && HudAction.TYPE_KEY.equals(mModel.action.type)
                && mModel.action.code > 0 && mModel.action.repeat;
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
                mDownX = event.getX();
                mDownY = y;
                mIsPressed = true;
                mPopupTriggered = false;
                mLongPressFired = false;
                if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                invalidate();
                if (hasLongPress()) {
                    mLongPressArmed = true;
                    mRepeatHandler.postDelayed(mLongPressTick, Math.max(HudButton.LONG_PRESS_MIN_MS,
                            Math.min(HudButton.LONG_PRESS_MAX_MS, mModel.longPressMs)));
                }
                mPress.down(mModel.sendOnRelease, this::sendPrimaryTap);
                startRepeat();
                return true;

            case MotionEvent.ACTION_MOVE:
                if (mLongPressArmed && Math.hypot(event.getX() - mDownX, y - mDownY) > LONG_PRESS_SLOP_DP * density) {
                    dropLongPress();
                }
                if (!mLongPressFired && mModel.popupAction != null && !mPopupTriggered && (mDownY - y) > 30 * density) {
                    mPopupTriggered = true;
                    mIsPressed = false;
                    stopRepeat();
                    dropLongPress();
                    mPress.cancel();
                    if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.CONFIRM);
                    invalidate();
                    if (mListener != null) {
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
                // A cancellation must not trigger a deferred tap.
                if (canceled) {
                    mPress.cancel();
                    return true;
                }
                if (mPopupTriggered) return true;
                mPress.up(this::sendPrimaryTap);
                return true;
        }
        return super.onTouchEvent(event);
    }
}
