package com.anland.consumer.hud;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

/**
 * Premium Handheld Console D-Pad widget with sliding continuous direction tracking.
 * Features:
 * - Authentic 3D sculpted 4-way cross arms with tactile center dish (Steam Deck / DualSense style).
 * - Smooth sliding between directions with active sector illumination.
 * - Simultaneous diagonal sector activation (UP+LEFT, UP+RIGHT, DOWN+LEFT, DOWN+RIGHT).
 * - Per-widget haptic feedback toggle.
 */
public final class HudDpadView extends View {

    public static final int DIR_UP = 1;
    public static final int DIR_DOWN = 2;
    public static final int DIR_LEFT = 3;
    public static final int DIR_RIGHT = 4;

    public interface DpadDispatcher {
        void onDpadPress(int direction, boolean pressed);
    }

    private final HudButton mModel;
    private final DpadDispatcher mDispatcher;
    private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mArmPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mArrowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF mArmRect = new RectF();

    private boolean mInEditMode = false;
    private boolean mIsPressed = false;
    private boolean mUpActive = false;
    private boolean mDownActive = false;
    private boolean mLeftActive = false;
    private boolean mRightActive = false;

    // Dead zone fraction
    private static final float DEADZONE_FRAC = 0.18f;

    public HudDpadView(Context context, HudButton model, DpadDispatcher dispatcher) {
        super(context);
        this.mModel = model;
        this.mDispatcher = dispatcher;

        mTextPaint.setColor(Color.WHITE);
        mTextPaint.setTextAlign(Paint.Align.CENTER);
        mTextPaint.setFakeBoldText(true);

        mArrowPaint.setStyle(Paint.Style.FILL);
    }

    public void setInEditMode(boolean editMode) {
        mInEditMode = editMode;
        if (editMode) releaseAll();
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
        if (w <= 0 || h <= 0) return;

        float cx = w * 0.5f;
        float cy = h * 0.5f;
        float minDim = Math.min(w, h);
        float radius = minDim * 0.5f - 2f * density;
        int activeAccent = mModel.activeColor != 0 ? mModel.activeColor : 0xFF80DEEA;
        int baseBg = mModel.bgColor != 0 ? mModel.bgColor : 0xE61A1C29;

        // 1. Outer Circular Bezel / Dish
        mPaint.setStyle(Paint.Style.FILL);
        mPaint.setColor(baseBg);
        mPaint.setAlpha(Math.round(mModel.opacity * 255));
        canvas.drawCircle(cx, cy, radius, mPaint);

        // Outer Bezel Rim
        mPaint.setStyle(Paint.Style.STROKE);
        mPaint.setStrokeWidth(Math.max(1, Math.round(1.5f * density)));
        mPaint.setColor(mIsPressed ? activeAccent : 0x5580DEEA);
        canvas.drawCircle(cx, cy, radius, mPaint);

        // Inner Tactile Bevel Ring
        float innerBevelR = radius * 0.88f;
        mPaint.setStrokeWidth(1.0f * density);
        mPaint.setColor(0x22FFFFFF);
        canvas.drawCircle(cx, cy, innerBevelR, mPaint);

        // 2. Sculpted Cross Arms (Top, Bottom, Left, Right)
        float armWidth = minDim * 0.32f;
        float armLength = radius * 0.85f;
        float armCorner = 8f * density;

        // Draw Vertical Cross Bar
        mArmRect.set(cx - armWidth * 0.5f, cy - armLength, cx + armWidth * 0.5f, cy + armLength);
        mArmPaint.setStyle(Paint.Style.FILL);
        mArmPaint.setColor(0x33000000);
        canvas.drawRoundRect(mArmRect, armCorner, armCorner, mArmPaint);

        // Draw Horizontal Cross Bar
        mArmRect.set(cx - armLength, cy - armWidth * 0.5f, cx + armLength, cy + armWidth * 0.5f);
        canvas.drawRoundRect(mArmRect, armCorner, armCorner, mArmPaint);

        // Draw Active Arm Highlights
        if (mUpActive) {
            drawActiveArmSegment(canvas, cx - armWidth * 0.5f, cy - armLength, cx + armWidth * 0.5f, cy, armCorner, activeAccent);
        }
        if (mDownActive) {
            drawActiveArmSegment(canvas, cx - armWidth * 0.5f, cy, cx + armWidth * 0.5f, cy + armLength, armCorner, activeAccent);
        }
        if (mLeftActive) {
            drawActiveArmSegment(canvas, cx - armLength, cy - armWidth * 0.5f, cx, cy + armWidth * 0.5f, armCorner, activeAccent);
        }
        if (mRightActive) {
            drawActiveArmSegment(canvas, cx, cy - armWidth * 0.5f, cx + armLength, cy + armWidth * 0.5f, armCorner, activeAccent);
        }

        // Cross Arm Outlines
        mArmPaint.setStyle(Paint.Style.STROKE);
        mArmPaint.setStrokeWidth(1.2f * density);
        mArmPaint.setColor(0x44FFFFFF);
        mArmRect.set(cx - armWidth * 0.5f, cy - armLength, cx + armWidth * 0.5f, cy + armLength);
        canvas.drawRoundRect(mArmRect, armCorner, armCorner, mArmPaint);
        mArmRect.set(cx - armLength, cy - armWidth * 0.5f, cx + armLength, cy + armWidth * 0.5f);
        canvas.drawRoundRect(mArmRect, armCorner, armCorner, mArmPaint);

        // 3. Directional Chevrons
        float arrowOffset = armLength * 0.65f;
        float arrowSize = minDim * 0.12f;

        // UP
        drawChevron(canvas, cx, cy - arrowOffset, arrowSize, 0, mUpActive, activeAccent);
        // DOWN
        drawChevron(canvas, cx, cy + arrowOffset, arrowSize, 180, mDownActive, activeAccent);
        // LEFT
        drawChevron(canvas, cx - arrowOffset, cy, arrowSize, 270, mLeftActive, activeAccent);
        // RIGHT
        drawChevron(canvas, cx + arrowOffset, cy, arrowSize, 90, mRightActive, activeAccent);

        // 4. Center Concave Pivot Dish
        float pivotR = armWidth * 0.36f;
        mPaint.setStyle(Paint.Style.FILL);
        mPaint.setColor(mIsPressed ? 0x6622283A : 0xDD1B1D2B);
        canvas.drawCircle(cx, cy, pivotR, mPaint);
        mPaint.setStyle(Paint.Style.STROKE);
        mPaint.setStrokeWidth(1.2f * density);
        mPaint.setColor(mIsPressed ? activeAccent : 0x4480DEEA);
        canvas.drawCircle(cx, cy, pivotR, mPaint);

        // Center jewel dot
        mPaint.setStyle(Paint.Style.FILL);
        mPaint.setColor(mIsPressed ? activeAccent : 0x66FFFFFF);
        canvas.drawCircle(cx, cy, 3f * density, mPaint);
    }

    private void drawActiveArmSegment(Canvas canvas, float l, float t, float r, float b, float corner, int color) {
        mArmRect.set(l, t, r, b);
        mArmPaint.setStyle(Paint.Style.FILL);
        mArmPaint.setColor(color);
        mArmPaint.setAlpha(100);
        canvas.drawRoundRect(mArmRect, corner, corner, mArmPaint);
    }

    private void drawChevron(Canvas canvas, float cx, float cy, float size, float rotation, boolean active, int activeColor) {
        Path path = new Path();
        path.moveTo(0, -size * 0.55f);
        path.lineTo(-size * 0.5f, size * 0.35f);
        path.lineTo(-size * 0.28f, size * 0.35f);
        path.lineTo(0, -size * 0.1f);
        path.lineTo(size * 0.28f, size * 0.35f);
        path.lineTo(size * 0.5f, size * 0.35f);
        path.close();

        canvas.save();
        canvas.translate(cx, cy);
        canvas.rotate(rotation);
        mArrowPaint.setColor(active ? activeColor : 0xDDFFFFFF);
        canvas.drawPath(path, mArrowPaint);
        canvas.restore();
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (mInEditMode) return false;

        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                mIsPressed = true;
                updateDirection(event.getX(), event.getY());
                return true;

            case MotionEvent.ACTION_MOVE:
                updateDirection(event.getX(), event.getY());
                return true;

            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                mIsPressed = false;
                releaseAll();
                invalidate();
                return true;
        }
        return super.onTouchEvent(event);
    }

    private void updateDirection(float x, float y) {
        float w = getWidth();
        float h = getHeight();
        float cx = w * 0.5f;
        float cy = h * 0.5f;
        float dx = x - cx;
        float dy = y - cy;
        float dist = (float) Math.sqrt(dx * dx + dy * dy);
        float maxR = Math.min(w, h) * 0.5f;

        boolean newUp = false, newDown = false, newLeft = false, newRight = false;

        if (dist > maxR * DEADZONE_FRAC) {
            float angle = (float) Math.toDegrees(Math.atan2(dy, dx));
            if (angle < 0) angle += 360f;

            // Cardinal + diagonal mapping with 45° sectors
            if (inRange(angle, 292.5f, 337.5f)) { newUp = true; newRight = true; }      // UP-RIGHT
            else if (inRange(angle, 247.5f, 292.5f)) { newUp = true; }                  // UP
            else if (inRange(angle, 202.5f, 247.5f)) { newUp = true; newLeft = true; }  // UP-LEFT
            else if (inRange(angle, 157.5f, 202.5f)) { newLeft = true; }                // LEFT
            else if (inRange(angle, 112.5f, 157.5f)) { newDown = true; newLeft = true; }// DOWN-LEFT
            else if (inRange(angle, 67.5f, 112.5f)) { newDown = true; }                 // DOWN
            else if (inRange(angle, 22.5f, 67.5f)) { newDown = true; newRight = true; } // DOWN-RIGHT
            else { newRight = true; }                                                     // RIGHT
        }

        boolean changed = newUp != mUpActive || newDown != mDownActive
                || newLeft != mLeftActive || newRight != mRightActive;
        // Release old directions before pressing their opposites, so a release
        // cannot overwrite the new direction's hat axis with zero.
        if (mUpActive && !newUp) { mUpActive = false; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_UP, false); }
        if (mDownActive && !newDown) { mDownActive = false; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_DOWN, false); }
        if (mLeftActive && !newLeft) { mLeftActive = false; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_LEFT, false); }
        if (mRightActive && !newRight) { mRightActive = false; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_RIGHT, false); }
        if (!mUpActive && newUp) { mUpActive = true; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_UP, true); }
        if (!mDownActive && newDown) { mDownActive = true; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_DOWN, true); }
        if (!mLeftActive && newLeft) { mLeftActive = true; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_LEFT, true); }
        if (!mRightActive && newRight) { mRightActive = true; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_RIGHT, true); }

        if (changed) {
            if (isHapticEnabled()) {
                performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
            }
            invalidate();
        }
    }

    private boolean inRange(float angle, float lo, float hi) {
        return angle >= lo && angle < hi;
    }

    @Override
    protected void onDetachedFromWindow() {
        releaseAll();
        super.onDetachedFromWindow();
    }

    private void releaseAll() {
        if (mUpActive) { mUpActive = false; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_UP, false); }
        if (mDownActive) { mDownActive = false; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_DOWN, false); }
        if (mLeftActive) { mLeftActive = false; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_LEFT, false); }
        if (mRightActive) { mRightActive = false; if (mDispatcher != null) mDispatcher.onDpadPress(DIR_RIGHT, false); }
    }
}
