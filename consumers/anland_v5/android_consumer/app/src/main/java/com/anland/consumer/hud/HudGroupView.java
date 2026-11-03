package com.anland.consumer.hud;

import com.anland.consumer.R;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.View;
import android.widget.FrameLayout;

/**
 * A container widget that keeps several keys together so they can be moved,
 * snapped and duplicated as a single unit.
 *
 * The children are ordinary {@link HudFreeformButtonView}s. They are arranged
 * automatically in a centred grid inside the group's content box, so the user
 * never has to position them by hand: dropping a key on the group is enough.
 * Because they are real child views, dragging the group drags everything with
 * it — no per-child bookkeeping is needed.
 */
public final class HudGroupView extends FrameLayout {

    /** Gap between grid cells, in dp. */
    private static final int GAP_DP = 4;
    /** Minimum inner height kept free when the group still has no keys. */
    private static final int EMPTY_MIN_HEIGHT_DP = 48;

    private final HudButton mModel;
    private final HudFreeformButtonView.ButtonActionListener mListener;
    private final Paint mFillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mBorderPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint mHintPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF mRect = new RectF();
    private boolean mInEditMode = false;
    private boolean mDropTarget = false;

    public HudGroupView(Context context, HudButton model,
                        HudFreeformButtonView.ButtonActionListener listener) {
        super(context);
        this.mModel = model;
        this.mListener = listener;
        setClipChildren(false);
        setClipToPadding(false);
        mFillPaint.setStyle(Paint.Style.FILL);
        mBorderPaint.setStyle(Paint.Style.STROKE);
        mBorderPaint.setPathEffect(new DashPathEffect(new float[]{dp(6), dp(4)}, 0));
        mHintPaint.setTextAlign(Paint.Align.CENTER);
        setWillNotDraw(false);
        buildChildren();
    }

    public HudButton getModel() {
        return mModel;
    }

    public void setInEditMode(boolean editMode) {
        mInEditMode = editMode;
        for (int i = 0; i < getChildCount(); i++) {
            View child = getChildAt(i);
            if (child instanceof HudFreeformButtonView)
                ((HudFreeformButtonView) child).setInEditMode(editMode);
        }
        invalidate();
    }

    /** Highlight the container while a key is dragged over it. */
    public void setDropTarget(boolean dropTarget) {
        if (mDropTarget == dropTarget) return;
        mDropTarget = dropTarget;
        invalidate();
    }

    /** Re-read the model after the editor added, removed or rebound a child. */
    public void rebuildChildren() {
        buildChildren();
        requestLayout();
        invalidate();
    }

    private void buildChildren() {
        removeAllViews();
        float density = getResources().getDisplayMetrics().density;
        for (HudButton child : mModel.children) {
            HudFreeformButtonView view = new HudFreeformButtonView(getContext(), child, mListener);
            view.setInEditMode(mInEditMode);
            int w = Math.max(1, Math.round(child.widthDp * density));
            int h = Math.max(1, Math.round(child.heightDp * density));
            addView(view, new FrameLayout.LayoutParams(w, h));
        }
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private int padPx() {
        return Math.max(0, dp(mModel.padDp));
    }

    /** Widest / tallest child, i.e. the size of one grid cell. */
    private int cellWidth() {
        int max = 0;
        for (int i = 0; i < getChildCount(); i++)
            max = Math.max(max, getChildAt(i).getMeasuredWidth());
        return max;
    }

    private int cellHeight() {
        int max = 0;
        for (int i = 0; i < getChildCount(); i++)
            max = Math.max(max, getChildAt(i).getMeasuredHeight());
        return max;
    }

    private static int columnCount(int contentWidth, int cellWidth, int gap, int count) {
        if (cellWidth <= 0) return Math.max(1, count);
        int cols = (contentWidth + gap) / (cellWidth + gap);
        cols = Math.max(1, Math.min(cols, Math.max(1, count)));
        return cols;
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        float density = getResources().getDisplayMetrics().density;
        int width = Math.max(1, Math.round(mModel.widthDp * density));
        int height = Math.max(1, Math.round(mModel.heightDp * density));
        int pad = padPx();
        int gap = dp(GAP_DP);
        int count = getChildCount();

        int contentWidth = Math.max(1, width - pad * 2);
        int cellW = 0;
        int cellH = 0;
        for (int i = 0; i < count; i++) {
            View child = getChildAt(i);
            // Children are measured below by super.onMeasure(), so use their
            // layout-params size here — that value is authoritative because
            // HudFreeformButtonView always measures itself from the model.
            cellW = Math.max(cellW, child.getLayoutParams().width);
            cellH = Math.max(cellH, child.getLayoutParams().height);
        }

        int needed;
        if (count == 0 || cellH <= 0) {
            needed = pad * 2 + dp(EMPTY_MIN_HEIGHT_DP);
        } else {
            int cols = columnCount(contentWidth, cellW, gap, count);
            int rows = (count + cols - 1) / cols;
            needed = pad * 2 + rows * cellH + Math.max(0, rows - 1) * gap;
        }
        if (needed > height) height = needed;
        // Never allow the container to be smaller than the widest key needs.
        int minWidth = pad * 2 + cellW;
        if (count > 0 && minWidth > width) width = minWidth;

        // Keep the model in sync with what we actually render, otherwise the
        // saved profile would store a size that clips the children and the
        // group would "jump" the next time it is rebuilt.
        mModel.widthDp = Math.max(1, Math.round(width / density));
        mModel.heightDp = Math.max(1, Math.round(height / density));

        int wSpec = MeasureSpec.makeMeasureSpec(width, MeasureSpec.EXACTLY);
        int hSpec = MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY);
        super.onMeasure(wSpec, hSpec);
        setMeasuredDimension(width, height);
    }

    @Override
    protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        int count = getChildCount();
        if (count == 0) return;
        int pad = padPx();
        int gap = dp(GAP_DP);
        int contentWidth = Math.max(1, getMeasuredWidth() - pad * 2);
        int contentHeight = Math.max(1, getMeasuredHeight() - pad * 2);

        int cellW = Math.max(1, cellWidth());
        int cellH = Math.max(1, cellHeight());
        int cols = columnCount(contentWidth, cellW, gap, count);
        int rows = (count + cols - 1) / cols;

        int gridW = cols * cellW + Math.max(0, cols - 1) * gap;
        int gridH = rows * cellH + Math.max(0, rows - 1) * gap;
        int originX = pad + Math.max(0, (contentWidth - gridW) / 2);
        int originY = pad + Math.max(0, (contentHeight - gridH) / 2);

        for (int i = 0; i < count; i++) {
            View child = getChildAt(i);
            int row = i / cols;
            int col = i % cols;
            int cellLeft = originX + col * (cellW + gap);
            int cellTop = originY + row * (cellH + gap);
            int cw = child.getMeasuredWidth();
            int ch = child.getMeasuredHeight();
            int cx = cellLeft + (cellW - cw) / 2;
            int cy = cellTop + (cellH - ch) / 2;
            child.layout(cx, cy, cx + cw, cy + ch);
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        // A faint container wash makes the group readable while editing without
        // hiding the desktop behind it.
        float corner = mModel.cornerRadiusDp * getResources().getDisplayMetrics().density;
        mRect.set(1, 1, getWidth() - 1, getHeight() - 1);
        mFillPaint.setColor(mModel.bgColor);
        int alpha = Math.round(Math.min(0.35f, Math.max(0.12f, mModel.opacity)) * 255);
        if (mDropTarget) alpha = Math.min(255, alpha + 70);
        mFillPaint.setAlpha(alpha);
        canvas.drawRoundRect(mRect, corner, corner, mFillPaint);

        if (mInEditMode && getChildCount() == 0) {
            mHintPaint.setColor(0xB3FFFFFF);
            mHintPaint.setTextSize(11f * getResources().getDisplayMetrics().scaledDensity);
            float textY = getHeight() * 0.5f
                    - ((mHintPaint.descent() + mHintPaint.ascent()) * 0.5f);
            canvas.drawText(getContext().getString(R.string.hud_group_drop_hint),
                    getWidth() * 0.5f, textY, mHintPaint);
        }
    }

    @Override
    protected void dispatchDraw(Canvas canvas) {
        super.dispatchDraw(canvas);
        // Border goes on top of the keys so the container stays readable even
        // when the children cover its whole surface.
        float density = getResources().getDisplayMetrics().density;
        float stroke = Math.max(1f, 1.5f * density);
        float corner = mModel.cornerRadiusDp * density;
        mRect.set(stroke, stroke, getWidth() - stroke, getHeight() - stroke);
        if (mDropTarget) {
            mBorderPaint.setPathEffect(null);
            mBorderPaint.setStrokeWidth(stroke);
            mBorderPaint.setColor(0xFF80DEEA);
        } else if (mInEditMode) {
            mBorderPaint.setPathEffect(new DashPathEffect(new float[]{dp(6), dp(4)}, 0));
            mBorderPaint.setStrokeWidth(stroke);
            mBorderPaint.setColor(0x8A80DEEA);
        } else {
            mBorderPaint.setPathEffect(null);
            mBorderPaint.setStrokeWidth(Math.max(1f, density));
            mBorderPaint.setColor(0x33FFFFFF);
        }
        canvas.drawRoundRect(mRect, corner, corner, mBorderPaint);
    }
}
