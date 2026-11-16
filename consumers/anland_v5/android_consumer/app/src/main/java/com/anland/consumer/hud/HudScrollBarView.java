package com.anland.consumer.hud;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * Customizable On-Screen Scroll Bar widget for HUD.
 * Automatically adapts between Vertical and Horizontal orientations based on aspect ratio.
 * Dragging along the bar sends continuous smooth scroll-wheel events.
 * Tapping the ends sends discrete step ticks.
 */
public final class HudScrollBarView extends View {

    public interface ScrollDispatcher {
        void onPointerScroll(float dx, float dy);
    }

    private final HudButton mModel;
    private final ScrollDispatcher mDispatcher;
    private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF mBounds = new RectF();
    private final RectF mThumbBounds = new RectF();
    private final Path mArrowPath1 = new Path();
    private final Path mArrowPath2 = new Path();

    private boolean mInEditMode = false;
    private boolean mIsTouching = false;
    private float mLastTouchPos = 0f;
    private float mAccumulatedDrag = 0f;
    private float mThumbOffset = 0f;

    public HudScrollBarView(Context context, HudButton model, ScrollDispatcher dispatcher) {
        super(context);
        this.mModel = model;
        this.mDispatcher = dispatcher;
        setClickable(true);
    }

    public void setInEditMode(boolean editMode) {
        if (mInEditMode == editMode) return;
        mInEditMode = editMode;
        if (editMode) {
            mIsTouching = false;
            mThumbOffset = 0f;
        }
        invalidate();
    }

    private boolean isHorizontal() {
        if ("horizontal".equalsIgnoreCase(mModel.trackpointMode)) return true;
        if ("vertical".equalsIgnoreCase(mModel.trackpointMode)) return false;
        return mModel.widthDp > mModel.heightDp;
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
        if (w <= 0 || h <= 0) return;

        boolean isHoriz = isHorizontal();
        float corner = mModel.cornerRadiusDp > 0 ? mModel.cornerRadiusDp * density : Math.min(w, h) * 0.5f;
        mBounds.set(2, 2, w - 2, h - 2);

        // 1. Background Track
        mPaint.setStyle(Paint.Style.FILL);
        int baseBg = mModel.bgColor != 0 ? mModel.bgColor : 0xE61A1B26;
        mPaint.setColor(mIsTouching ? 0xF0252838 : baseBg);
        mPaint.setAlpha(Math.round(mModel.opacity * 255));
        canvas.drawRoundRect(mBounds, corner, corner, mPaint);

        // 2. Outer Border with subtle accent glow
        mPaint.setStyle(Paint.Style.STROKE);
        mPaint.setStrokeWidth(Math.max(1, Math.round(1.5f * density)));
        int activeAccent = mModel.activeColor != 0 ? mModel.activeColor : 0xFF80DEEA;
        mPaint.setColor(mIsTouching ? activeAccent : 0x6680DEEA);
        canvas.drawRoundRect(mBounds, corner, corner, mPaint);

        // 3. Arrow indicators and sliding thumb
        mPaint.setStyle(Paint.Style.FILL);
        mPaint.setColor(mIsTouching ? activeAccent : 0xAA80DEEA);

        if (!isHoriz) {
            // ===== VERTICAL =====
            float cx = w * 0.5f;
            float arrowPad = Math.max(4f * density, h * 0.04f);
            float arrowSize = Math.min(w * 0.30f, 8f * density);

            // Top Arrow (▲)
            mArrowPath1.reset();
            mArrowPath1.moveTo(cx, arrowPad);
            mArrowPath1.lineTo(cx - arrowSize, arrowPad + arrowSize * 1.1f);
            mArrowPath1.lineTo(cx + arrowSize, arrowPad + arrowSize * 1.1f);
            mArrowPath1.close();
            canvas.drawPath(mArrowPath1, mPaint);

            // Bottom Arrow (▼)
            mArrowPath2.reset();
            mArrowPath2.moveTo(cx, h - arrowPad);
            mArrowPath2.lineTo(cx - arrowSize, h - arrowPad - arrowSize * 1.1f);
            mArrowPath2.lineTo(cx + arrowSize, h - arrowPad - arrowSize * 1.1f);
            mArrowPath2.close();
            canvas.drawPath(mArrowPath2, mPaint);

            // Sliding Thumb Pill
            float reservedEnds = arrowPad + arrowSize * 1.1f + 4f * density;
            float availableTrack = h - 2 * reservedEnds;
            float thumbH = Math.max(18f * density, Math.min(availableTrack * 0.40f, 40f * density));
            float thumbW = Math.max(8f * density, w - 8f * density);
            float maxTravel = (availableTrack - thumbH) * 0.5f;
            if (maxTravel < 0) maxTravel = 0;
            mThumbOffset = Math.max(-maxTravel, Math.min(maxTravel, mThumbOffset));
            float thumbY = (h - thumbH) * 0.5f + mThumbOffset;

            mThumbBounds.set(4f * density, thumbY, 4f * density + thumbW, thumbY + thumbH);
            mPaint.setColor(mIsTouching ? 0xEE80DEEA : 0x6680DEEA);
            canvas.drawRoundRect(mThumbBounds, thumbW * 0.5f, thumbW * 0.5f, mPaint);

            // Tactile grip grooves on thumb
            mPaint.setColor(mIsTouching ? 0xFFFFFFFF : 0x88FFFFFF);
            mPaint.setStyle(Paint.Style.STROKE);
            mPaint.setStrokeWidth(1.2f * density);
            float thumbCy = thumbY + thumbH * 0.5f;
            float lineW = thumbW * 0.35f;
            canvas.drawLine(cx - lineW, thumbCy - 3f * density, cx + lineW, thumbCy - 3f * density, mPaint);
            canvas.drawLine(cx - lineW, thumbCy, cx + lineW, thumbCy, mPaint);
            canvas.drawLine(cx - lineW, thumbCy + 3f * density, cx + lineW, thumbCy + 3f * density, mPaint);
            mPaint.setStyle(Paint.Style.FILL);
        } else {
            // ===== HORIZONTAL =====
            float cy = h * 0.5f;
            float arrowPad = Math.max(4f * density, w * 0.04f);
            float arrowSize = Math.min(h * 0.30f, 8f * density);

            // Left Arrow (◀)
            mArrowPath1.reset();
            mArrowPath1.moveTo(arrowPad, cy);
            mArrowPath1.lineTo(arrowPad + arrowSize * 1.1f, cy - arrowSize);
            mArrowPath1.lineTo(arrowPad + arrowSize * 1.1f, cy + arrowSize);
            mArrowPath1.close();
            canvas.drawPath(mArrowPath1, mPaint);

            // Right Arrow (▶)
            mArrowPath2.reset();
            mArrowPath2.moveTo(w - arrowPad, cy);
            mArrowPath2.lineTo(w - arrowPad - arrowSize * 1.1f, cy - arrowSize);
            mArrowPath2.lineTo(w - arrowPad - arrowSize * 1.1f, cy + arrowSize);
            mArrowPath2.close();
            canvas.drawPath(mArrowPath2, mPaint);

            // Sliding Thumb Pill
            float reservedEnds = arrowPad + arrowSize * 1.1f + 4f * density;
            float availableTrack = w - 2 * reservedEnds;
            float thumbW = Math.max(18f * density, Math.min(availableTrack * 0.40f, 40f * density));
            float thumbH = Math.max(8f * density, h - 8f * density);
            float maxTravel = (availableTrack - thumbW) * 0.5f;
            if (maxTravel < 0) maxTravel = 0;
            mThumbOffset = Math.max(-maxTravel, Math.min(maxTravel, mThumbOffset));
            float thumbX = (w - thumbW) * 0.5f + mThumbOffset;

            mThumbBounds.set(thumbX, 4f * density, thumbX + thumbW, 4f * density + thumbH);
            mPaint.setColor(mIsTouching ? 0xEE80DEEA : 0x6680DEEA);
            canvas.drawRoundRect(mThumbBounds, thumbH * 0.5f, thumbH * 0.5f, mPaint);

            // Tactile grip grooves on thumb
            mPaint.setColor(mIsTouching ? 0xFFFFFFFF : 0x88FFFFFF);
            mPaint.setStyle(Paint.Style.STROKE);
            mPaint.setStrokeWidth(1.2f * density);
            float thumbCx = thumbX + thumbW * 0.5f;
            float lineH = thumbH * 0.35f;
            canvas.drawLine(thumbCx - 3f * density, cy - lineH, thumbCx - 3f * density, cy + lineH, mPaint);
            canvas.drawLine(thumbCx, cy - lineH, thumbCx, cy + lineH, mPaint);
            canvas.drawLine(thumbCx + 3f * density, cy - lineH, thumbCx + 3f * density, cy + lineH, mPaint);
            mPaint.setStyle(Paint.Style.FILL);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (mInEditMode) return false;

        float density = getResources().getDisplayMetrics().density;
        boolean isHoriz = isHorizontal();
        float pos = isHoriz ? event.getX() : event.getY();
        float totalLen = isHoriz ? getWidth() : getHeight();

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                mIsTouching = true;
                mLastTouchPos = pos;
                mAccumulatedDrag = 0f;
                mThumbOffset = 0f;
                if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK);

                // Tap ends detection
                if (pos < totalLen * 0.25f && mDispatcher != null) {
                    // Tap top or left = scroll up
                    if (isHoriz && "horizontal".equalsIgnoreCase(mModel.trackpointMode)) {
                        mDispatcher.onPointerScroll(1, -2.0f);
                    } else {
                        mDispatcher.onPointerScroll(0, 2.0f);
                    }
                } else if (pos > totalLen * 0.75f && mDispatcher != null) {
                    // Tap bottom or right = scroll down
                    if (isHoriz && "horizontal".equalsIgnoreCase(mModel.trackpointMode)) {
                        mDispatcher.onPointerScroll(1, 2.0f);
                    } else {
                        mDispatcher.onPointerScroll(0, -2.0f);
                    }
                }
                invalidate();
                return true;
            }

            case MotionEvent.ACTION_MOVE: {
                if (mDispatcher == null) return true;
                float delta = pos - mLastTouchPos;
                mLastTouchPos = pos;
                mThumbOffset += delta * 0.6f;

                float sens = (mModel.trackpointSensitivity > 0) ? mModel.trackpointSensitivity : 1.5f;
                if (!isHoriz) {
                    // Drag up (delta < 0) -> scroll up (+); drag down (delta > 0) -> scroll down (-)
                    mDispatcher.onPointerScroll(0, -delta * sens * 0.25f);
                } else {
                    // Drag left (delta < 0) -> scroll up (+); drag right (delta > 0) -> scroll down (-)
                    if ("horizontal".equalsIgnoreCase(mModel.trackpointMode)) {
                        mDispatcher.onPointerScroll(1, delta * sens * 0.25f);
                    } else {
                        mDispatcher.onPointerScroll(0, -delta * sens * 0.25f);
                    }
                }

                mAccumulatedDrag += Math.abs(delta);
                if (mAccumulatedDrag > 12f * density) {
                    mAccumulatedDrag = 0f;
                    if (isHapticEnabled()) performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK);
                }
                invalidate();
                return true;
            }

            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL: {
                mIsTouching = false;
                mThumbOffset = 0f;
                invalidate();
                return true;
            }
        }
        return super.onTouchEvent(event);
    }
}
