/*
 * the idea behind this design is to replace the steady -> no predict transition from chen-baer rpt approach
 * with a confidence counter
 * confidence 0-1: don't prefetch (pattern not yet established)
 * 2-3: prefetch degree 1 (cautious)
 * 4-5: prefetch degree 2 (mature pattern)
 * 6-7: prefetch degree 4 (high confidence and therefore stable stream)
 *
 * each correct stride increments confidence
 * each stride break decrements it 
 */
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdint>
#include <cassert>
#include "pin.H"

using namespace std;

const static UINT64 ADDRESS_SIZE = 64;

KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "outfile", "cdp.out", "Cache results file name");
KNOB<string> KnobConfigFile(KNOB_MODE_WRITEONCE, "pintool", "config", "config-dm", "Configuration file name");
KNOB<UINT64> KnobInstructionCount(KNOB_MODE_WRITEONCE, "pintool", "max_inst", "200000000", "Number of instructions to profile");
KNOB<UINT64> KnobMode(KNOB_MODE_WRITEONCE, "pintool", "cdp_mode", "3", "0=none 1=fixed 2=global_fdp 3=cdp(novel)");
KNOB<UINT64> KnobMaxConf(KNOB_MODE_WRITEONCE, "pintool", "cdp_max_conf", "7", "Max confidence (saturating)");
KNOB<UINT64> KnobThPf(KNOB_MODE_WRITEONCE, "pintool", "cdp_threshold_pf", "1", "Confidence threshold to start prefetching");
KNOB<UINT64> KnobThD2(KNOB_MODE_WRITEONCE, "pintool", "cdp_threshold_d2", "4", "Confidence threshold for degree-2");
KNOB<UINT64> KnobThD4(KNOB_MODE_WRITEONCE, "pintool", "cdp_threshold_d4", "8", "Confidence threshold for degree-4");
KNOB<UINT64> KnobRPTSize(KNOB_MODE_WRITEONCE, "pintool", "rpt", "64", "RPT entries");
KNOB<UINT64> KnobPFBufEntries(KNOB_MODE_WRITEONCE, "pintool", "pf_buf_entries", "16", "Prefetch buffer entries");
KNOB<UINT64> KnobVcEntries(KNOB_MODE_WRITEONCE, "pintool", "vc_entries", "0", "Victim cache entries");
KNOB<UINT64> KnobFDPWindow(KNOB_MODE_WRITEONCE, "pintool","fdp_window", "100000", "FDP feedback window");
KNOB<UINT64> KnobAccHigh(KNOB_MODE_WRITEONCE, "pintool","fdp_acc_high", "75", "FDP accuracy threshold to ramp up (%)");
KNOB<UINT64> KnobAccLow(KNOB_MODE_WRITEONCE, "pintool", "fdp_acc_low", "40", "FDP accuracy threshold to ramp down (%)");
KNOB<UINT64> KnobPollHigh(KNOB_MODE_WRITEONCE, "pintool", "fdp_poll_high", "25", "FDP pollution threshold (%)");

struct cacheEntry {
    int LRU_status;
    unsigned long Tag;
    bool Valid;
};

class cache {
public:
    virtual ~cache() {}
    virtual void addressRequest(unsigned long address);
    virtual UINT64 getTotalMiss() { return totalMisses; }
    virtual UINT64 getHit() { return hits; }
    virtual UINT64 getRequest() { return requests; }
    virtual UINT64 getEntryRemoved() { return entriesKickedOut; }
    virtual int getCacheSize() { return totalCacheSz; }
    virtual int getCacheAssoc() { return assoc; }
    virtual int getCacheBlockSize() { return blockSz; }
    virtual unsigned int getTagSize() { return tagSize; }
    virtual unsigned int getBlockOffsetSize() { return blockOffsetSize; }
    virtual unsigned int getSetSize() { return setSize; }
protected:
    cache(int blockSize, int totalCacheSize, int associativity, cache* nextLevel, bool writebackDirty);
    unsigned int getTag(unsigned long address);
    unsigned int getSet(unsigned long address);
    int isHit(unsigned int tagBits, unsigned int setBits);
    void updateLRU(int setBits, int MRU_index);
    int getLRU(int setBits);
    void clearCache();
    void addTotalMiss() { totalMisses++; }
    void addHit() { hits++; }
    void addRequest() { requests++; }
    void addEntryRemoved() { entriesKickedOut++; }

    const int blockSz, totalCacheSz, assoc;
    const unsigned int blockOffsetSize, setSize, tagSize;
    const unsigned int tagMask, setMask;
    const int maxSetValue;
    UINT64 totalMisses, hits, requests, entriesKickedOut;
    cacheEntry* cacheMem;
    cache* const nextLevel;
    const bool writebackDirty;
};

class memory : public cache {
public:
    memory() : cache(1, 1, 1, nullptr, false) {}
    void addressRequest(unsigned long address) override { (void)address; addRequest(); }
};

class victim_cache : public cache {
public:
    victim_cache(int blockSize, int totalCacheSize) : cache(blockSize, totalCacheSize, totalCacheSize / blockSize, nullptr, false), vcHits(0) {}
    bool probe(unsigned long address) {
        unsigned int t = getTag(address); unsigned int s = getSet(address);
        int idx = isHit(t, s);
        if (idx != -1) { vcHits++; updateLRU(s, idx); return true; }
        return false;
    }
    bool contains(unsigned long address) { return isHit(getTag(address), getSet(address)) != -1; }
    unsigned long insert(unsigned long address) {
        unsigned int t = getTag(address); unsigned int s = getSet(address);
        int lru = getLRU(s);
        unsigned long evictedAddr = 0;
        if (cacheMem[lru + s*assoc].Valid)
            evictedAddr = (unsigned long)cacheMem[lru + s*assoc].Tag << (getSetSize() + getBlockOffsetSize());
        cacheMem[lru + s*assoc].Tag = t;
        cacheMem[lru + s*assoc].Valid = true;
        updateLRU(s, lru);
        addEntryRemoved();
        return evictedAddr;
    }
    UINT64 getVCHits() { return vcHits; }
private:
    UINT64 vcHits;
};

class prefetch_buffer : public cache {
public:
    prefetch_buffer(int blockSize, int totalCacheSize) : cache(blockSize, totalCacheSize, totalCacheSize / blockSize, nullptr, false), pfHits(0), pfInserts(0), pfPollutions(0) {
        used = new bool[totalCacheSize / blockSize]();
    }
    ~prefetch_buffer() { delete[] used; }
    bool probe(unsigned long address) {
        unsigned int t = getTag(address); unsigned int s = getSet(address);
        int idx = isHit(t, s);
        if (idx != -1) { pfHits++; used[idx + s*assoc] = true; updateLRU(s, idx); return true; }
        return false;
    }
    bool contains(unsigned long address) { return isHit(getTag(address), getSet(address)) != -1; }
    void insert(unsigned long address) {
        unsigned int t = getTag(address); unsigned int s = getSet(address);
        int idx = isHit(t, s);
        if (idx != -1) { updateLRU(s, idx); return; }
        int lru = getLRU(s);
        int slot = lru + s*assoc;
        if (cacheMem[slot].Valid && !used[slot]) pfPollutions++;
        cacheMem[slot].Tag = t;
        cacheMem[slot].Valid = true;
        used[slot] = false;
        updateLRU(s, lru);
        pfInserts++;
    }
    UINT64 getPFHits() { return pfHits; }
    UINT64 getPFInserts() { return pfInserts; }
    UINT64 getPFPollutions() { return pfPollutions; }
private:
    UINT64 pfHits, pfInserts, pfPollutions;
    bool *used;
};

class l1icache : public cache {
public:
    l1icache(int b, int t, int a, cache *n) : cache(b, t, a, n, false) {}
};

class l2cache : public cache {
public:
    l2cache(int b, int t, int a, cache *n) : cache(b, t, a, n, true) {}
};

class l1dcache : public cache {
public:
    l1dcache(int b, int t, int a, cache *n, victim_cache *v, prefetch_buffer *p) : cache(b, t, a, n, true), vc(v), pfb(p) {}
    void addressRequest(unsigned long address) override;
    void prefetchLine(unsigned long address);
    bool last_was_pf_hit = false;
private:
    victim_cache *vc;
    prefetch_buffer *pfb;
};

// rpt entry tracks pc, last address, stride, and confidence level
// confidence determines degree: low -> don't prefetch, high -> prefetch aggressively
struct RPTEntry {
    unsigned long pc;
    unsigned long last_addr;
    long stride;
    uint8_t confidence;
    bool valid;
};

class cd_prefetcher {
public:
    cd_prefetcher(UINT64 nEntries, l1dcache *dc, int blockSize, int mode, uint8_t max_conf, uint8_t th_pf, uint8_t th_d2, uint8_t th_d4) : nEntries(nEntries), dcache(dc), blockSize(blockSize), mode(mode), max_conf(max_conf), th_pf(th_pf), th_d2(th_d2), th_d4(th_d4), prefetches_issued(0), correct_predictions(0), global_aggressiveness(0), global_win_issued(0), global_win_hits(0), global_win_pollutions(0), feedback_adjustments(0) {
        rpt = new RPTEntry[nEntries];
        for (UINT64 i = 0; i < nEntries; i++) { rpt[i].valid = false; rpt[i].confidence = 0; }
    }
    void access(unsigned long pc, unsigned long addr);
    void runFeedbackAdjustment(UINT64 acc_high, UINT64 acc_low, UINT64 poll_high);
    UINT64 getPrefetchesIssued() { return prefetches_issued; }
    UINT64 getCorrectPredictions() { return correct_predictions; }
    UINT64 getFeedbackAdjustments() { return feedback_adjustments; }
    int getGlobalAggressiveness() { return global_aggressiveness; }
    void getConfidenceHistogram(UINT64 hist[]) {
        for (UINT64 i = 0; i <= max_conf; i++) hist[i] = 0;
        for (UINT64 i = 0; i < nEntries; i++)
            if (rpt[i].valid) hist[rpt[i].confidence]++;
    }
    uint8_t getMaxConf() { return max_conf; }
private:
    UINT64 nEntries;
    RPTEntry *rpt;
    l1dcache *dcache;
    int blockSize;
    int mode;
    uint8_t max_conf, th_pf, th_d2, th_d4;
    UINT64 prefetches_issued;
    UINT64 correct_predictions;
    int global_aggressiveness;
    UINT64 global_win_issued, global_win_hits, global_win_pollutions;
    UINT64 feedback_adjustments;
    UINT64 lookupSlot(unsigned long pc) { return (pc >> 2) & (nEntries - 1); }
    UINT64 confidenceToDegree(uint8_t c) const {
        if (c < th_pf) return 0;
        return 1;
    }
};

void cd_prefetcher::access(unsigned long pc, unsigned long addr) {
    if (mode == 0) return;
    UINT64 idx = lookupSlot(pc);
    RPTEntry &e = rpt[idx];
    if (!e.valid || e.pc != pc) {
        e.valid = true; e.pc = pc; e.last_addr = addr; e.stride = 0; e.confidence = 0;
        return;
    }

    long obs = (long)addr - (long)e.last_addr;

    if (dcache != nullptr && dcache->last_was_pf_hit) global_win_hits++;

    if (mode == 1) {
        if (obs == e.stride && e.stride != 0) {
            if (e.confidence < 2) e.confidence++;
            else correct_predictions++;
        } else {
            e.stride = obs;
            if (e.confidence > 0) e.confidence--;
        }
        e.last_addr = addr;
        if (e.confidence >= 2 && e.stride != 0) {
            unsigned long pf = ((unsigned long)((long)addr + e.stride)) & ~((unsigned long)blockSize - 1);
            if (dcache != nullptr) { dcache->prefetchLine(pf); prefetches_issued++; global_win_issued++; }
        }
        return;
    }

    if (mode == 2) {
        if (obs == e.stride && e.stride != 0) {
            if (e.confidence < 2) e.confidence++;
            else correct_predictions++;
        } else {
            e.stride = obs;
            if (e.confidence > 0) e.confidence--;
        }
        e.last_addr = addr;
        UINT64 deg_map[4] = {1, 2, 4, 8};
        UINT64 deg = deg_map[global_aggressiveness];
        if (e.confidence >= 2 && e.stride != 0) {
            for (UINT64 d = 0; d < deg; d++) {
                long ahead = e.stride * (long)(1 + d);
                unsigned long pf = ((unsigned long)((long)addr + ahead)) & ~((unsigned long)blockSize - 1);
                if (dcache != nullptr) { dcache->prefetchLine(pf); prefetches_issued++; global_win_issued++; }
            }
        }
        return;
    }

    // mode 3: cdp
    // wait for first real stride observation before doing anything
    if (e.stride == 0) {
        if (obs != 0) e.stride = obs;
        e.last_addr = addr;
        return;
    }

    if (obs == e.stride) {
        if (e.confidence < max_conf) e.confidence++;
        if (e.confidence >= th_pf) correct_predictions++;
    } else {
        // stride mismatch: decrement confidence, only update stride when fully drained
        if (e.confidence > 0) e.confidence--;
        if (e.confidence == 0) e.stride = obs;
    }
    e.last_addr = addr;

    UINT64 deg = confidenceToDegree(e.confidence);
    if (deg > 0 && e.stride != 0) {
        for (UINT64 d = 0; d < deg; d++) {
            long ahead = e.stride * (long)(1 + d);
            unsigned long pf = ((unsigned long)((long)addr + ahead)) & ~((unsigned long)blockSize - 1);
            if (dcache != nullptr) { dcache->prefetchLine(pf); prefetches_issued++; global_win_issued++; }
        }
    }
}

void cd_prefetcher::runFeedbackAdjustment(UINT64 acc_high, UINT64 acc_low, UINT64 poll_high) {
    if (mode != 2) { global_win_issued = global_win_hits = global_win_pollutions = 0; return; }
    feedback_adjustments++;
    if (global_win_issued > 0) {
        UINT64 acc = (100 * global_win_hits) / global_win_issued;
        UINT64 poll = (global_win_issued > 0) ? (100 * global_win_pollutions) / global_win_issued : 0;
        if (poll >= poll_high) { if (global_aggressiveness > 0) global_aggressiveness--; }
        else if (acc < acc_low) { if (global_aggressiveness > 0) global_aggressiveness--; }
        else if (acc >= acc_high) { if (global_aggressiveness < 3) global_aggressiveness++; }
    }
    global_win_issued = global_win_hits = global_win_pollutions = 0;
}

l1icache *icache = nullptr;
l1dcache *dcache = nullptr;
victim_cache *vcache = nullptr;
prefetch_buffer *pfbuf = nullptr;
l2cache *llcache = nullptr;
memory *mem = nullptr;
cd_prefetcher *sp = nullptr;

UINT64 icount = 0;
UINT64 dcacheMissesAvoidedByPF = 0;
UINT64 nextWindowAt = 0;

void PrintResults();

void l1dcache::addressRequest(unsigned long address) {
    last_was_pf_hit = false;
    unsigned long tagField = getTag(address);
    unsigned long setField = getSet(address);
    addRequest();
    int index = isHit(tagField, setField);
    if (index != -1) { addHit(); updateLRU(setField, index); return; }
    addTotalMiss();

    if (vc != nullptr && vc->probe(address)) {
        int lru = getLRU(setField);
        if (cacheMem[lru + setField*assoc].Valid) {
            unsigned long evt = cacheMem[lru + setField*assoc].Tag;
            unsigned long evictAddr = (evt << (getSetSize() + getBlockOffsetSize())) | ((unsigned long)setField << getBlockOffsetSize());
            vc->insert(evictAddr);
        }
        cacheMem[lru + setField*assoc].Tag = tagField;
        cacheMem[lru + setField*assoc].Valid = true;
        updateLRU(setField, lru);
        return;
    }

    if (pfb != nullptr && pfb->probe(address)) {
        dcacheMissesAvoidedByPF++;
        last_was_pf_hit = true;
        int lru = getLRU(setField);
        if (cacheMem[lru + setField*assoc].Valid) {
            addEntryRemoved();
            unsigned long evt = cacheMem[lru + setField*assoc].Tag;
            unsigned long evictAddr = (evt << (getSetSize() + getBlockOffsetSize())) | ((unsigned long)setField << getBlockOffsetSize());
            if (vc != nullptr) {
                unsigned long disp = vc->insert(evictAddr);
                if (disp != 0) nextLevel->addressRequest(disp);
            } else {
                nextLevel->addressRequest(evictAddr);
            }
        }
        cacheMem[lru + setField*assoc].Tag = tagField;
        cacheMem[lru + setField*assoc].Valid = true;
        updateLRU(setField, lru);
        return;
    }

    int lru = getLRU(setField);
    if (cacheMem[lru + setField*assoc].Valid) {
        addEntryRemoved();
        unsigned long evt = cacheMem[lru + setField*assoc].Tag;
        unsigned long evictAddr = (evt << (getSetSize() + getBlockOffsetSize())) | ((unsigned long)setField << getBlockOffsetSize());
        if (vc != nullptr) {
            unsigned long disp = vc->insert(evictAddr);
            if (disp != 0) nextLevel->addressRequest(disp);
        } else {
            nextLevel->addressRequest(evictAddr);
        }
    }
    nextLevel->addressRequest(address);
    cacheMem[lru + setField*assoc].Tag = tagField;
    cacheMem[lru + setField*assoc].Valid = true;
    updateLRU(setField, lru);
}

void l1dcache::prefetchLine(unsigned long address) {
    unsigned long t = getTag(address);
    unsigned long s = getSet(address);
    if (isHit(t, s) != -1) return;
    if (vc != nullptr && vc->contains(address)) return;
    if (pfb != nullptr && pfb->contains(address)) return;
    nextLevel->addressRequest(address);
    if (pfb != nullptr) pfb->insert(address);
}

cache::cache(int blockSize, int totalCacheSize, int associativity, cache* nextLevel_, bool wb) : blockSz(blockSize), totalCacheSz(totalCacheSize), assoc(associativity), blockOffsetSize((unsigned)log2(blockSize)), setSize((unsigned)log2(totalCacheSize / (blockSize * associativity))), tagSize(ADDRESS_SIZE - blockOffsetSize - setSize), tagMask((1u << tagSize) - 1u), setMask((1u << setSize) - 1u), maxSetValue((int)1 << setSize), nextLevel(nextLevel_), writebackDirty(wb) {
    cacheMem = new cacheEntry[totalCacheSize / blockSize];
    clearCache();
    totalMisses = hits = requests = entriesKickedOut = 0;
}

void cache::clearCache() {
    for (int i = 0; i < maxSetValue * assoc; i++) {
        cacheMem[i].LRU_status = (i % assoc);
        cacheMem[i].Tag = 0;
        cacheMem[i].Valid = false;
    }
}

unsigned int cache::getTag(unsigned long a) { return (unsigned int)((a >> (blockOffsetSize + setSize)) & tagMask); }
unsigned int cache::getSet(unsigned long a) { return (unsigned int)((a >> blockOffsetSize) & setMask); }

int cache::isHit(unsigned int t, unsigned int s) {
    for (int i = 0; i < assoc; i++)
        if (cacheMem[i + s*assoc].Valid && cacheMem[i + s*assoc].Tag == t) return i;
    return -1;
}

void cache::updateLRU(int s, int mru) {
    int upper = assoc - 1;
    for (int i = 0; i < assoc; i++)
        if (cacheMem[i + s*assoc].LRU_status >= 0 && cacheMem[i + s*assoc].LRU_status < upper)
            cacheMem[i + s*assoc].LRU_status++;
    cacheMem[mru + s*assoc].LRU_status = 0;
}

int cache::getLRU(int s) {
    for (int i = 0; i < assoc; i++)
        if (cacheMem[i + s*assoc].LRU_status == (assoc - 1)) return i;
    return -1;
}

void cache::addressRequest(unsigned long address) {
    unsigned long t = getTag(address);
    unsigned long s = getSet(address);
    int idx = isHit(t, s);
    addRequest();
    if (idx == -1) {
        int lru = getLRU(s);
        if (cacheMem[lru + s*assoc].Valid) addEntryRemoved();
        addTotalMiss();
        assert(nextLevel != nullptr);
        if (writebackDirty && cacheMem[lru + s*assoc].Valid) {
            unsigned long et = cacheMem[lru + s*assoc].Tag;
            unsigned long ea = (et << (getSetSize() + getBlockOffsetSize())) | ((unsigned long)s << getBlockOffsetSize());
            nextLevel->addressRequest(ea);
        }
        nextLevel->addressRequest(address);
        cacheMem[lru + s*assoc].Tag = t;
        cacheMem[lru + s*assoc].Valid = true;
        updateLRU(s, lru);
    } else {
        addHit(); updateLRU(s, idx);
    }
}

INT32 Usage() {
    cerr << "Confidence-Driven Prefetcher (CS1411 May 5)\n";
    cerr << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}

void CreateCaches() {
    ifstream config; config.open(KnobConfigFile.Value().c_str());
    if (!config.is_open()) { cerr << "Cannot open config file: " << KnobConfigFile.Value() << "\n"; Usage(); PIN_ExitProcess(EXIT_FAILURE); }
    mem = new memory();
    int i = 0;
    while (!config.eof()) {
        string line; getline(config, line);
        if (line.empty()) { i++; continue; }
        istringstream parser(line);
        int bsize, csize, assoc, vsize = 0; char comma;
        switch (i) {
            case 0:
                parser >> bsize >> comma >> csize >> comma >> assoc;
                llcache = new l2cache(bsize, csize, assoc, mem); break;
            case 1:
                parser >> bsize >> comma >> csize >> comma >> assoc;
                icache = new l1icache(bsize, csize, assoc, llcache); break;
            case 2: {
                parser >> bsize >> comma >> csize >> comma >> assoc >> comma >> vsize;
                int eff_v = (int)KnobVcEntries.Value();
                if (eff_v == 0) eff_v = vsize;
                vcache = (eff_v > 0) ? new victim_cache(bsize, bsize * eff_v) : nullptr;
                if (KnobMode.Value() != 0) pfbuf = new prefetch_buffer(bsize, bsize * (int)KnobPFBufEntries.Value());
                dcache = new l1dcache(bsize, csize, assoc, llcache, vcache, pfbuf);
                break;
            }
            default: break;
        }
        i++;
    }
    if (KnobMode.Value() != 0)
        sp = new cd_prefetcher(KnobRPTSize.Value(), dcache, dcache->getCacheBlockSize(), (int)KnobMode.Value(), (uint8_t)KnobMaxConf.Value(), (uint8_t)KnobThPf.Value(), (uint8_t)KnobThD2.Value(), (uint8_t)KnobThD4.Value());
    nextWindowAt = KnobFDPWindow.Value();
}

void CheckLimits() {
    if (KnobInstructionCount.Value() > 0 && icount > KnobInstructionCount.Value()) { PrintResults(); PIN_ExitProcess(EXIT_SUCCESS); }
}

void MemoryOp(ADDRINT pc, ADDRINT address) {
    dcache->addressRequest(address);
    if (sp != nullptr) sp->access((unsigned long)pc, (unsigned long)address);
}

void AllInstructions(ADDRINT ins_ptr) {
    icount++;
    icache->addressRequest(ins_ptr);
    if (sp != nullptr && KnobMode.Value() == 2 && icount >= nextWindowAt) {
        sp->runFeedbackAdjustment(KnobAccHigh.Value(), KnobAccLow.Value(), KnobPollHigh.Value());
        nextWindowAt += KnobFDPWindow.Value();
    }
    CheckLimits();
}

void PrintResults() {
    ofstream out(KnobOutputFile.Value().c_str());
    out.setf(ios::fixed, ios::floatfield); out.precision(2);

    out << "CDP Mode: " << KnobMode.Value() << "  (0=none, 1=fixed, 2=global_fdp, 3=cdp)\n";
    if (KnobMode.Value() == 3) {
        out << "CDP max_conf: " << KnobMaxConf.Value() << "\n" << "CDP threshold_pf: " << KnobThPf.Value() << "\n" << "CDP threshold_d2: " << KnobThD2.Value() << "\n" << "CDP threshold_d4: " << KnobThD4.Value() << "\n";
    }
    out << "Instructions: " << icount << "\n\n";

    out << "I-Cache Miss: " << icache->getTotalMiss() << " out of " << icache->getRequest() << "\n";
    out << "D-Cache Miss: " << dcache->getTotalMiss() << " out of " << dcache->getRequest() << "\n";
    out << "L2-Cache Miss: " << llcache->getTotalMiss() << " out of " << llcache->getRequest() << "\n";
    out << "Mem requests: " << mem->getRequest() << "\n";

    UINT64 vcHits = 0;
    if (vcache != nullptr) { vcHits = vcache->getVCHits(); out << "Victim Cache Hits: " << vcHits << "\n"; }
    if (pfbuf != nullptr) {
        out << "Prefetch Buffer Hits: " << pfbuf->getPFHits() << "\n" << "Prefetch Buffer Inserts: " << pfbuf->getPFInserts() << "\n" << "Prefetch Buffer Pollutions: " << pfbuf->getPFPollutions() << "\n" << "D-Cache Misses Serviced By Prefetch: " << dcacheMissesAvoidedByPF << "\n";
    }
    if (sp != nullptr) {
        out << "Stride Prefetches Issued: " << sp->getPrefetchesIssued() << "\n" << "Stride Correct Predictions: " << sp->getCorrectPredictions() << "\n";
        if (sp->getPrefetchesIssued() > 0 && pfbuf != nullptr) {
            double acc = 100.0 * (double)pfbuf->getPFHits() / (double)sp->getPrefetchesIssued();
            out << "Prefetch Accuracy: " << acc << "%\n";
        }
        if (KnobMode.Value() == 3) {
            UINT64 hist[16] = {0};
            sp->getConfidenceHistogram(hist);
            uint8_t mc = sp->getMaxConf();
            out << "Confidence Histogram (entries at each level):\n";
            for (uint8_t c = 0; c <= mc; c++) out << "  conf=" << (int)c << ": " << hist[c] << "\n";
        }
        if (KnobMode.Value() == 2) {
            out << "FDP Feedback Adjustments: " << sp->getFeedbackAdjustments() << "\n" << "Final Global Aggressiveness: " << sp->getGlobalAggressiveness() << "\n";
        }
    }

    UINT64 raw = dcache->getTotalMiss();
    UINT64 avoided = vcHits + dcacheMissesAvoidedByPF;
    UINT64 eff = (avoided <= raw) ? (raw - avoided) : 0;
    out << "EFFECTIVE D-Cache Misses: " << eff << "\n";
}

void Instruction(INS ins, VOID *v) {
    (void)v;
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AllInstructions, IARG_INST_PTR, IARG_END);
    if (INS_IsMemoryRead(ins)) INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryOp, IARG_INST_PTR, IARG_MEMORYREAD_EA, IARG_END);
    if (INS_IsMemoryWrite(ins)) INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryOp, IARG_INST_PTR, IARG_MEMORYWRITE_EA, IARG_END);
}

void Fini(int n, VOID *v) { (void)n; (void)v; PrintResults(); }

int main(int argc, char *argv[]) {
    if (PIN_Init(argc, argv)) return Usage();
    CreateCaches();
    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_StartProgram();
    return 0;
}