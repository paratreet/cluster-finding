#ifndef UNION_FIND_LIB
#define UNION_FIND_LIB

#include "unionFindLib.decl.h"
#include <NDMeshStreamer.h>

struct unionFindVertex {
    long int vertexID;
    long int parent;
    long int componentNumber = -1;
    std::vector<long int> need_boss_requests; //request queue for processing need_boss requests

    void pup(PUP::er &p) {
        p|vertexID;
        p|parent;
        p|componentNumber;
        p|need_boss_requests;
    }
};

struct componentCountMap {
    long int compNum;
    int count;

    void pup(PUP::er &p) {
        p|compNum;
        p|count;
    }
};


/* global variables */
/*readonly*/ extern CkGroupID libGroupID;
// declaration for custom reduction
extern CkReduction::reducerType mergeCountMapsReductionType;

// class definition for library chares
class UnionFindLib : public CBase_UnionFindLib {
    unionFindVertex *myVertices;
    int numMyVertices;
    int pathCompressionThreshold = 5;
    int componentPruneThreshold;
    std::pair<int, int> (*getLocationFromID)(long int vid);
    int myLocalNumBosses;
    int totalNumBosses;
    CkCallback postComponentLabelingCb;

    public:
    UnionFindLib() {}
    UnionFindLib(CkMigrateMessage *m) { }
    static CProxy_UnionFindLib unionFindInit(CkArrayID clientArray, int n);
    void register_phase_one_cb(CkCallback cb);
    void initialize_vertices(unionFindVertex *appVertices, int numVertices);
    void union_request(long int vid1, long int vid2);
    void find_boss1(int arrIdx, long int partnerID, long int senderID);
    void find_boss2(int arrIdx, long int boss1ID, long int senderID);
    void local_path_compression(unionFindVertex *src, long int compressedParent);
    bool check_same_chares(long int v1, long int v2);
    void short_circuit_parent(shortCircuitData scd);
    void compress_path(int arrIdx, long int compressedParent);
    unionFindVertex* return_vertices();
    void registerGetLocationFromID(std::pair<int, int> (*gloc)(long int v));

    // functions and data structures for finding connected components

    public:
    void find_components(CkCallback cb);
    void boss_count_prefix_done(int totalCount);
    void start_component_labeling();
    void insertDataNeedBoss(const uint64_t & data);
    void insertDataFindBoss(const findBossData & data);
    void need_boss(int arrIdx, long int fromID);
    void set_component(int arrIdx, long int compNum);
    void prune_components(int threshold, CkCallback appReturnCb);
    void perform_pruning();
    int get_total_num_bosses() {
        return totalNumBosses;
    }
    //void merge_count_results(CkReductionMsg *msg);
    //void merge_count_results(int* totalCounts, int numElems);
};

// library group chare class declarations
class UnionFindLibGroup : public CBase_UnionFindLibGroup {
    bool map_built;
    int* component_count_array;
    int thisPeMessages; //for profiling
    public:
    UnionFindLibGroup() {
        map_built = false;
        thisPeMessages = 0;
    }
    void build_component_count_array(int* totalCounts, int numComponents);
    int get_component_count(long int component_id);
    void increase_message_count();
    void contribute_count();
    void done_profiling(int);
};





#define CK_TEMPLATES_ONLY
#include "unionFindLib.def.h"
#undef CK_TEMPLATES_ONLY

#endif

/// Some old functions for backup/reference ///

/*
void UnionFindLib::
start_boss_propagation() {
    // iterate over local bosses and send messages to requestors
    CkPrintf("Should never get executed!\n");
    std::vector<int>::iterator iter = local_boss_indices.begin();
    while (iter != local_boss_indices.end()) {
        int bossIdx = *iter;
        long int bossID = myVertices[bossIdx].vertexID;
        std::vector<long int>::iterator req_iter = myVertices[bossIdx].need_boss_requests.begin();
        while (req_iter != myVertices[bossIdx].need_boss_requests.end()) {
            long int requestorID = *req_iter;
            std::pair<int,int> requestor_loc = appPtr->getLocationFromID(requestorID);
            this->thisProxy[requestor_loc.first].set_component(requestor_loc.second, bossID);
            // done with requestor, delete from requests queue
            req_iter = myVertices[bossIdx].need_boss_requests.erase(req_iter);
        }
        // done with this local boss, delete from vector
        iter = local_boss_indices.erase(iter);
    }
}*/

/*
void UnionFindLib::
merge_count_results(CkReductionMsg *msg) {
    componentCountMap *final_map = (componentCountMap*)msg->getData();
    int numComps = msg->getSize();
    numComps = numComps/ sizeof(componentCountMap);

    if (this->thisIndex == 0) {
        CkPrintf("Number of components found: %d\n", numComps);
    }

    // convert custom map back to STL, for easier lookup
    std::map<long int,int> quick_final_map;
    for (int i = 0; i < numComps; i++) {
        if (this->thisIndex == 0) {
            //CkPrintf("Component %ld : Total vertices count = %d\n", final_map[i].compNum, final_map[i].count);
        }
        quick_final_map[final_map[i].compNum] = final_map[i].count;
    }

    for (int i = 0; i < numMyVertices; i++) {
        if (quick_final_map[myVertices[i].componentNumber] <= componentPruneThreshold) {
            // vertex belongs to a minor component, ignore by setting to -1
            myVertices[i].componentNumber = -1;
        }
    }

    delete msg;
}*/
