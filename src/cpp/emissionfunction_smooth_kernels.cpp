#include <iostream>
#include <sstream>
#include <string>
#include <string.h>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <vector>
#include <stdio.h>
#include <random>
#include <algorithm>
#include <array>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "iS3D.h"
#include "readindata.h"
#include "emissionfunction.h"
#include "Stopwatch.h"
#include "arsenal.h"
#include "ParameterReader.h"
#include "deltafReader.h"
#include <gsl/gsl_sf_bessel.h> //for modified bessel functions
#include <gsl/gsl_linalg.h>
#include "gaussThermal.h"
#include "particle.h"


using namespace std;

double EmissionFunctionArray::correlationLength(double T, double muB){
    
    double pi = 3.14159265359;

    // some parameters in the parametrization
    
    double delta_T = 0.02;
    double delta_mu = 0.065;
    
    double A = 0.25;
    
    double Alpha = 4.6 * (pi / 180); // angle between h and r axes
    
    double xi_min = 1.0;
    double xi_max = 3.0;

    // critical exponent
    double nu = 2./3.;

    // rotation by angle Alpha
    double Tp  = (muB - MU_C) * sin(Alpha) + (T - T_C) * cos(Alpha);
    double mup = (muB - MU_C) * cos(Alpha) - (T - T_C) * sin(Alpha);

    // terms in the parameters
    double xi_ratio = xi_min / xi_max;
    double xi_ratio_nu = pow(xi_ratio, 2./nu);

    double mu2 = (mup / delta_mu) * (mup / delta_mu);
    double T2  = (Tp / delta_T) * (Tp / delta_T);

    double Tanh_term = tanh(mu2 + A * T2);
    double Br_term = Tanh_term * (1 - xi_ratio_nu) + xi_ratio_nu;

    return xi_min / pow(Br_term, nu/2);
    
}

void EmissionFunctionArray::calculate_dN_pTdpTdphidy(double *Mass, double *Sign, double *Degeneracy, double *Baryon,
  double *T_fo, double *P_fo, double *E_fo, double *tau_fo, double *eta_fo, double *ux_fo, double *uy_fo, double *un_fo,
  double *dat_fo, double *dax_fo, double *day_fo, double *dan_fo,
  double *pixx_fo, double *pixy_fo, double *pixn_fo, double *piyy_fo, double *piyn_fo, double *bulkPi_fo,
  double *muB_fo, double *nB_fo, double *Vx_fo, double *Vy_fo, double *Vn_fo, Deltaf_Data *df_data)
  {
    printf("computing thermal spectra from vhydro with df...\n\n");

    double prefactor = pow(2.0 * M_PI * hbarC, -3);   // prefactor of CFF

    long FO_chunk = FO_length / CORES;
    long remainder = FO_length  -  CORES * FO_chunk;

    cout << "Number of cores : " << CORES << endl;
    cout << "Chunk size = " << FO_chunk << endl;
    cout << "Remainder cells = " << remainder << endl;

    if(remainder != 0) FO_chunk++;

    // phi arrays
    double cosphiValues[phi_tab_length];
    double sinphiValues[phi_tab_length];

    for(long iphip = 0; iphip < phi_tab_length; iphip++)
    {
      double phi = phi_tab -> get(1, iphip + 1);
      cosphiValues[iphip] = cos(phi);
      sinphiValues[iphip] = sin(phi);
    }

    // pT array
    double pTValues[pT_tab_length];

    for(long ipT = 0; ipT < pT_tab_length; ipT++)
    {
      pTValues[ipT] = pT_tab -> get(1, ipT + 1);
    }

    // y and eta arrays
    double yValues[y_tab_length];
    double etaValues[eta_tab_length];
    double etaWeights[eta_tab_length];

    if(DIMENSION == 2)
    {
      yValues[0] = 0.0;

      for(long ieta = 0; ieta < eta_tab_length; ieta++)
      {
        etaValues[ieta] = eta_tab->get(1, ieta + 1);
        etaWeights[ieta] = eta_tab->get(2, ieta + 1);
      }
    }
    else if(DIMENSION == 3)
    {
      etaValues[0] = 0.0;     
      etaWeights[0] = 1.0; 
      for(long iy = 0; iy < y_tab_length; iy++)
      {
        yValues[iy] = y_tab->get(1, iy + 1);
      }
    }

    //declare a huge array of size CORES * npart * pT_tab_length * phi_tab_length * y_tab_length to hold spectra for each chunk
    double *dN_pTdpTdphidy_all = (double*)calloc(CORES * npart * pT_tab_length * phi_tab_length * y_tab_length, sizeof(double));
  
    // subdivide bite size chunks of freezeout surface across cores
    #pragma omp parallel for
    for(long n = 0; n < CORES; n++)
    {
      long endFO = FO_chunk;
      
      for(long icell = 0; icell < endFO; icell++)  // cell index inside each chunk
      {
      	if((icell == endFO - 1) && (remainder != 0) && (n > remainder - 1)) continue;

        long icell_glb = n  +  icell * CORES;

        double tau = tau_fo[icell_glb];         // longitudinal proper time
        double tau2 = tau * tau;
        if(DIMENSION == 3)
        {
          etaValues[0] = eta_fo[icell_glb];     // spacetime rapidity from surface file
        }

        double dat = dat_fo[icell_glb];         // covariant normal surface vector
        double dax = dax_fo[icell_glb];
        double day = day_fo[icell_glb];
        double dan = dan_fo[icell_glb];         // dan should be 0 for 2+1d

        double ux = ux_fo[icell_glb];           // contravariant fluid velocity
        double uy = uy_fo[icell_glb];           // enforce normalization
        double un = un_fo[icell_glb];
        double ux2 = ux * ux;             // useful expressions
        double uy2 = uy * uy;
        double utperp = sqrt(1.0 + ux2 + uy2);
        double tau2_un = tau2 * un;
        double ut = sqrt(utperp * utperp  +  tau2_un * un);
        double ut2 = ut * ut;

        // skip cells with u.dsigma < 0 
        if(ut * dat  +  ux * dax  +  uy * day  +  un * dan <= 0.0) continue; 

        double T = T_fo[icell_glb];             // temperature (GeV)
        double P = P_fo[icell_glb];             // equilibrium pressure (GeV/fm^3)
        double E = E_fo[icell_glb];             // energy density (GeV/fm^3)

        double pitt = 0.0;                      // contravariant shear stress tensor pi^munu (GeV/fm^3)
        double pitx = 0.0;                      // enforce orthogonality pi.u = 0
        double pity = 0.0;                      // and tracelessness Tr(pi) = 0
        double pitn = 0.0;
        double pixx = 0.0;
        double pixy = 0.0;
        double pixn = 0.0;
        double piyy = 0.0;
        double piyn = 0.0;
        double pinn = 0.0;

        if(INCLUDE_SHEAR_DELTAF)
        {
          pixx = pixx_fo[icell_glb];
          pixy = pixy_fo[icell_glb];
          pixn = pixn_fo[icell_glb];
          piyy = piyy_fo[icell_glb];
          piyn = piyn_fo[icell_glb];
          pinn = (pixx * (ux2 - ut2)  +  piyy * (uy2 - ut2)  +  2.0 * (pixy * ux * uy  +  tau2_un * (pixn * ux  +  piyn * uy))) / (tau2 * utperp * utperp);
          pitn = (pixn * ux  +  piyn * uy  +  tau2_un * pinn) / ut;
          pity = (pixy * ux  +  piyy * uy  +  tau2_un * piyn) / ut;
          pitx = (pixx * ux  +  pixy * uy  +  tau2_un * pixn) / ut;
          pitt = (pitx * ux  +  pity * uy  +  tau2_un * pitn) / ut;
        }

        double bulkPi = 0.0;                    // bulk pressure (GeV/fm^3)

        if(INCLUDE_BULK_DELTAF) bulkPi = bulkPi_fo[icell_glb];

        double muB = 0.0;                       // baryon chemical potential (GeV)
        double alphaB = 0.0;                    // muB / T
        double nB = 0.0;                        // net baryon density (fm^-3)
        double Vt = 0.0;                        // contravariant net baryon diffusion V^mu (fm^-3)
        double Vx = 0.0;                        // enforce orthogonality V.u = 0
        double Vy = 0.0;
        double Vn = 0.0;
        double baryon_enthalpy_ratio = 0.0;     // nB / (E + P)

        if(INCLUDE_BARYON && INCLUDE_BARYONDIFF_DELTAF)
        {
          muB = muB_fo[icell_glb];
          nB = nB_fo[icell_glb];
          Vx = Vx_fo[icell_glb];
          Vy = Vy_fo[icell_glb];
          Vn = Vn_fo[icell_glb];
          Vt = (Vx * ux  +  Vy * uy  +  Vn * tau2_un) / ut;

          alphaB = muB / T;
          baryon_enthalpy_ratio = nB / (E + P);
        }

        double tau2_pitn = tau2 * pitn;   // useful expressions
        double tau2_pixn = tau2 * pixn;
        double tau2_piyn = tau2 * piyn;
        double tau4_pinn = tau2 * tau2 * pinn;
        double tau2_Vn = tau2 * Vn;


        // set df coefficients
        deltaf_coefficients df = df_data->evaluate_df_coefficients(T, muB, E, P, bulkPi);

        double c0 = df.c0;             // 14 moment coefficients
        double c1 = df.c1;
        double c2 = df.c2;
        double c3 = df.c3;
        double c4 = df.c4;
        double shear14_coeff = df.shear14_coeff;

        double F = df.F;               // Chapman Enskog
        double G = df.G;
        double betabulk = df.betabulk;
        double betaV = df.betaV;
        double betapi = df.betapi;
          
        //printf("betaV=%lf\n",betaV);
                    
        //double corrL = df_data->correlationLength(T, muB);
        //printf("T=%lf\t mu=%lf\t xi=%lf\n",T,muB,corrL);
        if(CRITICAL){
            
            double corrL = correlationLength(T, muB);  
            betaV = betaV / corrL;
            
        }

        //printf("betaV_xi=%lf\n",betaV);

        // shear and bulk coefficients
        double shear_coeff = 0.0;
        double bulk0_coeff = 0.0;
        double bulk1_coeff = 0.0;
        double bulk2_coeff = 0.0;
        double diff0_coeff = 0.0;
        double diff1_coeff = 0.0;

        switch(DF_MODE)
        {
          case 1: // 14 moment
          {
            shear_coeff = 1.0 / shear14_coeff;
            bulk0_coeff = (c0 - c2) * bulkPi;
            bulk1_coeff = c1 * bulkPi;
            bulk2_coeff = (4.*c2 - c0) * bulkPi;
            diff0_coeff = c3;
            diff1_coeff = c4;
            break;
          }
          case 2: // Chapman enskog
          {
            shear_coeff = 0.5 / (betapi * T);
            bulk0_coeff = F / (T * T * betabulk) * bulkPi;
            bulk1_coeff = G / betabulk * bulkPi;
            bulk2_coeff = bulkPi / (3.0 * T * betabulk);
            diff0_coeff = baryon_enthalpy_ratio / betaV;
            diff1_coeff = 1.0 / betaV;
            break;
          }
          default:
          {
            printf("Error: set df_mode = (1,2) in parameters.dat\n");
          }
        }


        // now loop over all particle species and momenta
        for(long ipart = 0; ipart < npart; ipart++)
        {
          long iS0D = pT_tab_length * ipart;

          double mass = Mass[ipart];              // mass (GeV)
          double mass_squared = mass * mass;
          double sign = Sign[ipart];              // quantum statistics sign
            
          if(BOLTZMANN)
          {
              sign = 0.0;
          }
            
          double degeneracy = Degeneracy[ipart];  // spin degeneracy
          double baryon = Baryon[ipart];          // baryon number
          double chem = baryon * alphaB;          // chemical potential term in feq

          double weight1 = prefactor * degeneracy;

          for(long ipT = 0; ipT < pT_tab_length; ipT++)
          {
            long iS1D =  phi_tab_length * (ipT + iS0D);

            double pT = pTValues[ipT];              // p_T (GeV)
            double mT = sqrt(mass_squared  +  pT * pT);    // m_T (GeV)
            double mT_over_tau = mT / tau;

            for(long iphip = 0; iphip < phi_tab_length; iphip++)
            {
              long iS2D = y_tab_length * (iphip + iS1D);

              double px = pT * cosphiValues[iphip]; // p^x
              double py = pT * sinphiValues[iphip]; // p^y

              double px_dax = px * dax;   // useful expressions
              double py_day = py * day;

              double px_ux = px * ux;
              double py_uy = py * uy;

              double pixx_px_px = pixx * px * px;
              double piyy_py_py = piyy * py * py;
              double pitx_px = pitx * px;
              double pity_py = pity * py;
              double pixy_px_py = pixy * px * py;
              double tau2_pixn_px = tau2_pixn * px;
              double tau2_piyn_py = tau2_piyn * py;

              double Vx_px = Vx * px;
              double Vy_py = Vy * py;

              for(long iy = 0; iy < y_tab_length; iy++)
              {
                long iS3D = iy + iS2D;

                double y = yValues[iy];

                double eta_integral = 0.0;

                // sum over eta
                for(long ieta = 0; ieta < eta_tab_length; ieta++)
                {
                  double eta = etaValues[ieta];
                  double eta_weight = etaWeights[ieta];

                  double sinhyeta = sinh(y - eta);
                  double coshyeta = sqrt(1.0  +  sinhyeta * sinhyeta);

                  double pt = mT * coshyeta;           // p^tau
                  double pn = mT_over_tau * sinhyeta;  // p^eta

                  double pdotdsigma = pt * dat  +  px_dax  +  py_day  +  pn * dan;

                  if(OUTFLOW && pdotdsigma <= 0.0) continue;  // enforce outflow

                  double pdotu = pt * ut  -  px_ux  -  py_uy  -  pn * tau2_un;  // u.p
                  double feq = 1.0 / (exp(pdotu/T  -  chem) + sign);
                  
                  double feqbar = 1.0  -  sign * feq;

                  // pi^munu.p_mu.p_nu
                  double pimunu_pmu_pnu = pitt * pt * pt  +  pixx_px_px  +  piyy_py_py  +  tau4_pinn * pn * pn
                      + 2.0 * (-(pitx_px + pity_py) * pt  +  pixy_px_py  +  pn * (tau2_pixn_px  +  tau2_piyn_py  -  tau2_pitn * pt));

                  // V^mu.p_mu
                  double Vmu_pmu = Vt * pt  -  Vx_px  -  Vy_py  -  tau2_Vn * pn;

                  double df;

                  switch(DF_MODE)
                  {
                    case 1: // 14 moment
                    {
                      double df_shear = shear_coeff * pimunu_pmu_pnu;
                      double df_bulk = bulk0_coeff * mass_squared  +  (bulk1_coeff * baryon  +  bulk2_coeff * pdotu) * pdotu;
                      double df_diff = (diff0_coeff * baryon +  diff1_coeff * pdotu) * Vmu_pmu;

                      df = feqbar * (df_shear + df_bulk + df_diff);
                      break;
                    }
                    case 2: // Chapman enskog
                    {
                      double df_shear = shear_coeff * pimunu_pmu_pnu / pdotu;
                      double df_bulk = bulk0_coeff * pdotu  +  bulk1_coeff * baryon  +  bulk2_coeff * (pdotu  -  mass_squared / pdotu);
                      double df_diff = (diff0_coeff  -  diff1_coeff * baryon / pdotu) * Vmu_pmu;

                      df = feqbar * (df_shear + df_bulk + df_diff);
                      break;
                    }
                    default:
                    {
                      printf("Error: set df_mode = (1,2) in parameters.dat\n");
                    }
                  } // DF_MODE

                  if(REGULATE_DELTAF) df = max(-1.0, min(df, 1.0));

                  double f = feq * (1.0 + df);
                  
                  eta_integral += eta_weight * pdotdsigma * f;

                } // ieta

                dN_pTdpTdphidy_all[n  +  CORES * iS3D] += weight1 * eta_integral;

              } // rapidity points (iy)

            } // azimuthal angle points (iphip)

          } // transverse momentum points (ipT)

        } // particle species (ipart)

      } // freezeout cells in the chunk (icell)

    } // number of chunks / cores (n)


    

    // now perform the reduction over cores
    #pragma omp parallel for collapse(4)
    for(long ipart = 0; ipart < npart; ipart++)
    {
      for(long ipT = 0; ipT < pT_tab_length; ipT++)
      {
        for(long iphip = 0; iphip < phi_tab_length; iphip++)
        {
          for(long iy = 0; iy < y_tab_length; iy++)
          {
            long iS3D = iy  +  y_tab_length * (iphip  +  phi_tab_length * (ipT  +  pT_tab_length * ipart));

            double dN_pTdpTdphidy_tmp = 0.0; // reduction variable

            #pragma omp simd reduction(+:dN_pTdpTdphidy_tmp)
            for(long n = 0; n < CORES; n++)
            {
              dN_pTdpTdphidy_tmp += dN_pTdpTdphidy_all[n  +  CORES * iS3D];

            } // sum over the cores

            dN_pTdpTdphidy[iS3D] = dN_pTdpTdphidy_tmp; 

          } // rapidity points (iy)

        } // azimuthal angle points (iphip)

      } // transverse momentum points (ipT)

    } // particle species (ipart)



    // free memory
    free(dN_pTdpTdphidy_all);
  }


  void EmissionFunctionArray::calculate_dN_ptdptdphidy_feqmod(double *Mass, double *Sign, double *Degeneracy, double *Baryon, double *T_fo, double *P_fo, double *E_fo, double *tau_fo, double *eta_fo, double *ux_fo, double *uy_fo, double *un_fo, double *dat_fo, double *dax_fo, double *day_fo, double *dan_fo, double *pixx_fo, double *pixy_fo, double *pixn_fo, double *piyy_fo, double *piyn_fo, double *bulkPi_fo, double *muB_fo, double *nB_fo, double *Vx_fo, double *Vy_fo, double *Vn_fo, Gauss_Laguerre * laguerre, Deltaf_Data * df_data)
  {
    printf("computing thermal spectra from vhydro with feqmod...\n\n");

    double prefactor = pow(2.0 * M_PI * hbarC, -3);

    long FO_chunk = FO_length / CORES;
    long remainder = FO_length  -  CORES * FO_chunk;

    cout << "Number of cores = " << CORES << endl;
    cout << "Chunk size = " << FO_chunk << endl;
    cout << "Remainder cells = " << remainder << endl;

    if(remainder != 0) FO_chunk++;

    double detA_min = DETA_MIN;   // default value for minimum detA
    //long breakdown = 0;           // # times feqmod breaks down

    // phi arrays
    double cosphiValues[phi_tab_length];
    double sinphiValues[phi_tab_length];
    double phiWeights[phi_tab_length];

    for(long iphip = 0; iphip < phi_tab_length; iphip++)
    {
      double phi = phi_tab->get(1, iphip + 1);
      cosphiValues[iphip] = cos(phi);
      sinphiValues[iphip] = sin(phi);
      phiWeights[iphip] = phi_tab->get(2, iphip + 1);
    }

    // pT array
    double pTValues[pT_tab_length];

    for(long ipT = 0; ipT < pT_tab_length; ipT++)
    {
      pTValues[ipT] = pT_tab->get(1, ipT + 1);
    }

    double yValues[y_tab_length];
    double etaValues[eta_tab_length];
    double etaWeights[eta_tab_length];

    if(DIMENSION == 2)
    {
      yValues[0] = 0.0;

      for(long ieta = 0; ieta < eta_tab_length; ieta++)
      {
        etaValues[ieta] = eta_tab->get(1, ieta + 1);
        etaWeights[ieta] = eta_tab->get(2, ieta + 1);
      }
    }
    else if(DIMENSION == 3)
    {
      etaValues[0] = 0.0;          
      etaWeights[0] = 1.0; 
      for(long iy = 0; iy < y_tab_length; iy++)
      {
        yValues[iy] = y_tab->get(1, iy + 1);
      }
    }

    /// gauss laguerre roots
    const int pbar_pts = laguerre->points;

    double * pbar_root1 = laguerre->root[1];
    double * pbar_root2 = laguerre->root[2];

    double * pbar_weight1 = laguerre->weight[1];
    double * pbar_weight2 = laguerre->weight[2];

    //declare a huge array of size CORES * npart * pT_tab_length * phi_tab_length * y_tab_length to hold spectra for each chunk
    double *dN_pTdpTdphidy_all = (double*)calloc(CORES * npart * pT_tab_length * phi_tab_length * y_tab_length, sizeof(double));


    // subdivide bite size chunks of freezeout surface across cores
    #pragma omp parallel for
    for(long n = 0; n < CORES; n++)
    {
      double ** A_copy = (double**)calloc(3, sizeof(double*));
      for(int i = 0; i < 3; i++) A_copy[i] = (double*)calloc(3, sizeof(double));

      double ** A_inv = (double**)calloc(3, sizeof(double*));
      for(int i = 0; i < 3; i++) A_inv[i] = (double*)calloc(3, sizeof(double));

      long endFO = FO_chunk;
      
      for(long icell = 0; icell < endFO; icell++)  // cell index inside each chunk
      {
        if((icell == endFO - 1) && (remainder != 0) && (n > remainder - 1)) continue;

        long icell_glb = n  +  icell * CORES;

        double tau = tau_fo[icell_glb];     // longitudinal proper time
        double tau2 = tau * tau;
        if(DIMENSION == 3)
        {
          etaValues[0] = eta_fo[icell_glb]; // spacetime rapidity from freezeout cell
        }

        double dat = dat_fo[icell_glb];     // covariant normal surface vector
        double dax = dax_fo[icell_glb];
        double day = day_fo[icell_glb];
        double dan = dan_fo[icell_glb];

        double ux = ux_fo[icell_glb];       // contravariant fluid velocity
        double uy = uy_fo[icell_glb];       // enforce normalization u.u = 1
        double un = un_fo[icell_glb];
        double ut = sqrt(1.0 +  ux * ux  +  uy * uy  +  tau2 * un * un);

        // skip cells with u.dsigma < 0
        if(ut * dat  +  ux * dax  +  uy * day  +  un * dan <= 0.0) continue;       

        double ut2 = ut * ut;               // useful expressions
        double ux2 = ux * ux;
        double uy2 = uy * uy;
        double uperp = sqrt(ux * ux  +  uy * uy);
        double utperp = sqrt(1.0  +   ux * ux  +  uy * uy);

        double T = T_fo[icell_glb];             // temperature (GeV)
        double P = P_fo[icell_glb];             // equilibrium pressure (GeV/fm^3)
        double E = E_fo[icell_glb];             // energy density (GeV/fm^3)

        double pitt = 0.0;                  // contravariant shear stress tensor pi^munu
        double pitx = 0.0;                  // enforce orthogonality pi.u = 0
        double pity = 0.0;                  // and tracelessness Tr(pi) = 0
        double pitn = 0.0;
        double pixx = 0.0;
        double pixy = 0.0;
        double pixn = 0.0;
        double piyy = 0.0;
        double piyn = 0.0;
        double pinn = 0.0;

        if(INCLUDE_SHEAR_DELTAF)
        {
          pixx = pixx_fo[icell_glb];
          pixy = pixy_fo[icell_glb];
          pixn = pixn_fo[icell_glb];
          piyy = piyy_fo[icell_glb];
          piyn = piyn_fo[icell_glb];
          pinn = (pixx * (ux2 - ut2)  +  piyy * (uy2 - ut2)  +  2.0 * (pixy * ux * uy  +  tau2 * un * (pixn * ux  +  piyn * uy))) / (tau2 * utperp * utperp);
          pitn = (pixn * ux  +  piyn * uy  +  tau2 * pinn * un) / ut;
          pity = (pixy * ux  +  piyy * uy  +  tau2 * piyn * un) / ut;
          pitx = (pixx * ux  +  pixy * uy  +  tau2 * pixn * un) / ut;
          pitt = (pitx * ux  +  pity * uy  +  tau2 * pitn * un) / ut;
        }

        double bulkPi = 0.0;                // bulk pressure (GeV/fm^3)

        if(INCLUDE_BULK_DELTAF) bulkPi = bulkPi_fo[icell_glb];

        double muB = 0.0;                       // baryon chemical potential (GeV)
        double alphaB = 0.0;                    // muB / T
        double nB = 0.0;                        // net baryon density (fm^-3)
        double Vt = 0.0;                        // contravariant net baryon diffusion V^mu (fm^-3)
        double Vx = 0.0;                        // enforce orthogonality V.u = 0
        double Vy = 0.0;
        double Vn = 0.0;
        double baryon_enthalpy_ratio = 0.0;     // nB / (E + P)

        if(INCLUDE_BARYON && INCLUDE_BARYONDIFF_DELTAF)
        {
          muB = muB_fo[icell_glb];
          nB = nB_fo[icell_glb];
          Vx = Vx_fo[icell_glb];
          Vy = Vy_fo[icell_glb];
          Vn = Vn_fo[icell_glb];
          Vt = (Vx * ux  +  Vy * uy  +  tau2 * Vn * un) / ut;

          alphaB = muB / T;
          baryon_enthalpy_ratio = nB / (E + P);
        }

        // regulate bulk pressure if goes out of bounds given
        // by Jonah's feqmod to avoid gsl interpolation errors
        if(DF_MODE == 4)
        {
          double bulkPi_over_Peq_max = df_data->bulkPi_over_Peq_max;

          if(bulkPi < - P) bulkPi = - (1.0 - 1.e-5) * P;
          else if(bulkPi / P > bulkPi_over_Peq_max) bulkPi = P * (bulkPi_over_Peq_max - 1.e-5);
        }

        // set df coefficients
        deltaf_coefficients df = df_data->evaluate_df_coefficients(T, muB, E, P, bulkPi);

        // modified coefficients (Mike / Jonah)
        double F = df.F;
        double G = df.G;
        double betabulk = df.betabulk;
        double betaV = df.betaV;
        double betapi = df.betapi;
        double lambda = df.lambda;
        double z = df.z;
        double delta_lambda = df.delta_lambda;
        double delta_z = df.delta_z;

        // milne basis class
        Milne_Basis basis_vectors(ut, ux, uy, un, uperp, utperp, tau);
        basis_vectors.test_orthonormality(tau2);

        double Xt = basis_vectors.Xt;   double Yx = basis_vectors.Yx;
        double Xx = basis_vectors.Xx;   double Yy = basis_vectors.Yy;
        double Xy = basis_vectors.Xy;   double Zt = basis_vectors.Zt;
        double Xn = basis_vectors.Xn;   double Zn = basis_vectors.Zn;

        // shear stress class
        Shear_Stress pimunu(pitt, pitx, pity, pitn, pixx, pixy, pixn, piyy, piyn, pinn);
        pimunu.test_pimunu_orthogonality_and_tracelessness(ut, ux, uy, un, tau2);
        pimunu.boost_pimunu_to_lrf(basis_vectors, tau2);

        // baryon diffusion class
        Baryon_Diffusion Vmu(Vt, Vx, Vy, Vn);
        Vmu.test_Vmu_orthogonality(ut, ux, uy, un, tau2);
        Vmu.boost_Vmu_to_lrf(basis_vectors, tau2);


        // modified temperature / chemical potential
        double T_mod = T;
        double alphaB_mod = alphaB;

        if(DF_MODE == 3)
        {
          T_mod = T  +  bulkPi * F / betabulk;
          alphaB_mod = alphaB  +  bulkPi * G / betabulk;
        }

        // linearized Chapman Enskog df coefficients (for Mike only)
        double shear_coeff = 0.5 / (betapi * T);      // Jonah linear df also shares shear coeff
        double bulk0_coeff = F / (T * T * betabulk);
        double bulk1_coeff = G / betabulk;
        double bulk2_coeff = 1.0 / (3.0 * T * betabulk);

        // pimunu and Vmu LRF components
        double pixx_LRF = pimunu.pixx_LRF;
        double pixy_LRF = pimunu.pixy_LRF;
        double pixz_LRF = pimunu.pixz_LRF;
        double piyy_LRF = pimunu.piyy_LRF;
        double piyz_LRF = pimunu.piyz_LRF;
        double pizz_LRF = pimunu.pizz_LRF;

        double Vx_LRF = Vmu.Vx_LRF;
        double Vy_LRF = Vmu.Vy_LRF;
        double Vz_LRF = Vmu.Vz_LRF;

        // local momentum transformation matrix Mij = Aij
        // Aij = ideal + shear + bulk is symmetric
        // Mij is not symmetric if include baryon diffusion (leave for future work)

        // coefficients in Aij
        double shear_mod = 0.5 / betapi;
        double bulk_mod = bulkPi / (3.0 * betabulk);

        if(DF_MODE == 4) bulk_mod = lambda;

        double Axx = 1.0  +  pixx_LRF * shear_mod  +  bulk_mod;
        double Axy = pixy_LRF * shear_mod;
        double Axz = pixz_LRF * shear_mod;
        double Ayx = Axy;
        double Ayy = 1.0  +  piyy_LRF * shear_mod  +  bulk_mod;
        double Ayz = piyz_LRF * shear_mod;
        double Azx = Axz;
        double Azy = Ayz;
        double Azz = 1.0  +  pizz_LRF * shear_mod  +  bulk_mod;

        double detA = Axx * (Ayy * Azz  -  Ayz * Ayz)  -  Axy * (Axy * Azz  -  Ayz * Axz)  +  Axz * (Axy * Ayz  -  Ayy * Axz);
        double detA_bulk_two_thirds = pow(1.0 + bulk_mod, 2);
        
        // set Mij matrix
        double A[] = {Axx, Axy, Axz,
                      Ayx, Ayy, Ayz,
                      Azx, Azy, Azz};           // gsl matrix format

        A_copy[0][0] = Axx;  A_copy[0][1] = Axy;  A_copy[0][2] = Axz;
        A_copy[1][0] = Ayx;  A_copy[1][1] = Ayy;  A_copy[1][2] = Ayz;
        A_copy[2][0] = Azx;  A_copy[2][1] = Azy;  A_copy[2][2] = Azz;

        int s;
        gsl_matrix_view M = gsl_matrix_view_array(A, 3, 3);
        gsl_matrix_view LU = gsl_matrix_view_array(A, 3, 3);

        gsl_permutation * p = gsl_permutation_calloc(3);

        gsl_linalg_LU_decomp(&LU.matrix, p, &s);

        gsl_matrix * A_inverse = gsl_matrix_alloc(3,3);

        gsl_linalg_LU_invert(&LU.matrix, p, A_inverse);

        for(int i = 0; i < 3; i++)
        {
          for(int j = 0; j < 3; j++)
          {
            A_inv[i][j] = gsl_matrix_get(A_inverse, i, j);
          }
        }

         // prefactors for equilibrium, linear bulk correction and modified densities (Mike's feqmod)
        double neq_fact = T * T * T / two_pi2_hbarC3;
        double dn_fact = bulkPi / betabulk;
        double J20_fact = T * neq_fact;
        double N10_fact = neq_fact;
        double nmod_fact = T_mod * T_mod * T_mod / two_pi2_hbarC3;

        // determine if feqmod breaks down
        bool feqmod_breaks_down = does_feqmod_breakdown(MASS_PION0, T, F, bulkPi, betabulk, detA, detA_min, z, laguerre, DF_MODE, 0, T, F, betabulk);

        // if(feqmod_breaks_down)
        // {
        //   #pragma omp critical
        //   {
        //     breakdown++;
        //     cout << setw(5) << setprecision(4) << "feqmod breaks down at " << breakdown << " / " << FO_length << " cell at tau = " << tau << " fm/c:" << "\t detA = " << detA << "\t detA_min = " << detA_min << endl;
        //   }
        // }

        // uniformly rescale eta space by detA if modified momentum space elements are shrunk
        // this rescales the dsigma components orthogonal to the eta direction (only works for 2+1d, y = 0)
        // for integrating modified distribution with narrow (y-eta) distributions
        double eta_scale = 1.0;
        if(detA > detA_min && DIMENSION == 2)
        {
          eta_scale = detA / detA_bulk_two_thirds;
        }

        // loop over hadrons
        for(long ipart = 0; ipart < npart; ipart++)
        {
          long iS0D = pT_tab_length * ipart;
          
          // set particle properties
          double mass = Mass[ipart];              // mass (GeV)
          double mass2 = mass * mass;
          double sign = Sign[ipart];              // quantum statistics sign
          double degeneracy = Degeneracy[ipart];  // spin degeneracy
          double baryon = Baryon[ipart];          // baryon number

          double chem = baryon * alphaB;          // chemical potential term in feq
          double chem_mod = baryon * alphaB_mod;  // chemical potential term in feqmod

          double weight1 = prefactor * degeneracy;

          // modified renormalization factor
          double renorm = 1.0;

          if(INCLUDE_BULK_DELTAF)
          {
            if(DF_MODE == 3)
            {
              double mbar = mass / T;
              double mbar_mod = mass / T_mod;

              double neq = neq_fact * degeneracy * GaussThermal(neq_int, pbar_root1, pbar_weight1, pbar_pts, mbar, alphaB, baryon, sign);

              double N10 = baryon * N10_fact * degeneracy * GaussThermal(J10_int, pbar_root1, pbar_weight1, pbar_pts, mbar, alphaB, baryon, sign);

              double J20 = J20_fact * degeneracy * GaussThermal(J20_int, pbar_root2, pbar_weight2, pbar_pts, mbar, alphaB, baryon, sign);

              double n_linear = neq  +  dn_fact * (neq  +  N10 * G  +  J20 * F / T / T);

              double n_mod = nmod_fact * degeneracy * GaussThermal(neq_int, pbar_root1, pbar_weight1, pbar_pts, mbar_mod, alphaB_mod, baryon, sign);

              renorm = n_linear / n_mod;
            }
            else if(DF_MODE == 4)
            {
              renorm = z;
            }

          }

          if(DIMENSION == 2)
          {
            renorm /= detA_bulk_two_thirds;
          }
          else if(DIMENSION == 3)
          {
            renorm /= detA;
          }

          if((std::isnan(renorm) || std::isinf(renorm)))
          {
            cout << "Error: renormalization factor is " << renorm << endl;
            continue;
          }

          for(long ipT = 0; ipT < pT_tab_length; ipT++)
          {
            long iS1D =  phi_tab_length * (ipT + iS0D);
          
            double pT = pTValues[ipT];              // p_T (GeV)
            double mT = sqrt(mass2  +  pT * pT);    // m_T (GeV)
            double mT_over_tau = mT / tau;

            for(long iphip = 0; iphip < phi_tab_length; iphip++)
            {
              long iS2D = y_tab_length * (iphip + iS1D);
          
              double px = pT * cosphiValues[iphip]; // p^x
              double py = pT * sinphiValues[iphip]; // p^y

              for(long iy = 0; iy < y_tab_length; iy++)
              {
                long iS3D = iy + iS2D;

                double y = yValues[iy];

                double eta_integral = 0.0;  // Cooper Frye integral over eta

                // integrate over eta
                for(long ieta = 0; ieta < eta_tab_length; ieta++)
                {
                  double eta = etaValues[ieta];
                  double eta_weight = etaWeights[ieta];

                  bool feqmod_breaks_down_narrow = false;

                  if(DIMENSION == 3 && !feqmod_breaks_down)
                  {
                    if(detA < 0.01 && fabs(y - eta) < detA)
                    {
                      feqmod_breaks_down_narrow = true;
                    }
                  }

                  double pdotdsigma;
                  double f;           // feqmod (if breakdown do feq(1+df))

                  // calculate feqmod
                  if(feqmod_breaks_down || feqmod_breaks_down_narrow)
                  {
                    double pt = mT * cosh(y - eta);          // p^\tau (GeV)
                    double pn = mT_over_tau * sinh(y - eta); // p^\eta (GeV^2)
                    double tau2_pn = tau2 * pn;

                    pdotdsigma = eta_weight * (pt * dat  +  px * dax  +  py * day)  +  pn * dan;

                    //if(OUTFLOW && pdotdsigma <= 0.0) continue;  // enforce outflow

                    if(DF_MODE == 3)
                    {
                      double pdotu = pt * ut  -  px * ux  -  py * uy  -  tau2_pn * un;
                      double feq = 1.0 / (exp(pdotu / T  -  chem) + sign);
                      double feqbar = 1.0  -  sign * feq;

                       // pi^munu.p_mu.p_nu
                      double pimunu_pmu_pnu = pitt * pt * pt  +  pixx * px * px  +  piyy * py * py  +  pinn * tau2_pn * tau2_pn
                       + 2.0 * (-(pitx * px  +  pity * py) * pt  +  pixy * px * py  +  tau2_pn * (pixn * px  +  piyn * py  -  pitn * pt));

                      // V^mu.p_mu
                      double Vmu_pmu = Vt * pt  -  Vx * px  -  Vy * py  -  Vn * tau2_pn;

                      double df_shear = shear_coeff * pimunu_pmu_pnu / pdotu;
                      double df_bulk = (bulk0_coeff * pdotu  +  bulk1_coeff * baryon +  bulk2_coeff * (pdotu  -  mass2 / pdotu)) * bulkPi;
                      double df_diff = (baryon_enthalpy_ratio  -  baryon / pdotu) * Vmu_pmu / betaV;

                      double df = feqbar * (df_shear + df_bulk + df_diff);

                      if(REGULATE_DELTAF) df = max(-1.0, min(df, 1.0)); // regulate df

                      f = feq * (1.0 + df);
                    }
                    else if(DF_MODE == 4)
                    {
                      double pdotu = pt * ut  -  px * ux  -  py * uy  -  tau2_pn * un;
                      double feq = 1.0 / (exp(pdotu / T) + sign);
                      double feqbar = 1.0  -  sign * feq;

                       // pi^munu.p_mu.p_nu
                      double pimunu_pmu_pnu = pitt * pt * pt  +  pixx * px * px  +  piyy * py * py  +  pinn * tau2_pn * tau2_pn
                       + 2.0 * (-(pitx * px  +  pity * py) * pt  +  pixy * px * py  +  tau2_pn * (pixn * px  +  piyn * py  -  pitn * pt));

                      double df_shear = feqbar * shear_coeff * pimunu_pmu_pnu / pdotu;
                      double df_bulk = delta_z  -  3.0 * delta_lambda  +  feqbar * delta_lambda * (pdotu  -  mass2 / pdotu) / T;

                      double df = df_shear + df_bulk;

                      if(REGULATE_DELTAF) df = max(-1.0, min(df, 1.0)); // regulate df

                      f = feq * (1.0 + df);
                    }
                  } // feqmod breaks down
                  else
                  {
                    double pt = mT * cosh(y - eta_scale * eta);          // p^\tau (GeV)
                    double pn = mT_over_tau * sinh(y - eta_scale * eta); // p^\eta (GeV^2)
                    double tau2_pn = tau2 * pn;

                    pdotdsigma = eta_weight * (pt * dat  +  px * dax  +  py * day)  +  pn * dan;

                    //if(OUTFLOW && pdotdsigma <= 0.0) continue;  // enforce outflow

                    // LRF momentum components pi_LRF = - Xi.p
                    double px_LRF = -Xt * pt  +  Xx * px  +  Xy * py  +  Xn * tau2_pn;
                    double py_LRF = Yx * px  +  Yy * py;
                    double pz_LRF = -Zt * pt  +  Zn * tau2_pn;

                    double pLRF[3] = {px_LRF, py_LRF, pz_LRF};
                    double pLRF_prev[3];

                    double pLRF_mod_prev[3];
                    double pLRF_mod[3];

                    double dpLRF[3];
                    double dpLRF_mod[3];

                    matrix_multiplication(A_inv, pLRF, pLRF_mod, 3, 3);   // evaluate p_mod = A^-1.p at least once

                    double dp;
                    double eps = 1.e-16;

                    for(int i = 0; i < 5; i++)
                    {
                      vector_copy(pLRF_mod, pLRF_mod_prev, 3);                        // copy result for iteration
                      matrix_multiplication(A_copy, pLRF_mod_prev, pLRF_prev, 3, 3);  // compute pLRF error
                      vector_subtraction(pLRF, pLRF_prev, dpLRF, 3);

                      dp = sqrt(dpLRF[0] * dpLRF[0]  +  dpLRF[1] * dpLRF[1]  +  dpLRF[2] * dpLRF[2]);

                      if(dp <= eps) break;

                      matrix_multiplication(A_inv, dpLRF, dpLRF_mod, 3, 3);           // compute correction to pLRF_mod
                      vector_addition(pLRF_mod_prev, dpLRF_mod, pLRF_mod, 3);         // add correction to pLRF_mod
                    }

                    double px_LRF_mod = pLRF_mod[0];
                    double py_LRF_mod = pLRF_mod[1];
                    double pz_LRF_mod = pLRF_mod[2];

                    double E_mod = sqrt(mass2  +  px_LRF_mod * px_LRF_mod  +  py_LRF_mod * py_LRF_mod  +  pz_LRF_mod * pz_LRF_mod);

                    f = fabs(renorm) / (exp(E_mod / T_mod  -  chem_mod) + sign); // feqmod
                  }

                  if(OUTFLOW && pdotdsigma <= 0.0) continue;  // enforce outflow

                  eta_integral += (pdotdsigma * f); // add contribution to integral

                } // eta points (ieta)
                
                dN_pTdpTdphidy_all[n  +  CORES * iS3D] += weight1 * eta_integral;

              } // rapidity points (iy)

            } // azimuthal angle points (iphip)

          } // transverse momentum points (ipT)

        } // particle species (ipart)

        gsl_matrix_free(A_inverse);
        gsl_permutation_free(p);

      } // freezeout cells in the chunk (icell)

      free_2D(A_copy, 3);
      free_2D(A_inv, 3);

    } // number of chunks / cores (n)


    // now perform the reduction over cores
    #pragma omp parallel for collapse(4)
    for(long ipart = 0; ipart < npart; ipart++)
    {
      for(long ipT = 0; ipT < pT_tab_length; ipT++)
      {
        for(long iphip = 0; iphip < phi_tab_length; iphip++)
        {
          for(long iy = 0; iy < y_tab_length; iy++)
          {
            long iS3D = iy  +  y_tab_length * (iphip  +  phi_tab_length * (ipT  +  pT_tab_length * ipart));

            double dN_pTdpTdphidy_tmp = 0.0; // reduction variable

            #pragma omp simd reduction(+:dN_pTdpTdphidy_tmp)
            for(long n = 0; n < CORES; n++)
            {
              dN_pTdpTdphidy_tmp += dN_pTdpTdphidy_all[n  +  CORES * iS3D];

            } // sum over the cores

            dN_pTdpTdphidy[iS3D] = dN_pTdpTdphidy_tmp; 

          } // rapidity points (iy)

        } // azimuthal angle points (iphip)

      } // transverse momentum points (ipT)

    } // particle species (ipart)


    //free memory
    free(dN_pTdpTdphidy_all);
  }



  void EmissionFunctionArray::calculate_dN_dX(long *MCID, double *Mass, double *Sign, double *Degeneracy, double *Baryon,
  double *T_fo, double *P_fo, double *E_fo, double *tau_fo, double *x_fo, double *y_fo, double *eta_fo, double *ux_fo, double *uy_fo, double *un_fo,
  double *dat_fo, double *dax_fo, double *day_fo, double *dan_fo,
  double *pixx_fo, double *pixy_fo, double *pixn_fo, double *piyy_fo, double *piyn_fo, double *bulkPi_fo,
  double *muB_fo, double *nB_fo, double *Vx_fo, double *Vy_fo, double *Vn_fo, Deltaf_Data *df_data)
  {
    printf("computing thermal spacetime distribution from vhydro with df...\n\n");

    // dX = tau.dtau.deta, 2.pi.r.dr.deta or 2.pi.tau.r.dtau.dr.deta
    // only have boost invariance in mind right now so
    // deta = dy and only need to integrate over (pT,phi)

    double prefactor = pow(2.0 * M_PI * hbarC, -3);   // prefactor of CFF
  
    long FO_chunk = FO_length / CORES;
    long remainder = FO_length  -  CORES * FO_chunk;

    //cout << "Max number of threads = " << omp_get_max_threads() << endl;
    cout << "Number of threads : " << CORES << endl;
    cout << "Chunk size = " << FO_chunk << endl;
    cout << "Remainder cells = " << remainder << endl;

    if(remainder != 0) FO_chunk++;

    // phi arrays
    double cosphiValues[phi_tab_length];
    double sinphiValues[phi_tab_length];
    double phiWeights[phi_tab_length];

    for(long iphip = 0; iphip < phi_tab_length; iphip++)
    {
      double phi = phi_tab->get(1, iphip + 1);
      cosphiValues[iphip] = cos(phi);
      sinphiValues[iphip] = sin(phi);

      phiWeights[iphip] = phi_tab->get(2, iphip + 1);
    }

    // pT array
    double pTValues[pT_tab_length];
    double pTWeights[pT_tab_length];

    for(long ipT = 0; ipT < pT_tab_length; ipT++)
    {
      pTValues[ipT] = pT_tab->get(1, ipT + 1);
      pTWeights[ipT] = pT_tab->get(2, ipT + 1);
    }

    // y_minus_eta array
    double y_minus_eta_Values[y_minus_eta_tab_length];
    double y_minus_eta_Weights[y_minus_eta_tab_length];

    for(long iyeta = 0; iyeta < y_minus_eta_tab_length; iyeta++)
    {
      y_minus_eta_Values[iyeta] = eta_tab->get(1, iyeta + 1);
      y_minus_eta_Weights[iyeta] = eta_tab->get(2, iyeta + 1);
    }


    double *dN_dX_all =  (double*)calloc(CORES * spacetime_length, sizeof(double));


    #pragma omp parallel for
    for(long n = 0; n < CORES; n++)
    {
      long endFO = FO_chunk;

      for(long icell = 0; icell < endFO; icell++)  // cell index inside each chunk
      {
	      if((icell == endFO - 1) && (remainder != 0) && (n > remainder - 1)) continue;

	      long icell_glb = n  +  icell * CORES;


      
        double tau = tau_fo[icell_glb];         // longitudinal proper time
        double x = x_fo[icell_glb];             // x position
        double y = y_fo[icell_glb];             // y position

        double r = sqrt(x * x  +  y * y);
        double phi = atan2(y, x);
        if(phi < 0.0) phi += two_pi;

        double tau2 = tau * tau;
        double eta = 0.0;
        if(DIMENSION == 3)
        {
          eta = eta_fo[icell_glb];              // spacetime rapidity from surface file
        }

        // spacetime bin index
        long itau = (long)floor((tau - TAU_MIN) / TAU_WIDTH);
        long ir = (long)floor((r - R_MIN) / R_WIDTH);
        long iphi = (long)floor(phi / PHIP_WIDTH);
        long ieta = 0;
        if(DIMENSION == 3)
        { 
          ieta = (long)floor((eta + ETA_CUT) / ETA_WIDTH); 
        }

        double dat = dat_fo[icell_glb];         // covariant normal surface vector
        double dax = dax_fo[icell_glb];
        double day = day_fo[icell_glb];
        double dan = dan_fo[icell_glb];         // dan should be 0 for 2+1d

        double ux = ux_fo[icell_glb];           // contravariant fluid velocity
        double uy = uy_fo[icell_glb];           // enforce normalization
        double un = un_fo[icell_glb];
        double ux2 = ux * ux;                   // useful expressions
        double uy2 = uy * uy;
        double utperp = sqrt(1.0 + ux2 + uy2);
        double tau2_un = tau2 * un;
        double ut = sqrt(utperp * utperp  +  tau2_un * un);
        double ut2 = ut * ut;

         // skip cells with u.dsigma < 0
        if(ut * dat  +  ux * dax  +  uy * day  +  un * dan <= 0.0) continue;           

        double T = T_fo[icell_glb];             // temperature (GeV)
        double P = P_fo[icell_glb];             // equilibrium pressure (GeV/fm^3)
        double E = E_fo[icell_glb];             // energy density (GeV/fm^3)

        double pitt = 0.0;                      // contravariant shear stress tensor pi^munu (GeV/fm^3)
        double pitx = 0.0;                      // enforce orthogonality pi.u = 0
        double pity = 0.0;                      // and tracelessness Tr(pi) = 0
        double pitn = 0.0;
        double pixx = 0.0;
        double pixy = 0.0;
        double pixn = 0.0;
        double piyy = 0.0;
        double piyn = 0.0;
        double pinn = 0.0;

        if(INCLUDE_SHEAR_DELTAF)
        {
          pixx = pixx_fo[icell_glb];
          pixy = pixy_fo[icell_glb];
          pixn = pixn_fo[icell_glb];
          piyy = piyy_fo[icell_glb];
          piyn = piyn_fo[icell_glb];
          pinn = (pixx * (ux2 - ut2)  +  piyy * (uy2 - ut2)  +  2.0 * (pixy * ux * uy  +  tau2_un * (pixn * ux  +  piyn * uy))) / (tau2 * utperp * utperp);
          pitn = (pixn * ux  +  piyn * uy  +  tau2_un * pinn) / ut;
          pity = (pixy * ux  +  piyy * uy  +  tau2_un * piyn) / ut;
          pitx = (pixx * ux  +  pixy * uy  +  tau2_un * pixn) / ut;
          pitt = (pitx * ux  +  pity * uy  +  tau2_un * pitn) / ut;
        }

        double bulkPi = 0.0;                    // bulk pressure (GeV/fm^3)

        if(INCLUDE_BULK_DELTAF) bulkPi = bulkPi_fo[icell_glb];

        double muB = 0.0;                       // baryon chemical potential (GeV)
        double alphaB = 0.0;                    // muB / T
        double nB = 0.0;                        // net baryon density (fm^-3)
        double Vt = 0.0;                        // contravariant net baryon diffusion V^mu (fm^-3)
        double Vx = 0.0;                        // enforce orthogonality V.u = 0
        double Vy = 0.0;
        double Vn = 0.0;
        double baryon_enthalpy_ratio = 0.0;     // nB / (E + P)

        if(INCLUDE_BARYON && INCLUDE_BARYONDIFF_DELTAF)
        {
          muB = muB_fo[icell_glb];
          nB = nB_fo[icell_glb];
          Vx = Vx_fo[icell_glb];
          Vy = Vy_fo[icell_glb];
          Vn = Vn_fo[icell_glb];
          Vt = (Vx * ux  +  Vy * uy  +  Vn * tau2_un) / ut;

          alphaB = muB / T;
          baryon_enthalpy_ratio = nB / (E + P);
        }

        double tau2_pitn = tau2 * pitn;   // useful expressions
        double tau2_pixn = tau2 * pixn;
        double tau2_piyn = tau2 * piyn;
        double tau4_pinn = tau2 * tau2 * pinn;
        double tau2_Vn = tau2 * Vn;
            

        // set df coefficients
        deltaf_coefficients df = df_data->evaluate_df_coefficients(T, muB, E, P, bulkPi);

        double c0 = df.c0;             // 14 moment coefficients
        double c1 = df.c1;
        double c2 = df.c2;
        double c3 = df.c3;
        double c4 = df.c4;
        double shear14_coeff = df.shear14_coeff;

        double F = df.F;               // Chapman Enskog
        double G = df.G;
        double betabulk = df.betabulk;
        double betaV = df.betaV;
        double betapi = df.betapi;

        // shear and bulk coefficients
        double shear_coeff = 0.0;
        double bulk0_coeff = 0.0;
        double bulk1_coeff = 0.0;
        double bulk2_coeff = 0.0;
        double diff0_coeff = 0.0;
        double diff1_coeff = 0.0;

        switch(DF_MODE)
        {
          case 1: // 14 moment
          {
            shear_coeff = 1.0 / shear14_coeff;
            bulk0_coeff = (c0 - c2) * bulkPi;
            bulk1_coeff = c1 * bulkPi;
            bulk2_coeff = (4.*c2 - c0) * bulkPi;
            diff0_coeff = c3;
            diff1_coeff = c4;
            break;
          }
          case 2: // Chapman enskog
          {
            shear_coeff = 0.5 / (betapi * T);
            bulk0_coeff = F / (T * T * betabulk) * bulkPi;
            bulk1_coeff = G / betabulk * bulkPi;
            bulk2_coeff = bulkPi / (3.0 * T * betabulk);
            diff0_coeff = baryon_enthalpy_ratio / betaV;
            diff1_coeff = 1.0 / betaV;
            break;
          }
          default:
          {
            printf("Error: set df_mode = (1,2) in parameters.dat\n");
          }
        }

        for(long ipart = 0; ipart < npart; ipart++)
        {
          double mass = Mass[ipart];              // mass (GeV)
          double mass_squared = mass * mass;
          double sign = Sign[ipart];              // quantum statistics sign
          double baryon = Baryon[ipart];          // baryon number
          double chem = baryon * alphaB;          // chemical potential term in feq

          double weight1 = prefactor * Degeneracy[ipart];

          // compute the dN_deta of particle for each freezeout cell (integrating over pT,phi,y_minus_eta)
          double dN_deta = 0.0;

          for(long ipT = 0; ipT < pT_tab_length; ipT++)
          {
            double pT = pTValues[ipT];                  // p_T (GeV)
            double mT = sqrt(mass_squared  +  pT * pT); // m_T (GeV)
            double mT_over_tau = mT / tau;      

            double weight2 = weight1 * pTWeights[ipT];  

            for(long iphip = 0; iphip < phi_tab_length; iphip++)
            {
              double px = pT * cosphiValues[iphip]; // p^x
              double py = pT * sinphiValues[iphip]; // p^y

              double px_dax = px * dax;   // useful expressions
              double py_day = py * day;

              double px_ux = px * ux;
              double py_uy = py * uy;

              double pixx_px_px = pixx * px * px;
              double piyy_py_py = piyy * py * py;
              double pitx_px = pitx * px;
              double pity_py = pity * py;
              double pixy_px_py = pixy * px * py;
              double tau2_pixn_px = tau2_pixn * px;
              double tau2_piyn_py = tau2_piyn * py;

              double Vx_px = Vx * px;
              double Vy_py = Vy * py;

              double weight3 = weight2 * phiWeights[iphip];

              for(long iyeta = 0; iyeta < y_minus_eta_tab_length; iyeta++)
              {
                double y_minus_eta = y_minus_eta_Values[iyeta];                
              
                double sinhyeta = sinh(y_minus_eta);
                double coshyeta = sqrt(1.0  +  sinhyeta * sinhyeta);

                double pt = mT * coshyeta;           // p^tau
                double pn = mT_over_tau * sinhyeta;  // p^eta
                
                double pdotdsigma = pt * dat  +  px_dax  +  py_day  +  pn * dan;

                if(OUTFLOW && pdotdsigma <= 0.0) continue;  // enforce outflow

                double pdotu = pt * ut  -  px_ux  -  py_uy  -  pn * tau2_un;  // u.p
                double feq = 1.0 / (exp(pdotu/T  -  chem) + sign);
                double feqbar = 1.0  -  sign * feq;

                // pi^munu.p_mu.p_nu
                double pimunu_pmu_pnu = pitt * pt * pt  +  pixx_px_px  +  piyy_py_py  +  tau4_pinn * pn * pn
                    + 2.0 * (-(pitx_px + pity_py) * pt  +  pixy_px_py  +  pn * (tau2_pixn_px  +  tau2_piyn_py  -  tau2_pitn * pt));

                // V^mu.p_mu
                double Vmu_pmu = Vt * pt  -  Vx_px  -  Vy_py  -  tau2_Vn * pn;
                  

                double df;  // delta f correction

                switch(DF_MODE)
                {
                  case 1: // 14 moment
                  {
                    double df_shear = shear_coeff * pimunu_pmu_pnu;
                    double df_bulk = bulk0_coeff * mass_squared  +  (bulk1_coeff * baryon  +  bulk2_coeff * pdotu) * pdotu;
                    double df_diff = (diff0_coeff * baryon  +  diff1_coeff * pdotu) * Vmu_pmu;

                    df = feqbar * (df_shear + df_bulk + df_diff);
                    break;
                  }
                  case 2: // Chapman enskog
                  {
                    double df_shear = shear_coeff * pimunu_pmu_pnu / pdotu;
                    double df_bulk = bulk0_coeff * pdotu  +  bulk1_coeff * baryon   +  bulk2_coeff * (pdotu  -  mass_squared / pdotu);
                    double df_diff = (diff0_coeff  -  baryon * diff1_coeff / pdotu) * Vmu_pmu;

                    df = feqbar * (df_shear + df_bulk + df_diff);
                    break;
                  }
                  default:
                  {
                    printf("Error: set df_mode = (1,2) in parameters.dat\n"); exit(-1);
                  }
                } // DF_MODE

                if(REGULATE_DELTAF) df = max(-1.0, min(df, 1.0)); // regulate df

                dN_deta += weight3 * y_minus_eta_Weights[iyeta] * pdotdsigma * feq * (1.0 + df);

              } // rapidity_minus_eta points (iyeta)

            } // azimuthal angle points (iphip)

          } // transverse momentum points (ipT)


          // bin particle's dN_deta of freezeout cell in spacetime grid 
          if(ieta >= 0 && ieta < ETA_BINS)
          {
            if(itau >= 0 && itau < TAU_BINS) 
            {
              dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * itau))] += dN_deta;
            }

            if(ir >= 0 && ir < R_BINS) 
            {
              dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * (ir + TAU_BINS)))] += dN_deta;
            }

            if(iphi >= 0 && iphi < PHIP_BINS) 
            {
              dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * (iphi + TAU_BINS + R_BINS)))] += dN_deta;
            }
          }

        } // particles (ipart)

      } // freezeout cells (icell)

    } // chunks or cores (n)


    // now do reduction over cores
    for(long ipart = 0; ipart < npart; ipart++)
    {
      for(long ieta = 0; ieta < ETA_BINS; ieta++)
      {
        for(long itau = 0; itau < TAU_BINS; itau++)
        {
          // sum over the cores
          for(long n = 0; n < CORES; n++)
          {
            dN_dX[ipart  +  npart * (ieta  +  ETA_BINS * itau)] += dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * itau))];
          }
        }

        for(long ir = 0; ir < R_BINS; ir++)
        {
          // sum over the cores
          for(long n = 0; n < CORES; n++)
          {
            dN_dX[ipart  +  npart * (ieta  +  ETA_BINS * (ir + TAU_BINS))] += dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * (ir + TAU_BINS)))];
          }
        }

        for(long iphi = 0; iphi < PHIP_BINS; iphi++)
        {
          // sum over the cores
          for(long n = 0; n < CORES; n++)
          {
            dN_dX[ipart  +  npart * (ieta  +  ETA_BINS * (iphi + TAU_BINS + R_BINS))] += dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * (iphi + TAU_BINS + R_BINS)))];
          }
        }
      }
    } // reduction

    free(dN_dX_all);
  }


  void EmissionFunctionArray::calculate_dN_dX_feqmod(long *MCID, double *Mass, double *Sign, double *Degeneracy, double *Baryon,
  double *T_fo, double *P_fo, double *E_fo, double *tau_fo, double *x_fo, double *y_fo, double *eta_fo, double *ux_fo, double *uy_fo, double *un_fo,
  double *dat_fo, double *dax_fo, double *day_fo, double *dan_fo,
  double *pixx_fo, double *pixy_fo, double *pixn_fo, double *piyy_fo, double *piyn_fo, double *bulkPi_fo,
  double *muB_fo, double *nB_fo, double *Vx_fo, double *Vy_fo, double *Vn_fo, Gauss_Laguerre * laguerre, Deltaf_Data *df_data)
  {
    printf("computing thermal spacetime distribution from vhydro with feqmod...\n\n");

    // dX = tau.dtau.deta, 2.pi.r.dr.deta or 2.pi.tau.r.dtau.dr.deta
    // only have boost invariance in mind right now so
    // deta = dy and only need to integrate over (pT,phi)

    // you compute the spatial distributions by setting up a spacetime
    // grid and binning the freezeout cell's mean particle number

    double prefactor = pow(2.0 * M_PI * hbarC, -3);   // prefactor of CFF
    long FO_chunk = FO_length / CORES;
    long remainder = FO_length  -  CORES * FO_chunk;

    cout << "Number of threads : " << CORES << endl;
    cout << "Chunk size = " << FO_chunk << endl;
    cout << "Remainder cells = " << remainder << endl;

    if(remainder != 0) FO_chunk++;

    double detA_min = DETA_MIN;   // default value for minimum detA
    int breakdown = 0;            // # times feqmod breaks down

    // phi arrays
    double cosphiValues[phi_tab_length];
    double sinphiValues[phi_tab_length];
    double phiWeights[phi_tab_length];

    for(long iphip = 0; iphip < phi_tab_length; iphip++)
    {
      double phi = phi_tab->get(1, iphip + 1);
      cosphiValues[iphip] = cos(phi);
      sinphiValues[iphip] = sin(phi);

      phiWeights[iphip] = phi_tab->get(2, iphip + 1);
    }

    // pT array
    double pTValues[pT_tab_length];
    double pTWeights[pT_tab_length];

    for(long ipT = 0; ipT < pT_tab_length; ipT++)
    {
      pTValues[ipT] = pT_tab->get(1, ipT + 1);
      pTWeights[ipT] = pT_tab->get(2, ipT + 1);
    }

    // y_minus_eta array
    double y_minus_eta_Values[y_minus_eta_tab_length];
    double y_minus_eta_Weights[y_minus_eta_tab_length];

    for(long iyeta = 0; iyeta < y_minus_eta_tab_length; iyeta++)
    {
      y_minus_eta_Values[iyeta] = eta_tab->get(1, iyeta + 1);
      y_minus_eta_Weights[iyeta] = eta_tab->get(2, iyeta + 1);
    }


    /// gauss laguerre roots
    const int pbar_pts = laguerre->points;

    double * pbar_root1 = laguerre->root[1];
    double * pbar_root2 = laguerre->root[2];

    double * pbar_weight1 = laguerre->weight[1];
    double * pbar_weight2 = laguerre->weight[2];


    double *dN_dX_all =  (double*)calloc(CORES * spacetime_length, sizeof(double));


    #pragma omp parallel for
    for(long n = 0; n < CORES; n++)
    {
      double ** A_copy = (double**)calloc(3, sizeof(double*));
      for(int i = 0; i < 3; i++) A_copy[i] = (double*)calloc(3, sizeof(double));

      double ** A_inv = (double**)calloc(3, sizeof(double*));
      for(int i = 0; i < 3; i++) A_inv[i] = (double*)calloc(3, sizeof(double));

      long endFO = FO_chunk;

      for(long icell = 0; icell < endFO; icell++)  
      {
        if((icell == endFO - 1) && (remainder != 0) && (n > remainder - 1)) continue;

        long icell_glb = n  +  icell * CORES;   // global cell index

        double tau = tau_fo[icell_glb];         // longitudinal proper time
        double x = x_fo[icell_glb];             // x position
        double y = y_fo[icell_glb];             // y position

        double r = sqrt(x * x  +  y * y);
        double phi = atan2(y, x);
        if(phi < 0.0) phi += two_pi;

        double tau2 = tau * tau;

        double eta = 0.0;
        if(DIMENSION == 3)
        {
          eta = eta_fo[icell_glb];              // spacetime rapidity from surface file
        }

        // spacetime bin index
        long itau = (long)floor((tau - TAU_MIN) / TAU_WIDTH);
        long ir = (long)floor((r - R_MIN) / R_WIDTH);
        long iphi = (long)floor(phi / PHIP_WIDTH);
        long ieta = 0;
        if(DIMENSION == 3)
        { 
          ieta = (long)floor((eta + ETA_CUT) / ETA_WIDTH); 
        }

        double dat = dat_fo[icell_glb];         // covariant normal surface vector
        double dax = dax_fo[icell_glb];
        double day = day_fo[icell_glb];
        double dan = dan_fo[icell_glb];         // dan should be 0 for 2+1d

        double ux = ux_fo[icell_glb];           // contravariant fluid velocity
        double uy = uy_fo[icell_glb];           // enforce normalization
        double un = un_fo[icell_glb];
        double ux2 = ux * ux;                   // useful expressions
        double uy2 = uy * uy;
        double uperp = sqrt(ux2 + uy2);
        double utperp = sqrt(1.0 + ux2 + uy2);
        double tau2_un = tau2 * un;
        double ut = sqrt(utperp * utperp  +  tau2_un * un);
        double ut2 = ut * ut;

        double udsigma = ut * dat  +  ux * dax  +  uy * day  +  un * dan;  // u.dsigma / eta_weight

        if(udsigma <= 0.0) continue;            // skip cells with u.dsigma < 0

        double T = T_fo[icell_glb];             // temperature (GeV)
        double P = P_fo[icell_glb];             // equilibrium pressure (GeV/fm^3)
        double E = E_fo[icell_glb];             // energy density (GeV/fm^3)

        double pitt = 0.0;                      // contravariant shear stress tensor pi^munu (GeV/fm^3)
        double pitx = 0.0;                      // enforce orthogonality pi.u = 0
        double pity = 0.0;                      // and tracelessness Tr(pi) = 0
        double pitn = 0.0;
        double pixx = 0.0;
        double pixy = 0.0;
        double pixn = 0.0;
        double piyy = 0.0;
        double piyn = 0.0;
        double pinn = 0.0;

        if(INCLUDE_SHEAR_DELTAF)
        {
          pixx = pixx_fo[icell_glb];
          pixy = pixy_fo[icell_glb];
          pixn = pixn_fo[icell_glb];
          piyy = piyy_fo[icell_glb];
          piyn = piyn_fo[icell_glb];
          pinn = (pixx * (ux2 - ut2)  +  piyy * (uy2 - ut2)  +  2.0 * (pixy * ux * uy  +  tau2_un * (pixn * ux  +  piyn * uy))) / (tau2 * utperp * utperp);
          pitn = (pixn * ux  +  piyn * uy  +  tau2_un * pinn) / ut;
          pity = (pixy * ux  +  piyy * uy  +  tau2_un * piyn) / ut;
          pitx = (pixx * ux  +  pixy * uy  +  tau2_un * pixn) / ut;
          pitt = (pitx * ux  +  pity * uy  +  tau2_un * pitn) / ut;
        }

        double bulkPi = 0.0;                    // bulk pressure (GeV/fm^3)

        if(INCLUDE_BULK_DELTAF) bulkPi = bulkPi_fo[icell_glb];

        double muB = 0.0;                       // baryon chemical potential (GeV)
        double alphaB = 0.0;                    // muB / T
        double nB = 0.0;                        // net baryon density (fm^-3)
        double Vt = 0.0;                        // contravariant net baryon diffusion V^mu (fm^-3)
        double Vx = 0.0;                        // enforce orthogonality V.u = 0
        double Vy = 0.0;
        double Vn = 0.0;
        double baryon_enthalpy_ratio = 0.0;     // nB / (E + P)

        if(INCLUDE_BARYON && INCLUDE_BARYONDIFF_DELTAF)
        {
          muB = muB_fo[icell_glb];
          nB = nB_fo[icell_glb];
          Vx = Vx_fo[icell_glb];
          Vy = Vy_fo[icell_glb];
          Vn = Vn_fo[icell_glb];
          Vt = (Vx * ux  +  Vy * uy  +  Vn * tau2_un) / ut;

          alphaB = muB / T;
          baryon_enthalpy_ratio = nB / (E + P);
        }

        double tau2_pitn = tau2 * pitn;   // useful expressions
        double tau2_pixn = tau2 * pixn;
        double tau2_piyn = tau2 * piyn;
        double tau4_pinn = tau2 * tau2 * pinn;
        double tau2_Vn = tau2 * Vn;

        // regulate bulk pressure if goes out of bounds given
        // by Jonah's feqmod to avoid gsl interpolation errors
        if(DF_MODE == 4)
        {
          double bulkPi_over_Peq_max = df_data->bulkPi_over_Peq_max;

          if(bulkPi <= - P) bulkPi = - (1.0 - 1.e-5) * P;
          else if(bulkPi / P >= bulkPi_over_Peq_max) bulkPi = P * (bulkPi_over_Peq_max - 1.e-5);
        }

        // set df coefficients
        deltaf_coefficients df = df_data->evaluate_df_coefficients(T, muB, E, P, bulkPi);

        // modified coefficients (Mike / Jonah)
        double F = df.F;
        double G = df.G;
        double betabulk = df.betabulk;
        double betaV = df.betaV;
        double betapi = df.betapi;
        double lambda = df.lambda;
        double z = df.z;
        double delta_lambda = df.delta_lambda;
        double delta_z = df.delta_z;
        double delta_lambda_over_T = delta_lambda / T;

        // milne basis class
        Milne_Basis basis_vectors(ut, ux, uy, un, uperp, utperp, tau);
        basis_vectors.test_orthonormality(tau2);

        double Xt = basis_vectors.Xt;   double Yx = basis_vectors.Yx;
        double Xx = basis_vectors.Xx;   double Yy = basis_vectors.Yy;
        double Xy = basis_vectors.Xy;   double Zt = basis_vectors.Zt;
        double Xn = basis_vectors.Xn;   double Zn = basis_vectors.Zn;

        // shear stress class
        Shear_Stress pimunu(pitt, pitx, pity, pitn, pixx, pixy, pixn, piyy, piyn, pinn);
        pimunu.test_pimunu_orthogonality_and_tracelessness(ut, ux, uy, un, tau2);
        pimunu.boost_pimunu_to_lrf(basis_vectors, tau2);

        // baryon diffusion class
        Baryon_Diffusion Vmu(Vt, Vx, Vy, Vn);
        Vmu.test_Vmu_orthogonality(ut, ux, uy, un, tau2);
        Vmu.boost_Vmu_to_lrf(basis_vectors, tau2);


        // modified temperature / chemical potential
        double T_mod = T;
        double alphaB_mod = alphaB;

        if(DF_MODE == 3)
        {
          T_mod = T  +  bulkPi * F / betabulk;
          alphaB_mod = alphaB  +  bulkPi * G / betabulk;
        }


        // linearized Chapman Enskog df coefficients (for Mike only)
        double shear_coeff = 0.5 / (betapi * T);      // Jonah linear df also shares shear coeff
        double bulk0_coeff = F * bulkPi / (T * T * betabulk);
        double bulk1_coeff = G * bulkPi / betabulk;
        double bulk2_coeff = bulkPi / (3.0 * T * betabulk);
        double diff0_coeff = baryon_enthalpy_ratio / betaV;
        double diff1_coeff = 1.0 / betaV;

        // pimunu and Vmu LRF components
        double pixx_LRF = pimunu.pixx_LRF;
        double pixy_LRF = pimunu.pixy_LRF;
        double pixz_LRF = pimunu.pixz_LRF;
        double piyy_LRF = pimunu.piyy_LRF;
        double piyz_LRF = pimunu.piyz_LRF;
        double pizz_LRF = pimunu.pizz_LRF;

        double Vx_LRF = Vmu.Vx_LRF;
        double Vy_LRF = Vmu.Vy_LRF;
        double Vz_LRF = Vmu.Vz_LRF;

        // local momentum transformation matrix Mij = Aij
        // Aij = ideal + shear + bulk is symmetric
        // Mij is not symmetric if include baryon diffusion (leave for future work)

        // modified coefficients in Aij
        double shear_mod = 0.5 / betapi;
        double bulk_mod = bulkPi / (3.0 * betabulk);

        if(DF_MODE == 4) bulk_mod = lambda;

        // Aij elements
        double Axx = 1.0  +  pixx_LRF * shear_mod  +  bulk_mod;
        double Axy = pixy_LRF * shear_mod;
        double Axz = pixz_LRF * shear_mod;
        double Ayy = 1.0  +  piyy_LRF * shear_mod  +  bulk_mod;
        double Ayz = piyz_LRF * shear_mod;
        double Azz = 1.0  +  pizz_LRF * shear_mod  +  bulk_mod;

        double detA = Axx * (Ayy * Azz  -  Ayz * Ayz)  -  Axy * (Axy * Azz  -  Ayz * Axz)  +  Axz * (Axy * Ayz  -  Ayy * Axz);
        double detA_bulk_two_thirds = pow(1.0 + bulk_mod, 2);

        // determine if feqmod breaks down
        bool feqmod_breaks_down = does_feqmod_breakdown(MASS_PION0, T, F, bulkPi, betabulk, detA, detA_min, z, laguerre, DF_MODE, 0, T, F, betabulk);

        // set Aij matrix
        double A[] = {Axx, Axy, Axz,
                      Axy, Ayy, Ayz,
                      Axz, Ayz, Azz};           // gsl matrix format

        A_copy[0][0] = Axx;  A_copy[0][1] = Axy;  A_copy[0][2] = Axz;
        A_copy[1][0] = Axy;  A_copy[1][1] = Ayy;  A_copy[1][2] = Ayz;
        A_copy[2][0] = Axz;  A_copy[2][1] = Ayz;  A_copy[2][2] = Azz;

        // compute Aij^-1 using LUP decomposition
        int s;
        gsl_matrix_view LU = gsl_matrix_view_array(A, 3, 3);
        gsl_permutation * p = gsl_permutation_calloc(3);
        gsl_linalg_LU_decomp(&LU.matrix, p, &s);

        gsl_matrix * A_inverse = gsl_matrix_alloc(3, 3);
        gsl_linalg_LU_invert(&LU.matrix, p, A_inverse);

        for(int i = 0; i < 3; i++)
        {
          for(int j = 0; j < 3; j++)
          {
            A_inv[i][j] = gsl_matrix_get(A_inverse, i, j);
          }
        }


        // prefactors for equilibrium, linear bulk correction and modified densities (Mike's feqmod)
        double neq_fact = T * T * T / two_pi2_hbarC3;
        double dn_fact = bulkPi / betabulk;
        double J20_fact = T * neq_fact;
        double N10_fact = neq_fact;
        double nmod_fact = T_mod * T_mod * T_mod / two_pi2_hbarC3;

        // if(feqmod_breaks_down)
        // {
        //   #pragma omp critical
        //   {
        //     breakdown++;
        //     cout << setw(5) << setprecision(4) << "feqmod breaks down for " << breakdown << " / " << FO_length << " cells  (cell = " << icell << ", tau = " << tau << " fm/c, " << ", detA = " << detA << ", detA_min = " << detA_min << ")" << endl;
        //   }
        // }

        // rescale eta by detA if modified momentum space elements are shrunk
        // for integrating modified distribution with narrow (y-eta) distributions
        // note: this only works for boost invariant surfaces, where dsigma is always orthogonal to eta direction (dan = 0)
        double eta_scale = detA / detA_bulk_two_thirds;
       

        for(long ipart = 0; ipart < npart; ipart++)
        {
          double mass = Mass[ipart];              // mass (GeV)
          double mass_squared = mass * mass;
          double sign = Sign[ipart];              // quantum statistics sign
          double degeneracy = Degeneracy[ipart];  // spin degeneracy
          double baryon = Baryon[ipart];          // baryon number
          double chem = baryon * alphaB;          // chemical potential term in feq
          double chem_mod = baryon * alphaB_mod;

          double weight1 = prefactor * degeneracy;

          // compute the modified renormalization factor
          double renorm = 1.0;

          if(INCLUDE_BULK_DELTAF)
          {
            if(DF_MODE == 3)
            {
              double mbar = mass / T;
              double mbar_mod = mass / T_mod;

              double neq = neq_fact * degeneracy * GaussThermal(neq_int, pbar_root1, pbar_weight1, pbar_pts, mbar, alphaB, baryon, sign);

              double N10 = baryon * N10_fact * degeneracy * GaussThermal(J10_int, pbar_root1, pbar_weight1, pbar_pts, mbar, alphaB, baryon, sign);

              double J20 = J20_fact * degeneracy * GaussThermal(J20_int, pbar_root2, pbar_weight2, pbar_pts, mbar, alphaB, baryon, sign);

              double n_linear = neq  +  dn_fact * (neq  +  N10 * G  +  J20 * F / T / T);

              double n_mod = nmod_fact * degeneracy * GaussThermal(neq_int, pbar_root1, pbar_weight1, pbar_pts, mbar_mod, alphaB_mod, baryon, sign);

              renorm = n_linear / n_mod;
            }
            else if(DF_MODE == 4)
            {
              renorm = z;
            }
          }

          
          renorm /= detA_bulk_two_thirds;
          

          if(!feqmod_breaks_down && (std::isnan(renorm) || std::isinf(renorm)) )
          {
            cout << "Error: renormalization factor is " << renorm << endl;
            continue;
          }

          // compute the dNdy of each freezeout cell (integrating over pT,phi,y)
          double dN_deta = 0.0;

          for(long ipT = 0; ipT < pT_tab_length; ipT++)
          {
            double pT = pTValues[ipT];              // p_T (GeV)
            double pT_weight = pTWeights[ipT];

            double mT = sqrt(mass_squared  +  pT * pT);    // m_T (GeV)
            double mT_over_tau = mT / tau;

            double weight2 = weight1 * pT_weight;

            for(long iphip = 0; iphip < phi_tab_length; iphip++)
            {
              double px = pT * cosphiValues[iphip]; // p^x
              double py = pT * sinphiValues[iphip]; // p^y

              double phi_weight = phiWeights[iphip];

              double weight3 = weight2 * phi_weight;

              double px_dax = px * dax;   // useful expressions
              double py_day = py * day;

              double px_ux = px * ux;
              double py_uy = py * uy;

              double pixx_px_px = pixx * px * px;
              double piyy_py_py = piyy * py * py;
              double pitx_px = pitx * px;
              double pity_py = pity * py;
              double pixy_px_py = pixy * px * py;
              double tau2_pixn_px = tau2_pixn * px;
              double tau2_piyn_py = tau2_piyn * py;

              double Vx_px = Vx * px;
              double Vy_py = Vy * py;


              for(long iyeta = 0; iyeta < y_minus_eta_tab_length; iyeta++)
              {
                double y_minus_eta = y_minus_eta_Values[iyeta];                
                double y_minus_eta_weight = y_minus_eta_Weights[iyeta];
              
                double sinhyeta = sinh(y_minus_eta);
                double coshyeta = sqrt(1.0  +  sinhyeta * sinhyeta);


                double f, pdotdsigma;

                // calculate f
                if(feqmod_breaks_down)
                {
                  double pt = mT * coshyeta;           // p^tau
                  double pn = mT_over_tau * sinhyeta;  // p^eta
                  
                  pdotdsigma = pt * dat  +  px_dax  +  py_day  +  pn * dan;

                  double pdotu = pt * ut  -  px_ux  -  py_uy  -  pn * tau2_un;  // u.p
                  double feq = 1.0 / (exp(pdotu/T  -  chem) + sign);
                  double feqbar = 1.0  -  sign * feq;

                  // pi^munu.p_mu.p_nu
                  double pimunu_pmu_pnu = pitt * pt * pt  +  pixx_px_px  +  piyy_py_py  +  tau4_pinn * pn * pn
                      + 2.0 * (-(pitx_px + pity_py) * pt  +  pixy_px_py  +  pn * (tau2_pixn_px  +  tau2_piyn_py  -  tau2_pitn * pt));

                  if(DF_MODE == 3)
                  {
                    // V^mu.p_mu
                    double Vmu_pmu = Vt * pt  -  Vx_px  -  Vy_py  -  tau2_Vn * pn;
                    
                    double df_shear = shear_coeff * pimunu_pmu_pnu / pdotu;
                    double df_bulk = (bulk0_coeff * pdotu  +  bulk1_coeff * baryon  +  bulk2_coeff * (pdotu  -  mass_squared / pdotu));
                    double df_diff = (diff0_coeff -  baryon * diff1_coeff / pdotu) * Vmu_pmu;

                    double df = feqbar * (df_shear + df_bulk + df_diff);

                    if(REGULATE_DELTAF) df = max(-1.0, min(df, 1.0)); // regulate df

                    f = feq * (1.0 + df);
                  }
                  else if(DF_MODE == 4)
                  {
                    double df_shear = feqbar * shear_coeff * pimunu_pmu_pnu / pdotu;
                    double df_bulk = delta_z  -  3.0 * delta_lambda  +  feqbar * delta_lambda_over_T * (pdotu  -  mass_squared / pdotu);

                    double df = df_shear + df_bulk;

                    if(REGULATE_DELTAF) df = max(-1.0, min(df, 1.0)); // regulate df

                    f = feq * (1.0 + df);
                  }
                } // feqmod breaks down
                else
                {
                  double pt = mT * cosh(y_minus_eta * eta_scale);           // p^\tau (GeV)
                  double pn = mT_over_tau * sinh(y_minus_eta * eta_scale);  // p^\eta (GeV^2)

                  pdotdsigma = pt * dat  +  px_dax  +  py_day  +  pn * dan; // p.dsigma

                  // LRF momentum components pi_LRF = - Xi.p
                  double px_LRF = -Xt * pt  +  Xx * px  +  Xy * py  +  Xn * tau2 * pn;
                  double py_LRF = Yx * px  +  Yy * py;
                  double pz_LRF = -Zt * pt  +  Zn * tau2 * pn;

                  double pLRF[3] = {px_LRF, py_LRF, pz_LRF};
                  double pLRF_mod[3];

                  // LUP method with iterations

                  double pLRF_prev[3];
                  double pLRF_mod_prev[3];

                  double dpLRF[3];
                  double dpLRF_mod[3];

                  matrix_multiplication(A_inv, pLRF, pLRF_mod, 3, 3);   // evaluate pLRF_mod = A^-1.pLRF at least once

                  double dp;                                            // |pLRF| error (i.e. dp = pLRF - A.pLRF_mod)
                  double epsilon = 1.e-16;

                  // iterate the solution p_LRF_mod (sometimes it gets no improvement)
                  for(int i = 0; i < 5; i++)
                  {
                    vector_copy(pLRF_mod, pLRF_mod_prev, 3);                        // copy solution for iteration
                    matrix_multiplication(A_copy, pLRF_mod_prev, pLRF_prev, 3, 3);  // check |pLRF| error
                    vector_subtraction(pLRF, pLRF_prev, dpLRF, 3);

                    dp = sqrt(dpLRF[0] * dpLRF[0]  +  dpLRF[1] * dpLRF[1]  +  dpLRF[2] * dpLRF[2]);

                    if(dp <= epsilon) break;

                    matrix_multiplication(A_inv, dpLRF, dpLRF_mod, 3, 3);           // compute correction to pLRF_mod
                    vector_addition(pLRF_mod_prev, dpLRF_mod, pLRF_mod, 3);         // add correction to pLRF_mod
                  }

                  double E_mod = sqrt(mass_squared  +  pLRF_mod[0] * pLRF_mod[0]  +  pLRF_mod[1] * pLRF_mod[1]  +  pLRF_mod[2] * pLRF_mod[2]);

                  f = fabs(renorm) / (exp(E_mod / T_mod  -  chem_mod) + sign); // feqmod
                }

                if(OUTFLOW && pdotdsigma <= 0.0) continue;  // enforce outflow

                dN_deta += weight3 * y_minus_eta_weight * pdotdsigma * f;

              } // rapidity minus eta points (iyeta)

            } // azimuthal angle points (iphip)

          } // transverse momentum points (ipT)


          // bin particle's dN_deta of freezeout cell in spacetime grid 
          if(ieta >= 0 && ieta < ETA_BINS)
          {
            if(itau >= 0 && itau < TAU_BINS) 
            {
              dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * itau))] += dN_deta;
            }

            if(ir >= 0 && ir < R_BINS) 
            {
              dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * (ir + TAU_BINS)))] += dN_deta;
            }

            if(iphi >= 0 && iphi < PHIP_BINS) 
            {
              dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * (iphi + TAU_BINS + R_BINS)))] += dN_deta;
            }
          }

        } // particles (ipart)

        gsl_matrix_free(A_inverse);
        gsl_permutation_free(p);
    
      } // freezeout cells (icell)

      free_2D(A_copy, 3);
      free_2D(A_inv, 3);

    } // chunks or cores (n)

    // now do reduction over cores
    for(long ipart = 0; ipart < npart; ipart++)
    {
      for(long ieta = 0; ieta < ETA_BINS; ieta++)
      {
        for(long itau = 0; itau < TAU_BINS; itau++)
        {
          // sum over the cores
          for(long n = 0; n < CORES; n++)
          {
            dN_dX[ipart  +  npart * (ieta  +  ETA_BINS * itau)] += dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * itau))];
          }
        }

        for(long ir = 0; ir < R_BINS; ir++)
        {
          // sum over the cores
          for(long n = 0; n < CORES; n++)
          {
            dN_dX[ipart  +  npart * (ieta  +  ETA_BINS * (ir + TAU_BINS))] += dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * (ir + TAU_BINS)))];
          }
        }

        for(long iphi = 0; iphi < PHIP_BINS; iphi++)
        {
          // sum over the cores
          for(long n = 0; n < CORES; n++)
          {
            dN_dX[ipart  +  npart * (ieta  +  ETA_BINS * (iphi + TAU_BINS + R_BINS))] += dN_dX_all[n  +  CORES * (ipart  +  npart * (ieta  +  ETA_BINS * (iphi + TAU_BINS + R_BINS)))];
          }
        }
      }
    } // reduction

    free(dN_dX_all);
  }



/*
    void EmissionFunctionArray::calculate_dN_pTdpTdphidy_VAH_PL(double *Mass, double *Sign, double *Degeneracy,
      double *tau_fo, double *eta_fo, double *ux_fo, double *uy_fo, double *un_fo,
      double *dat_fo, double *dax_fo, double *day_fo, double *dan_fo, double *T_fo,
      double *pitt_fo, double *pitx_fo, double *pity_fo, double *pitn_fo, double *pixx_fo, double *pixy_fo, double *pixn_fo, double *piyy_fo, double *piyn_fo, double *pinn_fo,
      double *bulkPi_fo, double *Wx_fo, double *Wy_fo, double *Lambda_fo, double *aL_fo, double *c0_fo, double *c1_fo, double *c2_fo, double *c3_fo, double *c4_fo)
      {
        double prefactor = 1.0 / (8.0 * (M_PI * M_PI * M_PI)) / hbarC / hbarC / hbarC;
        int FO_chunk = 10000;

        double trig_phi_table[phi_tab_length][2]; // 2: 0,1-> cos,sin
        for (int j = 0; j < phi_tab_length; j++)
        {
          double phi = phi_tab->get(1,j+1);
          trig_phi_table[j][0] = cos(phi);
          trig_phi_table[j][1] = sin(phi);
        }

        //fill arrays with values points in pT and y
        double pTValues[pT_tab_length];
        for (int ipT = 0; ipT < pT_tab_length; ipT++) pTValues[ipT] = pT_tab->get(1, ipT + 1);

        // fill arrays with values points in y and eta
        int y_pts = y_tab_length;    // defaults pts for 3+1d
        int eta_pts = 1;

        if(DIMENSION == 2)
        {
          y_pts = 1;
          eta_pts = eta_tab_length;
        }

        double yValues[y_pts];
        double etaValues[eta_pts];
        double etaWeights[eta_pts];

        double delta_eta = (eta_tab->get(1,2)) - (eta_tab->get(1,1));  // assume uniform grid

        if(DIMENSION == 2)
        {
          yValues[0] = 0.0;
          for(int ieta = 0; ieta < eta_pts; ieta++)
          {
            etaValues[ieta] = eta_tab->get(1, ieta + 1);
            etaWeights[ieta] = (eta_tab->get(2, ieta + 1)) * delta_eta;
            //cout << etaValues[ieta] << "\t" << etaWeights[ieta] << endl;
          }
        }
        else if(DIMENSION == 3)
        {
          etaValues[0] = 0.0;       // below, will load eta_fo
          etaWeights[0] = 1.0; // 1.0 for 3+1d
          for (int iy = 0; iy < y_pts; iy++) yValues[iy] = y_tab->get(1, iy + 1);
        }

        //declare a huge array of size npart * FO_chunk * pT_tab_length * phi_tab_length * y_tab_length
        //to hold the spectra for each surface cell in a chunk, for all particle species
        double *dN_pTdpTdphidy_all;
        dN_pTdpTdphidy_all = (double*)calloc(npart * FO_chunk * pT_tab_length * phi_tab_length * y_tab_length, sizeof(double));

        //loop over bite size chunks of FO surface
        for (int n = 0; n < (FO_length / FO_chunk) + 1; n++)
        {
          printf("Progress : Finished chunk %d of %ld \n", n, FO_length / FO_chunk);
          int endFO = FO_chunk;
          if (n == (FO_length / FO_chunk)) endFO = FO_length - (n * FO_chunk); // don't go out of array bounds
          #pragma omp parallel for
          for (int icell = 0; icell < endFO; icell++) // cell index inside each chunk
          {
            int icell_glb = n * FO_chunk + icell; // global FO cell index

            // set freezeout info to local varibles
            double tau = tau_fo[icell_glb];         // longitudinal proper time
            double tau2 = tau * tau;                // useful expression
            if(DIMENSION == 3)
            {
              etaValues[0] = eta_fo[icell_glb];     // spacetime rapidity from surface file
            }
            double dat = dat_fo[icell_glb];         // covariant normal surface vector
            double dax = dax_fo[icell_glb];
            double day = day_fo[icell_glb];
            double dan = dan_fo[icell_glb];

            double T = T_fo[icell_glb];

            double ux = ux_fo[icell_glb];           // contravariant fluid velocity
            double uy = uy_fo[icell_glb];           // enforce normalization: u.u = 1
            double un = un_fo[icell_glb];
            double ut = sqrt(1.0 + ux*ux + uy*uy + tau2*un*un);

            double u0 = sqrt(1.0 + ux*ux + uy*uy);
            double zt = tau * un / u0;
            double zn = ut / (u0 * tau);            // z basis vector

            double pitt = pitt_fo[icell_glb];       // piperp^munu
            double pitx = pitx_fo[icell_glb];
            double pity = pity_fo[icell_glb];
            double pitn = pitn_fo[icell_glb];
            double pixx = pixx_fo[icell_glb];
            double pixy = pixy_fo[icell_glb];
            double pixn = pixn_fo[icell_glb];
            double piyy = piyy_fo[icell_glb];
            double piyn = piyn_fo[icell_glb];
            double pinn = pinn_fo[icell_glb];

            double bulkPi = bulkPi_fo[icell_glb];   // residual bulk pressure

            double Wx = Wx_fo[icell_glb];           // W^mu
            double Wy = Wy_fo[icell_glb];           // reinforce orthogonality
            double Wt = (ux * Wx  +  uy * Wy) * ut / (u0 * u0);
            double Wn = Wt * un / ut;

            double Lambda = Lambda_fo[icell_glb];   // anisotropic variables
            double aL = aL_fo[icell_glb];

            double c0 = c0_fo[icell_glb];           // 14-moment coefficients
            double c1 = c1_fo[icell_glb];
            double c2 = c2_fo[icell_glb];
            double c3 = c3_fo[icell_glb];
            double c4 = c4_fo[icell_glb];


            //now loop over all particle species and momenta
            for (int ipart = 0; ipart < number_of_chosen_particles; ipart++)
            {
              // set particle properties
              double mass = Mass[ipart];    // (GeV)
              double mass2 = mass * mass;
              double sign = Sign[ipart];
              double degeneracy = Degeneracy[ipart];

              for (int ipT = 0; ipT < pT_tab_length; ipT++)
              {
                // set transverse radial momentum and transverse mass (GeV)
                double pT = pTValues[ipT];
                double mT = sqrt(mass2 + pT * pT);
                // useful expression
                double mT_over_tau = mT / tau;

                for (int iphip = 0; iphip < phi_tab_length; iphip++)
                {
                  double px = pT * trig_phi_table[iphip][0]; //contravariant
                  double py = pT * trig_phi_table[iphip][1]; //contravariant

                  for (int iy = 0; iy < y_pts; iy++)
                  {
                    // all vector components are CONTRAVARIANT EXCEPT the surface normal vector dat, dax, day, dan, which are COVARIANT
                    double y = yValues[iy];

                    double pdotdsigma_f_eta_sum = 0.0;

                    // sum over eta
                    for (int ieta = 0; ieta < eta_pts; ieta++)
                    {
                      double eta = etaValues[ieta];
                      double eta_weight = etaWeights[ieta];

                      double pt = mT * cosh(y - eta);
                      double pn = mT_over_tau * sinh(y - eta);

                      // useful expression
                      double tau2_pn = tau2 * pn;

                      // momentum vector is contravariant, surface normal vector is COVARIANT
                      double pdotdsigma = pt * dat  +  px * dax  +  py * day  +  pn * dan;

                      //thermal equilibrium distributions - for viscous hydro
                      double pdotu = pt * ut  -  px * ux  -  py * uy  -  tau2_pn * un;    // u.p = LRF energy
                      double pdotz = pt * zt  -  tau2_pn * zn;                            // z.p = - LRF longitudinal momentum

                      double xiL = 1.0 / (aL * aL)  -  1.0;

                      #ifdef FORCE_F0
                      Lambda = T;
                      xiL = 0.0;
                      #endif

                      double Ea = sqrt(pdotu * pdotu  +  xiL * pdotz * pdotz);
                      double fa = 1.0 / ( exp(Ea / Lambda) + sign);

                      // residual viscous corrections
                      double fabar = 1.0 - sign*fa;

                      // residual shear correction:
                      double df_shear = 0.0;

                      if (INCLUDE_SHEAR_DELTAF)
                      {
                        // - (-z.p)p_{mu}.W^mu
                        double Wmu_pmu_pz = pdotz * (Wt * pt  -  Wx * px  -  Wy * py  -  Wn * tau2_pn);

                        // p_{mu.p_nu}.piperp^munu
                        double pimunu_pmu_pnu = pitt * pt * pt + pixx * px * px + piyy * py * py + pinn * tau2_pn * tau2_pn
                        + 2.0 * (-(pitx * px + pity * py) * pt + pixy * px * py + tau2_pn * (pixn * px + piyn * py - pitn * pt));

                        df_shear = c3 * Wmu_pmu_pz  +  c4 * pimunu_pmu_pnu;  // df / (feq*feqbar)
                      }

                      // residual bulk correction:
                      double df_bulk = 0.0;
                      if (INCLUDE_BULK_DELTAF) df_bulk = (c0 * mass2  +  c1 * pdotz * pdotz  +  c2 * pdotu * pdotu) * bulkPi;

                      double df = df_shear + df_bulk;

                      if (REGULATE_DELTAF)
                      {
                        double reg_df = max( -1.0, min( fabar * df, 1.0 ) );
                        pdotdsigma_f_eta_sum += (eta_weight * pdotdsigma * fa * (1.0 + reg_df));
                      }
                      else pdotdsigma_f_eta_sum += (eta_weight * pdotdsigma * fa * (1.0 + fabar * df));

                    } // ieta

                    long long int iSpectra = icell + endFO * (ipart + npart * (ipT + pT_tab_length * (iphip + phi_tab_length * iy)));

                    dN_pTdpTdphidy_all[iSpectra] = (prefactor * degeneracy * pdotdsigma_f_eta_sum);
                  } //iy
                } //iphip
              } //ipT
            } //ipart
          } //icell
          if(endFO != 0)
          {
            //now perform the reduction over cells
            #pragma omp parallel for collapse(4)
            for (int ipart = 0; ipart < npart; ipart++)
            {
              for (int ipT = 0; ipT < pT_tab_length; ipT++)
              {
                for (int iphip = 0; iphip < phi_tab_length; iphip++)
                {
                  for (int iy = 0; iy < y_pts; iy++)
                  {
                    //long long int is = ipart + (npart * ipT) + (npart * pT_tab_length * iphip) + (npart * pT_tab_length * phi_tab_length * iy);
                    long long int iS3D = ipart + npart * (ipT + pT_tab_length * (iphip + phi_tab_length * iy));
                    double dN_pTdpTdphidy_tmp = 0.0; //reduction variable
                    #pragma omp simd reduction(+:dN_pTdpTdphidy_tmp)
                    for (int icell = 0; icell < endFO; icell++)
                    {
                      //long long int iSpectra = icell + (endFO * ipart) + (endFO * npart * ipT) + (endFO * npart * pT_tab_length * iphip) + (endFO * npart * pT_tab_length * phi_tab_length * iy);
                      long long int iSpectra = icell + endFO * iS3D;
                      dN_pTdpTdphidy_tmp += dN_pTdpTdphidy_all[iSpectra];
                    }//icell
                    dN_pTdpTdphidy[iS3D] += dN_pTdpTdphidy_tmp; //sum over all chunks
                  }//iy
                }//iphip
              }//ipT
            }//ipart species
          } //if (endFO != 0 )
        }//n FO chunk

        //free memory
        free(dN_pTdpTdphidy_all);
      }
*/


