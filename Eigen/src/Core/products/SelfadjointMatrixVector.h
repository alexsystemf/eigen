// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2008-2009 Gael Guennebaud <g.gael@free.fr>
//
// Eigen is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 3 of the License, or (at your option) any later version.
//
// Alternatively, you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of
// the License, or (at your option) any later version.
//
// Eigen is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License or the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License and a copy of the GNU General Public License along with
// Eigen. If not, see <http://www.gnu.org/licenses/>.

#ifndef EIGEN_SELFADJOINT_MATRIX_VECTOR_H
#define EIGEN_SELFADJOINT_MATRIX_VECTOR_H

/* Optimized selfadjoint matrix * vector product:
 * This algorithm processes 2 columns at onces that allows to both reduce
 * the number of load/stores of the result by a factor 2 and to reduce
 * the instruction dependency.
 */
template<typename Scalar, int StorageOrder, int UpLo, bool ConjugateLhs, bool ConjugateRhs>
static EIGEN_DONT_INLINE void ei_product_selfadjoint_vector(
  int size,
  const Scalar*  lhs, int lhsStride,
  const Scalar* _rhs, int rhsIncr,
  Scalar* res, Scalar alpha)
{
  typedef typename ei_packet_traits<Scalar>::type Packet;
  const int PacketSize = sizeof(Packet)/sizeof(Scalar);

  enum {
    IsRowMajor = StorageOrder==RowMajorBit ? 1 : 0,
    IsLower = UpLo == LowerTriangularBit ? 1 : 0,
    FirstTriangular = IsRowMajor == IsLower
  };

  ei_conj_helper<NumTraits<Scalar>::IsComplex && EIGEN_LOGICAL_XOR(ConjugateLhs,  IsRowMajor), ConjugateRhs> cj0;
  ei_conj_helper<NumTraits<Scalar>::IsComplex && EIGEN_LOGICAL_XOR(ConjugateLhs, !IsRowMajor), ConjugateRhs> cj1;

  Scalar cjAlpha = ConjugateRhs ? ei_conj(alpha) : alpha;

  // if the rhs is not sequentially stored in memory we copy it to a temporary buffer,
  // this is because we need to extract packets
  const Scalar* EIGEN_RESTRICT rhs = _rhs;
  if (rhsIncr!=1)
  {
    Scalar* r = ei_aligned_stack_new(Scalar, size);
    const Scalar* it = _rhs;
    for (int i=0; i<size; ++i, it+=rhsIncr)
      r[i] = *it;
    rhs = r;
  }

  int bound = std::max(0,size-8) & 0xfffffffE;
  if (FirstTriangular)
    bound = size - bound;

  for (int j=FirstTriangular ? bound : 0;
       j<(FirstTriangular ? size : bound);j+=2)
  {
    register const Scalar* EIGEN_RESTRICT A0 = lhs + j*lhsStride;
    register const Scalar* EIGEN_RESTRICT A1 = lhs + (j+1)*lhsStride;

    Scalar t0 = cjAlpha * rhs[j];
    Packet ptmp0 = ei_pset1(t0);
    Scalar t1 = cjAlpha * rhs[j+1];
    Packet ptmp1 = ei_pset1(t1);

    Scalar t2 = 0;
    Packet ptmp2 = ei_pset1(t2);
    Scalar t3 = 0;
    Packet ptmp3 = ei_pset1(t3);

    size_t starti = FirstTriangular ? 0 : j+2;
    size_t endi   = FirstTriangular ? j : size;
    size_t alignedEnd = starti;
    size_t alignedStart = (starti) + ei_alignmentOffset(&res[starti], endi-starti);
    alignedEnd = alignedStart + ((endi-alignedStart)/(PacketSize))*(PacketSize);

    res[j] += cj0.pmul(A0[j], t0);
    if(FirstTriangular)
    {
      res[j+1] += cj0.pmul(A1[j+1], t1);
      res[j]   += cj0.pmul(A1[j],   t1);
      t3       += cj1.pmul(A1[j],   rhs[j]);
    }
    else
    {
      res[j+1] += cj0.pmul(A0[j+1],t0) + cj0.pmul(A1[j+1],t1);
      t2 += cj1.pmul(A0[j+1], rhs[j+1]);
    }

    for (size_t i=starti; i<alignedStart; ++i)
    {
      res[i] += t0 * A0[i] + t1 * A1[i];
      t2 += ei_conj(A0[i]) * rhs[i];
      t3 += ei_conj(A1[i]) * rhs[i];
    }
    // Yes this an optimization for gcc 4.3 and 4.4 (=> huge speed up)
    // gcc 4.2 does this optimization automatically.
    const Scalar* EIGEN_RESTRICT a0It  = A0  + alignedStart;
    const Scalar* EIGEN_RESTRICT a1It  = A1  + alignedStart;
    const Scalar* EIGEN_RESTRICT rhsIt = rhs + alignedStart;
          Scalar* EIGEN_RESTRICT resIt = res + alignedStart;
    for (size_t i=alignedStart; i<alignedEnd; i+=PacketSize)
    {
      Packet A0i = ei_ploadu(a0It);  a0It  += PacketSize;
      Packet A1i = ei_ploadu(a1It);  a1It  += PacketSize;
      Packet Bi  = ei_ploadu(rhsIt); rhsIt += PacketSize; // FIXME should be aligned in most cases
      Packet Xi  = ei_pload (resIt);

      Xi    = cj0.pmadd(A0i,ptmp0, cj0.pmadd(A1i,ptmp1,Xi));
      ptmp2 = cj1.pmadd(A0i,  Bi, ptmp2);
      ptmp3 = cj1.pmadd(A1i,  Bi, ptmp3);
      ei_pstore(resIt,Xi); resIt += PacketSize;
    }
    for (size_t i=alignedEnd; i<endi; i++)
    {
      res[i] += cj0.pmul(A0[i], t0) + cj0.pmul(A1[i],t1);
      t2 += cj1.pmul(A0[i], rhs[i]);
      t3 += cj1.pmul(A1[i], rhs[i]);
    }

    res[j]   += alpha * (t2 + ei_predux(ptmp2));
    res[j+1] += alpha * (t3 + ei_predux(ptmp3));
  }
  for (int j=FirstTriangular ? 0 : bound;j<(FirstTriangular ? bound : size);j++)
  {
    register const Scalar* EIGEN_RESTRICT A0 = lhs + j*lhsStride;

    Scalar t1 = cjAlpha * rhs[j];
    Scalar t2 = 0;
    res[j] += cj0.pmul(A0[j],t1);
    for (int i=FirstTriangular ? 0 : j+1; i<(FirstTriangular ? j : size); i++) {
      res[i] += cj0.pmul(A0[i], t1);
      t2 += cj1.pmul(A0[i], rhs[i]);
    }
    res[j] += alpha * t2;
  }

  if(rhsIncr!=1)
    ei_aligned_stack_delete(Scalar, const_cast<Scalar*>(rhs), size);
}

/***************************************************************************
* Wrapper to ei_product_selfadjoint_vector
***************************************************************************/

template<typename Lhs, int LhsMode, typename Rhs>
struct ei_traits<SelfadjointProductMatrix<Lhs,LhsMode,false,Rhs,0,true> >
  : ei_traits<ProductBase<SelfadjointProductMatrix<Lhs,LhsMode,false,Rhs,0,true>, Lhs, Rhs> >
{};

template<typename Lhs, int LhsMode, typename Rhs>
struct SelfadjointProductMatrix<Lhs,LhsMode,false,Rhs,0,true>
  : public ProductBase<SelfadjointProductMatrix<Lhs,LhsMode,false,Rhs,0,true>, Lhs, Rhs >
{
  EIGEN_PRODUCT_PUBLIC_INTERFACE(SelfadjointProductMatrix)

  enum {
    LhsUpLo = LhsMode&(UpperTriangularBit|LowerTriangularBit)
  };

  SelfadjointProductMatrix(const Lhs& lhs, const Rhs& rhs) : Base(lhs,rhs) {}

  template<typename Dest> void scaleAndAddTo(Dest& dst, Scalar alpha) const
  {
    ei_assert(dst.rows()==m_lhs.rows() && dst.cols()==m_rhs.cols());

    const ActualLhsType lhs = LhsBlasTraits::extract(m_lhs);
    const ActualRhsType rhs = RhsBlasTraits::extract(m_rhs);

    Scalar actualAlpha = alpha * LhsBlasTraits::extractScalarFactor(m_lhs)
                               * RhsBlasTraits::extractScalarFactor(m_rhs);

    ei_assert((&dst.coeff(1))-(&dst.coeff(0))==1 && "not implemented yet");
    ei_product_selfadjoint_vector<Scalar, ei_traits<_ActualLhsType>::Flags&RowMajorBit, int(LhsUpLo), bool(LhsBlasTraits::NeedToConjugate), bool(RhsBlasTraits::NeedToConjugate)>
      (
        lhs.rows(),                                     // size
        &lhs.coeff(0,0), lhs.stride(),                  // lhs info
        &rhs.coeff(0), (&rhs.coeff(1))-(&rhs.coeff(0)), // rhs info
        &dst.coeffRef(0),                               // result info
        actualAlpha                                     // scale factor
      );
  }
};


#endif // EIGEN_SELFADJOINT_MATRIX_VECTOR_H