//*************************************************************************
//* =====================
//*  TKalDetCradle Class
//* =====================
//*
//* (Description)
//*   A singleton class to hold information of detector system
//*   used in Kalman filter classes.
//* (Requires)
//*     TObjArray
//*     TVKalDetector
//* (Provides)
//*     class TKalDetCradle
//* (Update Recored)
//*   2005/02/23  A.Yamaguchi  	Original version.
//*   2005/08/14  K.Fujii        Removed CalcTable(), GetMeasLayerTable(),
//*                              GetPhiTable(), and GetDir() and added
//*                              Transport() to do their functions.
//*   2010/04/06  K.Fujii        Modified Transport() to allow a 1-dim hit,
//*                              for which pivot is at the xpected hit.
//*   2012/11/29  K.Fujii        Moved GetEnergyLoss and CalcQms from
//*                              TKalDetCradle.
//*
//*************************************************************************

#include "TKalDetCradle.h"   // from KalTrackLib
#include "TVMeasLayer.h"     // from KalTrackLib
#include "TVKalDetector.h"   // from KalTrackLib
#include "TKalTrackSite.h"   // from KalTrackLib
#include "TKalTrackState.h"  // from KalTrackLib
#include "TKalTrack.h"       // from KalTrackLib
#include "TVSurface.h"       // from GeomLib
#include <memory>            // from STL
#include <iostream>          // from STL

ClassImp(TKalDetCradle)

//_________________________________________________________________________
//  ----------------------------------
//   Ctors and Dtor
//  ----------------------------------

TKalDetCradle::TKalDetCradle(Int_t n)
             : TObjArray(n), fIsMSON(kTRUE), fIsDEDXON(kTRUE),
               fDone(kFALSE), fIsClosed(kFALSE)
{
}

TKalDetCradle::~TKalDetCradle()
{
}

//_________________________________________________________________________
//  ----------------------------------
//   Utility Methods
//  ----------------------------------
//_________________________________________________________________________
// -----------------
//  Install
// -----------------
//    installs a sub-detector into this cradle.
//
void TKalDetCradle::Install(TVKalDetector &det)
{
   if (IsClosed()) {
      std::cerr << ">>>> Error!! >>>> TKalDetCradle::Install" << std::endl
                << "      Cradle already closed. Abort!!"     << std::endl;
      abort();
   }
   TIter next(&det);
   TObject *mlp = 0;  // measment layer pointer
   while ((mlp = next())) {
      Add(mlp);
      dynamic_cast<TAttElement *>(mlp)->SetParentPtr(&det);
      det.SetParentPtr(this);
   }
   fDone = kFALSE;
}

//_________________________________________________________________________
// -----------------
//  Transport
// -----------------
//    transports state (sv) from site (from) to site (to), taking into 
//    account multiple scattering and energy loss and updates state (sv),
//    propagator matrix (F), and process noise matrix (Q).
//
void TKalDetCradle::Transport(const TKalTrackSite  &from,  // site from
                                    TKalTrackSite  &to,    // site to
                                    TKalMatrix     &sv,    // state vector
                                    TKalMatrix     &F,     // propagator matrix
                                    TKalMatrix     &Q)     // process noise matrix
{
   // ---------------------------------------------------------------------
   //  Sort measurement layers in this cradle if not
   // ---------------------------------------------------------------------
   if (!fDone) Update();

   // ---------------------------------------------------------------------
   //  Decide the mass of the current track
   // ---------------------------------------------------------------------
   static const Double_t kMpi = 0.13957018; // pion mass [GeV]
   TKalTrack *ktp  = static_cast<TKalTrack *>(from.GetParentPtr());
   Double_t   mass = ktp ? ktp->GetMass() : kMpi;
	
   // ---------------------------------------------------------------------
   //  Locate sites from and to in this cradle
   // ---------------------------------------------------------------------
   Int_t  fridx = from.GetHit().GetMeasLayer().GetIndex(); // index of site from
   Int_t  toidx = to.GetHit().GetMeasLayer().GetIndex();   // index of site to
   Int_t  di    = fridx > toidx ? -1 : 1;                  // layer increment
   Bool_t isout = di > 0 ? kTRUE : kFALSE;  // out-going or in-coming

   std::auto_ptr<TVTrack> help(&static_cast<TKalTrackState &>
                              (from.GetCurState()).CreateTrack()); // tmp track
   TVTrack &hel = *help;
	
   TVector3 xx;                // expected hit position vector
   Double_t fid     = 0.;      // deflection angle from the last hit

   Int_t sdim = sv.GetNrows();                // # track parameters
   F.UnitMatrix();
   Q.Zero();

   TKalMatrix DF(sdim, sdim);                 // propagator matrix segment

   // ---------------------------------------------------------------------
   //  Loop over layers and transport sv, F, and Q step by step
   // ---------------------------------------------------------------------
   Int_t ifr = fridx;

   for (Int_t ito=fridx+di; (di>0 && ito<=toidx)||(di<0 && ito>=toidx); ito += di) {
      if (dynamic_cast<TVSurface *>(At(ito))->CalcXingPointWith(hel, xx, fid)) {
         const TVMeasLayer   &ml  = *dynamic_cast<TVMeasLayer *>(At(ifr));
         const TMaterial     &mat = ml.GetMaterial(isout);
         TKalMatrix Qms(sdim, sdim);
         if (IsMSOn()) CalcQms(mat, hel, fid, Qms, mass); // Qms for this step

         hel.MoveTo(xx, fid, &DF);         // move to next expected hit
         if (sdim == 6) DF(5, 5) = 1.;     // t0 stays the same
         F = DF * F;                       // update F
         TKalMatrix DFt  = TKalMatrix(TMatrixD::kTransposed, DF);

         Q = DF * (Q + Qms) * DFt;         // transport Q to next expected hit 

         if (IsDEDXOn()) {
            hel.PutInto(sv);                              // copy hel to sv
            sv(2,0) += GetEnergyLoss(mat, hel, fid, mass); // correct for dE/dx
            hel.SetTo(sv, hel.GetPivot());                // save sv back to hel
         }
         ifr = ito;
      }
   }
   // ---------------------------------------------------------------------
   //  Move pivot from last expected hit to actural hit at site to
   // ---------------------------------------------------------------------
   if (to.GetDimension() > 1) {
      hel.MoveTo(to.GetPivot(), fid, &DF); // move pivot to actual hit (to)
   } else {
      const TVMeasLayer &ml  = to.GetHit().GetMeasLayer();
      dynamic_cast<const TVSurface *>(&ml)->CalcXingPointWith(hel, xx, fid);
      hel.MoveTo(xx, fid, &DF); // move pivot to expected hit
      to.SetPivot(xx);          // if it is a 1-dim hit
   }
   hel.PutInto(sv);                     // save updated hel to sv
   F = DF * F;                          // update F accordingly
}

//_________________________________________________________________________
// -----------------
//  Update
// -----------------
//    sorts meaurement layers according to layer's sorting policy
//    and puts index to layers from inside to outside.
//
void TKalDetCradle::Update()
{
   fDone = kTRUE;

   UnSort();   // unsort
   Sort();     // sort layers according to sorting policy

   TIter next(this);
   TVMeasLayer *mlp = 0;
   Int_t i = 0;
   while ((mlp = dynamic_cast<TVMeasLayer *>(next()))) {
      mlp->SetIndex(i++);
   }
}

Double_t TKalDetCradle::GetEnergyLoss_old(const TMaterial &mat,
                                          const TVTrack   &hel,
                                          Double_t   df,
                                          Double_t   mass) const
{
    Double_t cpa    = hel.GetKappa();
    Double_t tnl    = hel.GetTanLambda();
    Double_t tnl2   = tnl * tnl;
    Double_t tnl21  = 1. + tnl2;
    Double_t cslinv = TMath::Sqrt(tnl21);
    Double_t mom2   = tnl21 / (cpa * cpa);
    
    // -----------------------------------------
    // Bethe-Bloch eq. (Physical Review D P195.)
    // -----------------------------------------
    static const Double_t kK   = 0.307075e-3;     // [GeV*cm^2]
    static const Double_t kMe  = 0.510998902e-3;  // electron mass [GeV]
    
    Double_t dnsty = mat.GetDensity();		// density
    Double_t A     = mat.GetA();                 // atomic mass
    Double_t Z     = mat.GetZ();                 // atomic number
    //Double_t I    = Z * 1.e-8;			// mean excitation energy [GeV]
    //Double_t I    = (2.4 +Z) * 1.e-8;		// mean excitation energy [GeV]
    Double_t I    = (9.76 * Z + 58.8 * TMath::Power(Z, -0.19)) * 1.e-9;
    Double_t hwp  = 28.816 * TMath::Sqrt(dnsty * Z/A) * 1.e-9;
    Double_t bg2  = mom2 / (mass * mass);
    Double_t gm2  = 1. + bg2;
    Double_t meM  = kMe / mass;
    Double_t x    = log10(TMath::Sqrt(bg2));
    Double_t C0   = - (2. * log(I/hwp) + 1.);
    Double_t a    = -C0/27.;
    Double_t del;
    if (x >= 3.)            del = 4.606 * x + C0;
    else if (0.<=x && x<3.) del = 4.606 * x + C0 + a * TMath::Power(3.-x, 3.);
    else                    del = 0.;
    Double_t tmax = 2.*kMe*bg2 / (1. + meM*(2.*TMath::Sqrt(gm2) + meM));
    Double_t dedx = kK * Z/A * gm2/bg2 * (0.5*log(2.*kMe*bg2*tmax / (I*I))
                                          - bg2/gm2 - del);
    
    Double_t path = hel.IsInB()
    ? TMath::Abs(hel.GetRho()*df)*cslinv
    : TMath::Abs(df)*cslinv;
    
    //fg: switched from using cm to mm in KalTest - material (density) and energy still in GeV and cm
    //path /= 10. ;
    //By Jixie: the energy loss of this model does not work well in RTPC
    //protons loss too small energy 
    path *= 1.25;  //this number is from my comparison 

    Double_t edep = dedx * dnsty * path;
    
    
    Double_t cpaa = TMath::Sqrt(tnl21 / (mom2 + edep
                                         * (edep + 2. * TMath::Sqrt(mom2 + mass * mass))));
    Double_t dcpa = TMath::Abs(cpa) - cpaa;
    
    static const Bool_t kForward  = kTRUE;
    static const Bool_t kBackward = kFALSE;
    Bool_t isfwd = ((cpa > 0 && df < 0) || (cpa <= 0 && df > 0)) ? kForward : kBackward;
    return isfwd ? (cpa > 0 ? dcpa : -dcpa) : (cpa > 0 ? -dcpa : dcpa);
}
  

//_________________________________________________________________________
// -----------------
//  GetEnergyLoss
// -----------------
//    returns energy loss.
//the original code only work for 0.1<beta*gamma<1000, it does not include shell effect
//which is useful for 0.1>beta*gamma, and its desity effect is doubled 
//density effect will affect 2<beta*gamma
//By Jixie @20160829 Update for the following: 
//1) add a factor of 0.5 in density effect, which is useful for high energy particle
//2) include shell effect, which is useful for low energy paricle like 50 MeV/c proton
Double_t TKalDetCradle::GetEnergyLoss(const TMaterial &mat,
                                      const TVTrack   &hel,
                                            Double_t   df,
                                            Double_t   mass) const
{
    Double_t cpa    = hel.GetKappa();
    Double_t tnl    = hel.GetTanLambda();
    Double_t tnl2   = tnl * tnl;
    Double_t tnl21  = 1. + tnl2;
    Double_t cslinv = TMath::Sqrt(tnl21);
    Double_t mom2   = tnl21 / (cpa * cpa);
    
    // -----------------------------------------
    // Bethe-Bloch eq. (Physical Review D P195.)
    // -----------------------------------------
    static const Double_t kK   = 0.307075e-3;     // [GeV*cm^2]
    static const Double_t kMe  = 0.510998902e-3;  // electron mass [GeV]
    
    Double_t dnsty = mat.GetDensity();		// density
    Double_t A     = mat.GetA();                // atomic mass
    Double_t Z     = mat.GetZ();                // atomic number
    //Double_t I    = Z * 1.e-8;		// mean excitation energy [GeV]
    //Double_t I    = (2.4 +Z) * 1.e-8;		// mean excitation energy [GeV]
    //use Sterneimer's paper
    Double_t I    = (9.76 * Z + 58.8 * TMath::Power(Z, -0.19)) * 1.e-9;
    if(Z<13) I    = (12 * Z + 7.) * 1.e-9;
    Double_t hwp  = 28.816 * TMath::Sqrt(dnsty * Z/A) * 1.e-9;
    Double_t bg2  = mom2 / (mass * mass);       //(beta*gamma)^2
    Double_t gm2  = 1. + bg2;                   // gamma^2   
    Double_t beta2= bg2/gm2;
    Double_t meM  = kMe / mass;
    //the original density effect is strange
    Double_t x    = log10(TMath::Sqrt(bg2));
    Double_t C0   = - (2. * log(I/hwp) + 1.);
    Double_t a    = -C0/27.;
    Double_t del;
    if (x >= 3.)            del = 4.606 * x + C0;
    else if (0.<=x && x<3.) del = 4.606 * x + C0 + a * TMath::Power(3.-x, 3.);
    else                    del = 0.;
    Double_t tmax = 2.*kMe*bg2 / (1. + meM*(2.*TMath::Sqrt(gm2) + meM));
    //shell correction from Leo book P26, Eq.2.33, work only for beta*gamma>0.13
    Double_t Iev = I*1.0e9;
    Double_t Ce = 0;
    if(bg2<0.0169) {
      Double_t T = mass * (sqrt(gm2) - 1.0); //kenimatic energy T = E - M = M *(Gamma-1) 
      Double_t pbg2 = 0.13*0.13;
      Double_t Ce_bg0p13 = (0.422377/pbg2+0.0304043/pbg2/pbg2-0.00038106/pbg2/pbg2/pbg2)*1.e-6*Iev*Iev
	+ (3.850190/pbg2-0.1667989/pbg2/pbg2+0.00157955/pbg2/pbg2/pbg2)*1.e-9*Iev*Iev*Iev;
      Ce = Ce_bg0p13 * log(T/0.002) / log(0.0079/0.002);
    }
    else{
       Ce = (0.422377/bg2+0.0304043/bg2/bg2-0.00038106/bg2/bg2/bg2)*1.e-6*Iev*Iev
	+ (3.850190/bg2-0.1667989/bg2/bg2+0.00157955/bg2/bg2/bg2)*1.e-9*Iev*Iev*Iev;
    }
    Double_t dedx = kK * Z/A / beta2 * (0.5*log(2.*kMe*bg2*tmax / (I*I))
                                          - beta2 - 0.5*del - Ce/Z);
    
    Double_t path = hel.IsInB()
    ? TMath::Abs(hel.GetRho()*df)*cslinv
    : TMath::Abs(df)*cslinv;
    
    //By Jixie: the energy loss of this model does not work well in RTPC
    //protons loss too small energy 
    path *= 1.25;  //this number is from my comparison 

    Double_t edep = dedx * dnsty * path;
    
    
    Double_t cpaa = TMath::Sqrt(tnl21 / (mom2 + edep
                                         * (edep + 2. * TMath::Sqrt(mom2 + mass * mass))));
    Double_t dcpa = TMath::Abs(cpa) - cpaa;
    
    static const Bool_t kForward  = kTRUE;
    static const Bool_t kBackward = kFALSE;
    Bool_t isfwd = ((cpa > 0 && df < 0) || (cpa <= 0 && df > 0)) ? kForward : kBackward;
    return isfwd ? (cpa > 0 ? dcpa : -dcpa) : (cpa > 0 ? -dcpa : dcpa);
}

//_________________________________________________________________________
// -----------------
//  CalQms
// -----------------
//    calculates process noise matrix for multiple scattering with
//    thin layer approximation.
//
void TKalDetCradle::CalcQms(const TMaterial  &mat,
                            const TVTrack    &hel,
                                  Double_t    df,
                                  TKalMatrix &Qms,
                                  Double_t    mass) const
{
    Double_t cpa    = hel.GetKappa();
    Double_t tnl    = hel.GetTanLambda();
    Double_t tnl2   = tnl * tnl;
    Double_t tnl21  = 1. + tnl2;
    Double_t cpatnl = cpa * tnl;
    Double_t cslinv = TMath::Sqrt(tnl21);
    Double_t mom    = TMath::Abs(1. / cpa) * cslinv;
    Double_t   beta = mom / TMath::Sqrt(mom * mom + mass * mass);
    
    Double_t x0inv = 1. / mat.GetRadLength();  // radiation length inverse
    
    // *Calculate sigma_ms0 =============================================
    static const Double_t kMS1  = 0.0136;
    static const Double_t kMS12 = kMS1 * kMS1;
    static const Double_t kMS2  = 0.038;
    
    Double_t path = hel.IsInB()
    ? TMath::Abs(hel.GetRho()*df)*cslinv
    : TMath::Abs(df)*cslinv;
     
    Double_t xl   = path * x0inv;
    // ------------------------------------------------------------------
    // Very Crude Treatment!!
    Double_t tmp = 1. + kMS2 * TMath::Log(TMath::Max(1.e-4, xl));
    tmp /= (mom * beta);
    Double_t sgms2 = kMS12 * xl * tmp * tmp;
    // ------------------------------------------------------------------
    
    Qms(1,1) = sgms2 * tnl21;
    Qms(2,2) = sgms2 * cpatnl * cpatnl;
    Qms(2,4) = sgms2 * cpatnl * tnl21;
    Qms(4,2) = sgms2 * cpatnl * tnl21;
    Qms(4,4) = sgms2 * tnl21  * tnl21;
}
