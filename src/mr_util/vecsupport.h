/*
 *  This file is part of the MR utility library.
 *
 *  This code is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This code is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this code; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* Copyright (C) 2019 Max-Planck-Society
   Author: Martin Reinecke */

#ifndef MRUTIL_VECSUPPORT_H
#define MRUTIL_VECSUPPORT_H

#include <cstdlib>
#include <cmath>

namespace mr {

namespace vecsupport {

namespace detail {

using namespace std;

#if (defined(__AVX512F__))
constexpr size_t vbytes = 64;
#elif (defined(__AVX__))
constexpr size_t vbytes = 32;
#elif (defined(__SSE2__))
constexpr size_t vbytes = 16;
#elif (defined(__VSX__))
constexpr size_t vbytes = 16;
#endif

template<typename T, size_t len=vbytes/sizeof(T)> class vtp
  {
  protected:
    using Tv __attribute__ ((vector_size (len*sizeof(T)))) = T;
    static_assert((len>0) && ((len&(len-1))==0), "bad vector length");
    Tv v;

    void from_scalar(T other)
      { for (size_t i=0; i<len; ++i) v[i]=other; }

  public:
    static constexpr size_t vlen=len;
    vtp () {}
    vtp(T other)
      { from_scalar(other); }
    vtp(const Tv &other)
      : v(other) {}
    vtp(const vtp &other) = default;
    vtp &operator=(T other)
      { from_scalar(other); return *this; }
    vtp operator-() const { return vtp(-v); }
    vtp operator+(vtp other) const { return vtp(v+other.v); }
    vtp operator-(vtp other) const { return vtp(v-other.v); }
    vtp operator*(vtp other) const { return vtp(v*other.v); }
    vtp operator/(vtp other) const { return vtp(v/other.v); }
    vtp &operator+=(vtp other) { v+=other.v; return *this; }
    vtp &operator-=(vtp other) { v-=other.v; return *this; }
    vtp &operator*=(vtp other) { v*=other.v; return *this; }
    vtp &operator/=(vtp other) { v/=other.v; return *this; }
    vtp exp() const
      {
      vtp res;
      for (size_t i=0; i<len; ++i) res.v[i] = std::exp(v[i]);
      return res;
      }
    vtp sqrt() const
      {
      vtp res;
      for (size_t i=0; i<len; ++i) res.v[i] = std::sqrt(v[i]);
      return res;
      }

    template<typename I> void Set (I i, T val) { v[i]=val; }
    template<typename I> T operator[](I i) const { return v[i]; }
  };

template<typename T> class aligned_array
  {
  private:
    T *p;
    size_t sz;

#if (__cplusplus >= 201703L)
    static T *ralloc(size_t num)
      {
      if (num==0) return nullptr;
      void *res = aligned_alloc(64,num*sizeof(T));
      if (!res) throw bad_alloc();
      return reinterpret_cast<T *>(res);
      }
    static void dealloc(T *ptr)
      { free(ptr); }
#else // portable emulation
    static T *ralloc(size_t num)
      {
      if (num==0) return nullptr;
      void *ptr = malloc(num*sizeof(T)+64);
      if (!ptr) throw bad_alloc();
      T *res = reinterpret_cast<T *>
        ((reinterpret_cast<size_t>(ptr) & ~(size_t(63))) + 64);
      (reinterpret_cast<void**>(res))[-1] = ptr;
      return res;
      }
    static void dealloc(T *ptr)
      { if (ptr) free((reinterpret_cast<void**>(ptr))[-1]); }
#endif

  public:
    aligned_array() : p(0), sz(0) {}
    aligned_array(size_t n) : p(ralloc(n)), sz(n) {}
    aligned_array(aligned_array &&other)
      : p(other.p), sz(other.sz)
      { other.p=nullptr; other.sz=0; }
    ~aligned_array() { dealloc(p); }

    void resize(size_t n)
      {
      if (n==sz) return;
      dealloc(p);
      p = ralloc(n);
      sz = n;
      }

    T &operator[](size_t idx) { return p[idx]; }
    const T &operator[](size_t idx) const { return p[idx]; }

    T *data() { return p; }
    const T *data() const { return p; }

    size_t size() const { return sz; }
  };

}

using detail::vtp;
using detail::aligned_array;

}}

#endif
