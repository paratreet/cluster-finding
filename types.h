struct findBossData {
    uint64_t arrIdx;
    uint64_t partnerOrBossID;
    uint64_t initID;
    uint64_t isFBOne;

    void pup(PUP::er &p) {
    p|arrIdx;
    p|partnerOrBossID;
    p|initID;
    p|isFBOne;
    }
};