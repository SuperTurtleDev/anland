package com.anland.consumer.hud;

import android.content.Context;
import com.anland.consumer.R;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * 8-Shape Corner Selector matching Zalith Launcher and user's Alight Motion specification:
 * 1: Top-Left
 * 2: Top-Right
 * 3: Bottom-Left
 * 4: Bottom-Right
 * 5: Left Both (Top-Left + Bottom-Left)
 * 6: Right Both (Top-Right + Bottom-Right)
 * 7: All 4 Corners (Squircle)
 * 8: None (Sharp 90 deg)
 */
public final class HudCornerShapePickerView extends LinearLayout {

    public interface OnShapeSelectedListener {
        void onShapeSelected(int shape);
    }

    private static final int BG_NORMAL = 0xFF3A3A3A;
    private static final int BG_ACTIVE = 0xFF4E495C;
    private static final int STROKE_NORMAL = 0xFF5A5A5A;
    private static final int STROKE_ACTIVE = 0xFF80DEEA; // Cyan accent
    private static final int SHAPE_NORMAL = 0xFFD2D2DC;
    private static final int SHAPE_ACTIVE = 0xFFA39CB8; // Lilac pastel

    private final ShapeCardView[] mCards = new ShapeCardView[8];
    private int mSelectedShape = HudButton.CORNER_ALL;
    private OnShapeSelectedListener mListener;

    private static final class ShapeDef {
        final int shape;
        final int title;

        ShapeDef(int shape, int title) {
            this.shape = shape;
            this.title = title;
        }
    }

    private static final ShapeDef[] SHAPES = new ShapeDef[] {
            new ShapeDef(HudButton.CORNER_TOP_LEFT, R.string.hud_editor_shape_top_left),
            new ShapeDef(HudButton.CORNER_TOP_RIGHT, R.string.hud_editor_shape_top_right),
            new ShapeDef(HudButton.CORNER_BOTTOM_LEFT, R.string.hud_editor_shape_bottom_left),
            new ShapeDef(HudButton.CORNER_BOTTOM_RIGHT, R.string.hud_editor_shape_bottom_right),
            new ShapeDef(HudButton.CORNER_LEFT_BOTH, R.string.hud_editor_shape_left),
            new ShapeDef(HudButton.CORNER_RIGHT_BOTH, R.string.hud_editor_shape_right),
            new ShapeDef(HudButton.CORNER_ALL, R.string.hud_editor_shape_all),
            new ShapeDef(HudButton.CORNER_NONE, R.string.hud_editor_shape_none)
    };

    private GridLayout mGrid;

    public HudCornerShapePickerView(Context context) {
        super(context);
        initView();
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void initView() {
        setOrientation(VERTICAL);

        mGrid = new GridLayout(getContext());
        mGrid.setColumnCount(4);
        mGrid.setRowCount(GridLayout.UNDEFINED);
        mGrid.setAlignmentMode(GridLayout.ALIGN_BOUNDS);

        for (int i = 0; i < SHAPES.length; i++) {
            ShapeDef def = SHAPES[i];
            ShapeCardView card = new ShapeCardView(getContext(), def.shape, getContext().getString(def.title));
            final int shapeId = def.shape;
            card.setOnClickListener(v -> {
                setSelectedShape(shapeId);
                if (mListener != null) {
                    mListener.onShapeSelected(shapeId);
                }
            });
            mCards[i] = card;

            GridLayout.LayoutParams glp = new GridLayout.LayoutParams();
            glp.width = 0;
            glp.height = ViewGroup.LayoutParams.WRAP_CONTENT;
            glp.columnSpec = GridLayout.spec(i % 4, 1f);
            glp.rowSpec = GridLayout.spec(i / 4);
            glp.setMargins(dp(3), dp(3), dp(3), dp(3));
            mGrid.addView(card, glp);
        }

        addView(mGrid, new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        updateSelectionState();
    }

    public void setColumnCountForOrientation(boolean isLandscape) {
        if (mGrid != null) {
            int cols = isLandscape ? 4 : 2;
            for (int i = 0; i < mCards.length; i++) {
                if (mCards[i] != null) {
                    GridLayout.LayoutParams glp = (GridLayout.LayoutParams) mCards[i].getLayoutParams();
                    if (glp != null) {
                        glp.columnSpec = GridLayout.spec(i % cols, 1f);
                        glp.rowSpec = GridLayout.spec(i / cols);
                    }
                }
            }
            mGrid.setColumnCount(cols);
        }
    }

    public void setOnShapeSelectedListener(OnShapeSelectedListener listener) {
        this.mListener = listener;
    }

    public void setSelectedShape(int shape) {
        if (mSelectedShape != shape) {
            mSelectedShape = shape;
            updateSelectionState();
        }
    }

    public int getSelectedShape() {
        return mSelectedShape;
    }

    private void updateSelectionState() {
        for (ShapeCardView card : mCards) {
            if (card != null) {
                card.setSelected(card.mShape == mSelectedShape);
            }
        }
    }

    private final class ShapeCardView extends LinearLayout {
        private final int mShape;
        private final TextView mTitleView;
        private final MiniShapePreview mIconPreview;

        ShapeCardView(Context context, int shape, String title) {
            super(context);
            this.mShape = shape;
            setOrientation(VERTICAL);
            setGravity(Gravity.CENTER);
            setPadding(dp(4), dp(6), dp(4), dp(6));

            mIconPreview = new MiniShapePreview(context, shape);
            addView(mIconPreview, new LayoutParams(dp(44), dp(22)));

            mTitleView = new TextView(context);
            mTitleView.setText(title);
            mTitleView.setTextSize(9);
            mTitleView.setGravity(Gravity.CENTER);
            mTitleView.setSingleLine(true);
            mTitleView.setEllipsize(android.text.TextUtils.TruncateAt.END);
            LayoutParams tlp = new LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            tlp.topMargin = dp(4);
            addView(mTitleView, tlp);

            updateCardBackground(false);
        }

        @Override
        public void setSelected(boolean selected) {
            super.setSelected(selected);
            updateCardBackground(selected);
            mTitleView.setTextColor(selected ? Color.WHITE : 0xFFC3C3CD);
            mIconPreview.setActive(selected);
        }

        private void updateCardBackground(boolean selected) {
            android.graphics.drawable.GradientDrawable gd = new android.graphics.drawable.GradientDrawable();
            gd.setCornerRadius(dp(8));
            gd.setColor(selected ? BG_ACTIVE : BG_NORMAL);
            gd.setStroke(dp(selected ? 1.5f : 1.0f), selected ? STROKE_ACTIVE : STROKE_NORMAL);
            setBackground(gd);
        }
    }

    private static final class MiniShapePreview extends View {
        private final int mShape;
        private boolean mIsActive;
        private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF mBounds = new RectF();
        private final Path mPath = new Path();
        private final float[] mRadii = new float[8];

        MiniShapePreview(Context context, int shape) {
            super(context);
            this.mShape = shape;
        }

        void setActive(boolean active) {
            if (mIsActive != active) {
                mIsActive = active;
                invalidate();
            }
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float w = getWidth();
            float h = getHeight();
            if (w <= 0 || h <= 0) return;

            mBounds.set(1, 1, w - 1, h - 1);
            float r = Math.min(w, h) * 0.45f;
            HudFreeformButtonView.computeRadii(mShape, r, mRadii);

            mPath.reset();
            mPath.addRoundRect(mBounds, mRadii, Path.Direction.CW);

            mPaint.setStyle(Paint.Style.FILL);
            mPaint.setColor(mIsActive ? SHAPE_ACTIVE : SHAPE_NORMAL);
            canvas.drawPath(mPath, mPaint);
        }
    }
}
