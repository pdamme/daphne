/*
 * Copyright 2021 The DAPHNE Consortium
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_RUNTIME_DISTRIBUTED_COORDINATOR_KERNELS_DISTRIBUTE_H
#define SRC_RUNTIME_DISTRIBUTED_COORDINATOR_KERNELS_DISTRIBUTE_H

#include <runtime/local/context/DaphneContext.h>
#include <runtime/local/datastructures/DataObjectFactory.h>
#include <runtime/local/datastructures/DenseMatrix.h>

#include <grpcpp/grpcpp.h>
#include <runtime/distributed/proto/worker.pb.h>
#include <runtime/distributed/proto/worker.grpc.pb.h>
#include <runtime/distributed/coordinator/kernels/DistributedCaller.h>

#include <cassert>
#include <cstddef>
#include <runtime/local/datastructures/AllocationDescriptorDistributed.h>
#include <runtime/distributed/worker/ProtoDataConverter.h>

// ****************************************************************************
// Template speciliazations for each communication framework (gRPC, OPENMPI, etc.)
// ****************************************************************************

template<DISTRIBUTED_BACKEND backend, class DT>
struct DistributeImplementationClass
{
    static void apply(DT* mat) = delete;
};

template<DISTRIBUTED_BACKEND backend, class DT>
void distributeImplementation(DT* mat)
{
    DistributeImplementationClass<backend, DT>::apply(mat);
}
// ****************************************************************************
// gRPC implementation
// ****************************************************************************
template<class DT>
struct DistributeImplementationClass<DISTRIBUTED_BACKEND::GRPC, DT>
{
    static void apply(DT *mat)
    {        
        struct StoredInfo {
            size_t omd_id;
        };                
        DistributedCaller<StoredInfo, distributed::Matrix, distributed::StoredData> caller;

        auto omdVector = (mat->getObjectMetaDataByType(ALLOCATION_TYPE::DISTRIBUTED));
        for (auto &omd : *omdVector) {
            // Skip if already placed at workers
            if (dynamic_cast<AllocationDescriptorDistributed&>(*(omd->allocation)).getDistributedData().isPlacedAtWorker)
                continue;
            distributed::Matrix protoMat;
            // TODO: We need to handle different data types 
            // (this will be simplified when serialization is implemented)
            auto denseMat = dynamic_cast<const DenseMatrix<double>*>(mat);
            if (!denseMat){
                std::runtime_error("Distribute grpc only supports DenseMatrix<double> for now");
            }
            ProtoDataConverter<DenseMatrix<double>>::convertToProto(denseMat, &protoMat, 
                                                    omd->range->r_start,
                                                    omd->range->r_start + omd->range->r_len,
                                                    omd->range->c_start,
                                                    omd->range->c_start + omd->range->c_len);

            StoredInfo storedInfo({omd->omd_id}); 
            caller.asyncStoreCall(dynamic_cast<AllocationDescriptorDistributed&>(*(omd->allocation)).getLocation(), storedInfo, protoMat);
        }

        // ToDO triger transfers
        // get results        
        while (!caller.isQueueEmpty()){
            auto response = caller.getNextResult();
            auto omd_id = response.storedInfo.omd_id;
            auto omd = mat->getObjectMetaDataByID(omd_id);

            auto storedData = response.result;
            
            // storedData.set_type(response.storedInfo.dataType);
            DistributedData data;
            data.ix = dynamic_cast<AllocationDescriptorDistributed&>(*(omd->allocation)).getDistributedIndex();
            data.filename = storedData.filename();
            data.numRows = storedData.num_rows();
            data.numCols = storedData.num_cols();
            data.isPlacedAtWorker = true;
            dynamic_cast<AllocationDescriptorDistributed&>(*(omd->allocation)).updateDistributedData(data);
        }
        

    }
};
// ****************************************************************************
// Struct for partial template specialization
// ****************************************************************************

template<class DT>
struct Distribute
{
    static void apply(DT *mat, DCTX(ctx)) {
        auto envVar = std::getenv("DISTRIBUTED_WORKERS");
        assert(envVar && "Environment variable has to be set");
        std::string workersStr(envVar);
        std::string delimiter(",");

        size_t pos;
        std::vector<std::string> workers;
        while ((pos = workersStr.find(delimiter)) != std::string::npos) {
            workers.push_back(workersStr.substr(0, pos));
            workersStr.erase(0, pos + delimiter.size());
        }
        workers.push_back(workersStr);
    
        assert(mat != nullptr);

        auto r = 0ul;
        for (auto workerIx = 0ul; workerIx < workers.size() && r < mat->getNumRows(); workerIx++) {            
            auto workerAddr = workers.at(workerIx);                      

            auto k = mat->getNumRows() / workers.size();
            auto m = mat->getNumRows() % workers.size();            

            Range range;
            range.r_start = (workerIx * k) + std::min(workerIx, m);
            range.r_len = ((workerIx + 1) * k + std::min(workerIx + 1, m)) - range.r_start;
            range.c_start = 0;
            range.c_len = mat->getNumCols();
                        
            IAllocationDescriptor *allocationDescriptor;
            DistributedData data;
            data.ix = DistributedIndex(workerIx, 0);
            // If omd already exists simply
            // update range (in case we have a different one) and distributed data
            if (auto omd = mat->getObjectMetaDataByLocation(workerAddr)) {
                // TODO consider declaring objectmetadata functions const and objectmetadata array as mutable
                const_cast<typename std::remove_const<DT>::type*>(mat)->updateRangeObjectMetaDataByID(omd->omd_id, &range);     
                dynamic_cast<AllocationDescriptorDistributed&>(*(omd->allocation)).updateDistributedData(data);
            }
            else { // Else, create new object metadata entry
                allocationDescriptor = new AllocationDescriptorDistributed(
                                            ctx, 
                                            workerAddr,
                                            data);            
                const_cast<typename std::remove_const<DT>::type*>(mat)->addObjectMetaData(allocationDescriptor, &range);                    
            }
            // keep track of proccessed rows
            r = (workerIx + 1) * k + std::min(workerIx + 1, m);
        }
        // TODO choose implementation
        // Possible to call all implementations and each one will try to find allocators (MPI, gRPC)
        // if none is find, then the implementation simply returns.
        distributeImplementation<DISTRIBUTED_BACKEND::GRPC, DT>(mat);


    }
};

// ****************************************************************************
// Convenience function
// ****************************************************************************

template<class DT>
void distribute(DT *mat, DCTX(ctx))
{
    Distribute<DT>::apply(mat, ctx);
}


#endif //SRC_RUNTIME_DISTRIBUTED_COORDINATOR_KERNELS_DISTRIBUTE_H