/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_HWUI_DISPLAY_OPERATION_H
#define ANDROID_HWUI_DISPLAY_OPERATION_H

#ifndef LOG_TAG
    #define LOG_TAG "OpenGLRenderer"
#endif

#include <SkXfermode.h>

#include <private/hwui/DrawGlInfo.h>

#include "OpenGLRenderer.h"
#include "DeferredDisplayList.h"
#include "DisplayListRenderer.h"
#include "utils/LinearAllocator.h"

#define CRASH() do { \
    *(int *)(uintptr_t)0xbbadbeef = 0; \
    ((void(*)())0)(); /* More reliable, but doesn't say BBADBEEF */ \
} while(false)

#define MATRIX_STRING "[%.2f %.2f %.2f] [%.2f %.2f %.2f] [%.2f %.2f %.2f]"
#define MATRIX_ARGS(m) \
    m->get(0), m->get(1), m->get(2), \
    m->get(3), m->get(4), m->get(5), \
    m->get(6), m->get(7), m->get(8)
#define RECT_STRING "%.2f %.2f %.2f %.2f"
#define RECT_ARGS(r) \
    r.left, r.top, r.right, r.bottom

// Use OP_LOG for logging with arglist, OP_LOGS if just printing char*
#define OP_LOGS(s) OP_LOG("%s", s)
#define OP_LOG(s, ...) ALOGD( "%*s" s, level * 2, "", __VA_ARGS__ )

namespace android {
namespace uirenderer {

/**
 * Structure for storing canvas operations when they are recorded into a DisplayList, so that they
 * may be replayed to an OpenGLRenderer.
 *
 * To avoid individual memory allocations, DisplayListOps may only be allocated into a
 * LinearAllocator's managed memory buffers.  Each pointer held by a DisplayListOp is either a
 * pointer into memory also allocated in the LinearAllocator (mostly for text and float buffers) or
 * references a externally refcounted object (Sk... and Skia... objects). ~DisplayListOp() is
 * never called as LinearAllocators are simply discarded, so no memory management should be done in
 * this class.
 */
class DisplayListOp {
public:
    // These objects should always be allocated with a LinearAllocator, and never destroyed/deleted.
    // standard new() intentionally not implemented, and delete/deconstructor should never be used.
    virtual ~DisplayListOp() { CRASH(); }
    static void operator delete(void* ptr) { CRASH(); }
    /** static void* operator new(size_t size); PURPOSELY OMITTED **/
    static void* operator new(size_t size, LinearAllocator& allocator) {
        return allocator.alloc(size);
    }

    enum OpLogFlag {
        kOpLogFlag_Recurse = 0x1,
        kOpLogFlag_JSON = 0x2 // TODO: add?
    };

    virtual void defer(DeferStateStruct& deferStruct, int saveCount, int level) = 0;

    virtual void replay(ReplayStateStruct& replayStruct, int saveCount, int level) = 0;

    virtual void output(int level, uint32_t logFlags = 0) = 0;

    // NOTE: it would be nice to declare constants and overriding the implementation in each op to
    // point at the constants, but that seems to require a .cpp file
    virtual const char* name() = 0;

    /**
     * Stores the relevant canvas state of the object between deferral and replay (if the canvas
     * state supports being stored) See OpenGLRenderer::simpleClipAndState()
     *
     * TODO: don't reserve space for StateOps that won't be deferred
     */
    DeferredDisplayState state;

};

class StateOp : public DisplayListOp {
public:
    StateOp() {};

    virtual ~StateOp() {}

    virtual void defer(DeferStateStruct& deferStruct, int saveCount, int level) {
        // default behavior only affects immediate, deferrable state, issue directly to renderer
        applyState(deferStruct.mRenderer, saveCount);
    }

    /**
     * State operations are applied directly to the renderer, but can cause the deferred drawing op
     * list to flush
     */
    virtual void replay(ReplayStateStruct& replayStruct, int saveCount, int level) {
        applyState(replayStruct.mRenderer, saveCount);
    }

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const = 0;
};

class DrawOp : public DisplayListOp {
public:
    DrawOp(SkPaint* paint)
            : mPaint(paint), mQuickRejected(false) {}

    virtual void defer(DeferStateStruct& deferStruct, int saveCount, int level) {
        if (mQuickRejected &&
                CC_LIKELY(deferStruct.mReplayFlags & DisplayList::kReplayFlag_ClipChildren)) {
            return;
        }

        if (!getLocalBounds(state.mBounds)) {
            // empty bounds signify bounds can't be calculated
            state.mBounds.setEmpty();
        }

        deferStruct.mDeferredList.addDrawOp(deferStruct.mRenderer, this);
    }

    virtual void replay(ReplayStateStruct& replayStruct, int saveCount, int level) {
        if (mQuickRejected &&
                CC_LIKELY(replayStruct.mReplayFlags & DisplayList::kReplayFlag_ClipChildren)) {
            return;
        }

        replayStruct.mDrawGlStatus |= applyDraw(replayStruct.mRenderer, replayStruct.mDirty, level);
    }

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) = 0;

    virtual void onDrawOpDeferred(OpenGLRenderer& renderer) {
    }

    // returns true if bounds exist
    virtual bool getLocalBounds(Rect& localBounds) { return false; }

    // TODO: better refine localbounds usage
    void setQuickRejected(bool quickRejected) { mQuickRejected = quickRejected; }
    bool getQuickRejected() { return mQuickRejected; }

    /** Batching disabled by default, turned on for individual ops */
    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return DeferredDisplayList::kOpBatch_None;
    }

    float strokeWidthOutset() { return mPaint->getStrokeWidth() * 0.5f; }

protected:
    SkPaint* getPaint(OpenGLRenderer& renderer) {
        return renderer.filterPaint(mPaint);
    }

    SkPaint* mPaint; // should be accessed via getPaint() when applying
    bool mQuickRejected;
};

class DrawBoundedOp : public DrawOp {
public:
    DrawBoundedOp(float left, float top, float right, float bottom, SkPaint* paint)
            : DrawOp(paint), mLocalBounds(left, top, right, bottom) {}

    // Calculates bounds as smallest rect encompassing all points
    // NOTE: requires at least 1 vertex, and doesn't account for stroke size (should be handled in
    // subclass' constructor)
    DrawBoundedOp(const float* points, int count, SkPaint* paint)
            : DrawOp(paint), mLocalBounds(points[0], points[1], points[0], points[1]) {
        for (int i = 2; i < count; i += 2) {
            mLocalBounds.left = fminf(mLocalBounds.left, points[i]);
            mLocalBounds.right = fmaxf(mLocalBounds.right, points[i]);
            mLocalBounds.top = fminf(mLocalBounds.top, points[i + 1]);
            mLocalBounds.bottom = fmaxf(mLocalBounds.bottom, points[i + 1]);
        }
    }

    // default empty constructor for bounds, to be overridden in child constructor body
    DrawBoundedOp(SkPaint* paint)
            : DrawOp(paint) {}

    bool getLocalBounds(Rect& localBounds) {
        localBounds.set(mLocalBounds);
        return true;
    }

protected:
    Rect mLocalBounds; // displayed area in LOCAL coord. doesn't incorporate stroke, so check paint
};

///////////////////////////////////////////////////////////////////////////////
// STATE OPERATIONS - these may affect the state of the canvas/renderer, but do
//         not directly draw or alter output
///////////////////////////////////////////////////////////////////////////////

class SaveOp : public StateOp {
    friend class DisplayList; // give DisplayList private constructor/reinit access
public:
    SaveOp(int flags)
            : mFlags(flags) {}

    virtual void defer(DeferStateStruct& deferStruct, int saveCount, int level) {
        int newSaveCount = deferStruct.mRenderer.save(mFlags);
        deferStruct.mDeferredList.addSave(deferStruct.mRenderer, this, newSaveCount);
    }

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.save(mFlags);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Save flags %x", mFlags);
    }

    virtual const char* name() { return "Save"; }

    int getFlags() const { return mFlags; }
private:
    SaveOp() {}
    DisplayListOp* reinit(int flags) {
        mFlags = flags;
        return this;
    }

    int mFlags;
};

class RestoreToCountOp : public StateOp {
    friend class DisplayList; // give DisplayList private constructor/reinit access
public:
    RestoreToCountOp(int count)
            : mCount(count) {}

    virtual void defer(DeferStateStruct& deferStruct, int saveCount, int level) {
        deferStruct.mDeferredList.addRestoreToCount(deferStruct.mRenderer,
                this, saveCount + mCount);
        deferStruct.mRenderer.restoreToCount(saveCount + mCount);
    }

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.restoreToCount(saveCount + mCount);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Restore to count %d", mCount);
    }

    virtual const char* name() { return "RestoreToCount"; }

private:
    RestoreToCountOp() {}
    DisplayListOp* reinit(int count) {
        mCount = count;
        return this;
    }

    int mCount;
};

class SaveLayerOp : public StateOp {
    friend class DisplayList; // give DisplayList private constructor/reinit access
public:
    SaveLayerOp(float left, float top, float right, float bottom,
            int alpha, SkXfermode::Mode mode, int flags)
            : mArea(left, top, right, bottom), mAlpha(alpha), mMode(mode), mFlags(flags) {}

    virtual void defer(DeferStateStruct& deferStruct, int saveCount, int level) {
        // NOTE: don't bother with actual saveLayer, instead issuing it at flush time
        int newSaveCount = deferStruct.mRenderer.getSaveCount();
        deferStruct.mDeferredList.addSaveLayer(deferStruct.mRenderer, this, newSaveCount);

        // NOTE: don't issue full saveLayer, since that has side effects/is costly. instead just
        // setup the snapshot for deferral, and re-issue the op at flush time
        deferStruct.mRenderer.saveLayerDeferred(mArea.left, mArea.top, mArea.right, mArea.bottom,
                mAlpha, mMode, mFlags);
    }

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.saveLayer(mArea.left, mArea.top, mArea.right, mArea.bottom, mAlpha, mMode, mFlags);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("SaveLayer%s of area " RECT_STRING,
                (isSaveLayerAlpha() ? "Alpha" : ""),RECT_ARGS(mArea));
    }

    virtual const char* name() { return isSaveLayerAlpha() ? "SaveLayerAlpha" : "SaveLayer"; }

    int getFlags() { return mFlags; }

private:
    // Special case, reserved for direct DisplayList usage
    SaveLayerOp() {}
    DisplayListOp* reinit(float left, float top, float right, float bottom,
            int alpha, SkXfermode::Mode mode, int flags) {
        mArea.set(left, top, right, bottom);
        mAlpha = alpha;
        mMode = mode;
        mFlags = flags;
        return this;
    }

    bool isSaveLayerAlpha() { return mAlpha < 255 && mMode == SkXfermode::kSrcOver_Mode; }
    Rect mArea;
    int mAlpha;
    SkXfermode::Mode mMode;
    int mFlags;
};

class TranslateOp : public StateOp {
public:
    TranslateOp(float dx, float dy)
            : mDx(dx), mDy(dy) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.translate(mDx, mDy);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Translate by %f %f", mDx, mDy);
    }

    virtual const char* name() { return "Translate"; }

private:
    float mDx;
    float mDy;
};

class RotateOp : public StateOp {
public:
    RotateOp(float degrees)
            : mDegrees(degrees) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.rotate(mDegrees);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Rotate by %f degrees", mDegrees);
    }

    virtual const char* name() { return "Rotate"; }

private:
    float mDegrees;
};

class ScaleOp : public StateOp {
public:
    ScaleOp(float sx, float sy)
            : mSx(sx), mSy(sy) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.scale(mSx, mSy);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Scale by %f %f", mSx, mSy);
    }

    virtual const char* name() { return "Scale"; }

private:
    float mSx;
    float mSy;
};

class SkewOp : public StateOp {
public:
    SkewOp(float sx, float sy)
            : mSx(sx), mSy(sy) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.skew(mSx, mSy);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Skew by %f %f", mSx, mSy);
    }

    virtual const char* name() { return "Skew"; }

private:
    float mSx;
    float mSy;
};

class SetMatrixOp : public StateOp {
public:
    SetMatrixOp(SkMatrix* matrix)
            : mMatrix(matrix) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.setMatrix(mMatrix);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("SetMatrix " MATRIX_STRING, MATRIX_ARGS(mMatrix));
    }

    virtual const char* name() { return "SetMatrix"; }

private:
    SkMatrix* mMatrix;
};

class ConcatMatrixOp : public StateOp {
public:
    ConcatMatrixOp(SkMatrix* matrix)
            : mMatrix(matrix) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.concatMatrix(mMatrix);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("ConcatMatrix " MATRIX_STRING, MATRIX_ARGS(mMatrix));
    }

    virtual const char* name() { return "ConcatMatrix"; }

private:
    SkMatrix* mMatrix;
};

class ClipOp : public StateOp {
public:
    ClipOp(SkRegion::Op op) : mOp(op) {}

    virtual void defer(DeferStateStruct& deferStruct, int saveCount, int level) {
        // NOTE: must defer op BEFORE applying state, since it may read clip
        deferStruct.mDeferredList.addClip(deferStruct.mRenderer, this);

        // TODO: Can we avoid applying complex clips at defer time?
        applyState(deferStruct.mRenderer, saveCount);
    }

    bool canCauseComplexClip() {
        return ((mOp != SkRegion::kIntersect_Op) && (mOp != SkRegion::kReplace_Op)) || !isRect();
    }

protected:
    ClipOp() {}
    virtual bool isRect() { return false; }

    SkRegion::Op mOp;
};

class ClipRectOp : public ClipOp {
    friend class DisplayList; // give DisplayList private constructor/reinit access
public:
    ClipRectOp(float left, float top, float right, float bottom, SkRegion::Op op)
            : ClipOp(op), mArea(left, top, right, bottom) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.clipRect(mArea.left, mArea.top, mArea.right, mArea.bottom, mOp);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("ClipRect " RECT_STRING, RECT_ARGS(mArea));
    }

    virtual const char* name() { return "ClipRect"; }

protected:
    virtual bool isRect() { return true; }

private:
    ClipRectOp() {}
    DisplayListOp* reinit(float left, float top, float right, float bottom, SkRegion::Op op) {
        mOp = op;
        mArea.set(left, top, right, bottom);
        return this;
    }

    Rect mArea;
};

class ClipPathOp : public ClipOp {
public:
    ClipPathOp(SkPath* path, SkRegion::Op op)
            : ClipOp(op), mPath(path) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.clipPath(mPath, mOp);
    }

    virtual void output(int level, uint32_t logFlags) {
        SkRect bounds = mPath->getBounds();
        OP_LOG("ClipPath bounds " RECT_STRING,
                bounds.left(), bounds.top(), bounds.right(), bounds.bottom());
    }

    virtual const char* name() { return "ClipPath"; }

private:
    SkPath* mPath;
};

class ClipRegionOp : public ClipOp {
public:
    ClipRegionOp(SkRegion* region, SkRegion::Op op)
            : ClipOp(op), mRegion(region) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.clipRegion(mRegion, mOp);
    }

    virtual void output(int level, uint32_t logFlags) {
        SkIRect bounds = mRegion->getBounds();
        OP_LOG("ClipRegion bounds %d %d %d %d",
                bounds.left(), bounds.top(), bounds.right(), bounds.bottom());
    }

    virtual const char* name() { return "ClipRegion"; }

private:
    SkRegion* mRegion;
    SkRegion::Op mOp;
};

class ResetShaderOp : public StateOp {
public:
    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.resetShader();
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOGS("ResetShader");
    }

    virtual const char* name() { return "ResetShader"; }
};

class SetupShaderOp : public StateOp {
public:
    SetupShaderOp(SkiaShader* shader)
            : mShader(shader) {}
    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.setupShader(mShader);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("SetupShader, shader %p", mShader);
    }

    virtual const char* name() { return "SetupShader"; }

private:
    SkiaShader* mShader;
};

class ResetColorFilterOp : public StateOp {
public:
    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.resetColorFilter();
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOGS("ResetColorFilter");
    }

    virtual const char* name() { return "ResetColorFilter"; }
};

class SetupColorFilterOp : public StateOp {
public:
    SetupColorFilterOp(SkiaColorFilter* colorFilter)
            : mColorFilter(colorFilter) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.setupColorFilter(mColorFilter);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("SetupColorFilter, filter %p", mColorFilter);
    }

    virtual const char* name() { return "SetupColorFilter"; }

private:
    SkiaColorFilter* mColorFilter;
};

class ResetShadowOp : public StateOp {
public:
    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.resetShadow();
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOGS("ResetShadow");
    }

    virtual const char* name() { return "ResetShadow"; }
};

class SetupShadowOp : public StateOp {
public:
    SetupShadowOp(float radius, float dx, float dy, int color)
            : mRadius(radius), mDx(dx), mDy(dy), mColor(color) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.setupShadow(mRadius, mDx, mDy, mColor);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("SetupShadow, radius %f, %f, %f, color %#x", mRadius, mDx, mDy, mColor);
    }

    virtual const char* name() { return "SetupShadow"; }

private:
    float mRadius;
    float mDx;
    float mDy;
    int mColor;
};

class ResetPaintFilterOp : public StateOp {
public:
    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.resetPaintFilter();
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOGS("ResetPaintFilter");
    }

    virtual const char* name() { return "ResetPaintFilter"; }
};

class SetupPaintFilterOp : public StateOp {
public:
    SetupPaintFilterOp(int clearBits, int setBits)
            : mClearBits(clearBits), mSetBits(setBits) {}

    virtual void applyState(OpenGLRenderer& renderer, int saveCount) const {
        renderer.setupPaintFilter(mClearBits, mSetBits);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("SetupPaintFilter, clear %#x, set %#x", mClearBits, mSetBits);
    }

    virtual const char* name() { return "SetupPaintFilter"; }

private:
    int mClearBits;
    int mSetBits;
};


///////////////////////////////////////////////////////////////////////////////
// DRAW OPERATIONS - these are operations that can draw to the canvas's device
///////////////////////////////////////////////////////////////////////////////

class DrawBitmapOp : public DrawBoundedOp {
public:
    DrawBitmapOp(SkBitmap* bitmap, float left, float top, SkPaint* paint)
            : DrawBoundedOp(left, top, left + bitmap->width(), top + bitmap->height(),
                    paint),
            mBitmap(bitmap) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawBitmap(mBitmap, mLocalBounds.left, mLocalBounds.top,
                getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw bitmap %p at %f %f", mBitmap, mLocalBounds.left, mLocalBounds.top);
    }

    virtual const char* name() { return "DrawBitmap"; }
    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return DeferredDisplayList::kOpBatch_Bitmap;
    }

protected:
    SkBitmap* mBitmap;
};

class DrawBitmapMatrixOp : public DrawBoundedOp {
public:
    DrawBitmapMatrixOp(SkBitmap* bitmap, SkMatrix* matrix, SkPaint* paint)
            : DrawBoundedOp(paint), mBitmap(bitmap), mMatrix(matrix) {
        mLocalBounds.set(0, 0, bitmap->width(), bitmap->height());
        const mat4 transform(*matrix);
        transform.mapRect(mLocalBounds);
    }

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawBitmap(mBitmap, mMatrix, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw bitmap %p matrix " MATRIX_STRING, mBitmap, MATRIX_ARGS(mMatrix));
    }

    virtual const char* name() { return "DrawBitmap"; }
    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return DeferredDisplayList::kOpBatch_Bitmap;
    }

private:
    SkBitmap* mBitmap;
    SkMatrix* mMatrix;
};

class DrawBitmapRectOp : public DrawBoundedOp {
public:
    DrawBitmapRectOp(SkBitmap* bitmap, float srcLeft, float srcTop, float srcRight, float srcBottom,
            float dstLeft, float dstTop, float dstRight, float dstBottom, SkPaint* paint)
            : DrawBoundedOp(dstLeft, dstTop, dstRight, dstBottom, paint),
            mBitmap(bitmap), mSrc(srcLeft, srcTop, srcRight, srcBottom) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawBitmap(mBitmap, mSrc.left, mSrc.top, mSrc.right, mSrc.bottom,
                mLocalBounds.left, mLocalBounds.top, mLocalBounds.right, mLocalBounds.bottom,
                getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw bitmap %p src="RECT_STRING", dst="RECT_STRING,
                mBitmap, RECT_ARGS(mSrc), RECT_ARGS(mLocalBounds));
    }

    virtual const char* name() { return "DrawBitmapRect"; }
    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return DeferredDisplayList::kOpBatch_Bitmap;
    }

private:
    SkBitmap* mBitmap;
    Rect mSrc;
};

class DrawBitmapDataOp : public DrawBitmapOp {
public:
    DrawBitmapDataOp(SkBitmap* bitmap, float left, float top, SkPaint* paint)
            : DrawBitmapOp(bitmap, left, top, paint) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawBitmapData(mBitmap, mLocalBounds.left,
                mLocalBounds.top, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw bitmap %p", mBitmap);
    }

    virtual const char* name() { return "DrawBitmapData"; }
    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return DeferredDisplayList::kOpBatch_Bitmap;
    }
};

class DrawBitmapMeshOp : public DrawBoundedOp {
public:
    DrawBitmapMeshOp(SkBitmap* bitmap, int meshWidth, int meshHeight,
            float* vertices, int* colors, SkPaint* paint)
            : DrawBoundedOp(vertices, 2 * (meshWidth + 1) * (meshHeight + 1), paint),
            mBitmap(bitmap), mMeshWidth(meshWidth), mMeshHeight(meshHeight),
            mVertices(vertices), mColors(colors) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawBitmapMesh(mBitmap, mMeshWidth, mMeshHeight,
                mVertices, mColors, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw bitmap %p mesh %d x %d", mBitmap, mMeshWidth, mMeshHeight);
    }

    virtual const char* name() { return "DrawBitmapMesh"; }
    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return DeferredDisplayList::kOpBatch_Bitmap;
    }

private:
    SkBitmap* mBitmap;
    int mMeshWidth;
    int mMeshHeight;
    float* mVertices;
    int* mColors;
};

class DrawPatchOp : public DrawBoundedOp {
public:
    DrawPatchOp(SkBitmap* bitmap, const int32_t* xDivs,
            const int32_t* yDivs, const uint32_t* colors, uint32_t width, uint32_t height,
            int8_t numColors, float left, float top, float right, float bottom,
            int alpha, SkXfermode::Mode mode)
            : DrawBoundedOp(left, top, right, bottom, 0),
            mBitmap(bitmap), mxDivs(xDivs), myDivs(yDivs),
            mColors(colors), mxDivsCount(width), myDivsCount(height),
            mNumColors(numColors), mAlpha(alpha), mMode(mode) {};

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        // NOTE: not calling the virtual method, which takes a paint
        return renderer.drawPatch(mBitmap, mxDivs, myDivs, mColors,
                mxDivsCount, myDivsCount, mNumColors,
                mLocalBounds.left, mLocalBounds.top,
                mLocalBounds.right, mLocalBounds.bottom, mAlpha, mMode);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw patch "RECT_STRING, RECT_ARGS(mLocalBounds));
    }

    virtual const char* name() { return "DrawPatch"; }
    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return DeferredDisplayList::kOpBatch_Patch;
    }

private:
    SkBitmap* mBitmap;
    const int32_t* mxDivs;
    const int32_t* myDivs;
    const uint32_t* mColors;
    uint32_t mxDivsCount;
    uint32_t myDivsCount;
    int8_t mNumColors;
    int mAlpha;
    SkXfermode::Mode mMode;
};

class DrawColorOp : public DrawOp {
public:
    DrawColorOp(int color, SkXfermode::Mode mode)
            : DrawOp(0), mColor(color), mMode(mode) {};

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawColor(mColor, mMode);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw color %#x, mode %d", mColor, mMode);
    }

    virtual const char* name() { return "DrawColor"; }

private:
    int mColor;
    SkXfermode::Mode mMode;
};

class DrawStrokableOp : public DrawBoundedOp {
public:
    DrawStrokableOp(float left, float top, float right, float bottom, SkPaint* paint)
            : DrawBoundedOp(left, top, right, bottom, paint) {};

    bool getLocalBounds(Rect& localBounds) {
        localBounds.set(mLocalBounds);
        if (mPaint && mPaint->getStyle() != SkPaint::kFill_Style) {
            localBounds.outset(strokeWidthOutset());
        }
        return true;
    }

    virtual DeferredDisplayList::OpBatchId getBatchId() {
        if (mPaint->getPathEffect()) {
            return DeferredDisplayList::kOpBatch_AlphaMaskTexture;
        }
        return mPaint->isAntiAlias() ?
                DeferredDisplayList::kOpBatch_AlphaVertices :
                DeferredDisplayList::kOpBatch_Vertices;
    }
};

class DrawRectOp : public DrawStrokableOp {
public:
    DrawRectOp(float left, float top, float right, float bottom, SkPaint* paint)
            : DrawStrokableOp(left, top, right, bottom, paint) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawRect(mLocalBounds.left, mLocalBounds.top,
                mLocalBounds.right, mLocalBounds.bottom, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Rect "RECT_STRING, RECT_ARGS(mLocalBounds));
    }

    virtual const char* name() { return "DrawRect"; }
};

class DrawRectsOp : public DrawBoundedOp {
public:
    DrawRectsOp(const float* rects, int count, SkPaint* paint)
            : DrawBoundedOp(rects, count, paint),
            mRects(rects), mCount(count) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawRects(mRects, mCount, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Rects count %d", mCount);
    }

    virtual const char* name() { return "DrawRects"; }

    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return DeferredDisplayList::kOpBatch_Vertices;
    }

private:
    const float* mRects;
    int mCount;
};

class DrawRoundRectOp : public DrawStrokableOp {
public:
    DrawRoundRectOp(float left, float top, float right, float bottom,
            float rx, float ry, SkPaint* paint)
            : DrawStrokableOp(left, top, right, bottom, paint), mRx(rx), mRy(ry) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawRoundRect(mLocalBounds.left, mLocalBounds.top,
                mLocalBounds.right, mLocalBounds.bottom, mRx, mRy, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw RoundRect "RECT_STRING", rx %f, ry %f", RECT_ARGS(mLocalBounds), mRx, mRy);
    }

    virtual const char* name() { return "DrawRoundRect"; }

private:
    float mRx;
    float mRy;
};

class DrawCircleOp : public DrawStrokableOp {
public:
    DrawCircleOp(float x, float y, float radius, SkPaint* paint)
            : DrawStrokableOp(x - radius, y - radius, x + radius, y + radius, paint),
            mX(x), mY(y), mRadius(radius) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawCircle(mX, mY, mRadius, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Circle x %f, y %f, r %f", mX, mY, mRadius);
    }

    virtual const char* name() { return "DrawCircle"; }

private:
    float mX;
    float mY;
    float mRadius;
};

class DrawOvalOp : public DrawStrokableOp {
public:
    DrawOvalOp(float left, float top, float right, float bottom, SkPaint* paint)
            : DrawStrokableOp(left, top, right, bottom, paint) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawOval(mLocalBounds.left, mLocalBounds.top,
                mLocalBounds.right, mLocalBounds.bottom, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Oval "RECT_STRING, RECT_ARGS(mLocalBounds));
    }

    virtual const char* name() { return "DrawOval"; }
};

class DrawArcOp : public DrawStrokableOp {
public:
    DrawArcOp(float left, float top, float right, float bottom,
            float startAngle, float sweepAngle, bool useCenter, SkPaint* paint)
            : DrawStrokableOp(left, top, right, bottom, paint),
            mStartAngle(startAngle), mSweepAngle(sweepAngle), mUseCenter(useCenter) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawArc(mLocalBounds.left, mLocalBounds.top,
                mLocalBounds.right, mLocalBounds.bottom,
                mStartAngle, mSweepAngle, mUseCenter, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Arc "RECT_STRING", start %f, sweep %f, useCenter %d",
                RECT_ARGS(mLocalBounds), mStartAngle, mSweepAngle, mUseCenter);
    }

    virtual const char* name() { return "DrawArc"; }

private:
    float mStartAngle;
    float mSweepAngle;
    bool mUseCenter;
};

class DrawPathOp : public DrawBoundedOp {
public:
    DrawPathOp(SkPath* path, SkPaint* paint)
            : DrawBoundedOp(paint), mPath(path) {
        float left, top, offset;
        uint32_t width, height;
        PathCache::computePathBounds(path, paint, left, top, offset, width, height);
        left -= offset;
        top -= offset;
        mLocalBounds.set(left, top, left + width, top + height);
    }

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawPath(mPath, getPaint(renderer));
    }

    virtual void onDrawOpDeferred(OpenGLRenderer& renderer) {
        SkPaint* paint = getPaint(renderer);
        renderer.getCaches().pathCache.precache(mPath, paint);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Path %p in "RECT_STRING, mPath, RECT_ARGS(mLocalBounds));
    }

    virtual const char* name() { return "DrawPath"; }

    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return DeferredDisplayList::kOpBatch_AlphaMaskTexture;
    }
private:
    SkPath* mPath;
};

class DrawLinesOp : public DrawBoundedOp {
public:
    DrawLinesOp(float* points, int count, SkPaint* paint)
            : DrawBoundedOp(points, count, paint),
            mPoints(points), mCount(count) {
        mLocalBounds.outset(strokeWidthOutset());
    }

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawLines(mPoints, mCount, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Lines count %d", mCount);
    }

    virtual const char* name() { return "DrawLines"; }

    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return mPaint->isAntiAlias() ?
                DeferredDisplayList::kOpBatch_AlphaVertices :
                DeferredDisplayList::kOpBatch_Vertices;
    }

protected:
    float* mPoints;
    int mCount;
};

class DrawPointsOp : public DrawLinesOp {
public:
    DrawPointsOp(float* points, int count, SkPaint* paint)
            : DrawLinesOp(points, count, paint) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawPoints(mPoints, mCount, getPaint(renderer));
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Points count %d", mCount);
    }

    virtual const char* name() { return "DrawPoints"; }
};

class DrawSomeTextOp : public DrawOp {
public:
    DrawSomeTextOp(const char* text, int bytesCount, int count, SkPaint* paint)
            : DrawOp(paint), mText(text), mBytesCount(bytesCount), mCount(count) {};

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw some text, %d bytes", mBytesCount);
    }

    virtual void onDrawOpDeferred(OpenGLRenderer& renderer) {
        SkPaint* paint = getPaint(renderer);
        FontRenderer& fontRenderer = renderer.getCaches().fontRenderer->getFontRenderer(paint);
        fontRenderer.precache(paint, mText, mCount, mat4::identity());
    }

    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return mPaint->getColor() == 0xff000000 ?
                DeferredDisplayList::kOpBatch_Text :
                DeferredDisplayList::kOpBatch_ColorText;
    }
protected:
    const char* mText;
    int mBytesCount;
    int mCount;
};

class DrawTextOnPathOp : public DrawSomeTextOp {
public:
    DrawTextOnPathOp(const char* text, int bytesCount, int count,
            SkPath* path, float hOffset, float vOffset, SkPaint* paint)
            : DrawSomeTextOp(text, bytesCount, count, paint),
            mPath(path), mHOffset(hOffset), mVOffset(vOffset) {
        /* TODO: inherit from DrawBounded and init mLocalBounds */
    }

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawTextOnPath(mText, mBytesCount, mCount, mPath,
                mHOffset, mVOffset, getPaint(renderer));
    }

    virtual const char* name() { return "DrawTextOnPath"; }

private:
    SkPath* mPath;
    float mHOffset;
    float mVOffset;
};

class DrawPosTextOp : public DrawSomeTextOp {
public:
    DrawPosTextOp(const char* text, int bytesCount, int count,
            const float* positions, SkPaint* paint)
            : DrawSomeTextOp(text, bytesCount, count, paint), mPositions(positions) {
        /* TODO: inherit from DrawBounded and init mLocalBounds */
    }

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawPosText(mText, mBytesCount, mCount, mPositions, getPaint(renderer));
    }

    virtual const char* name() { return "DrawPosText"; }

private:
    const float* mPositions;
};

class DrawTextOp : public DrawBoundedOp {
public:
    DrawTextOp(const char* text, int bytesCount, int count, float x, float y,
            const float* positions, SkPaint* paint, float length)
            : DrawBoundedOp(paint), mText(text), mBytesCount(bytesCount), mCount(count),
            mX(x), mY(y), mPositions(positions), mLength(length) {
        // duplicates bounds calculation from OpenGLRenderer::drawText, but doesn't alter mX
        SkPaint::FontMetrics metrics;
        paint->getFontMetrics(&metrics, 0.0f);
        switch (paint->getTextAlign()) {
        case SkPaint::kCenter_Align:
            x -= length / 2.0f;
            break;
        case SkPaint::kRight_Align:
            x -= length;
            break;
        default:
            break;
        }
        mLocalBounds.set(x, mY + metrics.fTop, x + length, mY + metrics.fBottom);
        memset(&mPrecacheTransform.data[0], 0xff, 16 * sizeof(float));
    }

    /*
     * When this method is invoked the state field  is initialized to have the
     * final rendering state. We can thus use it to process data as it will be
     * used at draw time.
     */
    virtual void onDrawOpDeferred(OpenGLRenderer& renderer) {
        SkPaint* paint = getPaint(renderer);
        FontRenderer& fontRenderer = renderer.getCaches().fontRenderer->getFontRenderer(paint);
        const mat4& transform = renderer.findBestFontTransform(state.mMatrix);
        if (mPrecacheTransform != transform) {
            fontRenderer.precache(paint, mText, mCount, transform);
            mPrecacheTransform = transform;
        }
    }

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawText(mText, mBytesCount, mCount, mX, mY,
                mPositions, getPaint(renderer), mLength);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Text of count %d, bytes %d", mCount, mBytesCount);
    }

    virtual const char* name() { return "DrawText"; }

    virtual DeferredDisplayList::OpBatchId getBatchId() {
        return mPaint->getColor() == 0xff000000 ?
                DeferredDisplayList::kOpBatch_Text :
                DeferredDisplayList::kOpBatch_ColorText;
    }

private:
    const char* mText;
    int mBytesCount;
    int mCount;
    float mX;
    float mY;
    const float* mPositions;
    float mLength;
    mat4 mPrecacheTransform;
};

///////////////////////////////////////////////////////////////////////////////
// SPECIAL DRAW OPERATIONS
///////////////////////////////////////////////////////////////////////////////

class DrawFunctorOp : public DrawOp {
public:
    DrawFunctorOp(Functor* functor)
            : DrawOp(0), mFunctor(functor) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        renderer.startMark("GL functor");
        status_t ret = renderer.callDrawGLFunction(mFunctor, dirty);
        renderer.endMark();
        return ret;
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Functor %p", mFunctor);
    }

    virtual const char* name() { return "DrawFunctor"; }

private:
    Functor* mFunctor;
};

class DrawDisplayListOp : public DrawBoundedOp {
public:
    DrawDisplayListOp(DisplayList* displayList, int flags)
            : DrawBoundedOp(0, 0, displayList->getWidth(), displayList->getHeight(), 0),
            mDisplayList(displayList), mFlags(flags) {}

    virtual void defer(DeferStateStruct& deferStruct, int saveCount, int level) {
        if (mDisplayList && mDisplayList->isRenderable()) {
            mDisplayList->defer(deferStruct, level + 1);
        }
    }
virtual void replay(ReplayStateStruct& replayStruct, int saveCount, int level) {
        if (mDisplayList && mDisplayList->isRenderable()) {
            mDisplayList->replay(replayStruct, level + 1);
        }
    }

    // NOT USED since replay() is overridden
    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return DrawGlInfo::kStatusDone;
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Display List %p, flags %#x", mDisplayList, mFlags);
        if (mDisplayList && (logFlags & kOpLogFlag_Recurse)) {
            mDisplayList->output(level + 1);
        }
    }

    virtual const char* name() { return "DrawDisplayList"; }

private:
    DisplayList* mDisplayList;
    int mFlags;
};

class DrawLayerOp : public DrawOp {
public:
    DrawLayerOp(Layer* layer, float x, float y)
            : DrawOp(0), mLayer(layer), mX(x), mY(y) {}

    virtual status_t applyDraw(OpenGLRenderer& renderer, Rect& dirty, int level) {
        return renderer.drawLayer(mLayer, mX, mY);
    }

    virtual void output(int level, uint32_t logFlags) {
        OP_LOG("Draw Layer %p at %f %f", mLayer, mX, mY);
    }

    virtual const char* name() { return "DrawLayer"; }

private:
    Layer* mLayer;
    float mX;
    float mY;
};

}; // namespace uirenderer
}; // namespace android

#endif // ANDROID_HWUI_DISPLAY_OPERATION_H
