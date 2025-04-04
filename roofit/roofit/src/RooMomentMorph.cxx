/*
 * Project: RooFit
 *
 * Copyright (c) 2023, CERN
 *
 * Redistribution and use in source and binary forms,
 * with or without modification, are permitted according to the terms
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)
 */

/** \class RooMomentMorph
    \ingroup Roofit

**/

#include "Riostream.h"

#include "RooMomentMorph.h"
#include "RooAbsCategory.h"
#include "RooRealConstant.h"
#include "RooRealVar.h"
#include "RooFormulaVar.h"
#include "RooCustomizer.h"
#include "RooAddPdf.h"
#include "RooAddition.h"
#include "RooMoment.h"
#include "RooLinearVar.h"
#include "RooChangeTracker.h"

#include "TMath.h"
#include "TH1.h"

ClassImp(RooMomentMorph);

////////////////////////////////////////////////////////////////////////////////
/// coverity[UNINIT_CTOR]

RooMomentMorph::RooMomentMorph()
  : _cacheMgr(this,10,true,true), _curNormSet(nullptr), _mref(nullptr), _M(nullptr), _useHorizMorph(true)
{
}

////////////////////////////////////////////////////////////////////////////////
/// CTOR

RooMomentMorph::RooMomentMorph(const char *name, const char *title,
                           RooAbsReal& _m,
                           const RooArgList& varList,
                           const RooArgList& pdfList,
                           const TVectorD& mrefpoints,
                           Setting setting) :
  RooAbsPdf(name,title),
  _cacheMgr(this,10,true,true),
  m("m","m",this,_m),
  _varList("varList","List of variables",this),
  _pdfList("pdfList","List of pdfs",this),
  _setting(setting),
  _useHorizMorph(true)
{
  // observables
  for (auto *var : varList) {
    if (!dynamic_cast<RooAbsReal*>(var)) {
      coutE(InputArguments) << "RooMomentMorph::ctor(" << GetName() << ") ERROR: variable " << var->GetName() << " is not of type RooAbsReal" << std::endl ;
      throw std::string("RooPolyMorh::ctor() ERROR variable is not of type RooAbsReal") ;
    }
    _varList.add(*var) ;
  }

  // reference p.d.f.s

  for (auto const *pdf : pdfList) {
    if (!dynamic_cast<RooAbsPdf const*>(pdf)) {
      coutE(InputArguments) << "RooMomentMorph::ctor(" << GetName() << ") ERROR: pdf " << pdf->GetName() << " is not of type RooAbsPdf" << std::endl ;
      throw std::string("RooPolyMorh::ctor() ERROR pdf is not of type RooAbsPdf") ;
    }
    _pdfList.add(*pdf) ;
  }

  _mref      = new TVectorD(mrefpoints);

  // initialization
  initialize();
}

////////////////////////////////////////////////////////////////////////////////
/// CTOR

RooMomentMorph::RooMomentMorph(const char *name, const char *title,
                           RooAbsReal& _m,
                           const RooArgList& varList,
                           const RooArgList& pdfList,
                           const RooArgList& mrefList,
                           Setting setting) :
  RooAbsPdf(name,title),
  _cacheMgr(this,10,true,true),
  m("m","m",this,_m),
  _varList("varList","List of variables",this),
  _pdfList("pdfList","List of pdfs",this),
  _setting(setting),
  _useHorizMorph(true)
{
  // observables
  for (auto *var : varList) {
    if (!dynamic_cast<RooAbsReal*>(var)) {
      coutE(InputArguments) << "RooMomentMorph::ctor(" << GetName() << ") ERROR: variable " << var->GetName() << " is not of type RooAbsReal" << std::endl ;
      throw std::string("RooPolyMorh::ctor() ERROR variable is not of type RooAbsReal") ;
    }
    _varList.add(*var) ;
  }

  // reference p.d.f.s
  for (auto const *pdf : pdfList) {
    if (!dynamic_cast<RooAbsPdf const*>(pdf)) {
      coutE(InputArguments) << "RooMomentMorph::ctor(" << GetName() << ") ERROR: pdf " << pdf->GetName() << " is not of type RooAbsPdf" << std::endl ;
      throw std::string("RooPolyMorh::ctor() ERROR pdf is not of type RooAbsPdf") ;
    }
    _pdfList.add(*pdf) ;
  }

  // reference points in m
  _mref      = new TVectorD(mrefList.size());
  Int_t i = 0;
  for (auto *mref : mrefList) {
    if (!dynamic_cast<RooAbsReal*>(mref)) {
      coutE(InputArguments) << "RooMomentMorph::ctor(" << GetName() << ") ERROR: mref " << mref->GetName() << " is not of type RooAbsReal" << std::endl ;
      throw std::string("RooPolyMorh::ctor() ERROR mref is not of type RooAbsReal") ;
    }
    if (!dynamic_cast<RooConstVar*>(mref)) {
      coutW(InputArguments) << "RooMomentMorph::ctor(" << GetName() << ") WARNING mref point " << i << " is not a constant, taking a snapshot of its value" << std::endl ;
    }
    (*_mref)[i] = static_cast<RooAbsReal*>(mref)->getVal() ;
    i++;
  }

  // initialization
  initialize();
}

////////////////////////////////////////////////////////////////////////////////

RooMomentMorph::RooMomentMorph(const RooMomentMorph& other, const char* name) :
  RooAbsPdf(other,name),
  _cacheMgr(other._cacheMgr,this),
  _curNormSet(nullptr),
  m("m",this,other.m),
  _varList("varList",this,other._varList),
  _pdfList("pdfList",this,other._pdfList),
  _setting(other._setting),
  _useHorizMorph(other._useHorizMorph)
{
  _mref = new TVectorD(*other._mref) ;

  // initialization
  initialize();
}

////////////////////////////////////////////////////////////////////////////////

RooMomentMorph::~RooMomentMorph()
{
  if (_mref)   delete _mref;
  if (_M)      delete _M;
}

////////////////////////////////////////////////////////////////////////////////

void RooMomentMorph::initialize()
{
  Int_t nPdf = _pdfList.size();

  // other quantities needed
  if (nPdf!=_mref->GetNrows()) {
    coutE(InputArguments) << "RooMomentMorph::initialize(" << GetName() << ") ERROR: nPdf != nRefPoints" << std::endl ;
    assert(0) ;
  }

  TVectorD dm{nPdf};
  _M = new TMatrixD(nPdf,nPdf);

  // transformation matrix for non-linear extrapolation, needed in evaluate()
  TMatrixD M(nPdf,nPdf);
  for (Int_t i=0; i<_mref->GetNrows(); ++i) {
    dm[i] = (*_mref)[i]-(*_mref)[0];
    M(i,0) = 1.;
    if (i>0) M(0,i) = 0.;
  }
  for (Int_t i=1; i<_mref->GetNrows(); ++i) {
    for (Int_t j=1; j<_mref->GetNrows(); ++j) {
      M(i,j) = TMath::Power(dm[i],(double)j);
    }
  }
  (*_M) = M.Invert();
}

////////////////////////////////////////////////////////////////////////////////

RooMomentMorph::CacheElem::CacheElem(std::unique_ptr<RooAbsPdf> && sumPdf,
                                       std::unique_ptr<RooChangeTracker> && tracker,
                                       const RooArgList& flist)
  : _sumPdf(std::move(sumPdf)), _tracker(std::move(tracker)) {
  _frac.add(flist);
}

////////////////////////////////////////////////////////////////////////////////

RooMomentMorph::CacheElem* RooMomentMorph::getCache(const RooArgSet* /*nset*/) const
{
  if (auto* cache = static_cast<CacheElem*>(_cacheMgr.getObj(nullptr,static_cast<RooArgSet*>(nullptr)))) {
    return cache ;
  }
  Int_t nVar = _varList.size();
  Int_t nPdf = _pdfList.size();

  RooAbsReal* null = nullptr;
  std::vector<RooAbsReal*> meanrv(nPdf*nVar,null);
  std::vector<RooAbsReal*> sigmarv(nPdf*nVar,null);
  std::vector<RooAbsReal*> myrms(nVar,null);
  std::vector<RooAbsReal*> mypos(nVar,null);
  std::vector<RooAbsReal*> slope(nPdf*nVar,null);
  std::vector<RooAbsReal*> offs(nPdf*nVar,null);
  std::vector<RooAbsReal*> transVar(nPdf*nVar,null);
  std::vector<RooAbsReal*> transPdf(nPdf,null);

  RooArgSet ownedComps ;

  RooArgList fracl ;

  // fraction parameters
  RooArgList coefList("coefList");
  RooArgList coefList2("coefList2");
  for (Int_t i=0; i<2*nPdf; ++i) {
    std::string fracName = "frac_" + std::to_string(i);

    RooRealVar* frac = new RooRealVar(fracName.c_str(),fracName.c_str(),1.) ;

    fracl.add(*frac); // to be set later
    if (i<nPdf) coefList.add(*frac);
    else coefList2.add(*frac);
    ownedComps.add(*frac);
  }

  std::unique_ptr<RooAddPdf> theSumPdf;
  std::string sumpdfName = Form("%s_sumpdf",GetName());

  if (_useHorizMorph) {
    // mean and sigma
    RooArgList varList(_varList) ;
    for (Int_t i=0; i<nPdf; ++i) {
      for (Int_t j=0; j<nVar; ++j) {

   std::string meanName = Form("%s_mean_%d_%d",GetName(),i,j);
   std::string sigmaName = Form("%s_sigma_%d_%d",GetName(),i,j);

        RooAbsMoment* mom = nVar==1 ?
     ((RooAbsPdf*)_pdfList.at(i))->sigma((RooRealVar&)*varList.at(j)) :
     ((RooAbsPdf*)_pdfList.at(i))->sigma((RooRealVar&)*varList.at(j),varList) ;

   mom->setLocalNoDirtyInhibit(true) ;
   mom->mean()->setLocalNoDirtyInhibit(true) ;

   sigmarv[ij(i,j)] = mom ;
   meanrv[ij(i,j)]  = mom->mean() ;

   ownedComps.add(*sigmarv[ij(i,j)]) ;
      }
    }

    // slope and offset (to be set later, depend on m)
    for (Int_t j=0; j<nVar; ++j) {
      RooArgList meanList("meanList");
      RooArgList rmsList("rmsList");
      for (Int_t i=0; i<nPdf; ++i) {
   meanList.add(*meanrv[ij(i,j)]);
   rmsList.add(*sigmarv[ij(i,j)]);
      }
      std::string myrmsName = Form("%s_rms_%d",GetName(),j);
      std::string myposName = Form("%s_pos_%d",GetName(),j);
      myrms[j] = new RooAddition(myrmsName.c_str(),myrmsName.c_str(),rmsList,coefList2);
      mypos[j] = new RooAddition(myposName.c_str(),myposName.c_str(),meanList,coefList2);
      ownedComps.add(RooArgSet(*myrms[j],*mypos[j])) ;
    }

    // construction of unit pdfs
    RooArgList transPdfList;

    for (Int_t i=0; i<nPdf; ++i) {

      auto& pdf = static_cast<RooAbsPdf&>(_pdfList[i]);
      std::string pdfName = "pdf_" + std::to_string(i);
      RooCustomizer cust(pdf,pdfName.c_str());

      for (Int_t j=0; j<nVar; ++j) {
   // slope and offset formulas
   std::string slopeName = Form("%s_slope_%d_%d",GetName(),i,j);
   std::string offsetName = Form("%s_offset_%d_%d",GetName(),i,j);
   slope[ij(i,j)]  = new RooFormulaVar(slopeName.c_str(),"@0/@1", {*sigmarv[ij(i,j)],*myrms[j]});
   offs[ij(i,j)] = new RooFormulaVar(offsetName.c_str(),"@0-(@1*@2)", {*meanrv[ij(i,j)],*mypos[j],*slope[ij(i,j)]});
   ownedComps.add(RooArgSet(*slope[ij(i,j)],*offs[ij(i,j)])) ;
   // linear transformations, so pdf can be renormalized
   auto* var = static_cast<RooRealVar*>(_varList[j]);
   std::string transVarName = Form("%s_transVar_%d_%d",GetName(),i,j);
   //transVar[ij(i,j)] = new RooFormulaVar(transVarName.c_str(),transVarName.c_str(),"@0*@1+@2",RooArgList(*var,*slope[ij(i,j)],*offs[ij(i,j)]));

   transVar[ij(i,j)] = new RooLinearVar(transVarName.c_str(),transVarName.c_str(),*var,*slope[ij(i,j)],*offs[ij(i,j)]);

   // *** WVE this is important *** this declares that frac effectively depends on the morphing parameters
   // This will prevent the likelihood optimizers from erroneously declaring terms constant
   transVar[ij(i,j)]->addServer(const_cast<RooAbsReal&>(m.arg()));

   ownedComps.add(*transVar[ij(i,j)]) ;
   cust.replaceArg(*var,*transVar[ij(i,j)]);
      }
      transPdf[i] = (RooAbsPdf*) cust.build() ;
      transPdfList.add(*transPdf[i]);
      ownedComps.add(*transPdf[i]) ;
    }
    // sum pdf
    theSumPdf = std::make_unique<RooAddPdf>(sumpdfName.c_str(),sumpdfName.c_str(),transPdfList,coefList);
  }
  else {
    theSumPdf = std::make_unique<RooAddPdf>(sumpdfName.c_str(),sumpdfName.c_str(),_pdfList,coefList);
  }

  // *** WVE this is important *** this declares that frac effectively depends on the morphing parameters
  // This will prevent the likelihood optimizers from erroneously declaring terms constant
  theSumPdf->addServer(const_cast<RooAbsReal&>(m.arg()));
  theSumPdf->addOwnedComponents(ownedComps) ;

  // change tracker for fraction parameters
  std::string trackerName = Form("%s_frac_tracker",GetName()) ;
  auto tracker = std::make_unique<RooChangeTracker>(trackerName.c_str(),trackerName.c_str(),m.arg(),true) ;

  // Store it in the cache
  auto cache = new CacheElem(std::move(theSumPdf),std::move(tracker),fracl) ;
  _cacheMgr.setObj(nullptr,nullptr,cache,nullptr);

  cache->calculateFractions(*this, false);
  return cache ;
}

////////////////////////////////////////////////////////////////////////////////

RooArgList RooMomentMorph::CacheElem::containedArgs(Action)
{
  return RooArgList(*_sumPdf,*_tracker) ;
}

////////////////////////////////////////////////////////////////////////////////

RooMomentMorph::CacheElem::~CacheElem() {}

////////////////////////////////////////////////////////////////////////////////
/// Special version of getVal() overrides RooAbsReal::getVal() to save value of current normalization set

double RooMomentMorph::getVal(const RooArgSet* set) const
{
  _curNormSet = set ? (RooArgSet*)set : (RooArgSet*)&_varList ;
  return RooAbsPdf::getVal(set) ;
}

////////////////////////////////////////////////////////////////////////////////

RooAbsPdf* RooMomentMorph::sumPdf(const RooArgSet* nset)
{
  CacheElem* cache = getCache(nset ? nset : _curNormSet) ;

  if (cache->_tracker->hasChanged(true)) {
    cache->calculateFractions(*this,false); // verbose turned off
  }

  return cache->_sumPdf.get();
}

////////////////////////////////////////////////////////////////////////////////

double RooMomentMorph::evaluate() const
{
  CacheElem* cache = getCache(_curNormSet) ;

  if (cache->_tracker->hasChanged(true)) {
    cache->calculateFractions(*this,false); // verbose turned off
  }

  return cache->_sumPdf->getVal(_pdfList.nset());
}

////////////////////////////////////////////////////////////////////////////////

RooRealVar* RooMomentMorph::CacheElem::frac(Int_t i )
{
  return static_cast<RooRealVar*>(_frac.at(i))  ;
}

////////////////////////////////////////////////////////////////////////////////

const RooRealVar* RooMomentMorph::CacheElem::frac(Int_t i ) const
{
  return static_cast<RooRealVar*>(_frac.at(i))  ;
}

////////////////////////////////////////////////////////////////////////////////

void RooMomentMorph::CacheElem::calculateFractions(const RooMomentMorph& self, bool verbose) const
{
  Int_t nPdf = self._pdfList.size();

  double dm = self.m - (*self._mref)[0];

  // fully non-linear
  double sumposfrac=0.;
  for (Int_t i=0; i<nPdf; ++i) {
    double ffrac=0.;
    for (Int_t j=0; j<nPdf; ++j) { ffrac += (*self._M)(j,i) * (j==0?1.:TMath::Power(dm,(double)j)); }
    if (ffrac>=0) sumposfrac+=ffrac;
    // fractions for pdf
    ((RooRealVar*)frac(i))->setVal(ffrac);
    // fractions for rms and mean
    ((RooRealVar*)frac(nPdf+i))->setVal(ffrac);
    if (verbose) { std::cout << ffrac << std::endl; }
  }

  // various mode settings
  int imin = self.idxmin(self.m);
  int imax = self.idxmax(self.m);
  double mfrac = (self.m-(*self._mref)[imin])/((*self._mref)[imax]-(*self._mref)[imin]);
  switch (self._setting) {
    case NonLinear:
      // default already set above
    break;

    case SineLinear:
      mfrac = TMath::Sin( TMath::PiOver2()*mfrac ); // this gives a continuous differentiable transition between grid points.

      // now fall through to Linear case

    case Linear:
      for (Int_t i=0; i<2*nPdf; ++i)
        ((RooRealVar*)frac(i))->setVal(0.);
      if (imax>imin) { // m in between mmin and mmax
        ((RooRealVar*)frac(imin))->setVal(1.-mfrac);
        ((RooRealVar*)frac(nPdf+imin))->setVal(1.-mfrac);
        ((RooRealVar*)frac(imax))->setVal(mfrac);
        ((RooRealVar*)frac(nPdf+imax))->setVal(mfrac);
      } else if (imax==imin) { // m outside mmin and mmax
        ((RooRealVar*)frac(imin))->setVal(1.);
        ((RooRealVar*)frac(nPdf+imin))->setVal(1.);
      }
    break;
    case NonLinearLinFractions:
      for (Int_t i=0; i<nPdf; ++i)
        ((RooRealVar*)frac(i))->setVal(0.);
      if (imax>imin) { // m in between mmin and mmax
        ((RooRealVar*)frac(imin))->setVal(1.-mfrac);
        ((RooRealVar*)frac(imax))->setVal(mfrac);
      } else if (imax==imin) { // m outside mmin and mmax
        ((RooRealVar*)frac(imin))->setVal(1.);
      }
    break;
    case NonLinearPosFractions:
      for (Int_t i=0; i<nPdf; ++i) {
        if (((RooRealVar*)frac(i))->getVal()<0) ((RooRealVar*)frac(i))->setVal(0.);
        ((RooRealVar*)frac(i))->setVal(((RooRealVar*)frac(i))->getVal()/sumposfrac);
      }
    break;
  }

}

////////////////////////////////////////////////////////////////////////////////

int RooMomentMorph::idxmin(const double& mval) const
{
  int imin(0);
  Int_t nPdf = _pdfList.size();
  double mmin=-DBL_MAX;
  for (Int_t i=0; i<nPdf; ++i)
    if ( (*_mref)[i]>mmin && (*_mref)[i]<=mval ) { mmin=(*_mref)[i]; imin=i; }
  return imin;
}


////////////////////////////////////////////////////////////////////////////////

int RooMomentMorph::idxmax(const double& mval) const
{
  int imax(0);
  Int_t nPdf = _pdfList.size();
  double mmax=DBL_MAX;
  for (Int_t i=0; i<nPdf; ++i)
    if ( (*_mref)[i]<mmax && (*_mref)[i]>=mval ) { mmax=(*_mref)[i]; imax=i; }
  return imax;
}
