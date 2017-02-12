/*
    enoki/array_avx512.h -- Packed SIMD array (AVX512 specialization)

    Enoki is a C++ template library that enables transparent vectorization
    of numerical kernels using SIMD instruction sets available on current
    processor architectures.

    Copyright (c) 2017 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE.txt file.
*/

#pragma once

#include "array_avx2.h"

NAMESPACE_BEGIN(enoki)
NAMESPACE_BEGIN(detail)

template <> struct is_native<float, 16> : std::true_type { };
template <> struct is_native<double, 8> : std::true_type { };
template <typename T> struct is_native<T, 16, is_int32_t<T>> : std::true_type { };
template <typename T> struct is_native<T, 8,  is_int64_t<T>> : std::true_type { };

/// Wraps an individual bit of a mask register
struct KMaskBit {
    bool value : 1;

    operator bool() const { return value; }

    friend std::ostream &operator<<(std::ostream &os, const KMaskBit &b) {
        os << (b.value ? '1' : '0');
        return os;
    }
};

/// Wrapper for AVX512 k0-k7 mask registers
template <typename Type>
struct KMask : StaticArrayBase<detail::KMaskBit, sizeof(Type) * 8, false,
                               RoundingMode::Default, KMask<Type>> {
    using Base = StaticArrayBase<detail::KMaskBit, sizeof(Type) * 8, false,
                                 RoundingMode::Default, KMask<Type>>;
    static constexpr bool IsMask = true;
    static constexpr bool Native = true;
    using Base::Size;
    using Expr = KMask;
    using Mask = KMask;
    using HalfType = __mmask8;
    Type k;

    ENOKI_INLINE KMask() { }
    ENOKI_INLINE explicit KMask(Type k) : k(k) { }
    template <typename T, std::enable_if_t<std::is_same<T, bool>::value, int> = 0>
    ENOKI_INLINE KMask(T b) : k(b ? Type(-1) : Type(0)) { }
    ENOKI_INLINE KMask(KMask k, reinterpret_flag) : k(k.k) { }

#if defined(__AVX512VL__)
    ENOKI_REINTERPRET_KMASK(float, 8)
        : k(_mm256_test_epi32_mask(_mm256_castps_si256(a.derived().m),
                                   _mm256_castps_si256(a.derived().m))) { }
    ENOKI_REINTERPRET_KMASK(int32_t, 8)  : k(_mm256_test_epi32_mask(a.derived().m, a.derived().m)) { }
    ENOKI_REINTERPRET_KMASK(uint32_t, 8) : k(_mm256_test_epi32_mask(a.derived().m, a.derived().m)) { }
#else
    ENOKI_REINTERPRET_KMASK(float, 8)    : k(Type(_mm256_movemask_ps(a.derived().m))) { }
    ENOKI_REINTERPRET_KMASK(int32_t, 8)  : k(Type(_mm256_movemask_ps(_mm256_castsi256_ps(a.derived().m)))) { }
    ENOKI_REINTERPRET_KMASK(uint32_t, 8) : k(Type(_mm256_movemask_ps(_mm256_castsi256_ps(a.derived().m)))) { }
#endif

    ENOKI_REINTERPRET_KMASK(double, 16)   { k = _mm512_kunpackb(high(a).k, low(a).k); }
    ENOKI_REINTERPRET_KMASK(int64_t, 16)  { k = _mm512_kunpackb(high(a).k, low(a).k); }
    ENOKI_REINTERPRET_KMASK(uint64_t, 16) { k = _mm512_kunpackb(high(a).k, low(a).k); }

    ENOKI_INLINE KMask or_(KMask a) const {
        if (Size == 16) /* Use intrinsic if possible */
            return KMask(Type(_mm512_kor((__mmask16) k, (__mmask16) (a.k))));
        else
            return KMask(Type(k | a.k));
    }

    ENOKI_INLINE KMask and_(KMask a) const {
        if (Size == 16) /* Use intrinsic if possible */
            return KMask(Type(_mm512_kand((__mmask16) k, (__mmask16) (a.k))));
        else
            return KMask(Type(k & a.k));
    }

    ENOKI_INLINE KMask xor_(KMask a) const {
        if (Size == 16) /* Use intrinsic if possible */
            return KMask(Type(_mm512_kxor((__mmask16) k, (__mmask16) (a.k))));
        else
            return KMask(Type(k ^ a.k));
    }

    ENOKI_INLINE KMask not_() const {
        if (Size == 16) /* Use intrinsic if possible */
            return KMask(Type(_mm512_knot((__mmask16) k)));
        else
            return KMask(Type(~k));
    }

    ENOKI_INLINE bool all_() const {
        if (std::is_same<Type, __mmask16>::value)
            return _mm512_kortestc(k, k);
        else
            return k == Type((1 << Size) - 1);
    }

    ENOKI_INLINE bool none_() const {
        if (std::is_same<Type, __mmask16>::value)
            return _mm512_kortestz(k, k);
        else
            return k == Type(0);
    }

    ENOKI_INLINE bool any_() const {
        if (std::is_same<Type, __mmask16>::value)
            return !_mm512_kortestz(k, k);
        else
            return k != Type(0);
    }

    ENOKI_INLINE size_t count_() const {
        return (size_t) _mm_popcnt_u32((unsigned int) k);
    }

    ENOKI_INLINE KMaskBit coeff(size_t i) const {
        assert(i < Size);
        return KMaskBit { (k & (1 << i)) != 0 };
    }

    KMask<HalfType> low_()  const { return KMask<HalfType>(HalfType(k)); }
    KMask<HalfType> high_() const { return KMask<HalfType>(HalfType(k >> (Size/2))); }
};

NAMESPACE_END(detail)

/// Partial overload of StaticArrayImpl using AVX512 intrinsics (single precision)
template <bool Approx, RoundingMode Mode, typename Derived> struct alignas(64)
    StaticArrayImpl<float, 16, Approx, Mode, Derived>
    : StaticArrayBase<float, 16, Approx, Mode, Derived> {

    ENOKI_NATIVE_ARRAY(float, 16, Approx, __m512, Mode)
    using Mask = detail::KMask<__mmask16>;

    // -----------------------------------------------------------------------
    //! @{ \name Value constructors
    // -----------------------------------------------------------------------

    ENOKI_INLINE StaticArrayImpl(Scalar value) : m(_mm512_set1_ps(value)) { }
    ENOKI_INLINE StaticArrayImpl(Scalar f0, Scalar f1, Scalar f2, Scalar f3,
                                 Scalar f4, Scalar f5, Scalar f6, Scalar f7,
                                 Scalar f8, Scalar f9, Scalar f10, Scalar f11,
                                 Scalar f12, Scalar f13, Scalar f14, Scalar f15)
        : m(_mm512_setr_ps(f0, f1, f2, f3, f4, f5, f6, f7, f8,
                           f9, f10, f11, f12, f13, f14, f15)) { }


    // -----------------------------------------------------------------------
    //! @{ \name Type converting constructors
    // -----------------------------------------------------------------------

    ENOKI_CONVERT(half)
        : m(_mm512_cvtph_ps(_mm256_loadu_si256((const __m256i *) a.data()))) { }

    ENOKI_CONVERT(float) : m(a.derived().m) { }

    ENOKI_CONVERT(int32_t) : m(_mm512_cvt_roundepi32_ps(a.derived().m, (int) Mode)) { }

    ENOKI_CONVERT(uint32_t) : m(_mm512_cvt_roundepu32_ps(a.derived().m, (int) Mode)) { }

    ENOKI_CONVERT(double)
        : m(detail::concat(_mm512_cvt_roundpd_ps(low(a).m, (int) Mode),
                           _mm512_cvt_roundpd_ps(high(a).m, (int) Mode))) { }

#if defined(__AVX512DQ__)
    ENOKI_CONVERT(int64_t)
        : m(detail::concat(_mm512_cvt_roundepi64_ps(low(a).m, (int) Mode),
                           _mm512_cvt_roundepi64_ps(high(a).m, (int) Mode))) { }

    ENOKI_CONVERT(uint64_t)
        : m(detail::concat(_mm512_cvt_roundepu64_ps(low(a).m, (int) Mode),
                           _mm512_cvt_roundepu64_ps(high(a).m, (int) Mode))) { }
#endif

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Reinterpreting constructors, mask converters
    // -----------------------------------------------------------------------

    ENOKI_REINTERPRET(float) : m(a.derived().m) { }

    ENOKI_REINTERPRET(int32_t) : m(_mm512_castsi512_ps(a.derived().m)) { }
    ENOKI_REINTERPRET(uint32_t) : m(_mm512_castsi512_ps(a.derived().m)) { }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Converting from/to half size vectors
    // -----------------------------------------------------------------------

    StaticArrayImpl(const Array1 &a1, const Array2 &a2)
        : m(detail::concat(a1.m, a2.m)) { }

    ENOKI_INLINE Array1 low_()  const { return _mm512_castps512_ps256(m); }
    ENOKI_INLINE Array2 high_() const {
        #if defined(__AVX512DQ__)
            return _mm512_extractf32x8_ps(m, 1);
        #else
            return _mm256_castpd_ps(_mm512_extractf64x4_pd(_mm512_castps_pd(m), 1));
        #endif
    }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Vertical operations
    // -----------------------------------------------------------------------

    ENOKI_INLINE Derived add_(Arg a) const { return _mm512_add_round_ps(m, a.m, (int) Mode); }
    ENOKI_INLINE Derived sub_(Arg a) const { return _mm512_sub_round_ps(m, a.m, (int) Mode); }
    ENOKI_INLINE Derived mul_(Arg a) const { return _mm512_mul_round_ps(m, a.m, (int) Mode); }
    ENOKI_INLINE Derived div_(Arg a) const { return _mm512_div_round_ps(m, a.m, (int) Mode); }
    ENOKI_INLINE Derived or_ (Arg a) const {
        #if defined(__AVX512DQ__)
            return _mm512_or_ps(m, a.m);
        #else
            return _mm512_castsi512_ps(
                _mm512_or_si512(_mm512_castps_si512(m), _mm512_castps_si512(a.m)));
        #endif
    }

    ENOKI_INLINE Derived or_ (Mask a) const {
        return _mm512_mask_mov_ps(m, a.k, _mm512_set1_ps(memcpy_cast<Scalar>(int32_t(-1))));
    }

    ENOKI_INLINE Derived and_(Arg a) const {
        #if defined(__AVX512DQ__)
            return _mm512_and_ps(m, a.m);
        #else
            return _mm512_castsi512_ps(
                _mm512_and_si512(_mm512_castps_si512(m), _mm512_castps_si512(a.m)));
        #endif
    }

    ENOKI_INLINE Derived and_ (Mask a) const {
        return _mm512_maskz_mov_ps(a.k, m);
    }

    ENOKI_INLINE Derived xor_(Arg a) const {
        #if defined(__AVX512DQ__)
            return _mm512_xor_ps(m, a.m);
        #else
            return _mm512_castsi512_ps(
                _mm512_xor_si512(_mm512_castps_si512(m), _mm512_castps_si512(a.m)));
        #endif
    }

    ENOKI_INLINE Derived xor_ (Mask a) const {
        #if defined(__AVX512DQ__)
            const __m512 v1 = _mm512_set1_ps(memcpy_cast<Scalar>(int32_t(-1)));
            return _mm512_mask_xor_ps(m, a.k, m, v1);
        #else
            const __m512i v0 = _mm512_castps_si512(m);
            const __m512i v1 = _mm512_set1_epi32(int32_t(-1));
            return _mm512_castsi512_ps(_mm512_mask_xor_epi32(v0, a.k, v0, v1));
        #endif
    }

    ENOKI_INLINE Mask lt_ (Arg a) const { return Mask(_mm512_cmp_ps_mask(m, a.m, _CMP_LT_OQ));  }
    ENOKI_INLINE Mask gt_ (Arg a) const { return Mask(_mm512_cmp_ps_mask(m, a.m, _CMP_GT_OQ));  }
    ENOKI_INLINE Mask le_ (Arg a) const { return Mask(_mm512_cmp_ps_mask(m, a.m, _CMP_LE_OQ));  }
    ENOKI_INLINE Mask ge_ (Arg a) const { return Mask(_mm512_cmp_ps_mask(m, a.m, _CMP_GE_OQ));  }
    ENOKI_INLINE Mask eq_ (Arg a) const { return Mask(_mm512_cmp_ps_mask(m, a.m, _CMP_EQ_OQ));  }
    ENOKI_INLINE Mask neq_(Arg a) const { return Mask(_mm512_cmp_ps_mask(m, a.m, _CMP_NEQ_UQ)); }

    ENOKI_INLINE Derived abs_() const {
        #if defined(__AVX512DQ__)
            return _mm512_andnot_ps(_mm512_set1_ps(-0.f), m);
        #else
            return _mm512_castsi512_ps(
                _mm512_andnot_si512(_mm512_set1_epi32(memcpy_cast<int32_t>(-0.f)),
                                    _mm512_castps_si512(m)));
        #endif
    }

    ENOKI_INLINE Derived min_(Arg b) const { return _mm512_min_ps(b.m, m); }
    ENOKI_INLINE Derived max_(Arg b) const { return _mm512_max_ps(b.m, m); }
    ENOKI_INLINE Derived ceil_()     const { return _mm512_ceil_ps(m);     }
    ENOKI_INLINE Derived floor_()    const { return _mm512_floor_ps(m);    }
    ENOKI_INLINE Derived sqrt_()     const { return _mm512_sqrt_round_ps(m, (int) Mode); }

    ENOKI_INLINE Derived round_() const {
        return _mm512_roundscale_ps(m, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    }

    ENOKI_INLINE Derived fmadd_(Arg b, Arg c) const {
        return _mm512_fmadd_round_ps(m, b.m, c.m, (int) Mode);
    }

    ENOKI_INLINE Derived fmsub_(Arg b, Arg c) const {
        return _mm512_fmsub_round_ps(m, b.m, c.m, (int) Mode);
    }

    ENOKI_INLINE static Derived select_(const Mask &m, const Derived &t,
                                        const Derived &f) {
        return _mm512_mask_blend_ps(m.k, f.m, t.m);
    }

    template <size_t I0, size_t I1, size_t I2, size_t I3, size_t I4, size_t I5,
              size_t I6, size_t I7, size_t I8, size_t I9, size_t I10,
              size_t I11, size_t I12, size_t I13, size_t I14, size_t I15>
    ENOKI_INLINE Derived shuffle_() const {
        const __m512i idx =
            _mm512_setr_epi32(I0, I1, I2, I3, I4, I5, I6, I7, I8,
                              I9, I10, I11, I12, I13, I14, I15);
        return _mm512_permutexvar_ps(idx, m);
    }

    ENOKI_INLINE Derived rcp_() const {
        if (Approx) {
            /* Use best reciprocal approximation available on the current
               hardware and potentially refine */
            __m512 r;
            #if defined(__AVX512ER__)
                /* rel err < 2^28, use as is */
                r = _mm512_rcp28_ps(m);
            #else
                r = _mm512_rcp14_ps(m); /* rel error < 2^-14 */

                /* Refine using one Newton-Raphson iteration */
                const __m512 two = _mm512_set1_ps(2.f);
                r = _mm512_mul_ps(r, _mm512_fnmadd_ps(r, m, two));
            #endif

            return r;
        } else {
            return Base::rcp_();
        }
    }

    ENOKI_INLINE Derived rsqrt_() const {
        if (Approx) {
            /* Use best reciprocal square root approximation available
               on the current hardware and potentially refine */
            __m512 r;
            #if defined(__AVX512ER__)
                /* rel err < 2^28, use as is */
                r = _mm512_rsqrt28_ps(m);
            #else
                r = _mm512_rsqrt14_ps(m); /* rel error < 2^-14 */

                /* Refine using one Newton-Raphson iteration */
                const __m512 c0 = _mm512_set1_ps(1.5f);
                const __m512 c1 = _mm512_set1_ps(-0.5f);

                r = _mm512_fmadd_ps(
                    r, c0,
                    _mm512_mul_ps(_mm512_mul_ps(_mm512_mul_ps(m, c1), r),
                                  _mm512_mul_ps(r, r)));
            #endif

            return r;
        } else {
            return Base::rsqrt_();
        }
    }


#if defined(__AVX512ER__)
    ENOKI_INLINE Derived exp_() const {
        if (Approx) {
            return _mm512_exp2a23_ps(
                _mm512_mul_ps(m, _mm512_set1_ps(1.4426950408889634074f)));
        } else {
            return Base::exp_();
        }
    }
#endif

    ENOKI_INLINE Derived ldexp_(Arg arg) const { return _mm512_scalef_ps(m, arg.m); }

    ENOKI_INLINE std::pair<Derived, Derived> frexp_() const {
        return std::make_pair<Derived, Derived>(
            _mm512_getmant_ps(m, _MM_MANT_NORM_p5_1, _MM_MANT_SIGN_src),
            _mm512_add_ps(_mm512_getexp_ps(m), _mm512_set1_ps(1.f)));
    }

    // -----------------------------------------------------------------------
    //! @{ \name Horizontal operations
    // -----------------------------------------------------------------------

    ENOKI_INLINE Scalar hsum_()  const { return hsum(low_() + high_()); }
    ENOKI_INLINE Scalar hprod_() const { return hprod(low_() * high_()); }
    ENOKI_INLINE Scalar hmin_()  const { return hmin(min(low_(), high_())); }
    ENOKI_INLINE Scalar hmax_()  const { return hmax(max(low_(), high_())); }

    //! @}
    // -----------------------------------------------------------------------

    ENOKI_INLINE void store_(void *ptr) const { _mm512_store_ps((Scalar *) ptr, m); }
    ENOKI_INLINE void store_unaligned_(void *ptr) const { _mm512_storeu_ps((Scalar *) ptr, m); }

    ENOKI_INLINE static Derived load_(const void *ptr) { return _mm512_load_ps((const Scalar *) ptr); }
    ENOKI_INLINE static Derived load_unaligned_(const void *ptr) { return _mm512_loadu_ps((const Scalar *) ptr); }

    ENOKI_INLINE static Derived zero_() { return _mm512_setzero_ps(); }

#if defined(__AVX512PF__)
    ENOKI_REQUIRE_INDEX_PF(Index, int32_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index) {
        if (Write)
            _mm512_prefetch_i32scatter_ps(ptr, index.m, Stride, Level);
        else
            _mm512_prefetch_i32gather_ps(index.m, ptr, Stride, Level);
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int32_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index,
                                       const Mask &mask) {
        if (Write)
            _mm512_mask_prefetch_i32scatter_ps(ptr, mask.k, index.m, Stride, Level);
        else
            _mm512_mask_prefetch_i32gather_ps(index.m, mask.k, ptr, Stride, Level);
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int64_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index) {
        if (Write) {
            _mm512_prefetch_i64scatter_ps(ptr, low(index).m, Stride, Level);
            _mm512_prefetch_i64scatter_ps(ptr, high(index).m, Stride, Level);
        } else {
            _mm512_prefetch_i64gather_ps(low(index).m, ptr, Stride, Level);
            _mm512_prefetch_i64gather_ps(high(index).m, ptr, Stride, Level);
        }
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int64_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index,
                                       const Mask &mask) {
        if (Write) {
            _mm512_mask_prefetch_i64scatter_ps(ptr, low(mask).k, low(index).m, Stride, Level);
            _mm512_mask_prefetch_i64scatter_ps(ptr, high(mask).k, high(index).m, Stride, Level);
        } else {
            _mm512_mask_prefetch_i64gather_ps(low(index).m, low(mask).k, ptr, Stride, Level);
            _mm512_mask_prefetch_i64gather_ps(high(index).m, high(mask).k, ptr, Stride, Level);
        }
    }
#endif

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index) {
        return _mm512_i32gather_ps(index.m, (const float *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index, const Mask &mask) {
        return _mm512_mask_i32gather_ps(_mm512_setzero_ps(), mask.k, index.m, (const float *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index) {
        return detail::concat(
            _mm512_i64gather_ps(low(index).m, (const float *) ptr, Stride),
            _mm512_i64gather_ps(high(index).m, (const float *) ptr, Stride));
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index, const Mask &mask) {
        return detail::concat(
            _mm512_mask_i64gather_ps(_mm256_setzero_ps(),  low(mask).k,  low(index).m, (const float *) ptr, Stride),
            _mm512_mask_i64gather_ps(_mm256_setzero_ps(), high(mask).k, high(index).m, (const float *) ptr, Stride));
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index) const {
        _mm512_i32scatter_ps(ptr, index.m, m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index, const Mask &mask) const {
        _mm512_mask_i32scatter_ps(ptr, mask.k, index.m, m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index) const {
        _mm512_i64scatter_ps(ptr, low(index).m,  low(derived()).m,  Stride);
        _mm512_i64scatter_ps(ptr, high(index).m, high(derived()).m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index, const Mask &mask) const {
        _mm512_mask_i64scatter_ps(ptr, low(mask).k,   low(index).m,  low(derived()).m,  Stride);
        _mm512_mask_i64scatter_ps(ptr, high(mask).k, high(index).m, high(derived()).m, Stride);
    }

    ENOKI_INLINE void store_compress_(void *&ptr, const Mask &mask) const {
        __mmask16 k = mask.k;
        _mm512_storeu_ps((float *) ptr, _mm512_mask_compress_ps(_mm512_setzero_ps(), k, m));
        (Scalar *&) ptr += _mm_popcnt_u32(k);
    }

    ENOKI_INLINE void massign_(const Mask &mask, const Derived &e) {
        m = _mm512_mask_mov_ps(m, mask.k, e.m);
    }

    //! @}
    // -----------------------------------------------------------------------
};

/// Partial overload of StaticArrayImpl using AVX512 intrinsics (double precision)
template <bool Approx, RoundingMode Mode, typename Derived> struct alignas(64)
    StaticArrayImpl<double, 8, Approx, Mode, Derived>
    : StaticArrayBase<double, 8, Approx, Mode, Derived> {

    ENOKI_NATIVE_ARRAY(double, 8, Approx, __m512d, Mode)
    using Mask = detail::KMask<__mmask8>;

    // -----------------------------------------------------------------------
    //! @{ \name Value constructors
    // -----------------------------------------------------------------------

    ENOKI_INLINE StaticArrayImpl(Scalar value) : m(_mm512_set1_pd(value)) { }
    ENOKI_INLINE StaticArrayImpl(Scalar f0, Scalar f1, Scalar f2, Scalar f3,
                                 Scalar f4, Scalar f5, Scalar f6, Scalar f7)
        : m(_mm512_setr_pd(f0, f1, f2, f3, f4, f5, f6, f7)) { }


    // -----------------------------------------------------------------------
    //! @{ \name Type converting constructors
    // -----------------------------------------------------------------------

    ENOKI_CONVERT(half)
        : m(_mm512_cvtps_pd(
              _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *) a.data())))) { }

    ENOKI_CONVERT(float) : m(_mm512_cvtps_pd(a.derived().m)) { }

    ENOKI_CONVERT(double) : m(a.derived().m) { }

    ENOKI_CONVERT(int32_t) : m(_mm512_cvtepi32_pd(a.derived().m)) { }

    ENOKI_CONVERT(uint32_t) : m(_mm512_cvtepu32_pd(a.derived().m)) { }

#if defined(__AVX512DQ__)
    ENOKI_CONVERT(int64_t)
        : m(_mm512_cvt_roundepi64_pd(a.derived().m, (int) Mode)) { }

    ENOKI_CONVERT(uint64_t)
        : m(_mm512_cvt_roundepu64_pd(a.derived().m, (int) Mode)) { }
#endif

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Reinterpreting constructors, mask converters
    // -----------------------------------------------------------------------

    ENOKI_REINTERPRET(double) : m(a.derived().m) { }

    ENOKI_REINTERPRET(int64_t) : m(_mm512_castsi512_pd(a.derived().m)) { }
    ENOKI_REINTERPRET(uint64_t) : m(_mm512_castsi512_pd(a.derived().m)) { }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Converting from/to half size vectors
    // -----------------------------------------------------------------------

    StaticArrayImpl(const Array1 &a1, const Array2 &a2)
        : m(detail::concat(a1.m, a2.m)) { }

    ENOKI_INLINE Array1 low_()  const { return _mm512_castpd512_pd256(m); }
    ENOKI_INLINE Array2 high_() const { return _mm512_extractf64x4_pd(m, 1); }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Vertical operations
    // -----------------------------------------------------------------------

    ENOKI_INLINE Derived add_(Arg a) const { return _mm512_add_round_pd(m, a.m, (int) Mode); }
    ENOKI_INLINE Derived sub_(Arg a) const { return _mm512_sub_round_pd(m, a.m, (int) Mode); }
    ENOKI_INLINE Derived mul_(Arg a) const { return _mm512_mul_round_pd(m, a.m, (int) Mode); }
    ENOKI_INLINE Derived div_(Arg a) const { return _mm512_div_round_pd(m, a.m, (int) Mode); }
    ENOKI_INLINE Derived or_ (Arg a) const {
        #if defined(__AVX512DQ__)
            return _mm512_or_pd(m, a.m);
        #else
            return _mm512_castsi512_pd(
                _mm512_or_si512(_mm512_castpd_si512(m), _mm512_castpd_si512(a.m)));
        #endif
    }

    ENOKI_INLINE Derived or_ (Mask a) const {
        return _mm512_mask_mov_pd(m, a.k, _mm512_set1_pd(memcpy_cast<Scalar>(int64_t(-1))));
    }

    ENOKI_INLINE Derived and_(Arg a) const {
        #if defined(__AVX512DQ__)
            return _mm512_and_pd(m, a.m);
        #else
            return _mm512_castsi512_pd(
                _mm512_and_si512(_mm512_castpd_si512(m), _mm512_castpd_si512(a.m)));
        #endif
    }

    ENOKI_INLINE Derived and_ (Mask a) const {
        return _mm512_maskz_mov_pd(a.k, m);
    }

    ENOKI_INLINE Derived xor_(Arg a) const {
        #if defined(__AVX512DQ__)
            return _mm512_xor_pd(m, a.m);
        #else
            return _mm512_castsi512_pd(
                _mm512_xor_si512(_mm512_castpd_si512(m), _mm512_castpd_si512(a.m)));
        #endif
    }

    ENOKI_INLINE Derived xor_ (Mask a) const {
        #if defined(__AVX512DQ__)
            const __m512d v1 = _mm512_set1_pd(memcpy_cast<Scalar>(int64_t(-1)));
            return _mm512_mask_xor_pd(m, a.k, m, v1);
        #else
            const __m512i v0 = _mm512_castpd_si512(m);
            const __m512i v1 = _mm512_set1_epi64(int64_t(-1));
            return _mm512_castsi512_pd(_mm512_mask_xor_epi32(v0, a.k, v0, v1));
        #endif
    }

    ENOKI_INLINE Mask lt_ (Arg a) const { return Mask(_mm512_cmp_pd_mask(m, a.m, _CMP_LT_OQ));  }
    ENOKI_INLINE Mask gt_ (Arg a) const { return Mask(_mm512_cmp_pd_mask(m, a.m, _CMP_GT_OQ));  }
    ENOKI_INLINE Mask le_ (Arg a) const { return Mask(_mm512_cmp_pd_mask(m, a.m, _CMP_LE_OQ));  }
    ENOKI_INLINE Mask ge_ (Arg a) const { return Mask(_mm512_cmp_pd_mask(m, a.m, _CMP_GE_OQ));  }
    ENOKI_INLINE Mask eq_ (Arg a) const { return Mask(_mm512_cmp_pd_mask(m, a.m, _CMP_EQ_OQ));  }
    ENOKI_INLINE Mask neq_(Arg a) const { return Mask(_mm512_cmp_pd_mask(m, a.m, _CMP_NEQ_UQ)); }

    ENOKI_INLINE Derived abs_() const {
        #if defined(__AVX512DQ__)
            return _mm512_andnot_pd(_mm512_set1_pd(-0.), m);
        #else
            return _mm512_castsi512_pd(
                _mm512_andnot_si512(_mm512_set1_epi64(memcpy_cast<int64_t>(-0.)),
                                    _mm512_castpd_si512(m)));
        #endif
    }

    ENOKI_INLINE Derived min_(Arg b) const { return _mm512_min_pd(b.m, m); }
    ENOKI_INLINE Derived max_(Arg b) const { return _mm512_max_pd(b.m, m); }
    ENOKI_INLINE Derived ceil_()     const { return _mm512_ceil_pd(m);     }
    ENOKI_INLINE Derived floor_()    const { return _mm512_floor_pd(m);    }
    ENOKI_INLINE Derived sqrt_()     const { return _mm512_sqrt_round_pd(m, (int) Mode); }

    ENOKI_INLINE Derived round_() const {
        return _mm512_roundscale_pd(m, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    }

    ENOKI_INLINE Derived fmadd_(Arg b, Arg c) const {
        return _mm512_fmadd_round_pd(m, b.m, c.m, (int) Mode);
    }

    ENOKI_INLINE Derived fmsub_(Arg b, Arg c) const {
        return _mm512_fmsub_round_pd(m, b.m, c.m, (int) Mode);
    }

    ENOKI_INLINE static Derived select_(const Mask &m, const Derived &t,
                                        const Derived &f) {
        return _mm512_mask_blend_pd(m.k, f.m, t.m);
    }

    template <size_t I0, size_t I1, size_t I2, size_t I3, size_t I4, size_t I5,
              size_t I6, size_t I7>
    ENOKI_INLINE Derived shuffle_() const {
        const __m512i idx =
            _mm512_setr_epi64(I0, I1, I2, I3, I4, I5, I6, I7);
        return _mm512_permutexvar_pd(idx, m);
    }

    ENOKI_INLINE Derived rcp_() const {
        if (Approx) {
            /* Use best reciprocal approximation available on the current
               hardware and potentially refine */
            __m512d r;
            #if defined(__AVX512ER__)
                /* rel err < 2^28, use as is */
                r = _mm512_rcp28_pd(m);
            #else
                r = _mm512_rcp14_pd(m); /* rel error < 2^-14 */

                /* Refine using two Newton-Raphson iterations */
                const __m512d two = _mm512_set1_pd(2);
                for (int i = 0; i < 2; ++i)
                    r = _mm512_mul_pd(r, _mm512_fnmadd_pd(r, m, two));
            #endif

            return r;
        } else {
            return Base::rcp_();
        }
    }

    ENOKI_INLINE Derived rsqrt_() const {
        if (Approx) {
            /* Use best reciprocal square root approximation available
               on the current hardware and potentially refine */
            __m512d r;
            #if defined(__AVX512ER__)
                /* rel err < 2^28, use as is */
                r = _mm512_rsqrt28_pd(m);
            #else
                r = _mm512_rsqrt14_pd(m); /* rel error < 2^-14 */

                /* Refine using two Newton-Raphson iterations */
                const __m512d c0 = _mm512_set1_pd(1.5);
                const __m512d c1 = _mm512_set1_pd(-0.5);

                for (int i = 0; i< 2; ++i) {
                    r = _mm512_fmadd_pd(
                        r, c0,
                        _mm512_mul_pd(_mm512_mul_pd(_mm512_mul_pd(m, c1), r),
                                      _mm512_mul_pd(r, r)));
                }
            #endif

            return r;
        } else {
            return Base::rsqrt_();
        }
    }


#if defined(__AVX512ER__)
    ENOKI_INLINE Derived exp_() const {
        if (Approx) {
            return _mm512_exp2a23_pd(
                _mm512_mul_pd(m, _mm512_set1_pd(1.4426950408889634074f)));
        } else {
            return Base::exp_();
        }
    }
#endif

    ENOKI_INLINE Derived ldexp_(Arg arg) const { return _mm512_scalef_pd(m, arg.m); }

    ENOKI_INLINE std::pair<Derived, Derived> frexp_() const {
        return std::make_pair<Derived, Derived>(
            _mm512_getmant_pd(m, _MM_MANT_NORM_p5_1, _MM_MANT_SIGN_src),
            _mm512_add_pd(_mm512_getexp_pd(m), _mm512_set1_pd(1.f)));
    }

    // -----------------------------------------------------------------------
    //! @{ \name Horizontal operations
    // -----------------------------------------------------------------------

    ENOKI_INLINE Scalar hsum_()  const { return hsum(low_() + high_()); }
    ENOKI_INLINE Scalar hprod_() const { return hprod(low_() * high_()); }
    ENOKI_INLINE Scalar hmin_()  const { return hmin(min(low_(), high_())); }
    ENOKI_INLINE Scalar hmax_()  const { return hmax(max(low_(), high_())); }

    //! @}
    // -----------------------------------------------------------------------

    ENOKI_INLINE void store_(void *ptr) const { _mm512_store_pd((Scalar *) ptr, m); }
    ENOKI_INLINE void store_unaligned_(void *ptr) const { _mm512_storeu_pd((Scalar *) ptr, m); }

    ENOKI_INLINE static Derived load_(const void *ptr) { return _mm512_load_pd((const Scalar *) ptr); }
    ENOKI_INLINE static Derived load_unaligned_(const void *ptr) { return _mm512_loadu_pd((const Scalar *) ptr); }

    ENOKI_INLINE static Derived zero_() { return _mm512_setzero_pd(); }

#if defined(__AVX512PF__)
    ENOKI_REQUIRE_INDEX_PF(Index, int32_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index) {
        if (Write)
            _mm512_prefetch_i32scatter_pd(ptr, index.m, Stride, Level);
        else
            _mm512_prefetch_i32gather_pd(index.m, ptr, Stride, Level);
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int32_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index,
                                       const Mask &mask) {
        if (Write)
            _mm512_mask_prefetch_i32scatter_pd(ptr, mask.k, index.m, Stride, Level);
        else
            _mm512_mask_prefetch_i32gather_pd(index.m, mask.k, ptr, Stride, Level);
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int64_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index) {
        if (Write)
            _mm512_prefetch_i64scatter_pd(ptr, index.m, Stride, Level);
        else
            _mm512_prefetch_i64gather_pd(index.m, ptr, Stride, Level);
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int64_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index,
                                       const Mask &mask) {
        if (Write)
            _mm512_mask_prefetch_i64scatter_pd(ptr, mask.k, index.m, Stride, Level);
        else
            _mm512_mask_prefetch_i64gather_pd(index.m, mask.k, ptr, Stride, Level);
    }
#endif

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index) {
        return _mm512_i32gather_pd(index.m, (const double *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index, const Mask &mask) {
        return _mm512_mask_i32gather_pd(_mm512_setzero_pd(), mask.k, index.m, (const double *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index) {
        return _mm512_i64gather_pd(index.m, (const double *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index, const Mask &mask) {
        return _mm512_mask_i64gather_pd(_mm512_setzero_pd(), mask.k, index.m, (const double *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index) const {
        _mm512_i32scatter_pd(ptr, index.m, m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index, const Mask &mask) const {
        _mm512_mask_i32scatter_pd(ptr, mask.k, index.m, m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index) const {
        _mm512_i64scatter_pd(ptr, index.m, derived().m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index, const Mask &mask) const {
        _mm512_mask_i64scatter_pd(ptr, mask.k, index.m, m, Stride);
    }

    ENOKI_INLINE void store_compress_(void *&ptr, const Mask &mask) const {
        __mmask8 k = mask.k;
        _mm512_storeu_pd((double *) ptr, _mm512_mask_compress_pd(_mm512_setzero_pd(), k, m));
        (Scalar *&) ptr += _mm_popcnt_u32(k);
    }

    ENOKI_INLINE void massign_(const Mask &mask, const Derived &e) {
        m = _mm512_mask_mov_pd(m, mask.k, e.m);
    }

    //! @}
    // -----------------------------------------------------------------------
};

/// Partial overload of StaticArrayImpl using AVX512 intrinsics (32 bit integers)
template <typename Scalar_, typename Derived> struct alignas(64)
    StaticArrayImpl<Scalar_, 16, false, RoundingMode::Default, Derived, detail::is_int32_t<Scalar_>>
    : StaticArrayBase<Scalar_, 16, false, RoundingMode::Default, Derived> {

    ENOKI_NATIVE_ARRAY(Scalar_, 16, false, __m512i, RoundingMode::Default)
    using Mask = detail::KMask<__mmask16>;

    // -----------------------------------------------------------------------
    //! @{ \name Value constructors
    // -----------------------------------------------------------------------

    ENOKI_INLINE StaticArrayImpl(Scalar value) : m(_mm512_set1_epi32((int32_t) value)) { }
    ENOKI_INLINE StaticArrayImpl(Scalar f0, Scalar f1, Scalar f2, Scalar f3,
                                 Scalar f4, Scalar f5, Scalar f6, Scalar f7,
                                 Scalar f8, Scalar f9, Scalar f10, Scalar f11,
                                 Scalar f12, Scalar f13, Scalar f14, Scalar f15)
        : m(_mm512_setr_epi32(
              (int32_t) f0, (int32_t) f1, (int32_t) f2, (int32_t) f3,
              (int32_t) f4, (int32_t) f5, (int32_t) f6, (int32_t) f7,
              (int32_t) f8, (int32_t) f9, (int32_t) f10, (int32_t) f11,
              (int32_t) f12, (int32_t) f13, (int32_t) f14, (int32_t) f15)) { }

    // -----------------------------------------------------------------------
    //! @{ \name Type converting constructors
    // -----------------------------------------------------------------------

    ENOKI_CONVERT(int32_t) : m(a.derived().m) { }
    ENOKI_CONVERT(uint32_t) : m(a.derived().m) { }

    ENOKI_CONVERT(float) {
        if (std::is_signed<Scalar>::value) {
            m = _mm512_cvttps_epi32(a.derived().m);
        } else {
            m = _mm512_cvttps_epu32(a.derived().m);
        }
    }

    ENOKI_CONVERT(double) {
        if (std::is_signed<Scalar>::value) {
            m = detail::concat(_mm512_cvttpd_epi32(low(a).m),
                               _mm512_cvttpd_epi32(high(a).m));
        } else {
            m = detail::concat(_mm512_cvttpd_epu32(low(a).m),
                               _mm512_cvttpd_epu32(high(a).m));
        }
    }

    ENOKI_CONVERT(int64_t)
        : m(detail::concat(_mm512_cvtepi64_epi32(low(a).m),
                           _mm512_cvtepi64_epi32(high(a).m))) { }

    ENOKI_CONVERT(uint64_t)
        : m(detail::concat(_mm512_cvtepi64_epi32(low(a).m),
                           _mm512_cvtepi64_epi32(high(a).m))) { }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Reinterpreting constructors, mask converters
    // -----------------------------------------------------------------------

    ENOKI_REINTERPRET(float) : m(_mm512_castps_si512(a.derived().m)) { }
    ENOKI_REINTERPRET(int32_t) : m(a.derived().m) { }
    ENOKI_REINTERPRET(uint32_t) : m(a.derived().m) { }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Converting from/to half size vectors
    // -----------------------------------------------------------------------

    StaticArrayImpl(const Array1 &a1, const Array2 &a2)
        : m(detail::concat(a1.m, a2.m)) { }

    ENOKI_INLINE Array1 low_()  const { return _mm512_castsi512_si256(m); }
    ENOKI_INLINE Array2 high_() const {
        #if defined(__AVX512DQ__)
            return _mm512_extracti32x8_epi32(m, 1);
        #else
            return _mm512_extracti64x4_epi64(m, 1);
        #endif
    }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Vertical operations
    // -----------------------------------------------------------------------

    ENOKI_INLINE Derived add_(Arg a) const { return _mm512_add_epi32(m, a.m); }
    ENOKI_INLINE Derived sub_(Arg a) const { return _mm512_sub_epi32(m, a.m); }
    ENOKI_INLINE Derived mul_(Arg a) const { return _mm512_mullo_epi32(m, a.m); }
    ENOKI_INLINE Derived or_ (Arg a) const { return _mm512_or_epi32(m, a.m); }

    ENOKI_INLINE Derived or_ (Mask a) const {
        return _mm512_mask_mov_epi32(m, a.k, _mm512_set1_epi32(int32_t(-1)));
    }

    ENOKI_INLINE Derived and_(Arg a) const { return _mm512_and_epi32(m, a.m); }

    ENOKI_INLINE Derived and_ (Mask a) const {
        return _mm512_maskz_mov_epi32(a.k, m);
    }

    ENOKI_INLINE Derived xor_(Arg a) const { return _mm512_xor_epi32(m, a.m); }

    ENOKI_INLINE Derived xor_ (Mask a) const {
        return _mm512_mask_xor_epi32(m, a.k, m, _mm512_set1_epi32(int32_t(-1)));
    }

    template <size_t k> ENOKI_INLINE Derived sli_() const {
        return _mm512_slli_epi32(m, (int) k);
    }

    template <size_t k> ENOKI_INLINE Derived sri_() const {
        if (std::is_signed<Scalar>::value)
            return _mm512_srai_epi32(m, (int) k);
        else
            return _mm512_srli_epi32(m, (int) k);
    }

    ENOKI_INLINE Derived sl_(size_t k) const {
        return _mm512_sll_epi32(m, _mm_set1_epi64x((long long) k));
    }

    ENOKI_INLINE Derived sr_(size_t k) const {
        if (std::is_signed<Scalar>::value)
            return _mm512_sra_epi32(m, _mm_set1_epi64x((long long) k));
        else
            return _mm512_srl_epi32(m, _mm_set1_epi64x((long long) k));
    }

    ENOKI_INLINE Derived slv_(Arg k) const {
        return _mm512_sllv_epi32(m, k.m);
    }

    ENOKI_INLINE Derived srv_(Arg k) const {
        if (std::is_signed<Scalar>::value)
            return _mm512_srav_epi32(m, k.m);
        else
            return _mm512_srlv_epi32(m, k.m);
    }

    ENOKI_INLINE Derived rolv_(Arg k) const { return _mm512_rolv_epi32(m, k.m); }
    ENOKI_INLINE Derived rorv_(Arg k) const { return _mm512_rorv_epi32(m, k.m); }

    ENOKI_INLINE Derived rol_(size_t k) const { return rolv_(_mm512_set1_epi32((int32_t) k)); }
    ENOKI_INLINE Derived ror_(size_t k) const { return rorv_(_mm512_set1_epi32((int32_t) k)); }

    template <size_t Imm>
    ENOKI_INLINE Derived roli_() const { return _mm512_rol_epi32(m, (int) Imm); }

    template <size_t Imm>
    ENOKI_INLINE Derived rori_() const { return _mm512_ror_epi32(m, (int) Imm); }

    ENOKI_INLINE Mask lt_ (Arg a) const { return Mask(_mm512_cmp_epi32_mask(m, a.m, _MM_CMPINT_LT));  }
    ENOKI_INLINE Mask gt_ (Arg a) const { return Mask(_mm512_cmp_epi32_mask(m, a.m, _MM_CMPINT_GT));  }
    ENOKI_INLINE Mask le_ (Arg a) const { return Mask(_mm512_cmp_epi32_mask(m, a.m, _MM_CMPINT_LE));  }
    ENOKI_INLINE Mask ge_ (Arg a) const { return Mask(_mm512_cmp_epi32_mask(m, a.m, _MM_CMPINT_GE));  }
    ENOKI_INLINE Mask eq_ (Arg a) const { return Mask(_mm512_cmp_epi32_mask(m, a.m, _MM_CMPINT_EQ));  }
    ENOKI_INLINE Mask neq_(Arg a) const { return Mask(_mm512_cmp_epi32_mask(m, a.m, _MM_CMPINT_NE)); }

    ENOKI_INLINE Derived min_(Arg a) const {
        if (std::is_signed<Scalar>::value)
            return _mm512_min_epi32(a.m, m);
        else
            return _mm512_min_epu32(a.m, m);
    }

    ENOKI_INLINE Derived max_(Arg a) const {
        if (std::is_signed<Scalar>::value)
            return _mm512_max_epi32(a.m, m);
        else
            return _mm512_max_epu32(a.m, m);
    }

    ENOKI_INLINE Derived abs_() const {
        return std::is_signed<Scalar>::value ? _mm512_abs_epi32(m) : m;
    }

    ENOKI_INLINE static Derived select_(const Mask &m, const Derived &t,
                                        const Derived &f) {
        return _mm512_mask_blend_epi32(m.k, f.m, t.m);
    }

    template <size_t I0, size_t I1, size_t I2, size_t I3, size_t I4, size_t I5,
              size_t I6, size_t I7, size_t I8, size_t I9, size_t I10,
              size_t I11, size_t I12, size_t I13, size_t I14, size_t I15>
    ENOKI_INLINE Derived shuffle_() const {
        const __m512i idx =
            _mm512_setr_epi32(I0, I1, I2, I3, I4, I5, I6, I7, I8,
                              I9, I10, I11, I12, I13, I14, I15);
        return _mm512_permutexvar_epi32(idx, m);
    }

    ENOKI_INLINE Derived mulhi_(Arg a) const {
        const Mask blend(__mmask16(0b0101010101010101));

        if (std::is_signed<Scalar>::value) {
            Derived even(_mm512_srli_epi64(_mm512_mul_epi32(m, a.m), 32));
            Derived odd(_mm512_mul_epi32(_mm512_srli_epi64(m, 32),
                                         _mm512_srli_epi64(a.m, 32)));
            return select(blend, even, odd);
        } else {
            Derived even(_mm512_srli_epi64(_mm512_mul_epu32(m, a.m), 32));
            Derived odd(_mm512_mul_epu32(_mm512_srli_epi64(m, 32),
                                         _mm512_srli_epi64(a.m, 32)));
            return select(blend, even, odd);
        }
    }

    // -----------------------------------------------------------------------
    //! @{ \name Horizontal operations
    // -----------------------------------------------------------------------

    ENOKI_INLINE Scalar hsum_()  const { return hsum(low_() + high_()); }
    ENOKI_INLINE Scalar hprod_() const { return hprod(low_() * high_()); }
    ENOKI_INLINE Scalar hmin_()  const { return hmin(min(low_(), high_())); }
    ENOKI_INLINE Scalar hmax_()  const { return hmax(max(low_(), high_())); }

    //! @}
    // -----------------------------------------------------------------------

    ENOKI_INLINE void store_(void *ptr) const { _mm512_store_si512((__m512i *) ptr, m); }
    ENOKI_INLINE void store_unaligned_(void *ptr) const { _mm512_storeu_si512((__m512i *) ptr, m); }

    ENOKI_INLINE static Derived load_(const void *ptr) { return _mm512_load_si512((const __m512i *) ptr); }
    ENOKI_INLINE static Derived load_unaligned_(const void *ptr) { return _mm512_loadu_si512((const __m512i *) ptr); }

    ENOKI_INLINE static Derived zero_() { return _mm512_setzero_si512(); }

#if defined(__AVX512PF__)
    ENOKI_REQUIRE_INDEX_PF(Index, int32_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index) {
        if (Write)
            _mm512_prefetch_i32scatter_ps(ptr, index.m, Stride, Level);
        else
            _mm512_prefetch_i32gather_ps(index.m, ptr, Stride, Level);
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int32_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index,
                                       const Mask &mask) {
        if (Write)
            _mm512_mask_prefetch_i32scatter_ps(ptr, mask.k, index.m, Stride, Level);
        else
            _mm512_mask_prefetch_i32gather_ps(index.m, mask.k, ptr, Stride, Level);
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int64_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index) {
        if (Write) {
            _mm512_prefetch_i64scatter_ps(ptr, low(index).m, Stride, Level);
            _mm512_prefetch_i64scatter_ps(ptr, high(index).m, Stride, Level);
        } else {
            _mm512_prefetch_i64gather_ps(low(index).m, ptr, Stride, Level);
            _mm512_prefetch_i64gather_ps(high(index).m, ptr, Stride, Level);
        }
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int64_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index,
                                       const Mask &mask) {
        if (Write) {
            _mm512_mask_prefetch_i64scatter_ps(ptr, low(mask).k, low(index).m, Stride, Level);
            _mm512_mask_prefetch_i64scatter_ps(ptr, high(mask).k, high(index).m, Stride, Level);
        } else {
            _mm512_mask_prefetch_i64gather_ps(low(index).m, low(mask).k, ptr, Stride, Level);
            _mm512_mask_prefetch_i64gather_ps(high(index).m, high(mask).k, ptr, Stride, Level);
        }
    }
#endif

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index) {
        return _mm512_i32gather_epi32(index.m, (const float *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index, const Mask &mask) {
        return _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), mask.k, index.m, (const float *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index) {
        return detail::concat(
            _mm512_i64gather_epi32(low(index).m, (const float *) ptr, Stride),
            _mm512_i64gather_epi32(high(index).m, (const float *) ptr, Stride));
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index, const Mask &mask) {
        return detail::concat(
            _mm512_mask_i64gather_epi32(_mm256_setzero_si256(),  low(mask).k,  low(index).m, (const float *) ptr, Stride),
            _mm512_mask_i64gather_epi32(_mm256_setzero_si256(), high(mask).k, high(index).m, (const float *) ptr, Stride));
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index) const {
        _mm512_i32scatter_epi32(ptr, index.m, m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index, const Mask &mask) const {
        _mm512_mask_i32scatter_epi32(ptr, mask.k, index.m, m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index) const {
        _mm512_i64scatter_epi32(ptr, low(index).m,  low(derived()).m,  Stride);
        _mm512_i64scatter_epi32(ptr, high(index).m, high(derived()).m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index, const Mask &mask) const {
        _mm512_mask_i64scatter_epi32(ptr, low(mask).k,   low(index).m,  low(derived()).m,  Stride);
        _mm512_mask_i64scatter_epi32(ptr, high(mask).k, high(index).m, high(derived()).m, Stride);
    }

    ENOKI_INLINE void store_compress_(void *&ptr, const Mask &mask) const {
        __mmask16 k = mask.k;
        _mm512_storeu_si512((__m512i *) ptr, _mm512_mask_compress_epi32(_mm512_setzero_si512(), k, m));
        (Scalar *&) ptr += _mm_popcnt_u32(k);
    }

    ENOKI_INLINE void massign_(const Mask &mask, const Derived &e) {
        m = _mm512_mask_mov_epi32(m, mask.k, e.m);
    }

    //! @}
    // -----------------------------------------------------------------------
};

/// Partial overload of StaticArrayImpl using AVX512 intrinsics (64 bit integers)
template <typename Scalar_, typename Derived> struct alignas(64)
    StaticArrayImpl<Scalar_, 8, false, RoundingMode::Default, Derived, detail::is_int64_t<Scalar_>>
    : StaticArrayBase<Scalar_, 8, false, RoundingMode::Default, Derived> {

    ENOKI_NATIVE_ARRAY(Scalar_, 8, false, __m512i, RoundingMode::Default)
    using Mask = detail::KMask<__mmask8>;

    // -----------------------------------------------------------------------
    //! @{ \name Value constructors
    // -----------------------------------------------------------------------

    ENOKI_INLINE StaticArrayImpl(Scalar value) : m(_mm512_set1_epi64((long long) value)) { }
    ENOKI_INLINE StaticArrayImpl(Scalar f0, Scalar f1, Scalar f2, Scalar f3,
                                 Scalar f4, Scalar f5, Scalar f6, Scalar f7)
        : m(_mm512_setr_epi64((long long) f0, (long long) f1, (long long) f2,
                              (long long) f3, (long long) f4, (long long) f5,
                              (long long) f6, (long long) f7)) { }

    // -----------------------------------------------------------------------
    //! @{ \name Type converting constructors
    // -----------------------------------------------------------------------

#if defined(__AVX512DQ__)
    ENOKI_CONVERT(float) {
        if (std::is_signed<Scalar>::value) {
            m = _mm512_cvttps_epi64(a.derived().m);
        } else {
            m = _mm512_cvttps_epu64(a.derived().m);
        }
    }
#endif

    ENOKI_CONVERT(int32_t)
        : m(_mm512_cvtepi32_epi64(a.derived().m)) { }

    ENOKI_CONVERT(uint32_t)
        : m(_mm512_cvtepu32_epi64(a.derived().m)) { }

#if defined(__AVX512DQ__)
    ENOKI_CONVERT(double) {
        if (std::is_signed<Scalar>::value)
            m = _mm512_cvttpd_epi64(a.derived().m);
        else
            m = _mm512_cvttpd_epu64(a.derived().m);
    }
#endif

    ENOKI_CONVERT(int64_t) : m(a.derived().m) { }
    ENOKI_CONVERT(uint64_t) : m(a.derived().m) { }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Reinterpreting constructors, mask converters
    // -----------------------------------------------------------------------

    ENOKI_REINTERPRET(double) : m(_mm512_castpd_si512(a.derived().m)) { }
    ENOKI_REINTERPRET(int64_t) : m(a.derived().m) { }
    ENOKI_REINTERPRET(uint64_t) : m(a.derived().m) { }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Converting from/to half size vectors
    // -----------------------------------------------------------------------

    StaticArrayImpl(const Array1 &a1, const Array2 &a2)
        : m(detail::concat(a1.m, a2.m)) { }

    ENOKI_INLINE Array1 low_()  const { return _mm512_castsi512_si256(m); }
    ENOKI_INLINE Array2 high_() const { return _mm512_extracti64x4_epi64(m, 1); }

    //! @}
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    //! @{ \name Vertical operations
    // -----------------------------------------------------------------------

    ENOKI_INLINE Derived add_(Arg a) const { return _mm512_add_epi64(m, a.m); }
    ENOKI_INLINE Derived sub_(Arg a) const { return _mm512_sub_epi64(m, a.m); }

    ENOKI_INLINE Derived mul_(Arg a) const {
        #if defined(__AVX512DQ__) && defined(__AVX512VL__)
            return _mm512_mullo_epi64(m, a.m);
        #else
            __m512i h0    = _mm512_srli_epi64(m, 32);
            __m512i h1    = _mm512_srli_epi64(a.m, 32);
            __m512i low   = _mm512_mul_epu32(m, a.m);
            __m512i mix0  = _mm512_mul_epu32(m, h1);
            __m512i mix1  = _mm512_mul_epu32(h0, a.m);
            __m512i mix   = _mm512_add_epi64(mix0, mix1);
            __m512i mix_s = _mm512_slli_epi64(mix, 32);
            return  _mm512_add_epi64(mix_s, low);
        #endif
    }

    ENOKI_INLINE Derived mulhi_(Arg a) const {
        /* Signed high multiplication is too costly to emulate
           using intrinsics, fall back to scalar version */

        if (std::is_signed<Scalar>::value) {
            Derived result;
            for (size_t i = 0; i < Size; ++i)
                result.coeff(i) = mulhi(coeff(i), a.coeff(i));
            return result;
        }

        const __m512i low_bits = _mm512_set1_epi64(0xffffffffu);
        __m512i al = m, bl = a.m;

        __m512i ah = _mm512_srli_epi64(al, 32);
        __m512i bh = _mm512_srli_epi64(bl, 32);

        // 4x unsigned 32x32->64 bit multiplication
        __m512i albl = _mm512_mul_epu32(al, bl);
        __m512i albh = _mm512_mul_epu32(al, bh);
        __m512i ahbl = _mm512_mul_epu32(ah, bl);
        __m512i ahbh = _mm512_mul_epu32(ah, bh);

        // Calculate a possible carry from the low bits of the multiplication.
        __m512i carry = _mm512_add_epi64(
            _mm512_srli_epi64(albl, 32),
            _mm512_add_epi64(_mm512_and_epi64(albh, low_bits),
                             _mm512_and_epi64(ahbl, low_bits)));

        __m512i s0 = _mm512_add_epi64(ahbh, _mm512_srli_epi64(carry, 32));
        __m512i s1 = _mm512_add_epi64(_mm512_srli_epi64(albh, 32),
                                      _mm512_srli_epi64(ahbl, 32));

        return _mm512_add_epi64(s0, s1);
    }

    ENOKI_INLINE Derived or_ (Arg a) const { return _mm512_or_epi64(m, a.m); }

    ENOKI_INLINE Derived or_ (Mask a) const {
        return _mm512_mask_mov_epi64(m, a.k, _mm512_set1_epi64(int32_t(-1)));
    }

    ENOKI_INLINE Derived and_(Arg a) const { return _mm512_and_epi64(m, a.m); }

    ENOKI_INLINE Derived and_ (Mask a) const {
        return _mm512_maskz_mov_epi64(a.k, m);
    }

    ENOKI_INLINE Derived xor_(Arg a) const { return _mm512_xor_epi64(m, a.m); }

    ENOKI_INLINE Derived xor_ (Mask a) const {
        return _mm512_mask_xor_epi64(m, a.k, m, _mm512_set1_epi64(int32_t(-1)));
    }

    template <size_t k> ENOKI_INLINE Derived sli_() const {
        return _mm512_slli_epi64(m, (int) k);
    }

    template <size_t k> ENOKI_INLINE Derived sri_() const {
        if (std::is_signed<Scalar>::value)
            return _mm512_srai_epi64(m, (int) k);
        else
            return _mm512_srli_epi64(m, (int) k);
    }

    ENOKI_INLINE Derived sl_(size_t k) const {
        return _mm512_sll_epi64(m, _mm_set1_epi64x((long long) k));
    }

    ENOKI_INLINE Derived sr_(size_t k) const {
        if (std::is_signed<Scalar>::value)
            return _mm512_sra_epi64(m, _mm_set1_epi64x((long long) k));
        else
            return _mm512_srl_epi64(m, _mm_set1_epi64x((long long) k));
    }

    ENOKI_INLINE Derived slv_(Arg k) const {
        return _mm512_sllv_epi64(m, k.m);
    }

    ENOKI_INLINE Derived srv_(Arg k) const {
        if (std::is_signed<Scalar>::value)
            return _mm512_srav_epi64(m, k.m);
        else
            return _mm512_srlv_epi64(m, k.m);
    }

    ENOKI_INLINE Derived rolv_(Arg k) const { return _mm512_rolv_epi64(m, k.m); }
    ENOKI_INLINE Derived rorv_(Arg k) const { return _mm512_rorv_epi64(m, k.m); }

    ENOKI_INLINE Derived rol_(size_t k) const { return rolv_(_mm512_set1_epi64((int32_t) k)); }
    ENOKI_INLINE Derived ror_(size_t k) const { return rorv_(_mm512_set1_epi64((int32_t) k)); }

    template <size_t Imm>
    ENOKI_INLINE Derived roli_() const { return _mm512_rol_epi64(m, (int) Imm); }

    template <size_t Imm>
    ENOKI_INLINE Derived rori_() const { return _mm512_ror_epi64(m, (int) Imm); }

    ENOKI_INLINE Mask lt_ (Arg a) const { return Mask(_mm512_cmp_epi64_mask(m, a.m, _MM_CMPINT_LT));  }
    ENOKI_INLINE Mask gt_ (Arg a) const { return Mask(_mm512_cmp_epi64_mask(m, a.m, _MM_CMPINT_GT));  }
    ENOKI_INLINE Mask le_ (Arg a) const { return Mask(_mm512_cmp_epi64_mask(m, a.m, _MM_CMPINT_LE));  }
    ENOKI_INLINE Mask ge_ (Arg a) const { return Mask(_mm512_cmp_epi64_mask(m, a.m, _MM_CMPINT_GE));  }
    ENOKI_INLINE Mask eq_ (Arg a) const { return Mask(_mm512_cmp_epi64_mask(m, a.m, _MM_CMPINT_EQ));  }
    ENOKI_INLINE Mask neq_(Arg a) const { return Mask(_mm512_cmp_epi64_mask(m, a.m, _MM_CMPINT_NE)); }

    ENOKI_INLINE Derived min_(Arg a) const {
        if (std::is_signed<Scalar>::value)
            return _mm512_min_epi64(a.m, m);
        else
            return _mm512_min_epu32(a.m, m);
    }

    ENOKI_INLINE Derived max_(Arg a) const {
        if (std::is_signed<Scalar>::value)
            return _mm512_max_epi64(a.m, m);
        else
            return _mm512_max_epu32(a.m, m);
    }

    ENOKI_INLINE Derived abs_() const {
        return std::is_signed<Scalar>::value ? _mm512_abs_epi64(m) : m;
    }

    ENOKI_INLINE static Derived select_(const Mask &m, const Derived &t,
                                        const Derived &f) {
        return _mm512_mask_blend_epi64(m.k, f.m, t.m);
    }

    template <size_t I0, size_t I1, size_t I2, size_t I3, size_t I4, size_t I5,
              size_t I6, size_t I7>
    ENOKI_INLINE Derived shuffle_() const {
        const __m512i idx =
            _mm512_setr_epi64(I0, I1, I2, I3, I4, I5, I6, I7);
        return _mm512_permutexvar_epi64(idx, m);
    }


    // -----------------------------------------------------------------------
    //! @{ \name Horizontal operations
    // -----------------------------------------------------------------------

    ENOKI_INLINE Scalar hsum_()  const { return hsum(low_() + high_()); }
    ENOKI_INLINE Scalar hprod_() const { return hprod(low_() * high_()); }
    ENOKI_INLINE Scalar hmin_()  const { return hmin(min(low_(), high_())); }
    ENOKI_INLINE Scalar hmax_()  const { return hmax(max(low_(), high_())); }

    //! @}
    // -----------------------------------------------------------------------

    ENOKI_INLINE void store_(void *ptr) const { _mm512_store_si512((__m512i *) ptr, m); }
    ENOKI_INLINE void store_unaligned_(void *ptr) const { _mm512_storeu_si512((__m512i *) ptr, m); }

    ENOKI_INLINE static Derived load_(const void *ptr) { return _mm512_load_si512((const __m512i *) ptr); }
    ENOKI_INLINE static Derived load_unaligned_(const void *ptr) { return _mm512_loadu_si512((const __m512i *) ptr); }

    ENOKI_INLINE static Derived zero_() { return _mm512_setzero_si512(); }

#if defined(__AVX512PF__)
    ENOKI_REQUIRE_INDEX_PF(Index, int32_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index) {
        if (Write)
            _mm512_prefetch_i32scatter_pd(ptr, index.m, Stride, Level);
        else
            _mm512_prefetch_i32gather_pd(index.m, ptr, Stride, Level);
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int32_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index,
                                       const Mask &mask) {
        if (Write)
            _mm512_mask_prefetch_i32scatter_pd(ptr, mask.k, index.m, Stride, Level);
        else
            _mm512_mask_prefetch_i32gather_pd(index.m, mask.k, ptr, Stride, Level);
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int64_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index) {
        if (Write)
            _mm512_prefetch_i64scatter_pd(ptr, index.m, Stride, Level);
        else
            _mm512_prefetch_i64gather_pd(index.m, ptr, Stride, Level);
    }

    ENOKI_REQUIRE_INDEX_PF(Index, int64_t)
    ENOKI_INLINE static void prefetch_(const void *ptr, const Index &index,
                                       const Mask &mask) {
        if (Write)
            _mm512_mask_prefetch_i64scatter_pd(ptr, mask.k, index.m, Stride, Level);
        else
            _mm512_mask_prefetch_i64gather_pd(index.m, mask.k, ptr, Stride, Level);
    }
#endif

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index) {
        return _mm512_i32gather_epi64(index.m, (const float *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index, const Mask &mask) {
        return _mm512_mask_i32gather_epi64(_mm512_setzero_si512(), mask.k, index.m, (const float *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index) {
        return _mm512_i64gather_epi64(index.m, (const float *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE static Derived gather_(const void *ptr, const Index &index, const Mask &mask) {
        return _mm512_mask_i64gather_epi64(_mm512_setzero_si512(), mask.k, index.m, (const float *) ptr, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index) const {
        _mm512_i32scatter_epi64(ptr, index.m, m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int32_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index, const Mask &mask) const {
        _mm512_mask_i32scatter_epi64(ptr, mask.k, index.m, m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index) const {
        _mm512_i64scatter_epi64(ptr, index.m, m, Stride);
    }

    ENOKI_REQUIRE_INDEX(Index, int64_t)
    ENOKI_INLINE void scatter_(void *ptr, const Index &index, const Mask &mask) const {
        _mm512_mask_i64scatter_epi64(ptr, mask.k, index.m, m, Stride);
    }

    ENOKI_INLINE void store_compress_(void *&ptr, const Mask &mask) const {
        __mmask8 k = mask.k;
        _mm512_storeu_si512((__m512i *) ptr, _mm512_mask_compress_epi64(_mm512_setzero_si512(), k, m));
        (Scalar *&) ptr += _mm_popcnt_u32(k);
    }

    ENOKI_INLINE void massign_(const Mask &mask, const Derived &e) {
        m = _mm512_mask_mov_epi64(m, mask.k, e.m);
    }

    //! @}
    // -----------------------------------------------------------------------
};

NAMESPACE_END(enoki)