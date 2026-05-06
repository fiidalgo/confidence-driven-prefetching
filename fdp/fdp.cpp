/*
 * ive created 4 modes, this file extends stride.cpp, use the -fdp_mode flag
 * 0 = no prefetch
 * 1 = fixed stride
 * 2 = global FDP
 * 3 = per-PC FDP
 */

#include <unistd.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <cassert>
#include "pin.H"

using namespace std;

const static UINT64 ADDRESS_SIZE = 64;

//knobs 
KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "outfile", "fdp.out", "Cache results file name");
KNOB<string> KnobConfigFile(KNOB_MODE_WRITEONCE, "pintool", "config", "config-dm", "Configuration file name");
KNOB<UINT64> KnobInstructionCount(KNOB_MODE_WRITEONCE, "pintool", "max_inst", "200000000", "Number of instructions to profile");
KNOB<UINT64> KnobFDPMode(KNOB_MODE_WRITEONCE, "pintool", "fdp_mode", "3", "0=no_pf, 1=fixed_stride, 2=global_fdp, 3=per_pc_fdp");
KNOB<UINT64> KnobFDPWindow(KNOB_MODE_WRITEONCE, "pintool", "fdp_window", "100000", "FDP feedback window (instructions)");
KNOB<UINT64> KnobAccHigh(KNOB_MODE_WRITEONCE, "pintool", "fdp_acc_high", "75", "Increase aggressiveness if accuracy >= X%");
KNOB<UINT64> KnobAccLow(KNOB_MODE_WRITEONCE, "pintool","fdp_acc_low", "40", "Decrease aggressiveness if accuracy < X%");
KNOB<UINT64> KnobPollHigh(KNOB_MODE_WRITEONCE, "pintool", "fdp_poll_high", "25", "Decrease aggressiveness if pollution >= X%");
KNOB<UINT64> KnobRPTSize(KNOB_MODE_WRITEONCE, "pintool", "rpt", "64", "RPT entries");
KNOB<UINT64> KnobPFBufEntries(KNOB_MODE_WRITEONCE, "pintool", "pf_buf_entries", "16", "Prefetch buffer entries");
KNOB<UINT64> KnobVcEntries(KNOB_MODE_WRITEONCE, "pintool", "vc_entries", "0", "Victim cache entries (0 disables)");

// cache framework is the same as stride.cpp
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
    int getMRU(int setBits);
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
    void addressRequest(unsigned long address) override {
        (void)address; addRequest();
    }
};

// victim cache
class victim_cache : public cache {
public:
    victim_cache(int blockSize, int totalCacheSize) : cache(blockSize, totalCacheSize, totalCacheSize / blockSize,nullptr, false), vcHits(0) {}
    bool probe(unsigned long address) {
        unsigned int tag = getTag(address);
        unsigned int set = getSet(address);
        int idx = isHit(tag, set);
        if (idx != -1) { vcHits++; updateLRU(set, idx); return true; }
        return false;
    }
    bool contains(unsigned long address) {
        unsigned int tag = getTag(address);
        unsigned int set = getSet(address);
        return isHit(tag, set) != -1;
    }

    unsigned long insert(unsigned long address) {
        unsigned int tag = getTag(address);
        unsigned int set = getSet(address);
        int lruIndex = getLRU(set);
        unsigned long evictedAddr = 0;
        if (cacheMem[lruIndex + set * assoc].Valid)
            evictedAddr = (unsigned long)cacheMem[lruIndex + set * assoc].Tag << (getSetSize() + getBlockOffsetSize());
        cacheMem[lruIndex + set * assoc].Tag   = tag;
        cacheMem[lruIndex + set * assoc].Valid  = true;
        updateLRU(set, lruIndex);
        addEntryRemoved();
        return evictedAddr;
    }

    UINT64 getVCHits() { return vcHits; }

private:
    UINT64 vcHits;
};

/*
 * prefetch buffer (this is diff from stride.cpp)
 * now we have some metadata per entry:
 *   bool used: was this line hit before being evicted
 *   unsigned long pc: the rpt entry that issued this prefetch
 */
class prefetch_buffer : public cache {
public:
    prefetch_buffer(int blockSize, int totalCacheSize) : cache(blockSize, totalCacheSize, totalCacheSize / blockSize, nullptr, false), pfHits(0), pfInserts(0), pfPollutions(0)
    {
        used = new bool[totalCacheSize / blockSize]();
        issuer_pc = new unsigned long[totalCacheSize / blockSize]();
    }
    ~prefetch_buffer() { delete[] used; delete[] issuer_pc; }
    bool probe(unsigned long address) {
        unsigned int t = getTag(address); unsigned int s = getSet(address);
        int idx = isHit(t, s);
        if (idx != -1) {
            pfHits++;
            used[idx + s*assoc] = true;
            last_hit_issuer = issuer_pc[idx + s*assoc];
            updateLRU(s, idx);
            return true;
        }
        last_hit_issuer = 0;
        return false;
    }
    bool contains(unsigned long address) {
        return isHit(getTag(address), getSet(address)) != -1;
    }
    unsigned long getLastHitIssuer() { return last_hit_issuer; }

    // returns pc of issuer if a polluting eviction occurred, else 0
    unsigned long insert(unsigned long address, unsigned long issuer) {
        unsigned int t = getTag(address); unsigned int s = getSet(address);
        int idx = isHit(t, s);
        if (idx != -1) { updateLRU(s, idx); return 0; }
        int lru = getLRU(s);
        int slot = lru + s*assoc;
        unsigned long polluter = 0;
        if (cacheMem[slot].Valid && !used[slot]) {
            pfPollutions++;
            polluter = issuer_pc[slot];
        }
        cacheMem[slot].Tag = t;
        cacheMem[slot].Valid = true;
        used[slot] = false;
        issuer_pc[slot] = issuer;
        updateLRU(s, lru);
        pfInserts++;
        return polluter;
    }
    UINT64 getPFHits() { return pfHits; }
    UINT64 getPFInserts() { return pfInserts; }
    UINT64 getPFPollutions() { return pfPollutions; }

private:
    UINT64 pfHits, pfInserts, pfPollutions;
    bool *used;
    unsigned long *issuer_pc;
    unsigned long last_hit_issuer = 0;
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
    l1dcache(int b, int t, int a, cache *n, victim_cache *v, prefetch_buffer *p)
      : cache(b, t, a, n, true), vc(v), pfb(p) {}

    void addressRequest(unsigned long address) override;
    void prefetchLine(unsigned long address, unsigned long issuer);
    bool last_was_pf_hit = false;
    unsigned long last_pf_issuer  = 0;
private:
    victim_cache *vc;
    prefetch_buffer *pfb;
};

// stride prefetcher with fdp
enum RPTState { RPT_INIT = 0, RPT_TRANSIENT, RPT_STEADY, RPT_NO_PRED };

struct RPTEntry {
    unsigned long pc;
    unsigned long last_addr;
    long          stride;
    int           state;
    bool          valid;

    // mode 3 is the only mode that needs these
    uint8_t aggressiveness;
    UINT32  win_pf_issued;
    UINT32  win_pf_hits;
    UINT32  win_pollutions;
};

// aggressiveness level -> prefetch degree
static const UINT64 LEVEL_TO_DEGREE[4] = {1, 2, 4, 8};

class fdp_stride_prefetcher {
public:
    fdp_stride_prefetcher(UINT64 nEntries, l1dcache *dc, int blockSize, int mode) : nEntries(nEntries), dcache(dc), blockSize(blockSize), mode(mode),
        prefetches_issued(0), correct_predictions(0),
        global_aggressiveness(0),
        global_win_issued(0), global_win_hits(0), global_win_pollutions(0),
        feedback_adjustments(0)
    {
        rpt = new RPTEntry[nEntries];
        for (UINT64 i = 0; i < nEntries; i++) {
            rpt[i].valid = false;
            rpt[i].state = RPT_INIT;
            rpt[i].aggressiveness = 0;
            rpt[i].win_pf_issued = 0;
            rpt[i].win_pf_hits   = 0;
            rpt[i].win_pollutions= 0;
        }
    }

    void access(unsigned long pc, unsigned long addr);
    void recordPollution(unsigned long issuer_pc);
    void runFeedbackAdjustment(UINT64 acc_high, UINT64 acc_low, UINT64 poll_high);
    // stats
    UINT64 getPrefetchesIssued()   { return prefetches_issued; }
    UINT64 getCorrectPredictions() { return correct_predictions; }
    UINT64 getFeedbackAdjustments() { return feedback_adjustments; }
    int getGlobalAggressiveness() { return global_aggressiveness; }
    UINT64 lookupSlot(unsigned long pc) {
        return (pc >> 2) & (nEntries - 1);
    }

private:
    UINT64 nEntries;
    RPTEntry *rpt;
    l1dcache *dcache;
    int blockSize;
    int mode;
    UINT64 prefetches_issued;
    UINT64 correct_predictions;
    int global_aggressiveness;
    UINT64 global_win_issued;
    UINT64 global_win_hits;
    UINT64 global_win_pollutions;
    UINT64 feedback_adjustments;

    UINT64 getDegree(const RPTEntry &e) const {
        if (mode == 1) return 1;
        if (mode == 2) return LEVEL_TO_DEGREE[global_aggressiveness];
        if (mode == 3) return LEVEL_TO_DEGREE[e.aggressiveness];
        return 1;
    }
};

// global vars
l1icache *icache = nullptr;
l1dcache *dcache = nullptr;
victim_cache *vcache = nullptr;
prefetch_buffer *pfbuf = nullptr;
l2cache *llcache = nullptr;
memory *mem = nullptr;
fdp_stride_prefetcher *sp = nullptr;
UINT64 icount = 0;
UINT64 dcacheMissesAvoidedByPF = 0;
UINT64 nextWindowAt = 0;
void PrintResults();

// l1d access
void l1dcache::addressRequest(unsigned long address) {
    last_was_pf_hit = false;
    last_pf_issuer  = 0;
    unsigned long tagField = getTag(address);
    unsigned long setField = getSet(address);
    addRequest();
    int index = isHit(tagField, setField);
    if (index != -1) { addHit(); updateLRU(setField, index); return; }
    addTotalMiss();
    // check vc
    if (vc != nullptr && vc->probe(address)) {
        int lru = getLRU(setField);
        if (cacheMem[lru + setField*assoc].Valid) {
            unsigned long evt = cacheMem[lru + setField*assoc].Tag;
            unsigned long evictAddr =
                (evt << (getSetSize() + getBlockOffsetSize()))
                | ((unsigned long)setField << getBlockOffsetSize());
            vc->insert(evictAddr);
        }
        cacheMem[lru + setField*assoc].Tag = tagField;
        cacheMem[lru + setField*assoc].Valid = true;
        updateLRU(setField, lru);
        return;
    }

    // check prefetch buf
    if (pfb != nullptr && pfb->probe(address)) {
        dcacheMissesAvoidedByPF++;
        last_was_pf_hit = true;
        last_pf_issuer  = pfb->getLastHitIssuer();
        int lru = getLRU(setField);
        if (cacheMem[lru + setField*assoc].Valid) {
            addEntryRemoved();
            unsigned long evt = cacheMem[lru + setField*assoc].Tag;
            unsigned long evictAddr =
                (evt << (getSetSize() + getBlockOffsetSize()))
                | ((unsigned long)setField << getBlockOffsetSize());
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

    // thisis a real miss
    int lru = getLRU(setField);
    if (cacheMem[lru + setField*assoc].Valid) {
        addEntryRemoved();
        unsigned long evt = cacheMem[lru + setField*assoc].Tag;
        unsigned long evictAddr =
            (evt << (getSetSize() + getBlockOffsetSize()))
            | ((unsigned long)setField << getBlockOffsetSize());
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

void l1dcache::prefetchLine(unsigned long address, unsigned long issuer) {
    unsigned long t = getTag(address);
    unsigned long s = getSet(address);
    if (isHit(t, s) != -1) return;
    if (vc  != nullptr && vc->contains(address))  return;
    if (pfb != nullptr && pfb->contains(address)) return;
    nextLevel->addressRequest(address);
    if (pfb != nullptr) {
        unsigned long polluter = pfb->insert(address, issuer);
        if (polluter != 0 && sp != nullptr) sp->recordPollution(polluter);
    }
}

// fdp stride prefetcher impl
void fdp_stride_prefetcher::access(unsigned long pc, unsigned long addr) {
    if (mode == 0) return;
    UINT64 idx = lookupSlot(pc);
    RPTEntry &e = rpt[idx];
    if (!e.valid || e.pc != pc) {
        e.valid = true; e.pc = pc; e.last_addr = addr;
        e.stride = 0; e.state = RPT_INIT;
        e.aggressiveness = 0;
        e.win_pf_issued = e.win_pf_hits = e.win_pollutions = 0;
        return;
    }
    long obs = (long)addr - (long)e.last_addr;
    switch (e.state) {
        case RPT_INIT:
            e.stride = obs; e.state = RPT_TRANSIENT;
            break;
        case RPT_TRANSIENT:
            if (obs == e.stride) e.state = RPT_STEADY;
            else { e.stride = obs; e.state = RPT_INIT; }
            break;
        case RPT_STEADY:
            if (obs == e.stride) correct_predictions++;
            else e.state = RPT_NO_PRED;
            break;
        case RPT_NO_PRED:
            if (obs == e.stride) e.state = RPT_TRANSIENT;
            else { e.stride = obs; e.state = RPT_INIT; }
            break;
    }
    e.last_addr = addr;
    if (dcache != nullptr && dcache->last_was_pf_hit) {
        global_win_hits++;
        unsigned long issuer = dcache->last_pf_issuer;
        if (issuer != 0) {
            UINT64 issuer_idx = lookupSlot(issuer);
            RPTEntry &issuer_e = rpt[issuer_idx];
            if (issuer_e.valid && issuer_e.pc == issuer) {
                issuer_e.win_pf_hits++;
            }
        }
    }

    if (e.state == RPT_STEADY && e.stride != 0) {
        UINT64 deg = getDegree(e);
        for (UINT64 d = 0; d < deg; d++) {
            long ahead = e.stride * (long)(1 + d);
            unsigned long pf =
                ((unsigned long)((long)addr + ahead)) & ~((unsigned long)blockSize - 1);
            if (dcache != nullptr) {
                dcache->prefetchLine(pf, pc);
                prefetches_issued++;
                e.win_pf_issued++;
                global_win_issued++;
            }
        }
    }
}

void fdp_stride_prefetcher::recordPollution(unsigned long issuer_pc) {
    global_win_pollutions++;
    UINT64 idx = lookupSlot(issuer_pc);
    RPTEntry &e = rpt[idx];
    if (e.valid && e.pc == issuer_pc) e.win_pollutions++;
}

void fdp_stride_prefetcher::runFeedbackAdjustment(
    UINT64 acc_high, UINT64 acc_low, UINT64 poll_high)
{
    feedback_adjustments++;

    if (mode == 2) {
        if (global_win_issued > 0) {
            UINT64 acc  = (100 * global_win_hits)        / global_win_issued;
            UINT64 poll = (100 * global_win_pollutions)  / global_win_issued;
            if      (poll >= poll_high) {
                if (global_aggressiveness > 0) global_aggressiveness--;
            }
            else if (acc < acc_low) {
                if (global_aggressiveness > 0) global_aggressiveness--;
            }
            else if (acc >= acc_high) {
                if (global_aggressiveness < 3) global_aggressiveness++;
            }
        }
        global_win_issued = global_win_hits = global_win_pollutions = 0;
    }
    else if (mode == 3) {
        for (UINT64 i = 0; i < nEntries; i++) {
            RPTEntry &e = rpt[i];
            if (!e.valid || e.win_pf_issued == 0) {
                e.win_pf_issued = e.win_pf_hits = e.win_pollutions = 0;
                continue;
            }
            UINT64 acc  = (100 * e.win_pf_hits) / e.win_pf_issued;
            UINT64 poll = (100 * e.win_pollutions) / e.win_pf_issued;
            if      (poll >= poll_high) { if (e.aggressiveness > 0) e.aggressiveness--; }
            else if (acc < acc_low) { if (e.aggressiveness > 0) e.aggressiveness--; }
            else if (acc >= acc_high) { if (e.aggressiveness < 3) e.aggressiveness++; }
            e.win_pf_issued = e.win_pf_hits = e.win_pollutions = 0;
        }
    }
}

// generic cache
cache::cache(int blockSize, int totalCacheSize, int associativity, cache* nextLevel_, bool wb)
  : blockSz(blockSize), totalCacheSz(totalCacheSize), assoc(associativity),
    blockOffsetSize((unsigned)log2(blockSize)),
    setSize((unsigned)log2(totalCacheSize / (blockSize * associativity))),
    tagSize(ADDRESS_SIZE - blockOffsetSize - setSize),
    tagMask((1u << tagSize) - 1u), setMask((1u << setSize) - 1u),
    maxSetValue((int)1 << setSize), nextLevel(nextLevel_), writebackDirty(wb)
{
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

unsigned int cache::getTag(unsigned long a) {
    return (unsigned int)((a >> (blockOffsetSize + setSize)) & tagMask);
}
unsigned int cache::getSet(unsigned long a) {
    return (unsigned int)((a >> blockOffsetSize) & setMask);
}

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

// setup
INT32 Usage() {
    cerr << "FDP stride prefetcher\n";
    cerr << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}

void CreateCaches() {
    ifstream config; config.open(KnobConfigFile.Value().c_str());
    if (!config.is_open()) {
        cerr << "Cannot open config file: " << KnobConfigFile.Value() << "\n";
        Usage(); PIN_ExitProcess(EXIT_FAILURE);
    }
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
                vcache = (eff_v > 0)
                    ? new victim_cache(bsize, bsize * eff_v) : nullptr;
                if (KnobFDPMode.Value() != 0)
                    pfbuf = new prefetch_buffer(bsize, bsize * (int)KnobPFBufEntries.Value());
                dcache = new l1dcache(bsize, csize, assoc, llcache, vcache, pfbuf);
                break;
            }
            default: break;
        }
        i++;
    }
    if (KnobFDPMode.Value() != 0)
        sp = new fdp_stride_prefetcher(KnobRPTSize.Value(), dcache, dcache->getCacheBlockSize(), (int)KnobFDPMode.Value());
    nextWindowAt = KnobFDPWindow.Value();
}

void CheckLimits() {
    if (KnobInstructionCount.Value() > 0 && icount > KnobInstructionCount.Value()) {
        PrintResults(); PIN_ExitProcess(EXIT_SUCCESS);
    }
}

void MemoryOp(ADDRINT pc, ADDRINT address) {
    dcache->addressRequest(address);
    if (sp != nullptr) sp->access((unsigned long)pc, (unsigned long)address);
}

void AllInstructions(ADDRINT ins_ptr) {
    icount++;
    icache->addressRequest(ins_ptr);
    if (sp != nullptr && (UINT64)KnobFDPMode.Value() >= 2 && icount >= nextWindowAt) {
        sp->runFeedbackAdjustment(KnobAccHigh.Value(),
                                  KnobAccLow.Value(),
                                  KnobPollHigh.Value());
        nextWindowAt += KnobFDPWindow.Value();
    }
    CheckLimits();
}

void PrintResults() {
    ofstream out(KnobOutputFile.Value().c_str());
    out.setf(ios::fixed, ios::floatfield); out.precision(2);

    out << "FDP Mode: " << KnobFDPMode.Value() << "\t (0=none, 1=fixed, 2=global, 3=per-PC)\n" << "FDP Window: " << KnobFDPWindow.Value() << " instructions\n" << "FDP acc_high/acc_low/poll_high: " << KnobAccHigh.Value() << "/" << KnobAccLow.Value() << "/" << KnobPollHigh.Value() << " (%)\n" << "Instructions: " << icount << "\n\n";

    out << "Cache stats\n"
        << "I-Cache Miss: " << icache->getTotalMiss()
        << " out of " << icache->getRequest() << "\n"
        << "D-Cache Miss: " << dcache->getTotalMiss()
        << " out of " << dcache->getRequest() << "\n"
        << "L2-Cache Miss: " << llcache->getTotalMiss()
        << " out of " << llcache->getRequest() << "\n"
        << "Mem requests: " << mem->getRequest() << "\n";

    UINT64 vcHits = 0;
    if (vcache != nullptr) {
        vcHits = vcache->getVCHits();
        out << "Victim Cache Hits: " << vcHits
            << " (entries: " << vcache->getCacheAssoc() << ")\n";
    }
    if (pfbuf != nullptr) {
        out << "Prefetch Buffer Hits: " << pfbuf->getPFHits()       << "\n"
            << "Prefetch Buffer Inserts: " << pfbuf->getPFInserts()    << "\n"
            << "Prefetch Buffer Pollutions: " << pfbuf->getPFPollutions() << "\n"
            << "D-Cache Misses Serviced By Prefetch: "
            << dcacheMissesAvoidedByPF << "\n";
    }
    if (sp != nullptr) {
        out << "Stride Prefetches Issued: " << sp->getPrefetchesIssued() << "\n"
            << "Stride Correct Predictions: "  << sp->getCorrectPredictions() << "\n"
            << "FDP Feedback Adjustments: " << sp->getFeedbackAdjustments() << "\n"
            << "Final Global Aggressiveness: " << sp->getGlobalAggressiveness() << "\n";
        //get the prefetch accuracy
        if (sp->getPrefetchesIssued() > 0 && pfbuf != nullptr) {
            double acc = 100.0 * (double)pfbuf->getPFHits() / (double)sp->getPrefetchesIssued();
            out << "Prefetch Accuracy: " << acc << "%\n";
        }
    }

    UINT64 raw = dcache->getTotalMiss();
    UINT64 avoided = vcHits + dcacheMissesAvoidedByPF;
    UINT64 eff = (avoided <= raw) ? (raw - avoided) : 0;
    out << "EFFECTIVE D-Cache Misses: " << eff << "\n";
}

void Instruction(INS ins, VOID *v) {
    (void)v;
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AllInstructions,IARG_INST_PTR, IARG_END);
    if (INS_IsMemoryRead(ins))
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryOp, IARG_INST_PTR, IARG_MEMORYREAD_EA, IARG_END);
    if (INS_IsMemoryWrite(ins))
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryOp, IARG_INST_PTR, IARG_MEMORYWRITE_EA, IARG_END);
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
