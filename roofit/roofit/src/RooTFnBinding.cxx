/*
 * Project: RooFit
 *
 * Copyright (c) 2023, CERN
 *
 * Redistribution and use in source and binary forms,
 * with or without modification, are permitted according to the terms
 * listed in LICENSE (http://roofit.sourceforge.net/license.txt)
 */

/** \class RooTFnBinding
    \ingroup Roofit
  Use TF1, TF2, TF3 functions as RooFit objects. To create a function binding,
  either directly invoke RooTFnBinding::RooTFnBinding() or use the convenience
  functions RooFit::bindFunction().
**/

#include "Riostream.h"

#include "RooTFnBinding.h"
#include "RooAbsCategory.h"
#include "TF3.h"

using namespace std;

ClassImp(RooTFnBinding);

////////////////////////////////////////////////////////////////////////////////

RooTFnBinding::RooTFnBinding(const char *name, const char *title, TF1* func, const RooArgList& list) :
  RooAbsReal(name,title),
  _olist("obs","obs",this),
  _func(func)
{
  _olist.add(list) ;
}

////////////////////////////////////////////////////////////////////////////////

RooTFnBinding::RooTFnBinding(const char *name, const char *title, TF1* func, const RooArgList& obsList, const RooArgList& paramList) :
  RooAbsReal(name,title),
  _olist("obs","obs",this),
  _plist("params","params",this),
  _func(func)
{
  _olist.add(obsList) ;
  _plist.add(paramList) ;
}

////////////////////////////////////////////////////////////////////////////////

RooTFnBinding::RooTFnBinding(const RooTFnBinding& other, const char* name) :
  RooAbsReal(other,name),
  _olist("obs",this,other._olist),
  _plist("params",this,other._plist),
  _func(other._func)
{
}

////////////////////////////////////////////////////////////////////////////////

double RooTFnBinding::evaluate() const
{
  double x = _olist.at(0) ? ((RooAbsReal*)_olist.at(0))->getVal() : 0 ;
  double y = _olist.at(1) ? ((RooAbsReal*)_olist.at(1))->getVal() : 0 ;
  double z = _olist.at(2) ? ((RooAbsReal*)_olist.at(2))->getVal() : 0 ;
  for (Int_t i=0 ; i<_func->GetNpar() ; i++) {
    _func->SetParameter(i,_plist.at(i)?((RooAbsReal*)_plist.at(i))->getVal() : 0) ;
  }
  return _func->Eval(x,y,z) ;
}

////////////////////////////////////////////////////////////////////////////////

void RooTFnBinding::printArgs(ostream& os) const
{
  // Print object arguments and name/address of function pointer
  os << "[ TFn={" << _func->GetName() << "=" << _func->GetTitle() << "} " ;
  for (Int_t i=0 ; i<numProxies() ; i++) {
    RooAbsProxy* p = getProxy(i) ;
    if (!TString(p->name()).BeginsWith("!")) {
      p->print(os) ;
      os << " " ;
    }
  }
  os << "]" ;
}

////////////////////////////////////////////////////////////////////////////////

namespace RooFit {
  /// Bind a TFx function to RooFit variables. Also see RooTFnBinding.
  RooAbsReal* bindFunction(TF1* func,RooAbsReal& x) {
    return new RooTFnBinding(func->GetName(),func->GetTitle(),func,x) ;
  }
  /// Bind a TFx function to RooFit variables. Also see RooTFnBinding.
  RooAbsReal* bindFunction(TF2* func,RooAbsReal& x, RooAbsReal& y) {
    return new RooTFnBinding(func->GetName(),func->GetTitle(),func,RooArgList(x,y)) ;
  }
  /// Bind a TFx function to RooFit variables. Also see RooTFnBinding.
  RooAbsReal* bindFunction(TF3* func,RooAbsReal& x, RooAbsReal& y, RooAbsReal& z) {
    return new RooTFnBinding(func->GetName(),func->GetTitle(),func,RooArgList(x,y,z)) ;
  }
  /// Bind a TFx function to RooFit variables. Also see RooTFnBinding.
  RooAbsReal* bindFunction(TF1* func,RooAbsReal& x, const RooArgList& params) {
    return new RooTFnBinding(func->GetName(),func->GetTitle(),func,x,params) ;
  }
  /// Bind a TFx function to RooFit variables. Also see RooTFnBinding.
  RooAbsReal* bindFunction(TF2* func,RooAbsReal& x, RooAbsReal& y, const RooArgList& params) {
    return new RooTFnBinding(func->GetName(),func->GetTitle(),func,RooArgList(x,y),params) ;
  }
  /// Bind a TFx function to RooFit variables. Also see RooTFnBinding.
  RooAbsReal* bindFunction(TF3* func,RooAbsReal& x, RooAbsReal& y, RooAbsReal& z, const RooArgList& params) {
    return new RooTFnBinding(func->GetName(),func->GetTitle(),func,RooArgList(x,y,z),params) ;
  }

}
