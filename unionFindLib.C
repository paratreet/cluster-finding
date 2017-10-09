#include "unionFindLib.h"

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
        d.initID = vid1;
        d.isFBOne = 1;
        this->thisProxy[vid1_loc.first].insertDataFindBoss(d);

        //for profiling
        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
    }
}

void UnionFindLib::
find_boss1(int arrIdx, long int partnerID, long int initID) {
    unionFindVertex *src = &myVertices[arrIdx];

    if (src->parent == -1) {
        //boss1 found
        std::pair<int, int> partner_loc = getLocationFromID(partnerID);
        //message the chare containing the partner
        //initID for find_boss2 will be partnerID itself
        
        findBossData d;
        d.arrIdx = partner_loc.second;
        d.partnerOrBossID = src->vertexID;
        d.initID = partnerID;
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
                d.initID = initID;
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
        //message remote chare containing parent, pass on the initID
        
        findBossData d;
        d.arrIdx = parent_loc.second;
        d.partnerOrBossID = partnerID;
        d.initID = initID;
        d.isFBOne = 1;
        this->thisProxy[parent_loc.first].insertDataFindBoss(d);

        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
    }
}


void UnionFindLib::
find_boss2(int arrIdx, long int boss1ID, long int initID) {
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
                d.initID = initID;
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
        //message chare containing parent

        findBossData d;
        d.arrIdx = parent_loc.second;
        d.partnerOrBossID = boss1ID;
        d.initID = initID;
        d.isFBOne = 0;
        this->thisProxy[parent_loc.first].insertDataFindBoss(d);

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
    for (int i = 0; i < numMyVertices; i++) {
        unionFindVertex *v = &myVertices[i];
        if (v->parent == -1) {
            // one of the bosses/root found
            set_component(i, v->vertexID);
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
        CkStartQD(cb);
    }
}

void UnionFindLib::
insertDataFindBoss(const findBossData & data) {
    if (data.isFBOne == 1) {
        this->find_boss1(data.arrIdx, data.partnerOrBossID, data.initID);
    }
    else {
        this->find_boss2(data.arrIdx, data.partnerOrBossID, data.initID);
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
        this->thisProxy[requestor_loc.first].set_component(requestor_loc.second, compNum);
        // done with current requestor, delete from request queue
        req_iter = myVertices[arrIdx].need_boss_requests.erase(req_iter);
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
    libGroup.ckLocalBranch()->build_component_count_map(msg);

    for (int i = 0; i < numMyVertices; i++) {
        // query the group chare to get component count
        int myComponentCount = libGroup.ckLocalBranch()->get_component_count(myVertices[i].componentNumber);
        if (myComponentCount <= componentPruneThreshold) {
            // vertex belongs to a minor component, ignore by setting to -1
            myVertices[i].componentNumber = -1;
        }
    }
}


// library group chare class definitions
void UnionFindLibGroup::
build_component_count_map(CkReductionMsg *msg) {
    if (!map_built) {
        componentCountMap *final_map = (componentCountMap*)msg->getData();
        int numComps = msg->getSize();
        numComps /= sizeof(componentCountMap);

        if (CkMyPe() == 0) {
            CkPrintf("Number of components found: %d\n", numComps);
        }

        // convert custom map back to STL for quick lookup
        for (int i = 0; i < numComps; i++) {
            component_count_map[final_map[i].compNum] = final_map[i].count;
        }

        // map is built now on each PE, share among local chares
        map_built = true;
    }
}

int UnionFindLibGroup::
get_component_count(long int component_id) {
    return component_count_map[component_id];
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
unionFindInit(CkArrayID clientArray, CkCallback cb, int n) {
    CkArrayOptions opts(n);
    opts.bindTo(clientArray);
    CProxy_UnionFindLib libProxy = CProxy_UnionFindLib::ckNew(cb, opts, NULL);
    libGroupID = CProxy_UnionFindLibGroup::ckNew();
    return libProxy;
}

#include "unionFindLib.def.h"
