/* ----------------------------------------------------------------------
   SPPARKS - Stochastic Parallel PARticle Kinetic Simulator
   http://www.cs.sandia.gov/~sjplimp/spparks.html
   Steve Plimpton, sjplimp@sandia.gov, Sandia National Laboratories

   Copyright (2008) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level SPPARKS directory.
------------------------------------------------------------------------- */

#include "mpi.h"
#include "spparks.h"
#include "input.h"

#ifdef MUI
#include "mui.h"
#include "lib_mpi_split.h"
#endif

using namespace SPPARKS_NS;

/* ----------------------------------------------------------------------
   main program to drive SPPARKS
------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
  //MPI_Init(&argc,&argv);
#ifdef MUI
  MPI_Comm comm = mui::mpi_split_by_app(argc,argv);
  mui::uniface2d uniface( "mpi://KMC-side/FHD-KMC-coupling" );
  //SPPARKS *spk = new SPPARKS(argc,argv,MPI_COMM_WORLD);
  SPPARKS *spk = new SPPARKS(argc,argv,comm);
  spk->uniface = &uniface;
#else
  MPI_Init(&argc,&argv);
  SPPARKS *spk = new SPPARKS(argc,argv,MPI_COMM_WORLD);
#endif
  spk->input->file();
  delete spk;
  MPI_Finalize();
}
