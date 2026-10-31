package com.anland.consumer.hud;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.os.Handler;
import android.os.Looper;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * High-precision Virtual TrackPoint Nub (ThinkPad style).
 * Continuous 360-degree velocity dispatching with center deadzone and click detection.
 */
public final class TrackpointNubView extends View {

    public interface MotionDispatcher {
        void onPointerMove(float dx, float dy);
        void onPointerClick(int button); // 1 = left, 2 = right
        // Called when the nub is in "scroll" mode and the user drags on it.
        // dx/dy are pixel deltas (already in native scale, see the host
        // implementation in MainActivity). This is optional - hosts that
        // do not care about scroll mode can leave it empty.
        default void onPointerScroll(float dx, float dy) { /* no-op */ }
    }

    private final HudButton mModel;
    private final MotionDispatcher mDispatcher;
    private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private float mCenterTouchX;
    private float mCenterTouchY;
    private float mCurrentOffsetX;
    private float mCurrentOffsetY;
    private boolean mIsDragging;
    private long mTouchDownTime;
    // In edit mode, this nub must not act as a trackpoint. Drag-to-move is
    // handled by the parent HudOverlayView via its OnTouchListener. Without
    // this guard, a finger on the nub in edit mode would both move the cursor
    // AND try to drag the widget, producing jittery / contradictory motion.
    private boolean mInEditMode = false;

    private final Handler mLoopHandler = new Handler(Looper.getMainLooper());
    private final Runnable mLoopRunnable = new Runnable() {
        @Override
        public void run() {
            if (mIsDragging) {
                dispatchContinuousMotion();
                mLoopHandler.postDelayed(this, 16); // ~60-120Hz dispatch loop
            }
        }
    };

    public TrackpointNubView(Context context, HudButton model, MotionDispatcher dispatcher) {
        super(context);
        this.mModel = model;
        this.mDispatcher = dispatcher;
    }

    public void setInEditMode(boolean editMode) {
        if (mInEditMode == editMode) return;
        mInEditMode = editMode;
        if (editMode) {
            mIsDragging = false;
            mCurrentOffsetX = 0;
            mCurrentOffsetY = 0;
            mLoopHandler.removeCallbacks(mLoopRunnable);
        }
        invalidate();
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
        float cx = w * 0.5f;
        float cy = h * 0.5f;
        float radius = Math.min(w, h) * 0.5f - 3 * density;

        // Outer base ring
        mPaint.setStyle(Paint.Style.FILL);
        mPaint.setColor(0x66000000);
        canvas.drawCircle(cx, cy, radius, mPaint);

        // Outer rim stroke - color indicates mode
        boolean isScroll = HudButton.MODE_SCROLL.equals(mModel.trackpointMode);

        mPaint.setStyle(Paint.Style.STROKE);
        mPaint.setStrokeWidth(2f * density);
        int rimColor = isScroll ? 0xAA80DEEA : 0x88FFFFFF;
        mPaint.setColor(rimColor);
        canvas.drawCircle(cx, cy, radius, mPaint);

        // Center Nub / Nipple
        float nubRadius = radius * 0.45f;
        float nubCx = cx + mCurrentOffsetX;
        float nubCy = cy + mCurrentOffsetY;

        mPaint.setStyle(Paint.Style.FILL);
        int defaultNubColor = isScroll ? 0xFF00838F : 0xFFD32F2F;
        mPaint.setColor(mModel.bgColor != 0 ? mModel.bgColor : defaultNubColor);
        canvas.drawCircle(nubCx, nubCy, nubRadius, mPaint);

        // Nub textured grip dots
        mPaint.setColor(isScroll ? 0xFFE0F7FA : 0xFFFFFFFF);
        mPaint.setStyle(Paint.Style.FILL);
        float dotR = Math.max(1.5f, 2f * density);
        float dotOffset = nubRadius * 0.4f;
        canvas.drawCircle(nubCx, nubCy, dotR * 1.1f, mPaint);
        canvas.drawCircle(nubCx - dotOffset, nubCy, dotR, mPaint);
        canvas.drawCircle(nubCx + dotOffset, nubCy, dotR, mPaint);
        canvas.drawCircle(nubCx, nubCy - dotOffset, dotR, mPaint);
        canvas.drawCircle(nubCx, nubCy + dotOffset, dotR, mPaint);
    }

    private boolean isHapticEnabled() {
        return mModel.hapticEnabled;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        // Edit mode: the parent's OnTouchListener owns this widget for
        // drag-to-move. Defer completely so we don't also start a fake
        // trackpoint drag or send mouse motion.
        if (mInEditMode) return false;

        float x = event.getX();
        float y = event.getY();
        float cx = getWidth() * 0.5f;
        float cy = getHeight() * 0.5f;

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                mIsDragging = true;
                mTouchDownTime = System.currentTimeMillis();
                mCenterTouchX = x;
                mCenterTouchY = y;
                if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK);
                updateOffset(x - cx, y - cy);
                mLoopHandler.post(mLoopRunnable);
                return true;

            case MotionEvent.ACTION_MOVE:
                updateOffset(x - cx, y - cy);
                return true;

            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                mIsDragging = false;
                mLoopHandler.removeCallbacks(mLoopRunnable);

                // Quick tap detection for left click
                long duration = System.currentTimeMillis() - mTouchDownTime;
                float dist = (float) Math.hypot(x - mCenterTouchX, y - mCenterTouchY);
                if (event.getActionMasked() == MotionEvent.ACTION_UP && duration < 250 && dist < 15) {
                    if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    if (mDispatcher != null) {
                        mDispatcher.onPointerClick(0x110); // BTN_LEFT
                    }
                }

                mCurrentOffsetX = 0;
                mCurrentOffsetY = 0;
                invalidate();
                return true;
        }
        return super.onTouchEvent(event);
    }

    @Override
    protected void onDetachedFromWindow() {
        mLoopHandler.removeCallbacks(mLoopRunnable);
        mIsDragging = false;
        super.onDetachedFromWindow();
    }

    private void updateOffset(float dx, float dy) {
        float density = getResources().getDisplayMetrics().density;
        float radius = Math.min(getWidth(), getHeight()) * 0.5f - 3 * density;
        float nubRadius = radius * 0.45f;
        float maxAllowedTravel = Math.max(0, radius - nubRadius);
        float dist = (float) Math.hypot(dx, dy);
        if (dist > maxAllowedTravel && dist > 0) {
            dx = (dx / dist) * maxAllowedTravel;
            dy = (dy / dist) * maxAllowedTravel;
        }
        mCurrentOffsetX = dx;
        mCurrentOffsetY = dy;
        invalidate();
    }

    private void dispatchContinuousMotion() {
        if (mDispatcher == null) return;
        float density = getResources().getDisplayMetrics().density;
        float radius = Math.min(getWidth(), getHeight()) * 0.5f - 3 * density;
        float deadzone = mModel.trackpointDeadzoneDp * density;
        float dist = (float) Math.hypot(mCurrentOffsetX, mCurrentOffsetY);

        if (dist > deadzone) {
            float effectiveDist = dist - deadzone;
            float speedFactor = (effectiveDist / 20f) * mModel.trackpointSensitivity * 8f;
            float normX = mCurrentOffsetX / dist;
            float normY = mCurrentOffsetY / dist;

            if (HudButton.MODE_SCROLL.equals(mModel.trackpointMode)) {
                mDispatcher.onPointerScroll(normX * speedFactor, normY * speedFactor);
            } else {
                mDispatcher.onPointerMove(normX * speedFactor, normY * speedFactor);
            }
        }
    }
}
