#include <alpaka/alpaka.hpp>
#include <cmath>
#include "HeterogeneousCore/AlpakaInterface/interface/config.h"
#include "HeterogeneousCore/AlpakaInterface/interface/workdivision.h"

#include "RecoVertex/PrimaryVertexProducer_Alpaka/plugins/alpaka/BlockAlgo.h"

#define DEBUG_RECOVERTEX_PRIMARYVERTEXPRODUCER_ALPAKA_BLOCKALGO 1

namespace ALPAKA_ACCELERATOR_NAMESPACE {
  using namespace cms::alpakatools; 

  class createBlocksKernel {
  public:
    template <typename TAcc, typename = std::enable_if_t<alpaka::isAccelerator<TAcc>>>
    ALPAKA_FN_ACC void operator()(const TAcc& acc,  const portablevertex::TrackDeviceCollection::ConstView inputTracks,  portablevertex::TrackDeviceCollection::View trackInBlocks, double blockOverlap, int32_t blockSize) const{
      #ifdef DEBUG_RECOVERTEX_PRIMARYVERTEXPRODUCER_ALPAKA_BLOCKALGO
        printf("[BlockAlgo::operator()] Start\n");
        printf("[BlockAlgo::operator()] blockOverlap: %1.3f, blockSize %i\n",blockOverlap, blockSize);
      #endif
      int32_t nTOld = inputTracks.nT();
      #ifdef DEBUG_RECOVERTEX_PRIMARYVERTEXPRODUCER_ALPAKA_BLOCKALGO
        printf("[BlockAlgo::operator()] blockSize: %i, blockOverlap %1.3f, nTOld %i\n", blockSize, blockOverlap, nTOld);
      #endif
      int32_t nBlocks = nTOld > blockSize ? int32_t ((nTOld-1)/(blockOverlap*blockSize)) : 1; // If all fit within a block, no need to split
      #ifdef DEBUG_RECOVERTEX_PRIMARYVERTEXPRODUCER_ALPAKA_BLOCKALGO
        printf("[BlockAlgo::operator()] nBlocks: %i\n", nBlocks);
      #endif
      int32_t overlapStart = blockOverlap*blockSize; // First block starts at 0, second block starts at overlapStart, third at 2*overlapStart and so on
      #ifdef DEBUG_RECOVERTEX_PRIMARYVERTEXPRODUCER_ALPAKA_BLOCKALGO
        printf("[BlockAlgo::operator()] blockSize: %i\n", blockSize);
      #endif
      for (auto iNewTrack : elements_with_stride(acc, blockSize) ) { // The accelerator has as much threads as blockSize, so each thread will enter once on each block
	#ifdef DEBUG_RECOVERTEX_PRIMARYVERTEXPRODUCER_ALPAKA_BLOCKALGO
	  printf("[BlockAlgo::operator()] iNewTrack %i, nBlocks %i\n", iNewTrack, nBlocks);
	#endif 
        for (int32_t iblock = 0; iblock < nBlocks; iblock++){
      	  int32_t oldIndex = (iblock*overlapStart) + iNewTrack; // I.e. first track in the block in which we are + thread in which we are
	  if (oldIndex >= nTOld) break; // I.e. we reached the end of the input block
          int32_t newIndex = iNewTrack+iblock*blockSize;
	  #ifdef DEBUG_RECOVERTEX_PRIMARYVERTEXPRODUCER_ALPAKA_BLOCKALGO
	    printf("[BlockAlgo::operator()] iblock %i, oldIndex %i => newIndex %i, x: %1.5f, y: %1.5f, z:%1.5f\n", iblock, oldIndex, newIndex, inputTracks[oldIndex].x(),inputTracks[oldIndex].y(), inputTracks[oldIndex].z());
	  #endif
	  // And just copy in new places
	  trackInBlocks[newIndex].x()          = inputTracks[oldIndex].x();
          trackInBlocks[newIndex].y()          = inputTracks[oldIndex].y();
          trackInBlocks[newIndex].z()          = inputTracks[oldIndex].z();
          trackInBlocks[newIndex].px()         = inputTracks[oldIndex].px();
          trackInBlocks[newIndex].py()         = inputTracks[oldIndex].py();
          trackInBlocks[newIndex].pz()         = inputTracks[oldIndex].pz();
          trackInBlocks[newIndex].weight()     = inputTracks[oldIndex].weight();
          trackInBlocks[newIndex].tt_index()   = inputTracks[oldIndex].tt_index(); // Relevant to keep the index at hand, as it lets us merge tracks later
          trackInBlocks[newIndex].dz2()        = inputTracks[oldIndex].dz2();
          trackInBlocks[newIndex].oneoverdz2() = inputTracks[oldIndex].oneoverdz2();
          trackInBlocks[newIndex].dxy2AtIP()   = inputTracks[oldIndex].dxy2AtIP();
          trackInBlocks[newIndex].dxy2()       = inputTracks[oldIndex].dxy2();
          trackInBlocks[newIndex].sum_Z()      = inputTracks[oldIndex].order();
          trackInBlocks[newIndex].kmin()       = inputTracks[oldIndex].kmin();
          trackInBlocks[newIndex].kmax()       = inputTracks[oldIndex].kmax();
          trackInBlocks[newIndex].aux1()       = inputTracks[oldIndex].aux1();
          trackInBlocks[newIndex].aux2()       = inputTracks[oldIndex].aux2();
          trackInBlocks[newIndex].isGood()     = inputTracks[oldIndex].isGood();
	} // iblock for
      } // iNewTrack for
      if (once_per_block(acc)){
        trackInBlocks.nT() = (nBlocks-1)*blockSize + nTOld-blockSize*std::floor(nTOld/(blockOverlap*blockSize));
      }
      #ifdef DEBUG_RECOVERTEX_PRIMARYVERTEXPRODUCER_ALPAKA_BLOCKALGO
        printf("[BlockAlgo::operator()] End\n");
      #endif
    } // createBlocksKernel::operator()
  }; // class createBlocksKernel

  BlockAlgo::BlockAlgo() {
  } // BlockAlgo::BlockAlgo
  
  void BlockAlgo::createBlocks(Queue& queue, const portablevertex::TrackDeviceCollection& inputTracks, portablevertex::TrackDeviceCollection& trackInBlocks, int32_t bSize, double bOverlap){
    const int threadsPerBlock = bSize;// each thread will write nBlocks tracks
    const int blocks = 1;             // 1 block with all threads
    alpaka::exec<Acc1D>(queue,
		        make_workdiv<Acc1D>(blocks, threadsPerBlock),
			createBlocksKernel{},
			inputTracks.view(),
			trackInBlocks.view(),
			bOverlap,
			bSize
			); 
  } // BlockAlgo::createBlocks
} // namespace ALPAKA_ACCELERATOR_NAMESPACE
