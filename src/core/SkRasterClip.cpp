/*
 * Copyright 2010 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkPath.h"
#include "src/core/SkRasterClip.h"
#include "src/core/SkRegionPriv.h"

SkRasterClip::SkRasterClip(const SkRasterClip& that)
        : fIsBW(that.fIsBW)
        , fIsEmpty(that.fIsEmpty)
        , fIsRect(that.fIsRect)
        , fShader(that.fShader)
{
    AUTO_RASTERCLIP_VALIDATE(that);

    if (fIsBW) {
        fBW = that.fBW;
    } else {
        fAA = that.fAA;
    }

    SkDEBUGCODE(this->validate();)
}

SkRasterClip& SkRasterClip::operator=(const SkRasterClip& that) {
    AUTO_RASTERCLIP_VALIDATE(that);

    fIsBW = that.fIsBW;
    if (fIsBW) {
        fBW = that.fBW;
    } else {
        fAA = that.fAA;
    }

    fIsEmpty = that.isEmpty();
    fIsRect = that.isRect();
    fShader = that.fShader;
    SkDEBUGCODE(this->validate();)
    return *this;
}

SkRasterClip::SkRasterClip(const SkRegion& rgn) : fBW(rgn) {
    fIsBW = true;
    fIsEmpty = this->computeIsEmpty();  // bounds might be empty, so compute
    fIsRect = !fIsEmpty;
    SkDEBUGCODE(this->validate();)
}

SkRasterClip::SkRasterClip(const SkIRect& bounds) : fBW(bounds) {
    fIsBW = true;
    fIsEmpty = this->computeIsEmpty();  // bounds might be empty, so compute
    fIsRect = !fIsEmpty;
    SkDEBUGCODE(this->validate();)
}

SkRasterClip::SkRasterClip() {
    fIsBW = true;
    fIsEmpty = true;
    fIsRect = false;
    SkDEBUGCODE(this->validate();)
}

SkRasterClip::~SkRasterClip() {
    SkDEBUGCODE(this->validate();)
}

bool SkRasterClip::operator==(const SkRasterClip& other) const {
    if (fIsBW != other.fIsBW) {
        return false;
    }
    bool isEqual = fIsBW ? fBW == other.fBW : fAA == other.fAA;
#ifdef SK_DEBUG
    if (isEqual) {
        SkASSERT(fIsEmpty == other.fIsEmpty);
        SkASSERT(fIsRect == other.fIsRect);
    }
#endif
    return isEqual;
}

bool SkRasterClip::isComplex() const {
    return fIsBW ? fBW.isComplex() : !fAA.isEmpty();
}

const SkIRect& SkRasterClip::getBounds() const {
    return fIsBW ? fBW.getBounds() : fAA.getBounds();
}

bool SkRasterClip::setEmpty() {
    AUTO_RASTERCLIP_VALIDATE(*this);

    fIsBW = true;
    fBW.setEmpty();
    fAA.setEmpty();
    fIsEmpty = true;
    fIsRect = false;
    return false;
}

bool SkRasterClip::setRect(const SkIRect& rect) {
    AUTO_RASTERCLIP_VALIDATE(*this);

    fIsBW = true;
    fAA.setEmpty();
    fIsRect = fBW.setRect(rect);
    fIsEmpty = !fIsRect;
    return fIsRect;
}

/////////////////////////////////////////////////////////////////////////////////////

bool SkRasterClip::setConservativeRect(const SkRect& r, const SkIRect& clipR, bool isInverse) {
    SkRegion::Op op;
    if (isInverse) {
        op = SkRegion::kDifference_Op;
    } else {
        op = SkRegion::kIntersect_Op;
    }
    fBW.setRect(clipR);
    fBW.op(r.roundOut(), op);
    return this->updateCacheAndReturnNonEmpty();
}

/////////////////////////////////////////////////////////////////////////////////////

bool SkRasterClip::setPath(const SkPath& path, const SkRegion& clip, bool doAA) {
    AUTO_RASTERCLIP_VALIDATE(*this);

    if (this->isBW() && !doAA) {
        (void)fBW.setPath(path, clip);
    } else {
        // TODO: since we are going to over-write fAA completely (aren't we?)
        // we should just clear our BW data (if any) and set fIsAA=true
        if (this->isBW()) {
            this->convertToAA();
        }
        (void)fAA.setPath(path, &clip, doAA);
    }
    return this->updateCacheAndReturnNonEmpty();
}

bool SkRasterClip::op(const SkRRect& rrect, const SkMatrix& matrix, const SkIRect& devBounds,
                      SkRegion::Op op, bool doAA) {
    SkIRect bounds(devBounds);
    return this->op(SkPath::RRect(rrect), matrix, bounds, op, doAA);
}

bool SkRasterClip::op(const SkPath& path, const SkMatrix& matrix, const SkIRect& devBounds,
                      SkRegion::Op op, bool doAA) {
    AUTO_RASTERCLIP_VALIDATE(*this);
    SkIRect bounds(devBounds);

    // base is used to limit the size (and therefore memory allocation) of the
    // region that results from scan converting devPath.
    SkRegion base;

    SkPath devPath;
    if (matrix.isIdentity()) {
        devPath = path;
    } else {
        path.transform(matrix, &devPath);
        devPath.setIsVolatile(true);
    }
    if (SkRegion::kIntersect_Op == op) {
        // since we are intersect, we can do better (tighter) with currRgn's
        // bounds, than just using the device. However, if currRgn is complex,
        // our region blitter may hork, so we do that case in two steps.
        if (this->isRect()) {
            // FIXME: we should also be able to do this when this->isBW(),
            // but relaxing the test above triggers GM asserts in
            // SkRgnBuilder::blitH(). We need to investigate what's going on.
            return this->setPath(devPath, this->bwRgn(), doAA);
        } else {
            base.setRect(this->getBounds());
            SkRasterClip clip;
            clip.setPath(devPath, base, doAA);
            return this->op(clip, op);
        }
    } else {
        base.setRect(bounds);

        if (SkRegion::kReplace_Op == op) {
            return this->setPath(devPath, base, doAA);
        } else {
            SkRasterClip clip;
            clip.setPath(devPath, base, doAA);
            return this->op(clip, op);
        }
    }
}

bool SkRasterClip::setPath(const SkPath& path, const SkIRect& clip, bool doAA) {
    SkRegion tmp;
    tmp.setRect(clip);
    return this->setPath(path, tmp, doAA);
}

bool SkRasterClip::op(const SkIRect& rect, SkRegion::Op op) {
    AUTO_RASTERCLIP_VALIDATE(*this);

    fIsBW ? fBW.op(rect, op) : fAA.op(rect, op);
    return this->updateCacheAndReturnNonEmpty();
}

bool SkRasterClip::op(const SkRegion& rgn, SkRegion::Op op) {
    AUTO_RASTERCLIP_VALIDATE(*this);

    if (fIsBW) {
        (void)fBW.op(rgn, op);
    } else {
        SkAAClip tmp;
        tmp.setRegion(rgn);
        (void)fAA.op(tmp, op);
    }
    return this->updateCacheAndReturnNonEmpty();
}

bool SkRasterClip::op(const SkRasterClip& clip, SkRegion::Op op) {
    AUTO_RASTERCLIP_VALIDATE(*this);
    clip.validate();

    if (this->isBW() && clip.isBW()) {
        (void)fBW.op(clip.fBW, op);
    } else {
        SkAAClip tmp;
        const SkAAClip* other;

        if (this->isBW()) {
            this->convertToAA();
        }
        if (clip.isBW()) {
            tmp.setRegion(clip.bwRgn());
            other = &tmp;
        } else {
            other = &clip.aaRgn();
        }
        (void)fAA.op(*other, op);
    }
    return this->updateCacheAndReturnNonEmpty();
}

bool SkRasterClip::op(sk_sp<SkShader> sh) {
    AUTO_RASTERCLIP_VALIDATE(*this);

    if (!fShader) {
        fShader = sh;
    } else {
        fShader = SkShaders::Blend(SkBlendMode::kSrcIn, sh, fShader);
    }
    return !this->isEmpty();
}

/**
 *  Our antialiasing currently has a granularity of 1/4 of a pixel along each
 *  axis. Thus we can treat an axis coordinate as an integer if it differs
 *  from its nearest int by < half of that value (1.8 in this case).
 */
static bool nearly_integral(SkScalar x) {
    static const SkScalar domain = SK_Scalar1 / 4;
    static const SkScalar halfDomain = domain / 2;

    x += halfDomain;
    return x - SkScalarFloorToScalar(x) < domain;
}

bool SkRasterClip::op(const SkRect& localRect, const SkMatrix& matrix, const SkIRect& devBounds,
                      SkRegion::Op op, bool doAA) {
    AUTO_RASTERCLIP_VALIDATE(*this);
    SkRect devRect;

    const bool isScaleTrans = matrix.isScaleTranslate();
    if (!isScaleTrans) {
        SkPath path;
        path.addRect(localRect);
        path.setIsVolatile(true);
        return this->op(path, matrix, devBounds, op, doAA);
    }

    matrix.mapRect(&devRect, localRect);

    if (fIsBW && doAA) {
        // check that the rect really needs aa, or is it close enought to
        // integer boundaries that we can just treat it as a BW rect?
        if (nearly_integral(devRect.fLeft) && nearly_integral(devRect.fTop) &&
            nearly_integral(devRect.fRight) && nearly_integral(devRect.fBottom)) {
            doAA = false;
        }
    }

    if (fIsBW && !doAA) {
        SkIRect ir;
        devRect.round(&ir);
        (void)fBW.op(ir, op);
    } else {
        if (fIsBW) {
            this->convertToAA();
        }
        (void)fAA.op(devRect, op, doAA);
    }
    return this->updateCacheAndReturnNonEmpty();
}

void SkRasterClip::translate(int dx, int dy, SkRasterClip* dst) const {
    if (nullptr == dst) {
        return;
    }

    AUTO_RASTERCLIP_VALIDATE(*this);

    if (this->isEmpty()) {
        dst->setEmpty();
        return;
    }
    if (0 == (dx | dy)) {
        *dst = *this;
        return;
    }

    dst->fIsBW = fIsBW;
    if (fIsBW) {
        fBW.translate(dx, dy, &dst->fBW);
        dst->fAA.setEmpty();
    } else {
        fAA.translate(dx, dy, &dst->fAA);
        dst->fBW.setEmpty();
    }
    dst->updateCacheAndReturnNonEmpty();
}

bool SkRasterClip::quickContains(const SkIRect& ir) const {
    return fIsBW ? fBW.quickContains(ir) : fAA.quickContains(ir);
}

///////////////////////////////////////////////////////////////////////////////

const SkRegion& SkRasterClip::forceGetBW() {
    AUTO_RASTERCLIP_VALIDATE(*this);

    if (!fIsBW) {
        fBW.setRect(fAA.getBounds());
    }
    return fBW;
}

void SkRasterClip::convertToAA() {
    AUTO_RASTERCLIP_VALIDATE(*this);

    SkASSERT(fIsBW);
    fAA.setRegion(fBW);
    fIsBW = false;

    // since we are being explicitly asked to convert-to-aa, we pass false so we don't "optimize"
    // ourselves back to BW.
    (void)this->updateCacheAndReturnNonEmpty(false);
}

#ifdef SK_DEBUG
void SkRasterClip::validate() const {
    // can't ever assert that fBW is empty, since we may have called forceGetBW
    if (fIsBW) {
        SkASSERT(fAA.isEmpty());
    }

    SkRegionPriv::Validate(fBW);
    fAA.validate();

    SkASSERT(this->computeIsEmpty() == fIsEmpty);
    SkASSERT(this->computeIsRect() == fIsRect);
}
#endif

///////////////////////////////////////////////////////////////////////////////

SkAAClipBlitterWrapper::SkAAClipBlitterWrapper() {
    SkDEBUGCODE(fClipRgn = nullptr;)
    SkDEBUGCODE(fBlitter = nullptr;)
}

SkAAClipBlitterWrapper::SkAAClipBlitterWrapper(const SkRasterClip& clip,
                                               SkBlitter* blitter) {
    this->init(clip, blitter);
}

SkAAClipBlitterWrapper::SkAAClipBlitterWrapper(const SkAAClip* aaclip,
                                               SkBlitter* blitter) {
    SkASSERT(blitter);
    SkASSERT(aaclip);
    fBWRgn.setRect(aaclip->getBounds());
    fAABlitter.init(blitter, aaclip);
    // now our return values
    fClipRgn = &fBWRgn;
    fBlitter = &fAABlitter;
}

void SkAAClipBlitterWrapper::init(const SkRasterClip& clip, SkBlitter* blitter) {
    SkASSERT(blitter);
    if (clip.isBW()) {
        fClipRgn = &clip.bwRgn();
        fBlitter = blitter;
    } else {
        const SkAAClip& aaclip = clip.aaRgn();
        fBWRgn.setRect(aaclip.getBounds());
        fAABlitter.init(blitter, &aaclip);
        // now our return values
        fClipRgn = &fBWRgn;
        fBlitter = &fAABlitter;
    }
}
