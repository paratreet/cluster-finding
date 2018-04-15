#include "prefixBalance.h"
#include "unionFindLib.h"

/*readonly*/ CProxy_UnionFindLib _UfLibProxy;
/*readonly*/ CProxy_Prefix prefixLibArray;
/*readonly*/ CkGroupID libGroupID;
CkReduction::reducerType mergeCountMapsReductionType;

// custom reduction for merging local count maps
CkReductionMsg* merge_count_maps(int nMsgs, CkReductionMsg **msgs) {
    std::unordered_map<long int,int> merged_temp_map;
    for (int i = 0; i < nMsgs; i++) {
        // any sanity check for map size?
        // extract this message's local map
        componentCountMap *curr_map = (componentCountMap*)msgs[i]->getData();
        int numComps = msgs[i]->getSize();
        numComps = numComps / sizeof(componentCountMap);

        // convert custom map to STL map for easier lookup
        for (int j = 0; j < numComps; j++) {
            merged_temp_map[curr_map[j].compNum] += curr_map[j].count;
        }
    } // all messages processed

    // convert the STL back to custom map for messaging
    componentCountMap *merged_map = new componentCountMap[merged_temp_map.size()];
    std::unordered_map<long int,int>::iterator iter = merged_temp_map.begin();
    for (int i = 0; i < merged_temp_map.size(); i++) {
        componentCountMap entry;
        entry.compNum = iter->first;
        entry.count = iter->second;
        merged_map[i] = entry;
        iter++;
    }

    int retSize = sizeof(componentCountMap) * merged_temp_map.size();
    return CkReductionMsg::buildNew(retSize, merged_map);
}

// initnode function to register reduction
static void register_merge_count_maps_reduction() {
    mergeCountMapsReductionType = CkReduction::addReducer(merge_count_maps);
}

// class function implementations

void UnionFindLib::
registerGetLocationFromID(std::pair<int, int> (*gloc)(long int vid)) {
    getLocationFromID = gloc;
}

void UnionFindLib::
register_phase_one_cb(CkCallback cb) {
    if (thisIndex != 0)
        CkAbort("[UnionFindLib] Phase 1 callback must be registered on first chare only!");

    CkStartQD(cb);
}

void UnionFindLib::
initialize_vertices(unionFindVertex *appVertices, int numVertices) {
    // local vertices corresponding to one treepiece in application
    numMyVertices = numVertices;
    myVertices = appVertices; // no need to do memcpy, array in same address space as app
    /*myVertices = (unionFindVertex*)malloc(numVertices * sizeof(unionFindVertex));
    memcpy(myVertices, appVertices, sizeof(unionFindVertex)*numVertices);*/
    /*for (int i = 0; i < numMyVertices; i++) {
        CkPrintf("[LibProxy %d] myVertices[%d] - vertexID: %ld, parent: %ld, component: %d\n", this->thisIndex, i, myVertices[i].vertexID, myVertices[i].parent, myVertices[i].componentNumber);
    }*/
}

void UnionFindLib::
union_request(long int vid1, long int vid2) {
    if (vid2 < vid1) {
        // found a back edge, flip and reprocess
        union_request(vid2, vid1);
    }
    else {
        //std::pair<int,int> vid1_loc = appPtr->getLocationFromID(vid1);
        std::pair<int, int> vid1_loc = getLocationFromID(vid1);
        //message the chare containing first vertex to find boss1
        //pass the initilizer ID for initiating path compression

        findBossData d;
        d.arrIdx = vid1_loc.second;
        d.partnerOrBossID = vid2;
        d.senderID = -1; // TODO: Is this okay? Or use INT_MIN
        d.isFBOne = 1;
        this->thisProxy[vid1_loc.first].insertDataFindBoss(d);

        //for profiling
        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
    }
}

void UnionFindLib::
find_boss1(int arrIdx, long int partnerID, long int senderID) {
    unionFindVertex *src = &myVertices[arrIdx];

    if (src->parent == -1) {
        //boss1 found
        std::pair<int, int> partner_loc = getLocationFromID(partnerID);
        //message the chare containing the partner
        //senderID for first find_boss2 is not relevant, similar to first find_boss1

        findBossData d;
        d.arrIdx = partner_loc.second;
        d.partnerOrBossID = src->vertexID;
        d.senderID = -1;
        d.isFBOne = 0;
        this->thisProxy[partner_loc.first].insertDataFindBoss(d);

        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
        //message the initID to kick off path compression in boss1's chain
        /*std::pair<int,int> init_loc = appPtr->getLocationFromID(initID);
        this->thisProxy[init_loc.first].compress_path(init_loc.second, src->vertexID);
        libGroup.ckLocalBranch()->increase_message_count();*/
    }
    else {
        //boss1 not found, move to parent
        std::pair<int, int> parent_loc = getLocationFromID(src->parent);
        unionFindVertex *path_base = src;
        unionFindVertex *parent, *curr = src;

        /* Locality based optimization code:
           instead of using messages to traverse the tree, this
           technique uses a while loop to reach the top of "local" tree i.e
           the last node in the tree path that is locally present on current chare
           We combine this with a local path compression optimization to make
           all local trees completely shallow
        */
        while (parent_loc.first == this->thisIndex) {
            parent = &myVertices[parent_loc.second];

            // entire tree is local to chare
            if (parent->parent ==  -1) {
                local_path_compression(path_base, parent->vertexID);

                findBossData d;
                d.arrIdx = parent_loc.second;
                d.partnerOrBossID = partnerID;
                d.senderID = curr->vertexID;
                d.isFBOne = 1;
                this->insertDataFindBoss(d);

                return;
            }

            // move pointers to traverse tree
            curr = parent;
            parent_loc = getLocationFromID(curr->parent);
        } //end of local tree climbing

        if (path_base->vertexID != curr->vertexID) {
            local_path_compression(path_base, curr->vertexID);
        }
        else {
            //CkPrintf("Self-pointing bug avoided\n");
        }

        CkAssert(parent_loc.first != this->thisIndex);
        //message remote chare containing parent, set the senderID to curr

        findBossData d;
        d.arrIdx = parent_loc.second;
        d.partnerOrBossID = partnerID;
        d.senderID = curr->vertexID;
        d.isFBOne = 1;
        this->thisProxy[parent_loc.first].insertDataFindBoss(d);

        // check if sender and current vertex are on different chares
        if (senderID != -1 && !check_same_chares(senderID, curr->vertexID)) {
            // short circuit the sender to point to grandparent
            std::pair<int,int> sender_loc = getLocationFromID(senderID);
            shortCircuitData scd;
            scd.arrIdx = sender_loc.second;
            scd.grandparentID = curr->parent;
            thisProxy[sender_loc.first].short_circuit_parent(scd);
        }

        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
    }
}


void UnionFindLib::
find_boss2(int arrIdx, long int boss1ID, long int senderID) {
    unionFindVertex *src = &myVertices[arrIdx];

    if (src->parent == -1) {
        if (boss1ID > src->vertexID) {
            //do not point to somebody greater than you, min-heap property (mostly a cycle edge?)
            union_request(boss1ID, src->vertexID); // flipped and reprocessed
        }
        else {
            //valid edge
            if (boss1ID != src->vertexID) {//avoid self-loop
                src->parent = boss1ID;
                //message initID to start path compression in boss2's chain
                /*std::pair<int,int> init_loc = appPtr->getLocationFromID(initID);
                this->thisProxy[init_loc.first].compress_path(init_loc.second, boss1ID);
                CProxy_UnionFindLibGroup libGroup(libGroupID);
                libGroup.ckLocalBranch()->increase_message_count();*/
            }
        }
    }
    else {
        //boss2 not found, move to parent
        //std::pair<int,int> parent_loc = appPtr->getLocationFromID(src->parent);
        std::pair<int, int> parent_loc = getLocationFromID(src->parent);
        unionFindVertex *path_base = src;
        unionFindVertex *parent, *curr = src;

        // same optimizations as in find_boss1
        while (parent_loc.first == this->thisIndex) {
            parent = &myVertices[parent_loc.second];

            if (parent->parent ==  -1) {
                local_path_compression(path_base, parent->vertexID);

                // find_boss2(parent_loc.second, boss1ID, initID);
                findBossData d;
                d.arrIdx = parent_loc.second;
                d.partnerOrBossID = boss1ID;
                d.senderID = curr->vertexID;
                d.isFBOne = 0;
                this->insertDataFindBoss(d);

                return;
            }

            curr = parent;
            parent_loc = getLocationFromID(curr->parent);
        } //end of local tree climbing

        if (path_base->vertexID != curr->vertexID) {
            local_path_compression(path_base, curr->vertexID);
        }
        else {
            //CkPrintf("Self-pointing bug avoided\n");
        }

        CkAssert(parent_loc.first != this->thisIndex);
        //message remote chare containing parent

        findBossData d;
        d.arrIdx = parent_loc.second;
        d.partnerOrBossID = boss1ID;
        d.senderID = curr->vertexID;
        d.isFBOne = 0;
        this->thisProxy[parent_loc.first].insertDataFindBoss(d);

        // check if sender and current vertex are on different chares
        if (senderID != -1 && !check_same_chares(senderID, curr->vertexID)) {
            // short circuit the sender to point to grandparent
            std::pair<int,int> sender_loc = getLocationFromID(senderID);
            shortCircuitData scd;
            scd.arrIdx = sender_loc.second;
            scd.grandparentID = curr->parent;
            thisProxy[sender_loc.first].short_circuit_parent(scd);
        }

        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
    }
}

// perform local path compression
void UnionFindLib::
local_path_compression(unionFindVertex *src, long int compressedParent) {
    unionFindVertex* tmp;

    while (src->parent != compressedParent) {
        tmp = &myVertices[getLocationFromID(src->parent).second];
        src->parent = compressedParent;
        src =tmp;
    }
}

// check if two vertices are on same chare
bool UnionFindLib::
check_same_chares(long int v1, long int v2) {
    std::pair<int,int> v1_loc = getLocationFromID(v1);
    std::pair<int,int> v2_loc = getLocationFromID(v2);
    if (v1_loc.first == v2_loc.first)
        return true;
    return false;
}

// short circuit a vertex to point to grandparent
void UnionFindLib::
short_circuit_parent(shortCircuitData scd) {
    unionFindVertex *src = &myVertices[scd.arrIdx];
    //CkPrintf("[TP %d] Short circuiting %ld from current parent %ld to grandparent %ld\n", thisIndex, src->vertexID, src->parent, grandparentID);
    src->parent = scd.grandparentID;
}

// function to implement simple path compression; currently unused
void UnionFindLib::
compress_path(int arrIdx, long int compressedParent) {
    unionFindVertex *src = &myVertices[arrIdx];
    //message the parent before reseting it
    if (src->vertexID != compressedParent) {//reached the top of path
        std::pair<int, int> parent_loc = getLocationFromID(src->parent);
        this->thisProxy[parent_loc.first].compress_path(parent_loc.second, compressedParent);
        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
        src->parent = compressedParent;
    }
}

unionFindVertex* UnionFindLib::
return_vertices() {
    return myVertices;
}

/** Functions for finding connected components **/

void UnionFindLib::
find_components(CkCallback cb) {
    postComponentLabelingCb = cb;
    // count local numBosses
    myLocalNumBosses = 0;
    for (int i = 0; i < numMyVertices; i++) {
        if (myVertices[i].parent == -1) {
            myLocalNumBosses += 1;
        }
    }

    // send local count to prefix library
    CkCallback doneCb(CkReductionTarget(UnionFindLib, boss_count_prefix_done), thisProxy);
    Prefix* myPrefixElem = prefixLibArray[thisIndex].ckLocal();
    myPrefixElem->startPrefixCalculation(myLocalNumBosses, doneCb);
    //CkPrintf("[%d] Local num bosses: %d\n", thisIndex, myLocalNumBosses);
}

// Recveive total boss count from prefix library and start labelling phase
void UnionFindLib::
boss_count_prefix_done(int totalCount) {
    totalNumBosses = totalCount;
    // access value from prefix lib elem to find starting index
    Prefix* myPrefixElem = prefixLibArray[thisIndex].ckLocal();
    int v = myPrefixElem->getValue();
    int myStartIndex = v - myLocalNumBosses;
    //CkPrintf("[%d] My start index: %d\n", thisIndex, myStartIndex);

    // start labeling my local bosses from myStartIndex
    // ensures sequential numbering of components
    if (myLocalNumBosses != 0) {
        for (int i = 0; i < numMyVertices; i++) {
            if (myVertices[i].parent == -1) {
                myVertices[i].componentNumber = myStartIndex;
                myStartIndex++;
            }
        }
    }

    CkAssert(myStartIndex == v);

    // start the labeling phase for all vertices
    start_component_labeling();
}

void UnionFindLib::
start_component_labeling() {
    for (int i = 0; i < numMyVertices; i++) {
        unionFindVertex *v = &myVertices[i];
        if (v->parent == -1) {
            // one of the bosses/root found
            CkAssert(v->componentNumber != -1); // phase 2a assigned serial numbers
            set_component(i, v->componentNumber);
        }

        if (v->componentNumber == -1) {
            // an internal node or leaf node, request parent for boss
            std::pair<int, int> parent_loc = getLocationFromID(v->parent);
            //this->thisProxy[parent_loc.first].need_boss(parent_loc.second, v->vertexID);
            uint64_t data = ((uint64_t) parent_loc.second) << 32 | ((uint64_t) v->vertexID);
            this->thisProxy[parent_loc.first].insertDataNeedBoss(data);
        }
    }

    if (this->thisIndex == 0) {
        // return back to application after completing all messaging related to
        // connected components algorithm
        CkStartQD(postComponentLabelingCb);
    }
}

void UnionFindLib::
insertDataFindBoss(const findBossData & data) {
    if (data.isFBOne == 1) {
        this->find_boss1(data.arrIdx, data.partnerOrBossID, data.senderID);
    }
    else {
        this->find_boss2(data.arrIdx, data.partnerOrBossID, data.senderID);
    }
}

void UnionFindLib::
insertDataNeedBoss(const uint64_t & data) {
    int arrIdx = (int)(data >> 32);
    long int fromID = (long int)(data & 0xffffffff);
    this->need_boss(arrIdx, fromID);
}

void UnionFindLib::
need_boss(int arrIdx, long int fromID) {
    // one of children of this node needs boss, handle by either replying immediately
    // or queueing the request

    if (myVertices[arrIdx].componentNumber != -1) {
        // component already set, reply back
        std::pair<int, int> requestor_loc = getLocationFromID(fromID);
        if (requestor_loc.first == thisIndex)
            set_component(requestor_loc.second, myVertices[arrIdx].componentNumber);
        else
            this->thisProxy[requestor_loc.first].set_component(requestor_loc.second, myVertices[arrIdx].componentNumber);
    }
    else {
        // boss still not found, queue the request
        myVertices[arrIdx].need_boss_requests.push_back(fromID);
    }
}

void UnionFindLib::
set_component(int arrIdx, long int compNum) {
    myVertices[arrIdx].componentNumber = compNum;

    // since component number is set, respond to your requestors
    std::vector<long int>::iterator req_iter = myVertices[arrIdx].need_boss_requests.begin();
    while (req_iter != myVertices[arrIdx].need_boss_requests.end()) {
        long int requestorID = *req_iter;
        std::pair<int, int> requestor_loc = getLocationFromID(requestorID);
        if (requestor_loc.first == thisIndex)
            set_component(requestor_loc.second, compNum);
        else
            this->thisProxy[requestor_loc.first].set_component(requestor_loc.second, compNum);
        // done with current requestor, delete from request queue
        req_iter = myVertices[arrIdx].need_boss_requests.erase(req_iter);
    }
}

void UnionFindLib::
prune_components(int threshold, CkCallback appReturnCb) {
    componentPruneThreshold = threshold;

    //int *localCounts = new int[totalNumBosses]();
    std::vector<int> localCounts(totalNumBosses, 0);
    //if (localCounts == NULL) {
    //    CkAbort("We are out of memory!");
    //}

    for (int i = 0; i < numMyVertices; i++) {
        long int bossID = myVertices[i].componentNumber;
        CkAssert(bossID >= 0 && bossID < totalNumBosses);
        localCounts[bossID]++;
    }

    //CkPrintf("[TP %d] localCounts constructed\n", thisIndex);

    // bcast totalCounts to all group chares
    CProxy_UnionFindLibGroup libGroupProxy(libGroupID);
    CkCallback cb(CkReductionTarget(UnionFindLibGroup, build_component_count_array), libGroupProxy);
    //contribute(sizeof(int)*totalNumBosses, localCounts, CkReduction::sum_int, cb);
    contribute(localCounts, CkReduction::sum_int, cb);

    //delete[] localCounts;

    // start QD to return back to application
    if (thisIndex == 0) {
        CkStartQD(appReturnCb);
    }
}

// reductiontarget from group => all component count arrays are ready
void UnionFindLib::
perform_pruning() {

    CProxy_UnionFindLibGroup libGroup(libGroupID);

    for (int i = 0; i < numMyVertices; i++) {
        int myComponentCount = libGroup.ckLocalBranch()->get_component_count(myVertices[i].componentNumber);
        if (myComponentCount <= componentPruneThreshold) {
            myVertices[i].componentNumber = -1;
        }
    }

    if (thisIndex == 0) {
        CkPrintf("Number of components found: %d\n", totalNumBosses);
        int numPrunedComponents = 0;
        for (int i = 0; i < totalNumBosses; i++) {
            int compCount = libGroup.ckLocalBranch()->get_component_count(i);
            if (compCount <= componentPruneThreshold) {
                numPrunedComponents++;
            }
        }
        //CkPrintf("Number of components after pruning: %d\n", totalNumBosses-numPrunedComponents);
    }
}

// library group chare class definitions
void UnionFindLibGroup::
build_component_count_array(int *totalCounts, int numElems) {
    //CkPrintf("[PE %d] Count array size: %d\n", thisIndex, numElems);
    component_count_array = new int[numElems];
    memcpy(component_count_array, totalCounts, sizeof(int)*numElems);
    contribute(CkCallback(CkReductionTarget(UnionFindLib, perform_pruning), _UfLibProxy));
}

int UnionFindLibGroup::
get_component_count(long int component_id) {
    return component_count_array[component_id];
}

void UnionFindLibGroup::
increase_message_count() {
    thisPeMessages++;
}

void UnionFindLibGroup::
contribute_count() {
    CkCallback cb(CkReductionTarget(UnionFindLibGroup, done_profiling), thisProxy);
    contribute(sizeof(int), &thisPeMessages, CkReduction::sum_int, cb);
}

void UnionFindLibGroup::
done_profiling(int total_count) {
    if (CkMyPe() == 0) {
        CkPrintf("Phase 1 profiling done. Total number of messages is : %d\n", total_count);
        CkExit();
    }
}

// library initialization function
CProxy_UnionFindLib UnionFindLib::
unionFindInit(CkArrayID clientArray, int n) {
    CkArrayOptions opts(n);
    opts.bindTo(clientArray);
    _UfLibProxy = CProxy_UnionFindLib::ckNew(opts, NULL);

    // create prefix library array here, prefix library is used in Phase 1B
    // Binding order: prefix -> unionFind -> app array
    CkArrayOptions prefix_opts(n);
    prefix_opts.bindTo(_UfLibProxy);
    prefixLibArray = CProxy_Prefix::ckNew(n, prefix_opts);

    libGroupID = CProxy_UnionFindLibGroup::ckNew();
    return _UfLibProxy;
}

#include "unionFindLib.def.h"


/*------------------- Old Code: Reduction using custom structs & maps -----------------*/
#if 0
void UnionFindLib::
merge_count_results(int* totalCounts, int numElems) {

    CkAssert(numElems == totalNumBosses);
    for (int i = 0; i < numMyVertices; i++) {
        int myComponentCount = totalCounts[myVertices[i].componentNumber];
        if (myComponentCount <= componentPruneThreshold) {
            myVertices[i].componentNumber = -1;
        }
    }

    if (thisIndex == 0) {
        CkPrintf("Number of components found: %d\n", numElems);
        int numPrunedComponents = 0;
        for (int i = 0; i < numElems; i++) {
            if (totalCounts[i] <= componentPruneThreshold) {
                numPrunedComponents++;
            }
        }
        CkPrintf("Number of components after pruning: %d\n", numElems-numPrunedComponents);
    }
}

void UnionFindLib::
prune_components(int threshold, CkCallback appReturnCb) {
    //create a count map
    // key: componentNumber
    // value: local count of vertices belonging to component

    componentPruneThreshold = threshold;
    std::unordered_map<long int, int> temp_count;

    // populate local count map
    for (int i = 0; i < numMyVertices; i++) {
        temp_count[myVertices[i].componentNumber]++;
    }

    // Sanity check
    /*std::map<long int,int>::iterator it = temp_count.begin();
    while (it != temp_count.end()) {
        CkPrintf("[%d] %ld -> %d\n", this->thisIndex, it->first, it->second);
        it++;
    }*/

    // convert STL map to custom map (array of structures)
    // for contributing to reduction
    componentCountMap *local_map = new componentCountMap[temp_count.size()];
    std::unordered_map<long int,int>::iterator iter = temp_count.begin();
    for (int j = 0; j < temp_count.size(); j++) {
        if (iter == temp_count.end())
            CkAbort("Something corrupted in map memory!\n");

        componentCountMap entry;
        entry.compNum = iter->first;
        entry.count = iter->second;
        local_map[j] = entry;
        iter++;
    }

    CkCallback cb(CkIndex_UnionFindLib::merge_count_results(NULL), this->thisProxy);
    int contributeSize = sizeof(componentCountMap) * temp_count.size();
    this->contribute(contributeSize, local_map, mergeCountMapsReductionType, cb);

    // start QD to return back to application
    if (this->thisIndex == 0) {
        CkStartQD(appReturnCb);
    }

}

void UnionFindLib::
merge_count_results(CkReductionMsg *msg) {
    //ask lib group to build map
    CProxy_UnionFindLibGroup libGroup(libGroupID);
    libGroup.ckLocalBranch()->build_component_count_map(msg, totalNumBosses);

    for (int i = 0; i < numMyVertices; i++) {
        // query the group chare to get component count
        int myComponentCount = libGroup.ckLocalBranch()->get_component_count(myVertices[i].componentNumber);
        CkAssert(myVertices[i].componentNumber < totalNumBosses);
        if (myComponentCount <= componentPruneThreshold) {
            // vertex belongs to a minor component, ignore by setting to -1
            myVertices[i].componentNumber = -1;
        }
    }
}


// library group chare class definitions
void UnionFindLibGroup::
build_component_count_map(CkReductionMsg *msg, int numCompsOriginal) {
    if (!map_built) {
        componentCountMap *final_map = (componentCountMap*)msg->getData();
        int numComps = msg->getSize();
        numComps /= sizeof(componentCountMap);

        if (CkMyPe() == 0) {
            CkPrintf("Number of components found: %d\n", numComps);
            CkPrintf("Number of components before pruning: %d\n", numCompsOriginal);
        }

        // convert custom map back to STL for quick lookup
        for (int i = 0; i < numComps; i++) {
            component_count_map[final_map[i].compNum] = final_map[i].count;
            if (CkMyPe() == 0) {
                CkPrintf("Component %d has %d vertices\n", final_map[i].compNum, final_map[i].count);
            }
        }

        // map is built now on each PE, share among local chares
        map_built = true;
    }
}
#endif
