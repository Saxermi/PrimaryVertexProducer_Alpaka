#include <alpaka/alpaka.hpp>

#include "HeterogeneousCore/AlpakaInterface/interface/config.h"
#include "HeterogeneousCore/AlpakaInterface/interface/workdivision.h"
#include "HeterogeneousCore/AlpakaInterface/interface/radixSort.h"

#include "RecoVertex/PrimaryVertexProducer_Alpaka/plugins/alpaka/ClusterizerAlgo.h"

namespace ALPAKA_ACCELERATOR_NAMESPACE {
  using namespace cms::alpakatools;
  ////////////////////// 
  // Device functions //
  //////////////////////

   template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void set_vtx_range(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& osumtkwt, double& _beta){
    // These updates the range of vertices associated to each track through the kmin/kmax variables
    int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
    int threadIdx = alpaka::getIdx<alpaka::Block, alpaka::Threads>(acc)[0u]; // Thread number inside block
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    int maxVerticesPerBlock = (int) 512/blockSize; // Max vertices size is 512 over number of blocks in grid
    double zrange_min_= 0.1; // Hard coded as in CPU version
    for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){ // TODO:Saving and reading in the tracks dataformat might be a bit too much?
      // Based on current temperature (regularization term) and track position uncertainty, only keep relevant vertices
      double zrange     = std::max(cParams.zrange()/ sqrt((_beta) * tracks[itrack].oneoverdz2()), zrange_min_);
      double zmin       = tracks[itrack].z() - zrange;
      // First the lower bound
      int kmin = std::min((int) (maxVerticesPerBlock * blockIdx) + vertices.nV(blockIdx) - 1,  tracks[itrack].kmin()); //We might have deleted a vertex, so be careful if the track is in one extreme of the axis
      if (vertices[vertices[kmin].order()].z() > zmin){ // If the vertex position in z is bigger than the minimum, go down through all vertices position until finding one that is too far
          while ((kmin > maxVerticesPerBlock * blockIdx) && ((vertices[vertices[kmin-1].order()].z()) > zmin)) { // i.e., while we find another vertex within range that is before the previous initial step
          kmin--;
        }
      }
      else { // Otherwise go up
        while ((kmin < (maxVerticesPerBlock * blockIdx + (int) (vertices[blockIdx].nV()) - 1)) && ((vertices[vertices[kmin].order()].z()) < zmin)) { // Or it might happen that we have to take out vertices from the thing
          kmin++;
        }
      }
      // And now do the same for the upper bound
      double zmax       = tracks[itrack].z() + zrange;
      int kmax = std::min(maxVerticesPerBlock * blockIdx + (int) (vertices[blockIdx].nV()) - 1, (int) (tracks[itrack].kmax()) - 1);
      if (vertices[vertices[kmax].order()].z() < zmax) {
        while ((kmax < (maxVerticesPerBlock * blockIdx + (int) (vertices[blockIdx].nV())  - 1)) && ((vertices[vertices[kmax+1].order()].z()) < zmax)) { // As long as we have more vertex above kmax but within z range, we can add them to the collection, keep going
          kmax++;
        }
      }
      else { //Or maybe we have to restrict it
        while ((kmax > maxVerticesPerBlock * blockIdx) && (vertices[vertices[kmax].order()].z() > zmax)) {
          kmax--;
        }
      }
      if (kmin <= kmax){ // i.e. we have vertex associated to the track
        tracks[itrack].kmin() = (int) kmin;
	tracks[itrack].kmax() = (int) kmax;
      }
      else { // Otherwise, track goes in the most extreme vertex
        tracks[itrack].kmin() = (int) std::max(maxVerticesPerBlock * blockIdx, (int) std::min(kmin, kmax));
        tracks[itrack].kmax() = (int) std::min((maxVerticesPerBlock * blockIdx) + (int) vertices[blockIdx].nV(), (int) std::max(kmin, kmax) + 1);
      }
    } //end for
    alpaka::syncBlockThreads(acc);
  }

  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void update(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& osumtkwt, double& _beta, double rho0, bool updateTc){
    // Main function that updates the annealing parameters on each T step, computes all partition functions and so on
    int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
    int threadIdx = alpaka::getIdx<alpaka::Block, alpaka::Threads>(acc)[0u]; // Thread number inside block
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    int maxVerticesPerBlock = (int) 512/blockSize; // Max vertices size is 512 over number of blocks in grid
    double Zinit =  rho0 * exp(-(_beta) * cParams.dzCutOff() * cParams.dzCutOff()); // Initial partition function, really only used on the outlier rejection step to penalize
    for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){
      double botrack_dz2 = -(_beta) * tracks[itrack].oneoverdz2();
      tracks[itrack].sum_Z() = Zinit;
      for (int ivertexO = tracks[itrack].kmin(); ivertexO < tracks[itrack].kmax() ; ++ivertexO){
        int ivertex = vertices[ivertexO].order(); // Remember to always take ordering from here when dealing with vertices
	double mult_res = tracks[itrack].z() - vertices[ivertex].z();
	tracks[itrack].vert_exparg()[ivertex] = botrack_dz2*mult_res*mult_res; // -beta*(z_t-z_v)/dz^2
	tracks[itrack].vert_exp()[ivertex]    = exp(tracks[itrack].vert_exparg()[ivertex] ); // e^{-beta*(z_t-z_v)/dz^2}
        tracks[itrack].sum_Z()      += vertices[ivertex].rho()*tracks[itrack].vert_exp()[ivertex]; // Z_t = sum_v pho_v * e^{-beta*(z_t-z_v)/dz^2}, partition function of the track
      } //end vertex for
      if(not(std::isfinite(tracks[itrack].sum_Z()))) tracks[itrack].sum_Z() = 0; // Just in case something diverges
      if(tracks[itrack].sum_Z()>1e-100){ // If non-zero then the track has a non-trivial assignment to a vertex
        double sumw = tracks[itrack].weight()/tracks[itrack].sum_Z();
  	for (int ivertexO = tracks[itrack].kmin(); ivertexO < tracks[itrack].kmax() ; ++ivertexO){
          int ivertex = vertices[ivertexO].order(); // Remember to always take ordering from here when dealing with vertices
          tracks[itrack].vert_se()[ivertex] = tracks[itrack].vert_exp()[ivertex] * sumw; // From partition of track to contribution of track to vertex partition
          double w = vertices[ivertex].rho() * tracks[itrack].vert_exp()[ivertex] * sumw * tracks[itrack].oneoverdz2(); 
          tracks[itrack].vert_sw()[ivertex]  = w; // Contribution of track to vertex as weight
          tracks[itrack].vert_swz()[ivertex] = w * tracks[itrack].z(); // Weighted track position
          if (updateTc){ 
	    tracks[itrack].vert_swE()[ivertex] = -w * tracks[itrack].vert_exparg()[ivertex]/(_beta); // Only need it when changing the Tc (i.e. after a split), to recompute it
	  }
	  else{
	    tracks[itrack].vert_swE()[ivertex] = 0;
	  }
        } //end vertex for
      } //end if
    } //end track for
    alpaka::syncBlockThreads(acc);
    // After the track-vertex matrix assignment, we need to add up across vertices. This time, we use one thread per vertex
    for (int ivertexO = maxVerticesPerBlock * blockIdx + threadIdx; ivertexO < maxVerticesPerBlock * blockIdx + vertices[blockIdx].nV() ; ivertexO += blockSize){
      vertices[ivertexO].se() = 0.;
      vertices[ivertexO].sw() = 0.;
      vertices[ivertexO].swz() = 0.;
      vertices[ivertexO].aux1() = 0.;
      if (updateTc) vertices[ivertexO].swE() = 0.;
    } // end vertex for
    for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){
      for (int ivertexO = tracks[itrack].kmin(); ivertexO < tracks[itrack].kmax() ; ++ivertexO){
	// TODO: these atomics are going to be very slow. Can we optimize?
        int ivertex = vertices[ivertexO].order(); // Remember to always take ordering from here when dealing with vertices
        alpaka::atomicAdd(acc, &vertices[ivertex].se(), tracks[itrack].vert_se()[ivertex], alpaka::hierarchy::Threads{});
        alpaka::atomicAdd(acc, &vertices[ivertex].sw(), tracks[itrack].vert_sw()[ivertex], alpaka::hierarchy::Threads{});
        alpaka::atomicAdd(acc, &vertices[ivertex].swz(), tracks[itrack].vert_swz()[ivertex], alpaka::hierarchy::Threads{});
        if (updateTc) alpaka::atomicAdd(acc, &vertices[ivertex].swE(), tracks[itrack].vert_swE()[ivertex], alpaka::hierarchy::Threads{});
      } // end for
    }
    alpaka::syncBlockThreads(acc);
    // Last, evalute vertex properties
    for (int ivertexO = maxVerticesPerBlock * blockIdx + threadIdx; ivertexO < maxVerticesPerBlock * blockIdx + vertices[blockIdx].nV() ; ivertexO += blockSize){
      int ivertex = vertices[ivertexO].order(); // Remember to always take ordering from here when dealing with vertices
      if (vertices[ivertex].sw() > 0){ // If any tracks were assigned, update
        double znew = vertices[ivertex].swz()/vertices[ivertex].sw();
	vertices[ivertex].aux1() = abs(znew - vertices[ivertex].z()); // How much the vertex moved which we need to determine convergence in thermalize
	vertices[ivertex].z() = znew;
      }
      vertices[ivertex].rho() = vertices[ivertex].rho()*vertices[ivertex].se()*osumtkwt; // This is the 'size' or 'mass' of the vertex  
    } // end vertex for
    alpaka::syncBlockThreads(acc);
  } //end update

  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void merge(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& osumtkwt, double& _beta){
    // If two vertex are too close together, merge them
    int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
    int threadIdx = alpaka::getIdx<alpaka::Block, alpaka::Threads>(acc)[0u]; // Thread number inside block
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    int maxVerticesPerBlock = (int) 512/blockSize; // Max vertices size is 512 over number of blocks in grid
    int nprev = vertices[blockIdx].nV();
    if (nprev < 2) return;
    for (int ivertexO = maxVerticesPerBlock * blockIdx + threadIdx; ivertexO < maxVerticesPerBlock * blockIdx + vertices[blockIdx].nV() ; ivertexO += blockSize){
      int ivertex = vertices[ivertexO].order();
      int ivertexnext = vertices[ivertexO+1].order();
      vertices[ivertex].aux1() = abs(vertices[ivertex].z() - vertices[ivertexnext].z());
    }
    alpaka::syncBlockThreads(acc);
    // Sorter things
    auto& critical_dist = alpaka::declareSharedVar<float[128], __COUNTER__>(acc);
    auto& critical_index = alpaka::declareSharedVar<float[128], __COUNTER__>(acc);
    int& ncritical = alpaka::declareSharedVar<int, __COUNTER__>(acc);

    if (once_per_block(acc)){
      ncritical = 0;
      for (int ivertexO = maxVerticesPerBlock * blockIdx + threadIdx; ivertexO < maxVerticesPerBlock * blockIdx + vertices[blockIdx].nV() ; ivertexO += blockSize){
        int ivertex = vertices[ivertexO].order();
        if (vertices[ivertex].aux1() < cParams.zmerge()){ // i.e., if we are to split the vertex
          critical_dist[ncritical] = abs(vertices[ivertex].aux1());
          critical_index[ncritical] = ivertexO;
          ncritical++;
          if (ncritical > 128) break;
        }
      }
    } // end once_per_block
    alpaka::syncBlockThreads(acc);
    if (ncritical == 0) return;
    for (int sortO = 0; sortO < ncritical ; ++sortO){ // All threads are running the same code, to know where to exit, which is clunky
      if (ncritical == 0 || maxVerticesPerBlock == nprev) return;
      int ikO = 0;
      double minVal = 999999.;
      for (int sort1 = 0; sort1 < ncritical; ++sort1){
        if (critical_dist[sort1] > minVal){
          minVal = critical_dist[sort1];
          ikO    = sort1;
        }
      }
      critical_dist[ikO] = 9999999.;
      int ivertexO    = critical_index[ikO];
      int ivertex     = vertices[ivertexO].order();  // This will be splitted
      int ivertexnext = blockIdx * maxVerticesPerBlock + nprev -1;
      // A little bit of safety here. First is needed to avoid reading the -1 entry of vertices->order. Second is not as far as we don't go over 511 vertices, but better keep it just in case
      if (ivertexO < blockIdx * maxVerticesPerBlock + nprev -1) ivertexnext = vertices[ivertexO+1].order();  // This will be used in a couple of computations
      alpaka::syncBlockThreads(acc);
      if (once_per_block(acc)){ // Really no way of parallelizing this I'm afraid
        vertices[ivertex].isGood() = false; // Delete it!
        double rho =  vertices[ivertex].rho() + vertices[ivertexnext].rho();
        if (rho > 1.e-100){ 
          vertices[ivertexnext].z() = (vertices[ivertex].rho() * vertices[ivertex].z() + vertices[ivertexnext].rho() * vertices[ivertexnext].z()) / rho;
        } 
        else{
          vertices[ivertexnext].z() = 0.5 * (vertices[ivertex].z() + vertices[ivertexnext].z());
        } 
        vertices[ivertexnext].rho()  = rho;
        vertices[ivertexnext].sw()  += vertices[ivertex].sw();
        for (int ivertexOO = ivertexO ; ivertexOO < maxVerticesPerBlock * blockIdx + nprev -1 ; ++ivertexOO){
          vertices[ivertexOO].order() = vertices[ivertexOO+1].order();
        }
        vertices[blockIdx].nV() = vertices[blockIdx].nV()-1; // Also update nvertex
      } // end once_per_block
      alpaka::syncBlockThreads(acc); 
      for (int resort = 0; resort < ncritical ; ++resort){
        if (critical_index[resort] > ivertexO) critical_index[resort]--; // critical_index refers to the original vertices->order, so it needs to be updated 
      }
      nprev = vertices[blockIdx].nV(); // And to the counter of previous vertices
      for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){
        if (tracks[itrack].kmax() > ivertexO) tracks[itrack].kmax()--;
        if ((tracks[itrack].kmin() > ivertexO) || ((tracks[itrack].kmax() < (tracks[itrack].kmin() + 1)) && (tracks[itrack].kmin() > maxVerticesPerBlock*blockIdx))) tracks[itrack].kmin()--;
      }
      alpaka::syncBlockThreads(acc);
      set_vtx_range(acc, tracks, vertices, cParams, osumtkwt, _beta);
      return; 
    }
    alpaka::syncBlockThreads(acc);
    set_vtx_range(acc, tracks, vertices, cParams, osumtkwt, _beta);
    alpaka::syncBlockThreads(acc);
  }

  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void split(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& osumtkwt, double& _beta, double threshold){
    int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
    int threadIdx = alpaka::getIdx<alpaka::Block, alpaka::Threads>(acc)[0u]; // Thread number inside block
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    int maxVerticesPerBlock = (int) 512/blockSize; // Max vertices size is 512 over number of blocks in grid
    update(acc, tracks, vertices, cParams, osumtkwt, _beta, 0.0, false); // Update positions after merge
    alpaka::syncBlockThreads(acc);
    double epsilon = 1e-3;
    int nprev = vertices[blockIdx].nV();
    // Set critical T for all vertices
    for (int ivertexO = maxVerticesPerBlock * blockIdx + threadIdx; ivertexO < maxVerticesPerBlock * blockIdx + vertices[blockIdx].nV() ; ivertexO += blockSize){
      int ivertex = vertices[ivertexO].order(); // Remember to always take ordering from here when dealing with vertices
      double Tc = 2 * vertices[ivertex].swE() / vertices[ivertex].sw();
      vertices[ivertex].aux1() = Tc;
    }
    alpaka::syncBlockThreads(acc);
    // Sorter things
    auto& critical_temp = alpaka::declareSharedVar<float[128], __COUNTER__>(acc);
    auto& critical_index = alpaka::declareSharedVar<float[128], __COUNTER__>(acc);
    int& ncritical = alpaka::declareSharedVar<int, __COUNTER__>(acc);
    // Information for the vertex splitting properties
    double& p1 = alpaka::declareSharedVar<double, __COUNTER__>(acc);
    double& p2 = alpaka::declareSharedVar<double, __COUNTER__>(acc);
    double& z1 = alpaka::declareSharedVar<double, __COUNTER__>(acc);
    double& z2 = alpaka::declareSharedVar<double, __COUNTER__>(acc);
    double& w1 = alpaka::declareSharedVar<double, __COUNTER__>(acc);
    double& w2 = alpaka::declareSharedVar<double, __COUNTER__>(acc);

    if (once_per_block(acc)){
      ncritical = 0;
      for (int ivertexO = maxVerticesPerBlock * blockIdx + threadIdx; ivertexO < maxVerticesPerBlock * blockIdx + vertices[blockIdx].nV() ; ivertexO += blockSize){
        int ivertex = vertices[ivertexO].order();
        if (vertices[ivertex].aux1() * _beta > threshold){ // i.e., if we are to split the vertex
          critical_temp[ncritical] = abs(vertices[ivertex].aux1());
	  critical_index[ncritical] = ivertexO;
	  ncritical++;
	  if (ncritical > 128) break;
        }
      }
    } // end once_per_block
    alpaka::syncBlockThreads(acc);
    if (ncritical == 0 || maxVerticesPerBlock == nprev) return;
    for (int sortO = 0; sortO < ncritical ; ++sortO){ // All threads are running the same code, to know where to exit, which is clunky
      if (ncritical == 0 || maxVerticesPerBlock == nprev) return;
      int ikO = 0;
      double maxVal = -1.;
      for (int sort1 = 0; sort1 < ncritical; ++sort1){
        if (critical_temp[sort1] > maxVal){
          maxVal = critical_temp[sort1];
          ikO    = sort1;
        }
      }
      critical_temp[ikO] = -1.;
      int ivertexO    = critical_index[ikO];
      int ivertex     = vertices[ivertexO].order();  // This will be splitted
      int ivertexprev = blockIdx * maxVerticesPerBlock;
      int ivertexnext = blockIdx * maxVerticesPerBlock + nprev -1; 
      // A little bit of safety here. First is needed to avoid reading the -1 entry of vertices->order. Second is not as far as we don't go over 511 vertices, but better keep it just in case
      if (ivertexO > blockIdx * maxVerticesPerBlock) ivertexprev = vertices[ivertexO-1].order();  // This will be used in a couple of computations
      if (ivertexO < blockIdx * maxVerticesPerBlock + nprev -1) ivertexnext = vertices[ivertexO+1].order();  // This will be used in a couple of computations
      if (once_per_block(acc)){
        p1 = 0.;
	p2 = 0.;
	z1 = 0.;
	z2 = 0.;
	w1 = 0.;
	w2 = 0.;
      }
      alpaka::syncBlockThreads(acc);
      for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){
        if (tracks[itrack].sum_Z() > 1.e-100) {
          // winner-takes-all, usually overestimates splitting
          double tl = tracks[itrack].z() < vertices[ivertex].z() ? 1. : 0.;
          double tr = 1. - tl;
          // soften it, especially at low T
          double arg = (tracks[itrack].z() - vertices[ivertex].z()) * sqrt((_beta) * tracks[itrack].oneoverdz2());
          if (abs(arg) < 20) {
            double t = exp(-arg);
            tl = t / (t + 1.);
            tr = 1 / (t + 1.);
          }
	  // Recompute split vertex quantities
          double p = vertices[ivertex].rho() * tracks[itrack].weight() * exp(-(_beta) * (tracks[itrack].z()-vertices[ivertex].z())*(tracks[itrack].z()-vertices[ivertex].z())* tracks[itrack].oneoverdz2())/ tracks[itrack].sum_Z();
          double w = p * tracks[itrack].oneoverdz2();
	  alpaka::atomicAdd(acc, &p1, p*tl, alpaka::hierarchy::Threads{});
	  alpaka::atomicAdd(acc, &p2, p*tr, alpaka::hierarchy::Threads{});
	  alpaka::atomicAdd(acc, &z1, w*tl*tracks[itrack].z(), alpaka::hierarchy::Threads{});
	  alpaka::atomicAdd(acc, &z2, p*tr*tracks[itrack].z(), alpaka::hierarchy::Threads{});
	  alpaka::atomicAdd(acc, &w1, w*tl, alpaka::hierarchy::Threads{});
	  alpaka::atomicAdd(acc, &w2, w*tr, alpaka::hierarchy::Threads{});
        }
      }
      alpaka::syncBlockThreads(acc);
      if (once_per_block(acc)){
	// If one vertex is taking all the things, then set the others slightly off to help splitting
        if (w1 > 0){
          z1 = z1/w1;
	}
        else{
	  z1 = vertices[ivertex].z() - epsilon;	
	}
	if (w2 > 0){
          z2 = z2/w2;
        }
        else{
          z2 = vertices[ivertex].z() + epsilon;
        }
        // If there is not enough room, reduce split size
	if ((ivertexO > maxVerticesPerBlock*blockIdx) && (z1 < (0.6 * vertices[ivertex].z() + 0.4 * vertices[ivertexprev].z()))) { // First in the if is ivertexO, as we care on whether the vertex is the leftmost or rightmost
          z1 = 0.6 * vertices[ivertex].z() + 0.4 * vertices[ivertexprev].z();
        }
        if ((ivertexO < maxVerticesPerBlock* blockIdx +  nprev - 1) && (z2 > (0.6 * vertices[ivertex].z() + 0.4 * vertices[ivertexnext].z()))) {
          z2 = 0.6 * vertices[ivertex].z() + 0.4 * vertices[ivertexnext].z();
        }
      } // end once_per_block
      // Now save the properties of the new stuff
      alpaka::syncBlockThreads(acc);
      int nnew = 999999;
      if (abs(z2-z2) > epsilon){ 
        // Find the first empty index to save the vertex
        for (int icheck = maxVerticesPerBlock * blockIdx ; icheck < maxVerticesPerBlock * (blockIdx + 1); icheck ++ ){
          if (not(vertices[icheck].isGood())){
            nnew = icheck;
            break;
          }
        }
        if (nnew == 999999) break;
      }
      if (once_per_block(acc)){
        double pk1 = p1 * vertices[ivertex].rho() / (p1 + p2);
        double pk2 = p2 * vertices[ivertex].rho() / (p1 + p2);
        vertices[ivertex].z() = z2;
        vertices[ivertex].rho() = pk2;
        // Insert it into the first available slot           
        vertices[nnew].z()     = z1; 
        vertices[nnew].rho()   = pk1; 
        // And register it as used
        vertices[nnew].isGood()= true;
        // TODO:: this is likely not needed as far as it is reset anytime we call update
        vertices[nnew].sw()     = 0.;
        vertices[nnew].se()     = 0.;
        vertices[nnew].swz()    = 0.;
        vertices[nnew].swE()    = 0.;
        vertices[nnew].exp()    = 0.;
        vertices[nnew].exparg() = 0.;
	for (int ivnew = maxVerticesPerBlock * blockIdx +  nprev ; ivnew > ivertexO ; ivnew--){ // As we add a vertex, we update from the back downwards
          vertices[ivnew].order() = vertices[ivnew-1].order();
        }
	vertices[ivertexO].order() = nnew;
	vertices[blockIdx].nV() += 1;
      }
      alpaka::syncBlockThreads(acc);
      // Now, update kmin/kmax for all tracks
      for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){
        if (tracks[itrack].kmin() > ivertexO) tracks[itrack].kmin()++;
        if ((tracks[itrack].kmax() >= ivertexO) || (tracks[itrack].kmax() == tracks[itrack].kmin())) tracks[itrack].kmax()++;	
      }
      nprev = vertices[blockIdx].nV();
      if (once_per_block(acc)){
        // If we did a splitting or old sorted list of vertex index is scrambled, so we need to fix it
        for (int resort = 0; resort < ncritical ; ++resort){
          if (critical_index[resort] > ivertexO) critical_index[resort]++; // critical_index refers to the original vertices->order, so it needs to be updated
        }
      }
      alpaka::syncBlockThreads(acc);
    }
    alpaka::syncBlockThreads(acc);
  }
  
  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void purge(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& osumtkwt, double& _beta, double rho0){
    // Remove repetitive or low quality entries
    int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
    int threadIdx = alpaka::getIdx<alpaka::Block, alpaka::Threads>(acc)[0u]; // Thread number inside block
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    int maxVerticesPerBlock = (int) 512/blockSize; // Max vertices size is 512 over number of blocks in grid
    if (vertices[blockIdx].nV() < 2) return;
    double eps = 1e-100;
    int nunique_min = 2;
    double rhoconst = rho0*exp(-_beta*(cParams.dzCutOff()*cParams.dzCutOff()));
    int nprev = vertices[blockIdx].nV();
    // Reassign
    set_vtx_range(acc, tracks, vertices, cParams, osumtkwt, _beta);
    for (int ivertexO = maxVerticesPerBlock * blockIdx + threadIdx; ivertexO < maxVerticesPerBlock * blockIdx + vertices[blockIdx].nV() ; ivertexO += blockSize){
      int ivertex = vertices[ivertexO].order(); // Remember to always take ordering from here when dealing with vertices
      vertices[ivertex].aux1() = 0; // sum of track-vertex probabilities
      vertices[ivertex].aux2() = 0; // number of uniquely assigned tracks
    }
    alpaka::syncBlockThreads(acc);
    // Get quality of vertex in terms of #Tracks and sum of track probabilities
    for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){
      double track_aux1 = ((tracks[itrack].sum_Z() > eps) && (tracks[itrack].weight() > cParams.uniquetrkminp())) ? 1./tracks[itrack].sum_Z() : 0.;
      for (int ivertexO = tracks[itrack].kmin(); ivertexO < tracks[itrack].kmax() ; ++ivertexO){
        int ivertex = vertices[ivertexO].order(); // Remember to always take ordering from here when dealing with vertices
	double ppcut = cParams.uniquetrkweight() * vertices[ivertex].rho() / (vertices[ivertex].rho()+rhoconst);
	double track_vertex_aux1 = exp(-(_beta)*tracks[itrack].oneoverdz2() * ( (tracks[itrack].z()-vertices[ivertex].z())*(tracks[itrack].z()-vertices[ivertex].z()) ));
        double p = vertices[ivertex].rho()*track_vertex_aux1*track_aux1; // The whole track-vertex P_ij = rho_j*p_ij*p_i
        alpaka::atomicAdd(acc, &vertices[ivertex].aux1(), p, alpaka::hierarchy::Threads{});
        if (p>ppcut) {
          alpaka::atomicAdd(acc, &vertices[ivertex].aux2(), 1., alpaka::hierarchy::Threads{});
        }
      }
    }
    alpaka::syncBlockThreads(acc);
    // Find worst vertex to purge
    int& k0 = alpaka::declareSharedVar<int, __COUNTER__>(acc);

    if (once_per_block(acc)){
      double sumpmin = tracks.nT(); // So it is always bigger than aux for any vertex
      k0 = maxVerticesPerBlock * blockIdx + nprev;
      for (int ivertexO = maxVerticesPerBlock * blockIdx + threadIdx; ivertexO < maxVerticesPerBlock * blockIdx + (int) vertices[blockIdx].nV() ; ivertexO += blockSize){
        int ivertex = vertices[ivertexO].order();
        if ((vertices[ivertex].aux2() < nunique_min) && (vertices[ivertex].aux1() < sumpmin)){
          // Will purge 
	  sumpmin = vertices[ivertex].aux1();
	  k0 = ivertexO;
        }
      } // end vertex for
      if (k0 != (int) (maxVerticesPerBlock * blockIdx + nprev)){
        for (int ivertexOO = k0; ivertexOO < maxVerticesPerBlock * blockIdx + (int) nprev - 1; ++ivertexOO){ // TODO:: Any tricks here to multithread? I don't think so
          vertices[ivertexOO].order() =vertices[ivertexOO+1].order(); // Update vertex order taking out the purged one
        }
        vertices[blockIdx].nV()--; // Also update nvertex
      }
    }// end once_per_block 
    if (k0 != (int) (maxVerticesPerBlock * blockIdx + (int) nprev)){
      for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){
        if (tracks[itrack].kmax() > k0) tracks[itrack].kmax()--;
	if ((tracks[itrack].kmin() > k0) || ((tracks[itrack].kmax() < (tracks[itrack].kmin() + 1)) && (tracks[itrack].kmin() > (int) (maxVerticesPerBlock * blockIdx)))) tracks[itrack].kmin()--;   
      }
    } // end if 
    alpaka::syncBlockThreads(acc);
    if (nprev != vertices[blockIdx].nV()){
      set_vtx_range(acc, tracks, vertices, cParams, osumtkwt, _beta);
    }
  }

  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void initialize(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams){
    // Initialize all vertices as empty, a single vertex in each block will be initialized with all tracks associated to it
    int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
    int threadIdx = alpaka::getIdx<alpaka::Block, alpaka::Threads>(acc)[0u]; // Thread number inside block
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    int maxVerticesPerBlock = (int) 512/blockSize; // Max vertices size is 512 over number of blocks in grid
    vertices[blockIdx].nV() = 1; // We start with one vertex per block
    for (int ivertex = threadIdx+maxVerticesPerBlock*blockIdx; ivertex < maxVerticesPerBlock*(blockIdx+1); ivertex+=blockSize){ // Initialize vertices in parallel in the block
      vertices[ivertex].sw() = 0.;
      vertices[ivertex].se() = 0.;
      vertices[ivertex].swz() = 0.;
      vertices[ivertex].swE() = 0.;
      vertices[ivertex].exp() = 0.;
      vertices[ivertex].exparg() = 0.;
      vertices[ivertex].z() = 0.;
      vertices[ivertex].rho() = 0.;
      vertices[ivertex].isGood() = false;
      vertices[ivertex].order() = 9999;
      if (ivertex == maxVerticesPerBlock*blockIdx){ // Now set up the initial single vetex containing everything
        // TODO:: Probably there is a cleaner way of doing this in Alpaka
	vertices[ivertex].rho() = 1.;
	vertices[ivertex].order() = maxVerticesPerBlock*blockIdx;
	vertices[ivertex].isGood() = true;
      } 
    } // end for
    alpaka::syncBlockThreads(acc);
    // Now assign all tracks in the block to the single vertex
    for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){ // Technically not a loop as each thread will have one track in the per block approach, but in the more general case this can be extended to BlockSize in Alpaka != BlockSize in algorithm
      tracks.kmin(itrack) = maxVerticesPerBlock*blockIdx; // Tracks are associated to vertex in list kmin, kmin+1,... kmax-1, so this just assign all tracks to the vertex we just created!
      tracks.kmax(itrack) = maxVerticesPerBlock*blockIdx + 1;
    }
    if (once_per_block(acc)){
      for (int ivertex =0 ; ivertex < maxVerticesPerBlock*(blockIdx+1); ivertex += 1){
        vertices[ivertex].sw() = 0.;
        vertices[ivertex].se() = 0.;
        vertices[ivertex].swz() = 0.;
        vertices[ivertex].swE() = 0.;
        vertices[ivertex].exp() = 0.;
        vertices[ivertex].exparg() = 0.;
        vertices[ivertex].z() = 0.;
        vertices[ivertex].rho() = 0.;
        vertices[ivertex].isGood() = false;
        vertices[ivertex].order() = 9999;
        if (ivertex == maxVerticesPerBlock*blockIdx){ // Now set up the initial single vertex containing everything
          // TODO:: Probably there is a cleaner way of doing this in Alpaka
          vertices[ivertex].rho() = 1.;
          vertices[ivertex].order() = maxVerticesPerBlock*blockIdx;
          vertices[ivertex].isGood() = true;
        } 
      }
      for (int itrack = 0 ; itrack < (blockIdx+1)*blockSize ; itrack += blockSize){
        tracks.kmin(itrack) = maxVerticesPerBlock*blockIdx;
	tracks.kmax(itrack) = maxVerticesPerBlock*blockIdx + 1;
      }
    }
    alpaka::syncBlockThreads(acc);
  }
  
  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void getBeta0(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& _beta){
    // Computes first critical temperature
    int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
    int threadIdx = alpaka::getIdx<alpaka::Block, alpaka::Threads>(acc)[0u]; // Thread number inside block
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    int maxVerticesPerBlock = (int) 512/blockSize; // Max vertices size is 512 over number of blocks in grid
    for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){
      tracks[itrack].aux1() = tracks[itrack].weight()*tracks[itrack].oneoverdz2();  // Weighted weight
      tracks[itrack].aux2() = tracks[itrack].weight()*tracks[itrack].oneoverdz2()*tracks[itrack].z(); // Weighted position
    }
    // Initial vertex position
    alpaka::syncBlockThreads(acc);
    double& wnew = alpaka::declareSharedVar<double, __COUNTER__>(acc);
    double& znew = alpaka::declareSharedVar<double, __COUNTER__>(acc);
    if (once_per_block(acc)){
      wnew = 0.;
      znew = 0.;
    }
    alpaka::syncBlockThreads(acc);
    for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){ // TODO:Saving and reading in the tracks dataformat might be a bit too much?
      alpaka::atomicAdd(acc, &wnew, tracks[itrack].aux1(), alpaka::hierarchy::Threads{});
      alpaka::atomicAdd(acc, &znew, tracks[itrack].aux2(), alpaka::hierarchy::Threads{});
    }
    alpaka::syncBlockThreads(acc);
    if (once_per_block(acc)){
      vertices[maxVerticesPerBlock*blockIdx].z() = znew/wnew;
      znew = 0.;
    }
    alpaka::syncBlockThreads(acc);
    // Now do a chi-2 like of all tracks and save it again in znew
    for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){
      tracks[itrack].aux2() = tracks[itrack].aux1()*(vertices[maxVerticesPerBlock*blockIdx].z() - tracks[itrack].z() )*(vertices[maxVerticesPerBlock*blockIdx].z() - tracks[itrack].z())*tracks[itrack].oneoverdz2();
      alpaka::atomicAdd(acc, &znew, tracks[itrack].aux2(), alpaka::hierarchy::Threads{});
    }
    alpaka::syncBlockThreads(acc);
    if (once_per_block(acc)){
      _beta = 2 * znew/wnew; // 1/beta_C, or T_C
      if (_beta > cParams.TMin()){ // If T_C > T_Min we have a game to play
        int coolingsteps = 1 - int(std::log(_beta/ cParams.TMin()) / std::log(cParams.coolingFactor())); // A tricky conversion to round the number of cooling steps
        _beta = std::pow(cParams.coolingFactor(), coolingsteps)/cParams.TMin(); // First cooling step
      }
      else _beta = cParams.coolingFactor()/cParams.TMin(); // Otherwise, just one step
    }
    alpaka::syncBlockThreads(acc);
  }

  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void thermalize(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& osumtkwt, double& _beta, double delta_highT, double rho0){
    // At a fixed temperature, iterate vertex position update until stable
    int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    int maxVerticesPerBlock = (int) 512/blockSize; // Max vertices size is 512 over number of blocks in grid
    // Thermalizing iteration
    int niter = 0; 
    double zrange_min_ = 0.01; // Hard coded as in CPU
    double delta_max = cParams.delta_lowT();
    alpaka::syncBlockThreads(acc);
    // Stepping definition
    if (cParams.convergence_mode() == 0){
      delta_max = delta_highT;
    }
    else if (cParams.convergence_mode() == 1){
      delta_max = cParams.delta_lowT() / sqrt(std::max(_beta, 1.0));
    }
    int maxIterations = 1000;
    alpaka::syncBlockThreads(acc);
    // Always start by resetting track-vertex assignment
    set_vtx_range(acc, tracks, vertices, cParams, osumtkwt, _beta);
    alpaka::syncBlockThreads(acc);
    // Accumulator of variations
    double delta_sum_range = 0;
    while (niter++ < maxIterations){ // Loop until vertex position change is small
      // One iteration of new vertex positions
      update(acc, tracks, vertices, cParams, osumtkwt, _beta, rho0, false);
      alpaka::syncBlockThreads(acc);
      // One iteration of max variation
      double dmax = 0.;
      for (int ivertexO = maxVerticesPerBlock*blockIdx ; ivertexO < maxVerticesPerBlock*blockIdx + vertices[blockIdx].nV(); ivertexO++){ // TODO::Currently we are doing this in all threads in parallel, might be optimized in other way to multithread max finding?
        int ivertex = vertices[ivertexO].order();
        if (vertices[ivertex].aux1() >= dmax) dmax = vertices[ivertex].aux1();
      }
      delta_sum_range += dmax;
      alpaka::syncBlockThreads(acc);
      if (delta_sum_range > zrange_min_ && dmax > zrange_min_) {  // I.e., if a vertex moved too much we reassign
        set_vtx_range(acc, tracks, vertices, cParams, osumtkwt, _beta);
	delta_sum_range = 0.;
      }
      alpaka::syncBlockThreads(acc);
      if (dmax < delta_max){ // If it moved too little, we stop updating
        break;
      }
    } // end while
  } // thermalize

  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void coolingWhileSplitting(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& osumtkwt, double& _beta){
    // Perform cooling of the deterministic annealing
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    double betafreeze = (1./cParams.TMin()) * sqrt(cParams.coolingFactor()); // Last temperature
    while (_beta < betafreeze){ // The cooling loop
      alpaka::syncBlockThreads(acc);
      int nprev = vertices[blockIdx].nV();
      alpaka::syncBlockThreads(acc);
      merge(acc, tracks, vertices, cParams, osumtkwt, _beta);
      alpaka::syncBlockThreads(acc);
      while (nprev !=  vertices[blockIdx].nV() ) { // If we are here, we merged before, keep merging until stable
        nprev = vertices[blockIdx].nV();
	alpaka::syncBlockThreads(acc);
	update(acc, tracks, vertices, cParams, osumtkwt, _beta, 0.0, false); // Update positions after merge
	alpaka::syncBlockThreads(acc);
	merge(acc, tracks, vertices, cParams, osumtkwt, _beta);
	alpaka::syncBlockThreads(acc);
      } // end while after merging
      split(acc, tracks, vertices, cParams, osumtkwt, _beta, 1.0); // As we are close to a critical temperature, check if we need to split and if so, do it
      alpaka::syncBlockThreads(acc);
      if (once_per_block(acc)){ // Cool down
	_beta = _beta/cParams.coolingFactor();
      }
      alpaka::syncBlockThreads(acc);
      thermalize(acc, tracks, vertices, cParams, osumtkwt, _beta, cParams.delta_highT(), 0.0); // Stabilize positions after cooling
      alpaka::syncBlockThreads(acc);
      set_vtx_range(acc, tracks, vertices, cParams, osumtkwt, _beta); // Reassign tracks to vertex
      alpaka::syncBlockThreads(acc);
      update(acc, tracks, vertices, cParams, osumtkwt, _beta, 0.0, false); // Last, update positions again
      alpaka::syncBlockThreads(acc);
    }
  } // end coolingWhileSplitting

  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void reMergeTracks(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& osumtkwt, double& _beta){
    // After the cooling, we merge any closeby vertices
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    int nprev = vertices[blockIdx].nV();
    merge(acc, tracks, vertices, cParams, osumtkwt, _beta);
    while (nprev !=  vertices[blockIdx].nV() ) { // If we are here, we merged before, keep merging until stable
      set_vtx_range(acc, tracks, vertices, cParams, osumtkwt, _beta); // Reassign tracks to vertex
      alpaka::syncBlockThreads(acc);
      update(acc, tracks, vertices, cParams, osumtkwt, _beta, 0.0, false); // Update before any final merge
      alpaka::syncBlockThreads(acc);
      nprev = vertices[blockIdx].nV();
      merge(acc, tracks, vertices, cParams, osumtkwt, _beta);
      alpaka::syncBlockThreads(acc);
    } // end while
  } // end reMergeTracks
  
  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void reSplitTracks(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& osumtkwt, double& _beta){
    // Last splitting at the minimal temperature which is a bit more permissive
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    int ntry = 0; 
    double threshold = 1.0;
    int nprev = vertices[blockIdx].nV();
    split(acc, tracks, vertices, cParams, osumtkwt, _beta, threshold);
    while (nprev !=  vertices[blockIdx].nV() && (ntry++ < 10)) {
      thermalize(acc, tracks, vertices, cParams, osumtkwt, _beta, cParams.delta_highT(), 0.0);
      alpaka::syncBlockThreads(acc);
      nprev = vertices[blockIdx].nV();
      merge(acc, tracks, vertices, cParams, osumtkwt, _beta);
      alpaka::syncBlockThreads(acc);
      while (nprev !=  vertices[blockIdx].nV() ) {
	nprev = vertices[blockIdx].nV();
        update(acc, tracks, vertices, cParams, osumtkwt, _beta, 0.0, false);
	alpaka::syncBlockThreads(acc);
        merge(acc, tracks, vertices, cParams, osumtkwt, _beta);
	alpaka::syncBlockThreads(acc);
      }
      threshold *= 1.1; // Make it a bit easier to split
      split(acc, tracks, vertices, cParams, osumtkwt, _beta, threshold);
      alpaka::syncBlockThreads(acc);
    }
  }

  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void rejectOutliers(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, double& osumtkwt, double& _beta){
    // Treat outliers, either low quality vertex, or those with very far away tracks
    int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid
    double rho0 = 0.0; // Yes, here is where this thing is used
    if (cParams.dzCutOff() > 0){
      rho0 = vertices[blockIdx].nV() > 1 ? 1./vertices[blockIdx].nV() : 1.;
      for (int rhoindex = 0; rhoindex < 5 ; rhoindex++){ //Can't be parallelized in any reasonable way
        update(acc, tracks, vertices, cParams, osumtkwt, _beta, rhoindex*rho0/5., false);
        alpaka::syncBlockThreads(acc);
      }
    } // end if
    thermalize(acc, tracks, vertices, cParams, osumtkwt, _beta, cParams.delta_lowT(), rho0);
    int nprev = vertices[blockIdx].nV();
    alpaka::syncBlockThreads(acc);
    merge(acc, tracks, vertices, cParams, osumtkwt, _beta);
    alpaka::syncBlockThreads(acc);
    while (nprev !=  vertices[blockIdx].nV()) {
      set_vtx_range(acc, tracks, vertices, cParams, osumtkwt, _beta); // Reassign tracks to vertex
      alpaka::syncBlockThreads(acc);
      update(acc, tracks, vertices, cParams, osumtkwt, _beta, rho0, false); // At rho0 it changes the initial value of the partition function
      alpaka::syncBlockThreads(acc);
      nprev = vertices[blockIdx].nV();
      merge(acc, tracks, vertices, cParams, osumtkwt, _beta);
      alpaka::syncBlockThreads(acc);
    }
    while (_beta < 1./cParams.Tpurge()){ // Cool down to purge temperature
      alpaka::syncBlockThreads(acc);
      if (once_per_block(acc)){ // Cool down
        _beta = std::min(_beta/cParams.coolingFactor(), 1./cParams.Tpurge());
      }
      alpaka::syncBlockThreads(acc);
      thermalize(acc, tracks, vertices, cParams, osumtkwt, _beta, cParams.delta_lowT(), rho0);
    }
    alpaka::syncBlockThreads(acc);
    // And now purge
    nprev = vertices[blockIdx].nV();
    purge(acc, tracks, vertices, cParams, osumtkwt, _beta, rho0);
    while (nprev !=  vertices[blockIdx].nV()) {
      thermalize(acc, tracks, vertices, cParams, osumtkwt, _beta, cParams.delta_lowT(), rho0);
      nprev = vertices[blockIdx].nV();
      alpaka::syncBlockThreads(acc);
      purge(acc, tracks, vertices, cParams, osumtkwt, _beta, rho0);
      alpaka::syncBlockThreads(acc);
    }
    while (_beta < 1./cParams.Tstop()){ // Cool down to stop temperature
      alpaka::syncBlockThreads(acc);
      if (once_per_block(acc)){ // Cool down
        _beta = std::min(_beta/cParams.coolingFactor(), 1./cParams.Tstop());
      }
      alpaka::syncBlockThreads(acc);
      thermalize(acc, tracks, vertices, cParams, osumtkwt, _beta, cParams.delta_lowT(), rho0);
    }
    alpaka::syncBlockThreads(acc);
    // The last track to vertex assignment of the clusterizer!
    set_vtx_range(acc, tracks, vertices, cParams, osumtkwt, _beta);
    alpaka::syncBlockThreads(acc);
  } // rejectOutliers

  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void resortVerticesAndAssign(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, int32_t griddim){
    // Multiblock vertex arbitration
    double beta = 1./cParams.Tstop();
    int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
    int threadIdx = alpaka::getIdx<alpaka::Block, alpaka::Threads>(acc)[0u]; // Thread number inside block
    auto& z= alpaka::declareSharedVar<float[128], __COUNTER__>(acc);
    auto& rho= alpaka::declareSharedVar<float[128], __COUNTER__>(acc);
    alpaka::syncBlockThreads(acc);
    if (once_per_block(acc)){ 
      int nTrueVertex = 0;
      int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
      int maxVerticesPerBlock = (int) 512/blockSize; // Max vertices size is 512 over number of blocks in grid
      for (int32_t blockid = 0; blockid < griddim ; blockid++){
        for(int ivtx = blockid * maxVerticesPerBlock; ivtx < blockid * maxVerticesPerBlock + vertices[blockid].nV(); ivtx++){
          int ivertex = vertices[ivtx].order();
          if ((vertices[ivertex].rho()< 10000) && (abs(vertices[ivertex].z())<30)) {
            z[nTrueVertex] = vertices[ivertex].z();
            rho[nTrueVertex] = vertices[ivertex].rho();
            nTrueVertex ++;
            if (nTrueVertex==1024) break;
          }
        }
      }
      vertices[0].nV() = nTrueVertex;
    }
    alpaka::syncBlockThreads(acc);

    auto& orderedIndices = alpaka::declareSharedVar<uint16_t[1024], __COUNTER__>(acc);
    auto& sws            = alpaka::declareSharedVar<uint16_t[1024], __COUNTER__>(acc);
    
    int const& nvFinal = vertices[0].nV();

    cms::alpakatools::radixSort<Acc1D, float, 2>(acc, z, orderedIndices, sws, nvFinal);
    alpaka::syncBlockThreads(acc);
    if (once_per_block(acc)){ 
      // copy sorted vertices back to the SoA
      for (int ivtx=threadIdx; ivtx< vertices[0].nV(); ivtx+=blockSize){
        vertices[ivtx].z() = z[ivtx];
        vertices[ivtx].rho() = rho[ivtx];
        vertices[ivtx].order() = orderedIndices[ivtx];
      }
    }  
    alpaka::syncBlockThreads(acc);
    double zrange_min_ = 0.1;
     
    for (int itrack = threadIdx; itrack < tracks.nT() ; itrack += blockSize){
      if (not(tracks[itrack].isGood())) continue;
      double zrange     = std::max(cParams.zrange()/ sqrt((beta) * tracks[itrack].oneoverdz2()), zrange_min_);
      double zmin       = tracks[itrack].z() - zrange;
      int kmin = vertices[0].nV()-1;
      if (vertices[vertices[kmin].order()].z() > zmin){ // vertex properties always accessed through vertices->order
        while ((kmin > 0) && (vertices[vertices[kmin-1].order()].z() > zmin)) { // i.e., while we find another vertex within range that is before the previous initial step
          kmin--;
        }
      }
      else {
        while ((kmin < vertices[0].nV()) && (vertices[vertices[kmin].order()].z() < zmin)) { // Or it might happen that we have to take out vertices from the thing
          kmin++;
        }
      }
      // Now the same for the upper bound
      double zmax       = tracks[itrack].z() + zrange;
      int kmax = 0;
      if (vertices[vertices[kmax].order()].z()< zmax) {
        while (( kmax < vertices[0].nV()  - 1) && ( vertices[vertices[kmax+1].order()].z()< zmax )) { // As long as we have more vertex above kmax but within z range, we can add them to the collection, keep going
          kmax++;
        }
      }
      else { //Or maybe we have to restrict it
        while (( kmax > 0) && (vertices[vertices[kmax].order()].z() > zmax)) {
          kmax--;
        }
      }
      if (kmin <= kmax) {
        tracks[itrack].kmin() = kmin;
        tracks[itrack].kmax() = kmax + 1; //always looping to tracks->kmax(i) - 1
      }
      else { // If it is here, the whole vertex are under
        tracks[itrack].kmin() = std::max(0, std::min(kmin, kmax));
        tracks[itrack].kmax() = std::min(vertices[0].nV(), std::max(kmin, kmax) + 1);
      }
    }
    alpaka::syncBlockThreads(acc); 

    double mintrkweight_ = 0.5;
    double rho0 = vertices[0].nV() > 1 ? 1./vertices[0].nV() : 1.;
    double z_sum_init = rho0*exp(-(beta)*cParams.dzCutOff()*cParams.dzCutOff());
    for (int itrack = threadIdx; itrack < tracks.nT() ; itrack += blockSize){
      int kmin = tracks[itrack].kmin();
      int kmax = tracks[itrack].kmax();
      double p_max = -1; 
      int iMax = 10000; 
      double sum_Z = z_sum_init;
      for (auto k = kmin; k < kmax; k++) {
        double v_exp = exp(-(beta) * std::pow( tracks[itrack].z() - vertices[vertices[k].order()].z(), 2) * tracks[itrack].oneoverdz2());
        sum_Z += vertices[vertices[k].order()].rho() * v_exp;
      }
      double invZ = sum_Z > 1e-100 ? 1. / sum_Z : 0.0;
      for (auto k = kmin; k < kmax; k++) {
        float v_exp = exp(-(beta) * std::pow( tracks[itrack].z() - vertices[vertices[k].order()].z(), 2) * tracks[itrack].oneoverdz2()) ;
        float p = vertices[vertices[k].order()].rho() * v_exp * invZ;
        if (p > p_max && p > mintrkweight_) {
          // assign  track i -> vertex k (hard, mintrkweight_ should be >= 0.5 here)
          p_max = p;
          iMax = k;
        }
      }
      tracks[itrack].kmin() = iMax; 
      tracks[itrack].kmax() = iMax+1; 
    }
    alpaka::syncBlockThreads(acc);
  }

  template <bool debug = false, typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>> ALPAKA_FN_ACC static void finalizeVertices(const TAcc& acc, portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams){
    int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
    int threadIdx = alpaka::getIdx<alpaka::Block, alpaka::Threads>(acc)[0u]; // Thread number inside block
    // From here it used to be vertices
    if (once_per_block(acc)){
    for (int k = 0; k < vertices[0].nV(); k+= 1) { //TODO: ithread, blockSize
      int ivertex = vertices[k].order();
      vertices[ivertex].ntracks() = 0;
      for (int itrack = 0; itrack < tracks.nT(); itrack+= 1){
        if (not(tracks[itrack].isGood())) continue; // Remove duplicates
        int ivtxFromTk = tracks[itrack].kmin();
        if (ivtxFromTk == k){
	  bool isNew = true;
	  for (int ivtrack = 0; ivtrack < vertices[ivertex].ntracks(); ivtrack++){
	    if (tracks[itrack].tt_index() == tracks[vertices[ivertex].track_id()[ivtrack]].tt_index()) isNew = false;
          }
	  if (!isNew) continue;
	  vertices[ivertex].track_id()[vertices[ivertex].ntracks()] = itrack; //tracks[itrack].tt_index();
	  vertices[ivertex].track_weight()[vertices[ivertex].ntracks()] = 1.;
  	  vertices[ivertex].ntracks()++;
        }
      }
      if (vertices[ivertex].ntracks() < 2){
        vertices[ivertex].isGood() = false; // No longer needed
        continue; //Skip vertex if it has no tracks
      }
      vertices[ivertex].x() = 0;
      vertices[ivertex].y() = 0;
    }
    }
    alpaka::syncBlockThreads(acc);
    if (once_per_block(acc)){
      // So we now check whether each vertex is further enough from the previous one
      for (int k = 0; k < vertices[0].nV(); k++) {
        int prevVertex = ((int) k)-1;
        int thisVertex = (int) vertices[k].order();
        if (not(vertices[thisVertex].isGood())){
          continue;
        }
        while (!(vertices[vertices[prevVertex].order()].isGood()) && prevVertex >= 0){
          // Find the previous vertex that was good
          prevVertex--;
        }
        if ((prevVertex < 0)){ // If it is first, always good
          vertices[thisVertex].isGood() = true;
        }
        else if (abs(vertices[thisVertex].z()-vertices[prevVertex].z()) > (2* cParams.vertexSize())){ //If it is further away enough, it is also good
          vertices[thisVertex].isGood() = true;
        }
        else{
          vertices[thisVertex].isGood() = false;
        }
      }
      // This is new, basically we have to deal with the order being broken by the invalidation of vertexes and set back again the vertex multiplicity, unfortunately can't be parallelized without competing conditions
      int k = 0;
      while (k != vertices[0].nV()){
        int thisVertex = vertices[k].order();
        if (vertices[thisVertex].isGood()){ // If is good just continue
          k++;
        }
        else{
          for (int l = k ; l < vertices[0].nV() ; l++){ //If it is bad, move one position all indexes
  	    vertices[l].order() = vertices[l+1].order();
  	  }
          vertices[0].nV()--; // And reduce vertex number by 1
        }
      }
    }
    alpaka::syncBlockThreads(acc);
  }

  class clusterizeKernel {
  public:
    template <typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>>
    ALPAKA_FN_ACC void operator()(const TAcc& acc,  portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams) const{ 
      // This has the core of the clusterization algorithm
      // First, declare beta=1/T
      initialize(acc, tracks, vertices, cParams);
      int blockSize = alpaka::getWorkDiv<alpaka::Grid, alpaka::Blocks>(acc)[0u];
      int threadIdx = alpaka::getIdx<alpaka::Block, alpaka::Threads>(acc)[0u]; // Thread number inside block
      int blockIdx  = alpaka::getIdx<alpaka::Grid, alpaka::Blocks>(acc)[0u]; // Block number inside grid

      double& _beta = alpaka::declareSharedVar<double, __COUNTER__>(acc);
      double& osumtkwt = alpaka::declareSharedVar<double, __COUNTER__>(acc);
      for (int itrack = threadIdx+blockIdx*blockSize; itrack < threadIdx+(blockIdx+1)*blockSize ; itrack += blockSize){ // TODO:Saving and reading in the tracks dataformat might be a bit too much?
        alpaka::atomicAdd(acc, &osumtkwt, tracks[itrack].weight(), alpaka::hierarchy::Threads{});
      }
      alpaka::syncBlockThreads(acc);
      // In each block, initialize to a single vertex with all tracks
      initialize(acc, tracks, vertices, cParams);
      alpaka::syncBlockThreads(acc);
      // First estimation of critical temperature
      getBeta0(acc, tracks, vertices, cParams, _beta);
      alpaka::syncBlockThreads(acc);
      // Cool down to beta0 with rho = 0.0 (no regularization term)
      thermalize(acc, tracks, vertices, cParams, osumtkwt, _beta, cParams.delta_highT(), 0.0);
      alpaka::syncBlockThreads(acc);
      // Now the cooling loop
      coolingWhileSplitting(acc, tracks, vertices, cParams, osumtkwt, _beta);
      alpaka::syncBlockThreads(acc);
      // After cooling, merge closeby vertices
      reMergeTracks(acc,tracks, vertices,cParams, osumtkwt, _beta);
      alpaka::syncBlockThreads(acc);
      // And split those with tension
      reSplitTracks(acc,tracks, vertices,cParams, osumtkwt, _beta);
      alpaka::syncBlockThreads(acc);
      // After splitting we might get some candidates that are very low quality/have very far away tracks
      rejectOutliers(acc,tracks, vertices,cParams, osumtkwt, _beta);
      alpaka::syncBlockThreads(acc);
    }
  }; // class kernel


  class arbitrateKernel {
  public:
    template <typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>>
    ALPAKA_FN_ACC void operator()(const TAcc& acc,  portablevertex::TrackDeviceCollection::View tracks, portablevertex::VertexDeviceCollection::View vertices, const portablevertex::ClusterParamsHostCollection::ConstView cParams, int32_t nBlocks) const{
      // This has the core of the clusterization algorithm
      resortVerticesAndAssign(acc, tracks, vertices,cParams, nBlocks);
      alpaka::syncBlockThreads(acc);
      finalizeVertices(acc, tracks, vertices, cParams); // In CUDA it used to be verticesAndClusterize
      alpaka::syncBlockThreads(acc);
    }       
  }; // class kernel


  ClusterizerAlgo::ClusterizerAlgo(Queue& queue) {
  } // ClusterizerAlgo::ClusterizerAlgo
  
  void ClusterizerAlgo::clusterize(Queue& queue, portablevertex::TrackDeviceCollection& deviceTrack, portablevertex::VertexDeviceCollection& deviceVertex, const std::shared_ptr<portablevertex::ClusterParamsHostCollection> cParams, int32_t nBlocks, int32_t blockSize){
    const int blocks = divide_up_by(nBlocks*blockSize, blockSize); //nBlocks of size blockSize
    alpaka::exec<Acc1D>(queue,
		        make_workdiv<Acc1D>(blocks, blockSize),
			clusterizeKernel{},
			deviceTrack.view(), // TODO:: Maybe we can optimize the compiler by not making this const? Tracks would not be modified
			deviceVertex.view(),
			cParams->view());
  } // ClusterizerAlgo::clusterize

  void ClusterizerAlgo::arbitrate(Queue& queue, portablevertex::TrackDeviceCollection& deviceTrack, portablevertex::VertexDeviceCollection& deviceVertex, const std::shared_ptr<portablevertex::ClusterParamsHostCollection> cParams, int32_t nBlocks, int32_t blockSize){
    const int blocks = divide_up_by(blockSize, blockSize); //Single block, as it has to converge to a single collection
    alpaka::exec<Acc1D>(queue,
                        make_workdiv<Acc1D>(blocks, blockSize),
                        arbitrateKernel{},
                        deviceTrack.view(), // TODO:: Maybe we can optimize the compiler by not making this const? Tracks would not be modified
                        deviceVertex.view(),
                        cParams->view(),
			nBlocks);    
  } // arbitraterAlgo::arbitrate

} // namespace ALPAKA_ACCELERATOR_NAMESPACE
