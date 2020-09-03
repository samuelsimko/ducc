#ifndef GRIDDER_CXX_H
#define GRIDDER_CXX_H

/*
 *  This file is part of nifty_gridder.
 *
 *  nifty_gridder is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  nifty_gridder is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with nifty_gridder; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* Copyright (C) 2019-2020 Max-Planck-Society
   Author: Martin Reinecke */

#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <array>
#include <memory>

#include "ducc0/infra/error_handling.h"
#include "ducc0/math/fft.h"
#include "ducc0/infra/threading.h"
#include "ducc0/infra/misc_utils.h"
#include "ducc0/infra/useful_macros.h"
#include "ducc0/infra/mav.h"
#include "ducc0/infra/simd.h"
#include "ducc0/infra/timers.h"
#include "ducc0/math/gridding_kernel.h"

namespace ducc0 {

namespace detail_gridder {

using namespace std;

template<typename T> complex<T> hsum_cmplx(native_simd<T> vr, native_simd<T> vi)
  { return complex<T>(reduce(vr, std::plus<>()), reduce(vi, std::plus<>())); }

#if (defined(__AVX__) && (!defined(__AVX512F__)))
inline complex<float> hsum_cmplx(native_simd<float> vr, native_simd<float> vi)
  {
  auto t1 = _mm256_hadd_ps(vr, vi);
  auto t2 = _mm_hadd_ps(_mm256_extractf128_ps(t1, 0), _mm256_extractf128_ps(t1, 1));
  t2 += _mm_shuffle_ps(t2, t2, _MM_SHUFFLE(1,0,3,2));
  return complex<float>(t2[0], t2[1]);
  }
#endif

template<size_t ndim> void checkShape
  (const array<size_t, ndim> &shp1, const array<size_t, ndim> &shp2)
  { MR_assert(shp1==shp2, "shape mismatch"); }

template<typename T> inline T fmod1 (T v)
  { return v-floor(v); }

//
// Start of real gridder functionality
//

template<typename T> void complex2hartley
  (const mav<complex<T>, 2> &grid, mav<T,2> &grid2, size_t nthreads)
  {
  MR_assert(grid.conformable(grid2), "shape mismatch");
  size_t nu=grid.shape(0), nv=grid.shape(1);

  execParallel(nthreads, [&](Scheduler &sched)
    {
    auto tid = sched.thread_num();
    auto [lo, hi] = calcShare(nthreads, tid, nu);
    for(auto u=lo; u<hi; ++u)
      {
      size_t xu = (u==0) ? 0 : nu-u;
      for (size_t v=0; v<nv; ++v)
        {
        size_t xv = (v==0) ? 0 : nv-v;
        grid2.v(u,v) = T(0.5)*(grid( u, v).real()+grid( u, v).imag()+
                               grid(xu,xv).real()-grid(xu,xv).imag());
        }
      }
    });
  }

template<typename T> void hartley2complex
  (const mav<T,2> &grid, mav<complex<T>,2> &grid2, size_t nthreads)
  {
  MR_assert(grid.conformable(grid2), "shape mismatch");
  size_t nu=grid.shape(0), nv=grid.shape(1);

  execParallel(nthreads, [&](Scheduler &sched)
    {
    auto tid = sched.thread_num();
    auto [lo, hi] = calcShare(nthreads, tid, nu);
    for(auto u=lo; u<hi; ++u)
      {
      size_t xu = (u==0) ? 0 : nu-u;
      for (size_t v=0; v<nv; ++v)
        {
        size_t xv = (v==0) ? 0 : nv-v;
        T v1 = T(0.5)*grid( u, v);
        T v2 = T(0.5)*grid(xu,xv);
        grid2.v(u,v) = std::complex<T>(v1+v2, v1-v2);
        }
      }
    });
  }

template<typename T> void hartley2_2D(mav<T,2> &arr, size_t vlim,
  bool first_fast, size_t nthreads)
  {
  size_t nu=arr.shape(0), nv=arr.shape(1);
  fmav<T> farr(arr);
  if (2*vlim<nv)
    {
    if (!first_fast)
      r2r_separable_hartley(farr, farr, {1}, T(1), nthreads);
    auto flo = farr.subarray({0,0},{farr.shape(0),vlim});
    r2r_separable_hartley(flo, flo, {0}, T(1), nthreads);
    auto fhi = farr.subarray({0,farr.shape(1)-vlim},{farr.shape(0),vlim});
    r2r_separable_hartley(fhi, fhi, {0}, T(1), nthreads);
    if (first_fast)
      r2r_separable_hartley(farr, farr, {1}, T(1), nthreads);
    }
  else
    r2r_separable_hartley(farr, farr, {0,1}, T(1), nthreads);

  execParallel(nthreads, [&](Scheduler &sched)
    {
    auto tid = sched.thread_num();
    auto [lo, hi] = calcShare(nthreads, tid, (nu+1)/2-1);
    for(auto i=lo+1; i<hi+1; ++i)
      for(size_t j=1; j<(nv+1)/2; ++j)
         {
         T a = arr(i,j);
         T b = arr(nu-i,j);
         T c = arr(i,nv-j);
         T d = arr(nu-i,nv-j);
         arr.v(i,j) = T(0.5)*(a+b+c-d);
         arr.v(nu-i,j) = T(0.5)*(a+b+d-c);
         arr.v(i,nv-j) = T(0.5)*(a+c+d-b);
         arr.v(nu-i,nv-j) = T(0.5)*(b+c+d-a);
         }
     });
  }

class visrange
  {
  public:
    uint32_t row;
    uint16_t tile_u, tile_v, minplane, ch_begin, ch_end;

  public:
    visrange(uint16_t tile_u_, uint16_t tile_v_, uint16_t minplane_,
             uint32_t row_, uint16_t ch_begin_, uint16_t ch_end_)
      : row(row_), tile_u(tile_u_), tile_v(tile_v_), minplane(minplane_),
        ch_begin(ch_begin_), ch_end(ch_end_) {}
    uint64_t uvwidx() const
      { return (uint64_t(tile_u)<<32) + (uint64_t(tile_v)<<16) + minplane; }
  };

using VVR = vector<visrange>;

struct UVW
  {
  double u, v, w;
  UVW() {}
  UVW(double u_, double v_, double w_) : u(u_), v(v_), w(w_) {}
  UVW operator* (double fct) const
    { return UVW(u*fct, v*fct, w*fct); }
  void Flip() { u=-u; v=-v; w=-w; }
  bool FixW()
    {
    bool flip = w<0;
    if (flip) Flip();
    return flip;
    }
  };

class Baselines
  {
  protected:
    vector<UVW> coord;
    vector<double> f_over_c;
    size_t nrows, nchan;
    double umax, vmax;

  public:
    Baselines() = default;
    template<typename T> Baselines(const mav<T,2> &coord_,
      const mav<T,1> &freq, bool negate_v=false)
      {
      constexpr double speedOfLight = 299792458.;
      MR_assert(coord_.shape(1)==3, "dimension mismatch");
      nrows = coord_.shape(0);
      nchan = freq.shape(0);
      f_over_c.resize(nchan);
      double fcmax = 0;
      for (size_t i=0; i<nchan; ++i)
        {
        MR_assert(freq(i)>0, "negative channel frequency encountered");
        f_over_c[i] = freq(i)/speedOfLight;
        fcmax = max(fcmax, abs(f_over_c[i]));
        }
      coord.resize(nrows);
      double vfac = negate_v ? -1 : 1;
      vmax=0;
      for (size_t i=0; i<coord.size(); ++i)
        {
        coord[i] = UVW(coord_(i,0), vfac*coord_(i,1), coord_(i,2));
        umax = max(umax, abs(coord_(i,0)));
        vmax = max(vmax, abs(coord_(i,1)));
        }
      umax *= fcmax;
      vmax *= fcmax;
      }

    UVW effectiveCoord(size_t row, size_t chan) const
      { return coord[row]*f_over_c[chan]; }
    size_t Nrows() const { return nrows; }
    size_t Nchannels() const { return nchan; }
    double Umax() const { return umax; }
    double Vmax() const { return vmax; }
  };


constexpr int logsquare=4;

template<typename T> class Params
  {
  private:
    bool gridding;
    TimerHierarchy timers;
    const mav<complex<T>,2> &ms_in;
    mav<complex<T>,2> &ms_out;
    const mav<T,2> &dirty_in;
    mav<T,2> &dirty_out;
    const mav<T,2> &wgt;
    const mav<uint8_t,2> &mask;
    double pixsize_x, pixsize_y;
    size_t nxdirty, nydirty;
    double epsilon;
    bool do_wgridding;
    size_t nthreads;
    size_t verbosity;
    bool negate_v, divide_by_n;

    Baselines bl;
    VVR ranges;
    double wmin_d, wmax_d;
    size_t nvis;
    double wmin, dw;
    size_t nplanes;
    double nm1min;
    vector<uint8_t> active;

    size_t nu, nv;
    double ofactor;

    shared_ptr<HornerKernel<T>> krn;

    size_t supp, nsafe;
    double ushift, vshift;
    int maxiu0, maxiv0;
    size_t vlim;
    bool uv_side_fast;

    static T phase (T x, T y, T w, bool adjoint)
      {
      constexpr T pi = T(3.141592653589793238462643383279502884197);
      T tmp = 1-x-y;
// FIXME: shouldn't this be 0?
      if (tmp<=0) return 1; // no phase factor beyond the horizon
      T nm1 = (-x-y)/(sqrt(tmp)+1); // more accurate form of sqrt(1-x-y)-1
      T phs = 2*pi*w*nm1;
      if (adjoint) phs *= -1;
      return phs;
      }

    void grid2dirty_post(mav<T,2> &tmav,
      mav<T,2> &dirty) const
      {
      checkShape(dirty.shape(), {nxdirty, nydirty});
      auto cfu = krn->corfunc(nxdirty/2+1, 1./nu, nthreads);
      auto cfv = krn->corfunc(nydirty/2+1, 1./nv, nthreads);
      execParallel(nthreads, [&](Scheduler &sched)
        {
        auto tid = sched.thread_num();
        auto [lo, hi] = calcShare(nthreads, tid, nxdirty);
        for (auto i=lo; i<hi; ++i)
          {
          int icfu = abs(int(nxdirty/2)-int(i));
          for (size_t j=0; j<nydirty; ++j)
            {
            int icfv = abs(int(nydirty/2)-int(j));
            size_t i2 = nu-nxdirty/2+i;
            if (i2>=nu) i2-=nu;
            size_t j2 = nv-nydirty/2+j;
            if (j2>=nv) j2-=nv;
            dirty.v(i,j) = tmav(i2,j2)*T(cfu[icfu]*cfv[icfv]);
            }
          }
        });
      }
    void grid2dirty_post2(
      mav<complex<T>,2> &tmav, mav<T,2> &dirty, T w) const
      {
      checkShape(dirty.shape(), {nxdirty,nydirty});
      double x0 = -0.5*nxdirty*pixsize_x,
             y0 = -0.5*nydirty*pixsize_y;
      execParallel(nthreads, [&](Scheduler &sched)
        {
        auto tid = sched.thread_num();
        auto [lo, hi] = calcShare(nthreads, tid, nxdirty/2+1);
        using vtype = native_simd<T>;
        constexpr size_t vlen=vtype::size();
        size_t nvec = (nydirty/2+1+(vlen-1))/vlen;
        vector<vtype> ph(nvec), sp(nvec), cp(nvec);
        for (auto i=lo; i<hi; ++i)
          {
          T fx = T(x0+i*pixsize_x);
          fx *= fx;
          size_t ix = nu-nxdirty/2+i;
          if (ix>=nu) ix-=nu;
          size_t i2 = nxdirty-i;
          size_t ix2 = nu-nxdirty/2+i2;
          if (ix2>=nu) ix2-=nu;
          for (size_t j=0; j<=nydirty/2; ++j)
            {
            T fy = T(y0+j*pixsize_y);
            ph[j/vlen][j%vlen] = phase(fx, fy*fy, w, true);
            }
          for (size_t j=0; j<nvec; ++j)
            for (size_t k=0; k<vlen; ++k)
               sp[j][k]=sin(ph[j][k]);
          for (size_t j=0; j<nvec; ++j)
            for (size_t k=0; k<vlen; ++k)
              cp[j][k]=cos(ph[j][k]);
          if ((i>0)&&(i<i2))
            for (size_t j=0, jx=nv-nydirty/2; j<nydirty; ++j, jx=(jx+1>=nv)? jx+1-nv : jx+1)
              {
              size_t j2 = min(j, nydirty-j);
              T re = cp[j2/vlen][j2%vlen], im = sp[j2/vlen][j2%vlen];
              dirty.v(i,j) += tmav(ix,jx).real()*re - tmav(ix,jx).imag()*im;
              dirty.v(i2,j) += tmav(ix2,jx).real()*re - tmav(ix2,jx).imag()*im;
              }
          else
            for (size_t j=0, jx=nv-nydirty/2; j<nydirty; ++j, jx=(jx+1>=nv)? jx+1-nv : jx+1)
              {
              size_t j2 = min(j, nydirty-j);
              T re = cp[j2/vlen][j2%vlen], im = sp[j2/vlen][j2%vlen];
              dirty.v(i,j) += tmav(ix,jx).real()*re - tmav(ix,jx).imag()*im; // lower left
              }
          }
        });
      }

    void grid2dirty_overwrite(mav<T,2> &grid, mav<T,2> &dirty)
      {
      timers.push("FFT");
      checkShape(grid.shape(), {nu,nv});
      hartley2_2D<T>(grid, vlim, uv_side_fast, nthreads);
      timers.poppush("grid correction");
      grid2dirty_post(grid, dirty);
      timers.pop();
      }

    void grid2dirty_c_overwrite_wscreen_add
      (mav<complex<T>,2> &grid, mav<T,2> &dirty, T w)
      {
      timers.push("FFT");
      checkShape(grid.shape(), {nu,nv});
      fmav<complex<T>> inout(grid);
      if (2*vlim<nv)
        {
        if (!uv_side_fast)
          c2c(inout, inout, {1}, BACKWARD, T(1), nthreads);
        auto inout_lo = inout.subarray({0,0},{inout.shape(0),vlim});
        c2c(inout_lo, inout_lo, {0}, BACKWARD, T(1), nthreads);
        auto inout_hi = inout.subarray({0,inout.shape(1)-vlim},{inout.shape(0),vlim});
        c2c(inout_hi, inout_hi, {0}, BACKWARD, T(1), nthreads);
        if (uv_side_fast)
          c2c(inout, inout, {1}, BACKWARD, T(1), nthreads);
        }
      else
        c2c(inout, inout, {0,1}, BACKWARD, T(1), nthreads);
      timers.poppush("wscreen+grid correction");
      grid2dirty_post2(grid, dirty, w);
      timers.pop();
      }

    void dirty2grid_pre(const mav<T,2> &dirty,
      mav<T,2> &grid) const
      {
      checkShape(dirty.shape(), {nxdirty, nydirty});
      checkShape(grid.shape(), {nu, nv});
      auto cfu = krn->corfunc(nxdirty/2+1, 1./nu, nthreads);
      auto cfv = krn->corfunc(nydirty/2+1, 1./nv, nthreads);
      // FIXME: maybe we don't have to fill everything and can save some time
//      grid.fill(0);
      execParallel(nthreads, [&](Scheduler &sched)
        {
        auto tid = sched.thread_num();
        auto [lo, hi] = calcShare(nthreads, tid, nu);
        for (auto i=lo; i<hi; ++i)
          {
          size_t lo2=0, hi2=nv;
          if ((i<nxdirty/2) || (i>=nu-nxdirty/2))
            { lo2=nydirty/2; hi2=nv-nydirty/2+1; }
          for (auto j=lo2; j<hi2; ++j)
            grid.v(i,j) = 0;
          }
        });
      execParallel(nthreads, [&](Scheduler &sched)
        {
        auto tid = sched.thread_num();
        auto [lo, hi] = calcShare(nthreads, tid, nxdirty);
        for (auto i=lo; i<hi; ++i)
          {
          int icfu = abs(int(nxdirty/2)-int(i));
          for (size_t j=0; j<nydirty; ++j)
            {
            int icfv = abs(int(nydirty/2)-int(j));
            size_t i2 = nu-nxdirty/2+i;
            if (i2>=nu) i2-=nu;
            size_t j2 = nv-nydirty/2+j;
            if (j2>=nv) j2-=nv;
            grid.v(i2,j2) = dirty(i,j)*T(cfu[icfu]*cfv[icfv]);
            }
          }
        });
      }
    void dirty2grid_pre2(const mav<T,2> &dirty,
      mav<complex<T>,2> &grid, T w) const
      {
      checkShape(dirty.shape(), {nxdirty, nydirty});
      checkShape(grid.shape(), {nu, nv});
      // FIXME: maybe we don't have to fill everything and can save some time
//      grid.fill(0);
      execParallel(nthreads, [&](Scheduler &sched)
        {
        auto tid = sched.thread_num();
        auto [lo, hi] = calcShare(nthreads, tid, nu);
        for (auto i=lo; i<hi; ++i)
          {
          size_t lo2=0, hi2=nv;
          if ((i<nxdirty/2) || (i>=nu-nxdirty/2))
            { lo2=nydirty/2; hi2=nv-nydirty/2+1; }
          for (auto j=lo2; j<hi2; ++j)
            grid.v(i,j) = 0;
          }
        });

      double x0 = -0.5*nxdirty*pixsize_x,
             y0 = -0.5*nydirty*pixsize_y;
      execParallel(nthreads, [&](Scheduler &sched)
        {
        auto tid = sched.thread_num();
        auto [lo, hi] = calcShare(nthreads, tid, nxdirty/2+1);
        using vtype = native_simd<T>;
        constexpr size_t vlen=vtype::size();
        size_t nvec = (nydirty/2+1+(vlen-1))/vlen;
        vector<vtype> ph(nvec), sp(nvec), cp(nvec);
        for(auto i=lo; i<hi; ++i)
          {
          T fx = T(x0+i*pixsize_x);
          fx *= fx;
          size_t ix = nu-nxdirty/2+i;
          if (ix>=nu) ix-=nu;
          size_t i2 = nxdirty-i;
          size_t ix2 = nu-nxdirty/2+i2;
          if (ix2>=nu) ix2-=nu;
          for (size_t j=0; j<=nydirty/2; ++j)
            {
            T fy = T(y0+j*pixsize_y);
            ph[j/vlen][j%vlen] = phase(fx, fy*fy, w, false);
            }
          for (size_t j=0; j<nvec; ++j)
            for (size_t k=0; k<vlen; ++k)
               sp[j][k]=sin(ph[j][k]);
          for (size_t j=0; j<nvec; ++j)
            for (size_t k=0; k<vlen; ++k)
              cp[j][k]=cos(ph[j][k]);
          if ((i>0)&&(i<i2))
            for (size_t j=0, jx=nv-nydirty/2; j<nydirty; ++j, jx=(jx+1>=nv)? jx+1-nv : jx+1)
              {
              size_t j2 = min(j, nydirty-j);
              auto ws = complex<T>(cp[j2/vlen][j2%vlen],sp[j2/vlen][j2%vlen]);
              grid.v(ix,jx) = dirty(i,j)*ws; // lower left
              grid.v(ix2,jx) = dirty(i2,j)*ws; // lower right
              }
          else
            for (size_t j=0, jx=nv-nydirty/2; j<nydirty; ++j, jx=(jx+1>=nv)? jx+1-nv : jx+1)
              {
              size_t j2 = min(j, nydirty-j);
              auto ws = complex<T>(cp[j2/vlen][j2%vlen],sp[j2/vlen][j2%vlen]);
              grid.v(ix,jx) = dirty(i,j)*ws; // lower left
              }
          }
        });
      }

    void dirty2grid(const mav<T,2> &dirty, mav<T,2> &grid)
      {
      timers.push("grid correction");
      dirty2grid_pre(dirty, grid);
      timers.poppush("FFT");
      hartley2_2D<T>(grid, vlim, !uv_side_fast, nthreads);
      timers.pop();
      }

    void dirty2grid_c_wscreen(const mav<T,2> &dirty,
      mav<complex<T>,2> &grid, T w)
      {
      timers.push("wscreen+grid correction");
      dirty2grid_pre2(dirty, grid, w);
      timers.poppush("FFT");
      fmav<complex<T>> inout(grid);
      if (2*vlim<nv)
        {
        if (uv_side_fast)
          c2c(inout, inout, {1}, FORWARD, T(1), nthreads);
        auto inout_lo = inout.subarray({0,0},{inout.shape(0),vlim});
        c2c(inout_lo, inout_lo, {0}, FORWARD, T(1), nthreads);
        auto inout_hi = inout.subarray({0,inout.shape(1)-vlim},{inout.shape(0),vlim});
        c2c(inout_hi, inout_hi, {0}, FORWARD, T(1), nthreads);
        if (!uv_side_fast)
          c2c(inout, inout, {1}, FORWARD, T(1), nthreads);
        }
      else
        c2c(inout, inout, {0,1}, FORWARD, T(1), nthreads);
      timers.pop();
      }

    [[gnu::always_inline]] void getpix(double u_in, double v_in, double &u, double &v, int &iu0, int &iv0) const
      {
      u=fmod1(u_in*pixsize_x)*nu;
      iu0 = min(int(u+ushift)-int(nu), maxiu0);
      v=fmod1(v_in*pixsize_y)*nv;
      iv0 = min(int(v+vshift)-int(nv), maxiv0);
      }

    void report()
      {
      if (verbosity==0) return;
      cout << (gridding ? "Gridding" : "Degridding")
           << ": nthreads=" << nthreads << ", "
           << "dirty=(" << nxdirty << "x" << nydirty << "), "
           << "grid=(" << nu << "x" << nv;
      if (do_wgridding) cout << "x" << nplanes;
      cout << "), nvis=" << nvis
           << ", supp=" << supp
           << ", eps=" << (epsilon * (do_wgridding ? 3 : 2))
           << endl;
      cout << "  w=[" << wmin_d << "; " << wmax_d << "], min(n-1)=" << nm1min << ", dw=" << dw
           << ", wmax/dw=" << wmax_d/dw << ", nranges=" << ranges.size() << endl;
      }

    void scanData()
      {
      timers.push("Initial scan");
      size_t nrow=bl.Nrows(),
             nchan=bl.Nchannels();
      bool have_wgt=wgt.size()!=0;
      if (have_wgt) checkShape(wgt.shape(),{nrow,nchan});
      bool have_ms=ms_in.size()!=0;
      if (have_ms) checkShape(ms_in.shape(), {nrow,nchan});
      bool have_mask=mask.size()!=0;
      if (have_mask) checkShape(mask.shape(), {nrow,nchan});

      active.resize(nrow*nchan, 0);
      nvis=0;
      wmin_d=1e300;
      wmax_d=-1e300;
      mutex mut;
      execParallel(nthreads, [&](Scheduler &sched)
        {
        double lwmin_d=1e300, lwmax_d=-1e300;
        size_t lnvis=0;
        auto tid = sched.thread_num();
        auto [lo, hi] = calcShare(nthreads, tid, nrow);
        for(auto irow=lo; irow<hi; ++irow)
          for (size_t ichan=0, idx=irow*nchan; ichan<nchan; ++ichan, ++idx)
            if (((!have_ms ) || (norm(ms_in(irow,ichan))!=0)) &&
                ((!have_wgt) || (wgt(irow,ichan)!=0)) &&
                ((!have_mask) || (mask(irow,ichan)!=0)))
              {
              ++lnvis;
              active[irow*nchan+ichan] = 1;
              auto uvw = bl.effectiveCoord(irow,ichan);
              double w = abs(uvw.w);
              lwmin_d = min(lwmin_d, w);
              lwmax_d = max(lwmax_d, w);
              }
        {
        lock_guard<mutex> lock(mut);
        wmin_d = min(wmin_d, lwmin_d);
        wmax_d = max(wmax_d, lwmax_d);
        nvis += lnvis;
        }
        });
      timers.pop();
      }

    auto getNuNv()
      {
      timers.push("parameter calculation");
      double x0 = -0.5*nxdirty*pixsize_x,
             y0 = -0.5*nydirty*pixsize_y;
      nm1min = sqrt(max(1.-x0*x0-y0*y0,0.))-1.;
      if (x0*x0+y0*y0>1.)
        nm1min = -sqrt(abs(1.-x0*x0-y0*y0))-1.;
      auto idx = getAvailableKernels<T>(epsilon);
      double mincost = 1e300;
      constexpr double nref_fft=2048;
      constexpr double costref_fft=0.0693;
      size_t minnu=0, minnv=0, minidx=KernelDB.size();
      constexpr size_t vlen = native_simd<T>::size();
      for (size_t i=0; i<idx.size(); ++i)
        {
        const auto &krn(KernelDB[idx[i]]);
        auto supp = krn.W;
        auto nvec = (supp+vlen-1)/vlen;
        auto ofactor = krn.ofactor;
        size_t nu=2*good_size_complex(size_t(nxdirty*ofactor*0.5)+1);
        size_t nv=2*good_size_complex(size_t(nydirty*ofactor*0.5)+1);
        double logterm = log(nu*nv)/log(nref_fft*nref_fft);
        double fftcost = nu/nref_fft*nv/nref_fft*logterm*costref_fft;
        double gridcost = 2.2e-10*nvis*(supp*nvec*vlen + ((2*nvec+1)*(supp+3)*vlen));
        if (do_wgridding)
          {
          double dw = 0.5/ofactor/abs(nm1min);
          size_t nplanes = size_t((wmax_d-wmin_d)/dw+supp);
          fftcost *= nplanes;
          gridcost *= supp;
          }
        double cost = fftcost+gridcost;
        if (cost<mincost)
          {
          mincost=cost;
          minnu=nu;
          minnv=nv;
          minidx = idx[i];
          }
        }
      timers.pop();
      nu = minnu;
      nv = minnv;
      return minidx;
      }

    void countRanges()
      {
      timers.push("range count");
      size_t nrow=bl.Nrows(),
             nchan=bl.Nchannels();

      if (do_wgridding)
        {
        dw = 0.5/ofactor/abs(nm1min);
        nplanes = size_t((wmax_d-wmin_d)/dw+supp);
        wmin = (wmin_d+wmax_d)*0.5 - 0.5*(nplanes-1)*dw;
        }
      else
        {
        dw = 0;
        nplanes = 0;
        wmin = 0;
        }

      struct bufvec
        {
        VVR v;
        uint64_t dummy[8];
        };
      auto Sorter = [](const visrange &a, const visrange &b) { return a.uvwidx()<b.uvwidx(); };
      vector<bufvec> lranges(nthreads);
      execParallel(nthreads, [&](Scheduler &sched)
        {
        auto tid = sched.thread_num();
        auto &myranges(lranges[tid].v);
        auto [lo, hi] = calcShare(nthreads, tid, nrow);
        for(auto irow=lo; irow<hi; ++irow)
          {
          bool on=false;
          int iulast, ivlast, plast;
          size_t chan0=0;
          for (size_t ichan=0; ichan<nchan; ++ichan)
            {
            if (active[irow*nchan+ichan])
              {
              auto uvw = bl.effectiveCoord(irow, ichan);
              if (uvw.w<0) uvw.Flip();
              double u, v;
              int iu0, iv0, iw;
              getpix(uvw.u, uvw.v, u, v, iu0, iv0);
              iu0 = (iu0+nsafe)>>logsquare;
              iv0 = (iv0+nsafe)>>logsquare;
              iw = do_wgridding ?
                max(0,int(1+(abs(uvw.w)-(0.5*supp*dw)-wmin)/dw)) : 0;
              if (!on) // new active region
                {
                on=true;
                iulast=iu0; ivlast=iv0; plast=iw; chan0=ichan;
                }
              else if ((iu0!=iulast) || (iv0!=ivlast) || (iw!=plast)) // change of active region
                {
                myranges.emplace_back(iulast, ivlast, plast, irow, chan0, ichan);
                iulast=iu0; ivlast=iv0; plast=iw; chan0=ichan;
                }
              }
            else if (on) // end of active region
              {
              myranges.emplace_back(iulast, ivlast, plast, irow, chan0, ichan);
              on=false;
              }
            }
          if (on) // end of active region at last channel
            myranges.emplace_back(iulast, ivlast, plast, irow, chan0, nchan);
          }
        sort(myranges.begin(), myranges.end(), Sorter);
        });

      // free mask memory
      vector<uint8_t>().swap(active);
      timers.poppush("range merging");
      size_t nth = nthreads;
      while (nth>1)
        {
        auto nmerge=nth/2;
        execParallel(nmerge, [&](Scheduler &sched)
          {
          auto tid = sched.thread_num();
          auto tid_partner = nth-1-tid;
          VVR tmp;
          tmp.reserve(lranges[tid].v.size() + lranges[tid_partner].v.size());
          merge(lranges[tid].v.begin(), lranges[tid].v.end(),
                lranges[tid_partner].v.begin(), lranges[tid_partner].v.end(),
                back_inserter(tmp), Sorter);
          lranges[tid].v.swap(tmp);
          VVR().swap(lranges[tid_partner].v);
          });
        nth-=nmerge;
        }
      ranges.swap(lranges[0].v);
      timers.pop();
      }

    void apply_global_corrections(mav<T,2> &dirty)
      {
      timers.push("global corrections");
      double x0 = -0.5*nxdirty*pixsize_x,
             y0 = -0.5*nydirty*pixsize_y;
      auto cfu = krn->corfunc(nxdirty/2+1, 1./nu, nthreads);
      auto cfv = krn->corfunc(nydirty/2+1, 1./nv, nthreads);
      execParallel(nthreads, [&](Scheduler &sched)
        {
        auto tid = sched.thread_num();
        auto [lo, hi] = calcShare(nthreads, tid, nxdirty/2+1);
        for(auto i=lo; i<hi; ++i)
          {
          auto fx = T(x0+i*pixsize_x);
          fx *= fx;
          for (size_t j=0; j<=nydirty/2; ++j)
            {
            auto fy = T(y0+j*pixsize_y);
            fy*=fy;
            T fct = 0;
            auto tmp = 1-fx-fy;
            if (tmp>=0)
              {
              auto nm1 = (-fx-fy)/(sqrt(tmp)+1); // accurate form of sqrt(1-x-y)-1
              fct = T(krn->corfunc(nm1*dw));
              if (divide_by_n)
                fct /= nm1+1;
              }
            else // beyond the horizon, don't really know what to do here
              {
              if (divide_by_n)
                fct=0;
              else
                {
                auto nm1 = sqrt(-tmp)-1;
                fct = T(krn->corfunc(nm1*dw));
                }
              }
            fct *= T(cfu[nxdirty/2-i]*cfv[nydirty/2-j]);
            size_t i2 = nxdirty-i, j2 = nydirty-j;
            dirty.v(i,j)*=fct;
            if ((i>0)&&(i<i2))
              {
              dirty.v(i2,j)*=fct;
              if ((j>0)&&(j<j2))
                dirty.v(i2,j2)*=fct;
              }
            if ((j>0)&&(j<j2))
              dirty.v(i,j2)*=fct;
            }
          }
        });
      timers.pop();
      }

    template<size_t supp, bool wgrid> class HelperX2g2
      {
      public:
        static constexpr size_t vlen = native_simd<T>::size();
        static constexpr size_t nvec = (supp+vlen-1)/vlen;

      private:
        static constexpr int nsafe = (supp+1)/2;
        static constexpr int su = 2*nsafe+(1<<logsquare);
        static constexpr int sv = 2*nsafe+(1<<logsquare);
        static constexpr int svvec = ((sv+vlen-1)/vlen)*vlen;
        static constexpr double xsupp=2./supp;
        const Params *parent;
        TemplateKernel<supp, T> tkrn;
        mav<complex<T>,2> &grid;
        int iu0, iv0; // start index of the current visibility
        int bu0, bv0; // start index of the current buffer

        mav<T,2> bufr, bufi;
        T *px0r, *px0i;
        double w0, xdw;
        vector<std::mutex> &locks;

        DUCC0_NOINLINE void dump()
          {
          int inu = int(parent->nu);
          int inv = int(parent->nv);
          if (bu0<-nsafe) return; // nothing written into buffer yet

          int idxu = (bu0+inu)%inu;
          int idxv0 = (bv0+inv)%inv;
          for (int iu=0; iu<su; ++iu)
            {
            int idxv = idxv0;
            {
            std::lock_guard<std::mutex> lock(locks[idxu]);
            for (int iv=0; iv<sv; ++iv)
              {
              grid.v(idxu,idxv) += complex<T>(bufr(iu,iv), bufi(iu,iv));
              bufr.v(iu,iv) = bufi.v(iu,iv) = 0;
              if (++idxv>=inv) idxv=0;
              }
            }
            if (++idxu>=inu) idxu=0;
            }
          }

      public:
        T * DUCC0_RESTRICT p0r, * DUCC0_RESTRICT p0i;
        union kbuf {
          T scalar[2*nvec*vlen];
          native_simd<T> simd[2*nvec];
          };
        kbuf buf;

        HelperX2g2(const Params *parent_, mav<complex<T>,2> &grid_,
          vector<std::mutex> &locks_, double w0_=-1, double dw_=-1)
          : parent(parent_), tkrn(*parent->krn), grid(grid_),
            iu0(-1000000), iv0(-1000000),
            bu0(-1000000), bv0(-1000000),
            bufr({size_t(su),size_t(svvec)}),
            bufi({size_t(su),size_t(svvec)}),
            px0r(bufr.vdata()), px0i(bufi.vdata()),
            w0(w0_),
            xdw(T(1)/dw_),
            locks(locks_)
          { checkShape(grid.shape(), {parent->nu,parent->nv}); }
        ~HelperX2g2() { dump(); }

        constexpr int lineJump() const { return svvec; }
        [[gnu::always_inline]] [[gnu::hot]] void prep(const UVW &in)
          {
          double u, v;
          auto iu0old = iu0;
          auto iv0old = iv0;
          parent->getpix(in.u, in.v, u, v, iu0, iv0);
          T x0 = (iu0-T(u))*2+(supp-1);
          T y0 = (iv0-T(v))*2+(supp-1);
          if constexpr(wgrid)
            tkrn.eval2s(x0, y0, T(xdw*(w0-in.w)), &buf.simd[0]);
          else
            tkrn.eval2(x0, y0, &buf.simd[0]);
          if ((iu0==iu0old) && (iv0==iv0old)) return;
          if ((iu0<bu0) || (iv0<bv0) || (iu0+int(supp)>bu0+su) || (iv0+int(supp)>bv0+sv))
            {
            dump();
            bu0=((((iu0+nsafe)>>logsquare)<<logsquare))-nsafe;
            bv0=((((iv0+nsafe)>>logsquare)<<logsquare))-nsafe;
            }
          auto ofs = (iu0-bu0)*svvec + iv0-bv0;
          p0r = px0r+ofs;
          p0i = px0i+ofs;
          }
      };


    template<size_t SUPP, bool wgrid> [[gnu::hot]] void x2grid_c_helper
      (mav<complex<T>,2> &grid,
       size_t p0, double w0)
      {
      bool have_wgt = wgt.size()!=0;
      vector<std::mutex> locks(nu);

      execGuided(ranges.size(), nthreads, 100, 0.2, [&](Scheduler &sched)
        {
        constexpr size_t vlen=native_simd<T>::size();
        constexpr size_t NVEC((SUPP+vlen-1)/vlen);
        HelperX2g2<SUPP,wgrid> hlp(this, grid, locks, w0, dw);
        constexpr int jump = hlp.lineJump();
        const T * DUCC0_RESTRICT ku = hlp.buf.scalar;
        const auto * DUCC0_RESTRICT kv = hlp.buf.simd+NVEC;

        while (auto rng=sched.getNext()) for(auto irng=rng.lo; irng<rng.hi; ++irng)
          {
          if ((!wgrid) || ((ranges[irng].minplane+SUPP>p0)&&(ranges[irng].minplane<=p0)))
            {
            size_t row = ranges[irng].row;
            for (size_t ch=ranges[irng].ch_begin; ch<ranges[irng].ch_end; ++ch)
              {
              UVW coord = bl.effectiveCoord(row, ch);
              auto flip = coord.FixW();
              hlp.prep(coord);
              auto v(ms_in(row, ch));

              if (flip) v=conj(v);
              if (have_wgt) v*=wgt(row, ch);
              native_simd<T> vr(v.real()), vi(v.imag());
              for (size_t cu=0; cu<SUPP; ++cu)
                {
                if constexpr (NVEC==1)
                  {
                  auto fct = kv[0]*ku[cu];
                  auto * DUCC0_RESTRICT pxr = hlp.p0r+cu*jump;
                  auto * DUCC0_RESTRICT pxi = hlp.p0i+cu*jump;
                  auto tr = native_simd<T>::loadu(pxr);
                  auto ti = native_simd<T>::loadu(pxi);
                  tr += vr*fct;
                  ti += vi*fct;
                  tr.storeu(pxr);
                  ti.storeu(pxi);
                  }
                else
                  {
                  native_simd<T> tmpr=vr*ku[cu], tmpi=vi*ku[cu];
                  for (size_t cv=0; cv<NVEC; ++cv)
                    {
                    auto * DUCC0_RESTRICT pxr = hlp.p0r+cu*jump+cv*hlp.vlen;
                    auto * DUCC0_RESTRICT pxi = hlp.p0i+cu*jump+cv*hlp.vlen;
                    auto tr = native_simd<T>::loadu(pxr);
                    tr += tmpr*kv[cv];
                    tr.storeu(pxr);
                    auto ti = native_simd<T>::loadu(pxi);
                    ti += tmpi*kv[cv];
                    ti.storeu(pxi);
                    }
                  }
                }
              }
            }
          }
        });
      }

    template<bool wgrid> void x2grid_c
      (mav<complex<T>,2> &grid,
       size_t p0, double w0=-1)
      {
      timers.push("gridding proper");
      checkShape(grid.shape(), {nu, nv});

      if constexpr (is_same<T, float>::value)
        switch(supp)
          {
          case  4: x2grid_c_helper< 4, wgrid>(grid, p0, w0); break;
          case  5: x2grid_c_helper< 5, wgrid>(grid, p0, w0); break;
          case  6: x2grid_c_helper< 6, wgrid>(grid, p0, w0); break;
          case  7: x2grid_c_helper< 7, wgrid>(grid, p0, w0); break;
          case  8: x2grid_c_helper< 8, wgrid>(grid, p0, w0); break;
          default: MR_fail("must not happen");
          }
      else
        switch(supp)
          {
          case  4: x2grid_c_helper< 4, wgrid>(grid, p0, w0); break;
          case  5: x2grid_c_helper< 5, wgrid>(grid, p0, w0); break;
          case  6: x2grid_c_helper< 6, wgrid>(grid, p0, w0); break;
          case  7: x2grid_c_helper< 7, wgrid>(grid, p0, w0); break;
          case  8: x2grid_c_helper< 8, wgrid>(grid, p0, w0); break;
          case  9: x2grid_c_helper< 9, wgrid>(grid, p0, w0); break;
          case 10: x2grid_c_helper<10, wgrid>(grid, p0, w0); break;
          case 11: x2grid_c_helper<11, wgrid>(grid, p0, w0); break;
          case 12: x2grid_c_helper<12, wgrid>(grid, p0, w0); break;
          case 13: x2grid_c_helper<13, wgrid>(grid, p0, w0); break;
          case 14: x2grid_c_helper<14, wgrid>(grid, p0, w0); break;
          case 15: x2grid_c_helper<15, wgrid>(grid, p0, w0); break;
          case 16: x2grid_c_helper<16, wgrid>(grid, p0, w0); break;
          default: MR_fail("must not happen");
          }
      timers.pop();
      }

    void x2dirty()
      {
      if (do_wgridding)
        {
        timers.push("zeroing dirty image");
        dirty_out.fill(0);
        timers.poppush("allocating grid");
        auto grid = mav<complex<T>,2>::build_noncritical({nu,nv});
        timers.pop();
        for (size_t pl=0; pl<nplanes; ++pl)
          {
          double w = wmin+pl*dw;
          timers.push("zeroing grid");
#if 0
//FIXME: we don't need to zero the entire array here...
          execParallel(nthreads, [&](Scheduler &sched)
            {
            auto tid = sched.thread_num();
            auto [lo, hi] = calcShare(nthreads, tid, nu);
            for (auto i=lo; i<hi; ++i)
              for (size_t j=0; j<nv; ++j)
                grid.v(i,j) = 0;
            });
#else
          grid.fill(0);
#endif
          timers.pop();
          x2grid_c<true>(grid, pl, w);
          grid2dirty_c_overwrite_wscreen_add(grid, dirty_out, T(w));
          }
        // correct for w gridding etc.
        apply_global_corrections(dirty_out);
        }
      else
        {
        timers.push("allocating grid");
        auto grid = mav<complex<T>,2>::build_noncritical({nu,nv});
        timers.pop();
        x2grid_c<false>(grid, 0);
        timers.push("allocating rgrid");
        auto rgrid = mav<T,2>::build_noncritical(grid.shape());
        timers.poppush("complex2hartley");
        complex2hartley(grid, rgrid, nthreads);
        timers.pop();
        grid2dirty_overwrite(rgrid, dirty_out);
        }
      }
    template<size_t supp, bool wgrid> class HelperG2x2
      {
      public:
        static constexpr size_t vlen = native_simd<T>::size();
        static constexpr size_t nvec = (supp+vlen-1)/vlen;

      private:
        static constexpr int nsafe = (supp+1)/2;
        static constexpr int su = 2*nsafe+(1<<logsquare);
        static constexpr int sv = 2*nsafe+(1<<logsquare);
        static constexpr int svvec = ((sv+vlen-1)/vlen)*vlen;
        static constexpr double xsupp=2./supp;
    const Params *parent;

        TemplateKernel<supp, T> tkrn;
        const mav<complex<T>,2> &grid;
        int iu0, iv0; // start index of the current visibility
        int bu0, bv0; // start index of the current buffer

        mav<T,2> bufr, bufi;
        const T *px0r, *px0i;
        double w0, xdw;

        DUCC0_NOINLINE void load()
          {
          int inu = int(parent->nu);
          int inv = int(parent->nv);
          int idxu = (bu0+inu)%inu;
          int idxv0 = (bv0+inv)%inv;
          for (int iu=0; iu<su; ++iu)
            {
            int idxv = idxv0;
            for (int iv=0; iv<sv; ++iv)
              {
              bufr.v(iu,iv) = grid(idxu, idxv).real();
              bufi.v(iu,iv) = grid(idxu, idxv).imag();
              if (++idxv>=inv) idxv=0;
              }
            if (++idxu>=inu) idxu=0;
            }
          }

      public:
        const T * DUCC0_RESTRICT p0r, * DUCC0_RESTRICT p0i;
        union kbuf {
          T scalar[2*nvec*vlen];
          native_simd<T> simd[2*nvec];
          };
        kbuf buf;

        HelperG2x2(const Params *parent_, const mav<complex<T>,2> &grid_,
          double w0_=-1, double dw_=-1)
          : parent(parent_), tkrn(*parent->krn), grid(grid_),
            iu0(-1000000), iv0(-1000000),
            bu0(-1000000), bv0(-1000000),
            bufr({size_t(su),size_t(svvec)}),
            bufi({size_t(su),size_t(svvec)}),
            px0r(bufr.data()), px0i(bufi.data()),
            w0(w0_),
            xdw(T(1)/dw_)
          { checkShape(grid.shape(), {parent->nu,parent->nv}); }

        constexpr int lineJump() const { return svvec; }
        [[gnu::always_inline]] [[gnu::hot]] void prep(const UVW &in)
          {
          double u, v;
          auto iu0old = iu0;
          auto iv0old = iv0;
          parent->getpix(in.u, in.v, u, v, iu0, iv0);
          T x0 = (iu0-T(u))*2+(supp-1);
          T y0 = (iv0-T(v))*2+(supp-1);
          if constexpr(wgrid)
            tkrn.eval2s(x0, y0, T(xdw*(w0-in.w)), &buf.simd[0]);
          else
            tkrn.eval2(x0, y0, &buf.simd[0]);
          if ((iu0==iu0old) && (iv0==iv0old)) return;
          if ((iu0<bu0) || (iv0<bv0) || (iu0+int(supp)>bu0+su) || (iv0+int(supp)>bv0+sv))
            {
            bu0=((((iu0+nsafe)>>logsquare)<<logsquare))-nsafe;
            bv0=((((iv0+nsafe)>>logsquare)<<logsquare))-nsafe;
            load();
            }
          auto ofs = (iu0-bu0)*svvec + iv0-bv0;
          p0r = px0r+ofs;
          p0i = px0i+ofs;
          }
      };

    template<size_t SUPP, bool wgrid> [[gnu::hot]] void grid2x_c_helper
      (const mav<complex<T>,2> &grid,
       size_t p0, double w0)
      {
      bool have_wgt = wgt.size()!=0;

      // Loop over sampling points
      execGuided(ranges.size(), nthreads, 1000, 0.5, [&](Scheduler &sched)
        {
        constexpr size_t vlen=native_simd<T>::size();
        constexpr size_t NVEC((SUPP+vlen-1)/vlen);
        HelperG2x2<SUPP,wgrid> hlp(this, grid, w0, dw);
        constexpr int jump = hlp.lineJump();
        const T * DUCC0_RESTRICT ku = hlp.buf.scalar;
        const auto * DUCC0_RESTRICT kv = hlp.buf.simd+NVEC;

        while (auto rng=sched.getNext()) for(auto irng=rng.lo; irng<rng.hi; ++irng)
          {
          if ((!wgrid) || ((ranges[irng].minplane+SUPP>p0)&&(ranges[irng].minplane<=p0)))
            {
            size_t row = ranges[irng].row;
            for (size_t ch=ranges[irng].ch_begin; ch<ranges[irng].ch_end; ++ch)
              {
              UVW coord = bl.effectiveCoord(row, ch);
              auto flip = coord.FixW();
              hlp.prep(coord);
              native_simd<T> rr=0, ri=0;
              for (size_t cu=0; cu<SUPP; ++cu)
                {
#if 0
// whether this is advantageous seems to depend on the hardware ...
                if constexpr(NVEC==1)
                  {
                  auto fct = kv[0]*ku[cu];
                  const auto * DUCC0_RESTRICT pxr = hlp.p0r + cu*jump;
                  const auto * DUCC0_RESTRICT pxi = hlp.p0i + cu*jump;
                  rr += native_simd<T>::loadu(pxr)*fct;
                  ri += native_simd<T>::loadu(pxi)*fct;
                  }
               else
#endif
                  {
                  native_simd<T> tmpr(0), tmpi(0);
                  for (size_t cv=0; cv<NVEC; ++cv)
                    {
                    const auto * DUCC0_RESTRICT pxr = hlp.p0r + cu*jump + hlp.vlen*cv;
                    const auto * DUCC0_RESTRICT pxi = hlp.p0i + cu*jump + hlp.vlen*cv;
                    tmpr += kv[cv]*native_simd<T>::loadu(pxr);
                    tmpi += kv[cv]*native_simd<T>::loadu(pxi);
                    }
                  rr += ku[cu]*tmpr;
                  ri += ku[cu]*tmpi;
                  }
                }
              auto r = hsum_cmplx(rr,ri);
    //          auto r = complex<T>(reduce(rr, std::plus<>()), reduce(ri, std::plus<>()));
              if (flip) r=conj(r);
              if (have_wgt) r*=wgt(row, ch);
              ms_out.v(row, ch) += r;
              }
            }
          }
        });
      }

    template<bool wgrid> void grid2x_c
      (const mav<complex<T>,2> &grid,
       size_t p0, double w0=-1)
      {
      timers.push("degridding proper");
      checkShape(grid.shape(), {nu, nv});

      if constexpr (is_same<T, float>::value)
        switch(supp)
          {
          case  4: grid2x_c_helper< 4, wgrid>(grid, p0, w0); break;
          case  5: grid2x_c_helper< 5, wgrid>(grid, p0, w0); break;
          case  6: grid2x_c_helper< 6, wgrid>(grid, p0, w0); break;
          case  7: grid2x_c_helper< 7, wgrid>(grid, p0, w0); break;
          case  8: grid2x_c_helper< 8, wgrid>(grid, p0, w0); break;
          default: MR_fail("must not happen");
          }
      else
        switch(supp)
          {
          case  4: grid2x_c_helper< 4, wgrid>(grid, p0, w0); break;
          case  5: grid2x_c_helper< 5, wgrid>(grid, p0, w0); break;
          case  6: grid2x_c_helper< 6, wgrid>(grid, p0, w0); break;
          case  7: grid2x_c_helper< 7, wgrid>(grid, p0, w0); break;
          case  8: grid2x_c_helper< 8, wgrid>(grid, p0, w0); break;
          case  9: grid2x_c_helper< 9, wgrid>(grid, p0, w0); break;
          case 10: grid2x_c_helper<10, wgrid>(grid, p0, w0); break;
          case 11: grid2x_c_helper<11, wgrid>(grid, p0, w0); break;
          case 12: grid2x_c_helper<12, wgrid>(grid, p0, w0); break;
          case 13: grid2x_c_helper<13, wgrid>(grid, p0, w0); break;
          case 14: grid2x_c_helper<14, wgrid>(grid, p0, w0); break;
          case 15: grid2x_c_helper<15, wgrid>(grid, p0, w0); break;
          case 16: grid2x_c_helper<16, wgrid>(grid, p0, w0); break;
          default: MR_fail("must not happen");
          }
      timers.pop();
      }

    void dirty2x()
      {
      if (do_wgridding)
        {
        timers.push("copying dirty image");
        mav<T,2> tdirty({nxdirty,nydirty});
        tdirty.apply(dirty_in, [](T&a, T b) {a=b;});
        timers.pop();
        // correct for w gridding etc.
        apply_global_corrections(tdirty);
        timers.push("allocating grid");
        auto grid = mav<complex<T>,2>::build_noncritical({nu,nv});
        timers.pop();
        for (size_t pl=0; pl<nplanes; ++pl)
          {
          double w = wmin+pl*dw;
          dirty2grid_c_wscreen(tdirty, grid, T(w));
          grid2x_c<true>(grid, pl, w);
          }
        }
      else
        {
        timers.push("allocating rgrid");
        auto rgrid = mav<T,2>::build_noncritical({nu,nv});
        timers.pop();
        dirty2grid(dirty_in, rgrid);
        timers.push("allocating grid");
        auto grid = mav<complex<T>,2>::build_noncritical(rgrid.shape());
        timers.poppush("hartley2complex");
        hartley2complex(rgrid, grid, nthreads);
        timers.pop();
        grid2x_c<false>(grid, 0);
        }
      }

  public:
    Params(const mav<double,2> &uvw, const mav<double,1> &freq,
           const mav<complex<T>,2> &ms_in_, mav<complex<T>,2> &ms_out_,
           const mav<T,2> &dirty_in_, mav<T,2> &dirty_out_,
           const mav<T,2> &wgt_, const mav<uint8_t,2> &mask_,
           double pixsize_x_, double pixsize_y_, double epsilon_,
           bool do_wgridding_, size_t nthreads_, size_t verbosity_,
           bool negate_v_, bool divide_by_n_)
      : gridding(ms_in_.size()>0), 
        timers(gridding ? "gridding" : "degridding"),
        ms_in(ms_in_), ms_out(ms_out_),
        dirty_in(dirty_in_), dirty_out(dirty_out_),
        wgt(wgt_), mask(mask_),
        pixsize_x(pixsize_x_), pixsize_y(pixsize_y_),
        nxdirty(gridding ? dirty_out.shape(0) : dirty_in.shape(0)),
        nydirty(gridding ? dirty_out.shape(1) : dirty_in.shape(1)),
        epsilon(epsilon_),
        do_wgridding(do_wgridding_),
        nthreads(nthreads_), verbosity(verbosity_),
        negate_v(negate_v_), divide_by_n(divide_by_n_)
      {
      timers.push("Baseline construction");
      bl = Baselines(uvw, freq, negate_v);
      timers.pop();
      // adjust for increased error when gridding in 2 or 3 dimensions
      epsilon /= do_wgridding ? 3 : 2;
      if (!gridding)
        {
        timers.push("MS zeroing");
        ms_out.fill(0);
        timers.pop();
        }
      scanData();
      if (nvis==0)
        {
        if (gridding) dirty_out.fill(0);
        return;
        }
      auto kidx = getNuNv();
      ofactor = min(double(nu)/nxdirty, double(nv)/nydirty);
      krn = selectKernel<T>(ofactor, epsilon,kidx);
      supp = krn->support();
      nsafe = (supp+1)/2;
      ushift = supp*(-0.5)+1+nu;
      vshift = supp*(-0.5)+1+nv;
      maxiu0 = (nu+nsafe)-supp;
      maxiv0 = (nv+nsafe)-supp;
      vlim = min(nv/2, size_t(nv*bl.Vmax()*pixsize_y+0.5*supp+1));
      uv_side_fast = true;
      size_t vlim2 = (nydirty+1)/2+(supp+1)/2;
      if (vlim2<vlim)
        {
        vlim = vlim2;
        uv_side_fast = false;
        }
      MR_assert(nu>=2*nsafe, "nu too small");
      MR_assert(nv>=2*nsafe, "nv too small");
      MR_assert((nxdirty&1)==0, "nx_dirty must be even");
      MR_assert((nydirty&1)==0, "ny_dirty must be even");
      MR_assert((nu&1)==0, "nu must be even");
      MR_assert((nv&1)==0, "nv must be even");
      MR_assert(epsilon>0, "epsilon must be positive");
      MR_assert(pixsize_x>0, "pixsize_x must be positive");
      MR_assert(pixsize_y>0, "pixsize_y must be positive");
      countRanges();
      report();
      gridding ? x2dirty() : dirty2x();

      if (verbosity>0)
        timers.report(cout);
      }
  };

// Note to self: divide_by_n should always be true when doing Bayesian imaging,
// but wsclean needs it to be false, so this must be kept as a parameter.
template<typename T> void ms2dirty(const mav<double,2> &uvw,
  const mav<double,1> &freq, const mav<complex<T>,2> &ms,
  const mav<T,2> &wgt, const mav<uint8_t,2> &mask, double pixsize_x, double pixsize_y, double epsilon,
  bool do_wgridding, size_t nthreads, mav<T,2> &dirty, size_t verbosity,
  bool negate_v=false, bool divide_by_n=true)
  {
  mav<complex<T>,2> ms_out(nullptr, {0,0}, false);
  mav<T,2> dirty_in(nullptr, {0,0}, false);
  Params<T> par(uvw, freq, ms, ms_out, dirty_in, dirty, wgt, mask, pixsize_x, pixsize_y, epsilon, do_wgridding, nthreads, verbosity, negate_v, divide_by_n);
  }

template<typename T> void dirty2ms(const mav<double,2> &uvw,
  const mav<double,1> &freq, const mav<T,2> &dirty,
  const mav<T,2> &wgt, const mav<uint8_t,2> &mask, double pixsize_x, double pixsize_y,
  double epsilon, bool do_wgridding, size_t nthreads, mav<complex<T>,2> &ms,
  size_t verbosity, bool negate_v=false, bool divide_by_n=true)
  {
  mav<complex<T>,2> ms_in(nullptr, {0,0}, false);
  mav<T,2> dirty_out(nullptr, {0,0}, false);
  Params<T> par(uvw, freq, ms_in, ms, dirty, dirty_out, wgt, mask, pixsize_x, pixsize_y, epsilon, do_wgridding, nthreads, verbosity, negate_v, divide_by_n);
  }

} // namespace detail_gridder

// public names
using detail_gridder::ms2dirty;
using detail_gridder::dirty2ms;

} // namespace ducc0

#endif
