package com.anland.consumer.hud;

import android.graphics.RectF;

import java.util.ArrayList;
import java.util.List;

/**
 * Clean magnetic snapping engine inspired by Zalith Launcher 2 & PojavLauncher.
 *
 * Design principles learned from Zalith Launcher:
 * 1. Zero grid interference: Does not force an invisible coarse grid during dragging.
 *    Dragging is 100% fluid and 1:1 with finger touch unless approaching an alignment target.
 * 2. Crisp magnetic threshold (~6dp): Magnetic pull engages only when close to an edge or center.
 * 3. Minimalist guidelines: Only draws lines for true alignment axes (single nearest match),
 *    never flooding the screen with self-bounding boxes or spurious grid lines.
 * 4. Dual alignment support: Both adjacent (flush touching / 4dp gap) and co-linear (edge / center matching).
 */
public final class SnapGeometryEngine {
    public static final class SnapResult {
        public float snappedX;
        public float snappedY;
        public boolean hasSnapX;
        public boolean hasSnapY;
        public final List<Float> verticalGuidelines = new ArrayList<>();
        public final List<Float> horizontalGuidelines = new ArrayList<>();
    }

    private static final class SnapCandidate {
        final float targetCoord;
        final float guideCoord;
        final float distance;

        SnapCandidate(float targetCoord, float guideCoord, float distance) {
            this.targetCoord = targetCoord;
            this.guideCoord = guideCoord;
            this.distance = distance;
        }
    }

    private final float mDensity;
    private final float mSnapThresholdPx;
    private final float mGapPx;

    public SnapGeometryEngine(float density) {
        this.mDensity = density;
        // 6dp magnetic snap threshold (matches Zalith Launcher 4-6dp sweet spot)
        this.mSnapThresholdPx = 6f * density;
        this.mGapPx = 4f * density;
    }

    /**
     * Calculates the snapped position for the active button bounding box against screen bounds and siblings.
     */
    public SnapResult computeSnap(
            RectF candidate,
            List<RectF> siblings,
            float screenWidth,
            float screenHeight,
            float dockTopBoundary
    ) {
        SnapResult res = new SnapResult();
        float x = candidate.left;
        float y = candidate.top;
        float w = candidate.width();
        float h = candidate.height();
        float effectiveMaxY = dockTopBoundary > 0 ? (dockTopBoundary - h) : (screenHeight - h);

        List<SnapCandidate> candidatesX = new ArrayList<>();
        List<SnapCandidate> candidatesY = new ArrayList<>();

        // -------------------------------------------------------------
        // 1. Screen Alignment Targets (Left, Right, Center X)
        // -------------------------------------------------------------
        // Screen Left (0)
        float dScreenL = Math.abs(x);
        if (dScreenL < mSnapThresholdPx) {
            candidatesX.add(new SnapCandidate(0f, 0f, dScreenL));
        }
        // Screen Right (screenWidth - w)
        float dScreenR = Math.abs(x + w - screenWidth);
        if (dScreenR < mSnapThresholdPx) {
            candidatesX.add(new SnapCandidate(screenWidth - w, screenWidth, dScreenR));
        }
        // Screen Center X
        float screenMidX = screenWidth * 0.5f;
        float dScreenMidX = Math.abs((x + w * 0.5f) - screenMidX);
        if (dScreenMidX < mSnapThresholdPx) {
            candidatesX.add(new SnapCandidate(screenMidX - w * 0.5f, screenMidX, dScreenMidX));
        }

        // -------------------------------------------------------------
        // 2. Screen Alignment Targets (Top, Bottom, Center Y)
        // -------------------------------------------------------------
        // Screen Top (0)
        float dScreenT = Math.abs(y);
        if (dScreenT < mSnapThresholdPx) {
            candidatesY.add(new SnapCandidate(0f, 0f, dScreenT));
        }
        // Screen Bottom (effectiveMaxY)
        float dScreenB = Math.abs(y - effectiveMaxY);
        if (dScreenB < mSnapThresholdPx) {
            candidatesY.add(new SnapCandidate(effectiveMaxY, effectiveMaxY + h, dScreenB));
        }
        // Screen Center Y
        float screenMidY = screenHeight * 0.5f;
        float dScreenMidY = Math.abs((y + h * 0.5f) - screenMidY);
        if (dScreenMidY < mSnapThresholdPx) {
            candidatesY.add(new SnapCandidate(screenMidY - h * 0.5f, screenMidY, dScreenMidY));
        }

        // -------------------------------------------------------------
        // 3. Sibling Alignment Targets (Zalith Launcher algorithm)
        // -------------------------------------------------------------
        if (siblings != null) {
            for (RectF s : siblings) {
                // Sibling Left-to-Left (Aligned left edge)
                float dLL = Math.abs(x - s.left);
                if (dLL < mSnapThresholdPx) {
                    candidatesX.add(new SnapCandidate(s.left, s.left, dLL));
                }

                // Sibling Right-to-Right (Aligned right edge)
                float dRR = Math.abs((x + w) - s.right);
                if (dRR < mSnapThresholdPx) {
                    candidatesX.add(new SnapCandidate(s.right - w, s.right, dRR));
                }

                // Sibling Center-to-Center X
                float dCCX = Math.abs((x + w * 0.5f) - s.centerX());
                if (dCCX < mSnapThresholdPx) {
                    candidatesX.add(new SnapCandidate(s.centerX() - w * 0.5f, s.centerX(), dCCX));
                }

                // Sibling Adjacent: My Left to Sibling Right (flush or 4dp gap)
                float dAdjR_flush = Math.abs(x - s.right);
                if (dAdjR_flush < mSnapThresholdPx) {
                    candidatesX.add(new SnapCandidate(s.right, s.right, dAdjR_flush));
                }
                float dAdjR_gap = Math.abs(x - (s.right + mGapPx));
                if (dAdjR_gap < mSnapThresholdPx) {
                    candidatesX.add(new SnapCandidate(s.right + mGapPx, s.right, dAdjR_gap));
                }

                // Sibling Adjacent: My Right to Sibling Left (flush or 4dp gap)
                float dAdjL_flush = Math.abs((x + w) - s.left);
                if (dAdjL_flush < mSnapThresholdPx) {
                    candidatesX.add(new SnapCandidate(s.left - w, s.left, dAdjL_flush));
                }
                float dAdjL_gap = Math.abs((x + w) - (s.left - mGapPx));
                if (dAdjL_gap < mSnapThresholdPx) {
                    candidatesX.add(new SnapCandidate(s.left - mGapPx - w, s.left, dAdjL_gap));
                }

                // Sibling Top-to-Top (Aligned top edge)
                float dTT = Math.abs(y - s.top);
                if (dTT < mSnapThresholdPx) {
                    candidatesY.add(new SnapCandidate(s.top, s.top, dTT));
                }

                // Sibling Bottom-to-Bottom (Aligned bottom edge)
                float dBB = Math.abs((y + h) - s.bottom);
                if (dBB < mSnapThresholdPx) {
                    candidatesY.add(new SnapCandidate(s.bottom - h, s.bottom, dBB));
                }

                // Sibling Center-to-Center Y
                float dCCY = Math.abs((y + h * 0.5f) - s.centerY());
                if (dCCY < mSnapThresholdPx) {
                    candidatesY.add(new SnapCandidate(s.centerY() - h * 0.5f, s.centerY(), dCCY));
                }

                // Sibling Adjacent: My Top to Sibling Bottom (flush or 4dp gap)
                float dAdjB_flush = Math.abs(y - s.bottom);
                if (dAdjB_flush < mSnapThresholdPx) {
                    candidatesY.add(new SnapCandidate(s.bottom, s.bottom, dAdjB_flush));
                }
                float dAdjB_gap = Math.abs(y - (s.bottom + mGapPx));
                if (dAdjB_gap < mSnapThresholdPx) {
                    candidatesY.add(new SnapCandidate(s.bottom + mGapPx, s.bottom, dAdjB_gap));
                }

                // Sibling Adjacent: My Bottom to Sibling Top (flush or 4dp gap)
                float dAdjT_flush = Math.abs((y + h) - s.top);
                if (dAdjT_flush < mSnapThresholdPx) {
                    candidatesY.add(new SnapCandidate(s.top - h, s.top, dAdjT_flush));
                }
                float dAdjT_gap = Math.abs((y + h) - (s.top - mGapPx));
                if (dAdjT_gap < mSnapThresholdPx) {
                    candidatesY.add(new SnapCandidate(s.top - mGapPx - h, s.top, dAdjT_gap));
                }
            }
        }

        // -------------------------------------------------------------
        // 4. Select the Single Best Candidate per Axis (Minimum distance)
        // -------------------------------------------------------------
        SnapCandidate bestX = null;
        for (SnapCandidate c : candidatesX) {
            if (bestX == null || c.distance < bestX.distance) {
                bestX = c;
            }
        }

        SnapCandidate bestY = null;
        for (SnapCandidate c : candidatesY) {
            if (bestY == null || c.distance < bestY.distance) {
                bestY = c;
            }
        }

        if (bestX != null) {
            x = bestX.targetCoord;
            res.verticalGuidelines.add(bestX.guideCoord);
            res.hasSnapX = true;
        }

        if (bestY != null) {
            y = bestY.targetCoord;
            res.horizontalGuidelines.add(bestY.guideCoord);
            res.hasSnapY = true;
        }

        // Clamp to screen boundaries
        x = Math.max(0, Math.min(screenWidth - w, x));
        y = Math.max(0, Math.min(effectiveMaxY, y));

        res.snappedX = x;
        res.snappedY = y;
        return res;
    }
}
