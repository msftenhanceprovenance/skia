/*
 * Copyright 2019 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkColorFilter.h"
#include "include/private/SkMacros.h"
#include "src/core/SkArenaAlloc.h"
#include "src/core/SkBlendModePriv.h"
#include "src/core/SkColorSpacePriv.h"
#include "src/core/SkColorSpaceXformSteps.h"
#include "src/core/SkCoreBlitters.h"
#include "src/core/SkLRUCache.h"
#include "src/core/SkVM.h"

namespace {

    enum class Coverage { Full, UniformA8, MaskA8, MaskLCD16, Mask3D };

    SK_BEGIN_REQUIRE_DENSE;
    struct Key {
        SkColorType          colorType;
        SkAlphaType          alphaType;
        Coverage             coverage;
        SkBlendMode          blendMode;
        sk_sp<SkColorSpace>  colorSpace;
        sk_sp<SkShader>      shader;
        sk_sp<SkColorFilter> colorFilter;

        Key withCoverage(Coverage c) const {
            Key k = *this;
            k.coverage = c;
            return k;
        }
    };
    SK_END_REQUIRE_DENSE;

    static bool operator==(const Key& x, const Key& y) {
        return x.colorType   == y.colorType
            && x.alphaType   == y.alphaType
            && x.coverage    == y.coverage
            && x.blendMode   == y.blendMode
            && x.colorSpace  == y.colorSpace   // SkColorSpace::Equals() would make hashing unsound.
            && x.shader      == y.shader
            && x.colorFilter == y.colorFilter;
    }

    static SkString debug_name(const Key& key) {
        return SkStringPrintf("CT%d-AT%d-Cov%d-Blend%d-CS%d-Shader%d-CF%d",
                              key.colorType,
                              key.alphaType,
                              key.coverage,
                              key.blendMode,
                              SkToBool(key.colorSpace),
                              SkToBool(key.shader),
                              SkToBool(key.colorFilter));
    }

    static bool debug_dump(const Key& key) {
    #if 0
        SkDebugf("%s\n", debug_name(key).c_str());
        return true;
    #else
        return false;
    #endif
    }

    static SkLRUCache<Key, skvm::Program>* try_acquire_program_cache() {
    #if 0 || defined(SK_BUILD_FOR_IOS)
        // iOS doesn't support thread_local on versions less than 9.0. pthread
        // based fallbacks must be used there. We could also use an SkSpinlock
        // and tryAcquire()/release(), or...
        return nullptr;  // ... we could just not cache programs on those platforms.
    #else
        thread_local static auto* cache = new SkLRUCache<Key, skvm::Program>{8};
        return cache;
    #endif
    }

    static void release_program_cache() { }


    struct Uniforms {
        uint32_t paint_color;
        uint8_t  coverage;   // Used when Coverage::UniformA8.
    };

    struct Builder : public skvm::Builder {
        //using namespace skvm;

        struct Color { skvm::I32 r,g,b,a; };


        // TODO: provide this in skvm::Builder, with a custom NEON impl.
        skvm::I32 div255(skvm::I32 v) {
            // This should be a bit-perfect version of (v+127)/255,
            // implemented as (v + ((v+128)>>8) + 128)>>8.
            skvm::I32 v128 = add(v, splat(128));
            return shr(add(v128, shr(v128, 8)), 8);
        }

        skvm::I32 scale_unorm8(skvm::I32 x, skvm::I32 y) {
            return div255(mul(x,y));
        }

        skvm::I32 lerp_unorm8(skvm::I32 x, skvm::I32 y, skvm::I32 t) {
            return div255(add(mul(x, sub(splat(255), t)),
                              mul(y,                 t )));
        }

        Color unpack_8888(skvm::I32 rgba) {
            return {
                extract(rgba,  0, splat(0xff)),
                extract(rgba,  8, splat(0xff)),
                extract(rgba, 16, splat(0xff)),
                extract(rgba, 24, splat(0xff)),
            };
        }

        skvm::I32 pack_8888(Color c) {
            return pack(pack(c.r, c.g, 8),
                        pack(c.b, c.a, 8), 16);
        }

        Color unpack_565(skvm::I32 bgr) {
            // N.B. kRGB_565_SkColorType is named confusingly;
            //      blue is in the low bits and red the high.
            skvm::I32 r = extract(bgr, 11, splat(0b011'111)),
                      g = extract(bgr,  5, splat(0b111'111)),
                      b = extract(bgr,  0, splat(0b011'111));
            return {
                // Scale 565 up to 888.
                bit_or(shl(r, 3), shr(r, 2)),
                bit_or(shl(g, 2), shr(g, 4)),
                bit_or(shl(b, 3), shr(b, 2)),
                splat(0xff),
            };
        }

        skvm::I32 pack_565(Color c) {
            skvm::I32 r = scale_unorm8(c.r, splat(31)),
                      g = scale_unorm8(c.g, splat(63)),
                      b = scale_unorm8(c.b, splat(31));
            return pack(pack(b, g,5), r,11);
        }

        // TODO: add native min/max ops to skvm::Builder
        skvm::I32 min(skvm::I32 x, skvm::I32 y) { return select(lt(x,y), x,y); }
        skvm::I32 max(skvm::I32 x, skvm::I32 y) { return select(gt(x,y), x,y); }

        static bool CanBuild(const Key& key) {
            // These checks parallel the TODOs in Builder::Builder().
            if (key.shader) {
                // TODO: maybe passing the whole Key is going to be what we'll want here?
                if (!as_SB(key.shader)->program(nullptr,
                                                key.colorSpace.get(),
                                                skvm::Arg{0}, 0,
                                                nullptr,nullptr,nullptr,nullptr)) {
                    return false;
                }
            }
            if (key.colorFilter) { return false; }

            switch (key.colorType) {
                default: return false;
                case kRGB_565_SkColorType:   break;
                case kRGBA_8888_SkColorType: break;
                case kBGRA_8888_SkColorType: break;
            }

            if (key.alphaType == kUnpremul_SkAlphaType) { return false; }

            switch (key.blendMode) {
                default: return false;
                case SkBlendMode::kSrc:     break;
                case SkBlendMode::kSrcOver: break;
            }

            return true;
        }

        explicit Builder(const Key& key) {
        #define TODO SkUNREACHABLE
            SkASSERT(CanBuild(key));
            skvm::Arg uniforms = uniform(),
                      dst_ptr  = arg(SkColorTypeBytesPerPixel(key.colorType));
            // If coverage is Mask3D there'll next come two varyings for mul and add planes,
            // and then finally if coverage is any Mask?? format, a varying for the mask.

            Color src = unpack_8888(uniform32(uniforms, offsetof(Uniforms, paint_color)));
            if (key.shader) {
                skvm::I32 paint_alpha = src.a;
                SkAssertResult(as_SB(key.shader)->program(this,
                                                          key.colorSpace.get(),
                                                          uniforms, sizeof(Uniforms),
                                                          &src.r, &src.g, &src.b, &src.a));

                // TODO: skip when paint is opaque.
                if (false) {
                    src.r = scale_unorm8(src.r, paint_alpha);
                    src.g = scale_unorm8(src.g, paint_alpha);
                    src.b = scale_unorm8(src.b, paint_alpha);
                    src.a = scale_unorm8(src.a, paint_alpha);
                }
            }
            if (key.colorFilter) { TODO; }

            if (key.coverage == Coverage::Mask3D) {
                skvm::I32 M = load8(varying<uint8_t>()),
                          A = load8(varying<uint8_t>());

                src.r = min(add(scale_unorm8(src.r, M), A), src.a);
                src.g = min(add(scale_unorm8(src.g, M), A), src.a);
                src.b = min(add(scale_unorm8(src.b, M), A), src.a);
            }

            // There are several orderings here of when we load dst and coverage
            // and how coverage is applied, and to complicate things, LCD coverage
            // needs to know dst.a.  We're careful to assert it's loaded in time.
            Color dst;
            SkDEBUGCODE(bool dst_loaded = false;)

            // load_coverage() returns false when there's no need to apply coverage.
            auto load_coverage = [&](Color* cov) {
                switch (key.coverage) {
                    case Coverage::Full: return false;

                    case Coverage::UniformA8: cov->r = cov->g = cov->b = cov->a =
                                              uniform8(uniforms, offsetof(Uniforms, coverage));
                                              return true;

                    case Coverage::Mask3D:
                    case Coverage::MaskA8: cov->r = cov->g = cov->b = cov->a =
                                           load8(varying<uint8_t>());
                                           return true;

                    case Coverage::MaskLCD16:
                        SkASSERT(dst_loaded);
                        *cov = unpack_565(load16(varying<uint16_t>()));
                        cov->a = select(lt(src.a, dst.a), min(cov->r, min(cov->g,cov->b))
                                                        , max(cov->r, max(cov->g,cov->b)));
                        return true;
                }
                // GCC insists...
                return false;
            };

            // The math for some blend modes lets us fold coverage into src before the blend,
            // obviating the need for the lerp afterwards. This early-coverage strategy tends
            // to be both faster and require fewer registers.
            bool lerp_coverage_post_blend = true;
            if (SkBlendMode_ShouldPreScaleCoverage(key.blendMode,
                                                   key.coverage == Coverage::MaskLCD16)) {
                Color cov;
                if (load_coverage(&cov)) {
                    src.r = scale_unorm8(src.r, cov.r);
                    src.g = scale_unorm8(src.g, cov.g);
                    src.b = scale_unorm8(src.b, cov.b);
                    src.a = scale_unorm8(src.a, cov.a);
                }
                lerp_coverage_post_blend = false;
            }

            // Load up the destination color.
            SkDEBUGCODE(dst_loaded = true;)
            switch (key.colorType) {
                default: TODO;
                case kRGB_565_SkColorType:   dst = unpack_565 (load16(dst_ptr)); break;
                case kRGBA_8888_SkColorType: dst = unpack_8888(load32(dst_ptr)); break;
                case kBGRA_8888_SkColorType: dst = unpack_8888(load32(dst_ptr));
                                             std::swap(dst.r, dst.b);
                                             break;
            }

            // When a destination is tagged opaque, we may assume it both starts and stays fully
            // opaque, ignoring any math that disagrees.  So anything involving force_opaque is
            // optional, and sometimes helps cut a small amount of work in these programs.
            const bool force_opaque = true && key.alphaType == kOpaque_SkAlphaType;
            if (force_opaque) { dst.a = splat(0xff); }

            // We'd need to premul dst after loading and unpremul before storing.
            if (key.alphaType == kUnpremul_SkAlphaType) { TODO; }

            // Blend src and dst.
            switch (key.blendMode) {
                default: TODO;

                case SkBlendMode::kSrc: break;

                case SkBlendMode::kSrcOver: {
                    auto invA = sub(splat(255), src.a);
                    src.r = add(src.r, scale_unorm8(dst.r, invA));
                    src.g = add(src.g, scale_unorm8(dst.g, invA));
                    src.b = add(src.b, scale_unorm8(dst.b, invA));
                    src.a = add(src.a, scale_unorm8(dst.a, invA));
                } break;
            }

            // Lerp with coverage post-blend if needed.
            Color cov;
            if (lerp_coverage_post_blend && load_coverage(&cov)) {
                src.r = lerp_unorm8(dst.r, src.r, cov.r);
                src.g = lerp_unorm8(dst.g, src.g, cov.g);
                src.b = lerp_unorm8(dst.b, src.b, cov.b);
                src.a = lerp_unorm8(dst.a, src.a, cov.a);
            }

            if (force_opaque) { src.a = splat(0xff); }

            // Store back to the destination.
            switch (key.colorType) {
                default: SkUNREACHABLE;

                case kRGB_565_SkColorType:   store16(dst_ptr, pack_565(src)); break;

                case kBGRA_8888_SkColorType: std::swap(src.r, src.b);  // fallthrough
                case kRGBA_8888_SkColorType: store32(dst_ptr, pack_8888(src)); break;
            }
        #undef TODO
        }
    };

    class Blitter final : public SkBlitter {
    public:
        bool ok = false;

        Blitter(const SkPixmap& device, const SkPaint& paint)
            : fDevice(device)
            , fKey {
                device.colorType(),
                device.alphaType(),
                Coverage::Full,
                paint.getBlendMode(),
                device.refColorSpace(),
                paint.refShader(),
                paint.refColorFilter(),
            }
        {
            SkColor4f color = paint.getColor4f();
            SkColorSpaceXformSteps{sk_srgb_singleton(), kUnpremul_SkAlphaType,
                                   device.colorSpace(), kUnpremul_SkAlphaType}.apply(color.vec());

            if (color.fitsInBytes() && Builder::CanBuild(fKey)) {
                uint32_t rgba = color.premul().toBytes_RGBA();
                memcpy(fUniforms.data() + offsetof(Uniforms, paint_color), &rgba, sizeof(rgba));
                ok = true;
            }
        }

        ~Blitter() override {
            if (SkLRUCache<Key, skvm::Program>* cache = try_acquire_program_cache()) {
                auto cache_program = [&](skvm::Program&& program, Coverage coverage) {
                    if (!program.empty()) {
                        Key key = fKey.withCoverage(coverage);
                        if (skvm::Program* found = cache->find(key)) {
                            *found = std::move(program);
                        } else {
                            cache->insert(key, std::move(program));
                        }
                    }
                };
                cache_program(std::move(fBlitH),         Coverage::Full);
                cache_program(std::move(fBlitAntiH),     Coverage::UniformA8);
                cache_program(std::move(fBlitMaskA8),    Coverage::MaskA8);
                cache_program(std::move(fBlitMask3D),    Coverage::Mask3D);
                cache_program(std::move(fBlitMaskLCD16), Coverage::MaskLCD16);

                release_program_cache();
            }
        }

    private:
        SkPixmap             fDevice;  // TODO: can this be const&?
        const Key            fKey;
        std::vector<uint8_t> fUniforms{sizeof(Uniforms)};
        skvm::Program fBlitH,
                      fBlitAntiH,
                      fBlitMaskA8,
                      fBlitMask3D,
                      fBlitMaskLCD16;

        skvm::Program buildProgram(Coverage coverage) {
            Key key = fKey.withCoverage(coverage);
            {
                skvm::Program p;
                if (SkLRUCache<Key, skvm::Program>* cache = try_acquire_program_cache()) {
                    if (skvm::Program* found = cache->find(key)) {
                        p = std::move(*found);
                    }
                    release_program_cache();
                }
                if (!p.empty()) {
                    return p;
                }
            }
        #if 0
            static std::atomic<int> done{0};
            if (0 == done++) {
                atexit([]{ SkDebugf("%d calls to done\n", done.load()); });
            }
        #endif
            Builder builder{key};
            skvm::Program program = builder.done(debug_name(key).c_str());
            if (!program.hasJIT() && debug_dump(key)) {
                SkDebugf("\nfalling back to interpreter for blitter with this key.\n");
                builder.dump();
                program.dump();
            }
            return program;
        }

        void updateUniforms() {
            if (const SkShaderBase* shader = as_SB(fKey.shader)) {
                size_t extra = shader->uniforms(fKey.colorSpace.get(), nullptr);
                fUniforms.resize(sizeof(Uniforms) + extra);
                shader->uniforms(fKey.colorSpace.get(), fUniforms.data() + sizeof(Uniforms));
            }
        }

        void blitH(int x, int y, int w) override {
            if (fBlitH.empty()) {
                fBlitH = this->buildProgram(Coverage::Full);
            }
            this->updateUniforms();
            fBlitH.eval(w, fUniforms.data(), fDevice.addr(x,y));
        }

        void blitAntiH(int x, int y, const SkAlpha cov[], const int16_t runs[]) override {
            if (fBlitAntiH.empty()) {
                fBlitAntiH = this->buildProgram(Coverage::UniformA8);
            }
            this->updateUniforms();
            for (int16_t run = *runs; run > 0; run = *runs) {
                memcpy(fUniforms.data() + offsetof(Uniforms, coverage), cov, sizeof(*cov));
                fBlitAntiH.eval(run, fUniforms.data(), fDevice.addr(x,y));

                x    += run;
                runs += run;
                cov  += run;
            }
        }

        void blitMask(const SkMask& mask, const SkIRect& clip) override {
            if (mask.fFormat == SkMask::kBW_Format) {
                // TODO: native BW masks?
                return SkBlitter::blitMask(mask, clip);
            }

            const skvm::Program* program = nullptr;
            switch (mask.fFormat) {
                default: SkUNREACHABLE;     // ARGB and SDF masks shouldn't make it here.

                case SkMask::k3D_Format:
                    if (fBlitMask3D.empty()) {
                        fBlitMask3D = this->buildProgram(Coverage::Mask3D);
                    }
                    program = &fBlitMask3D;
                    break;

                case SkMask::kA8_Format:
                    if (fBlitMaskA8.empty()) {
                        fBlitMaskA8 = this->buildProgram(Coverage::MaskA8);
                    }
                    program = &fBlitMaskA8;
                    break;

                case SkMask::kLCD16_Format:
                    if (fBlitMaskLCD16.empty()) {
                        fBlitMaskLCD16 = this->buildProgram(Coverage::MaskLCD16);
                    }
                    program = &fBlitMaskLCD16;
                    break;
            }

            SkASSERT(program);
            if (program) {
                this->updateUniforms();
                for (int y = clip.top(); y < clip.bottom(); y++) {
                    void* dptr =        fDevice.writable_addr(clip.left(), y);
                    auto  mptr = (const uint8_t*)mask.getAddr(clip.left(), y);

                    if (program == &fBlitMask3D) {
                        size_t plane = mask.computeImageSize();
                        program->eval(clip.width(), fUniforms.data(),
                                      dptr,
                                      mptr + 1*plane,
                                      mptr + 2*plane,
                                      mptr + 0*plane);
                    } else {
                        program->eval(clip.width(), fUniforms.data(),
                                      dptr, mptr);
                    }
                }
            }
        }
    };

}  // namespace


SkBlitter* SkCreateSkVMBlitter(const SkPixmap& device,
                               const SkPaint& paint,
                               const SkMatrix& ctm,
                               SkArenaAlloc* alloc) {
    auto blitter = alloc->make<Blitter>(device, paint);
    return blitter->ok ? blitter
                       : nullptr;
}
