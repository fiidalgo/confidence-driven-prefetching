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

// Knobs
KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "outfile", "stride.out", "Cache results file name");
KNOB<string> KnobConfigFile(KNOB_MODE_WRITEONCE, "pintool","config", "config-base", "Configuration file name");
KNOB<UINT64> KnobInstructionCount(KNOB_MODE_WRITEONCE, "pintool", "max_inst", "1000000000", "Number of instructions to profile");
KNOB<BOOL>   KnobPrefetch(KNOB_MODE_WRITEONCE, "pintool", "prefetch", "0", "Enable stride prefetcher");
KNOB<UINT64> KnobRPTSize(KNOB_MODE_WRITEONCE, "pintool", "rpt", "64", "Reference Prediction Table entries");
KNOB<UINT64> KnobPFBufEntries(KNOB_MODE_WRITEONCE, "pintool", "pf_buf_entries", "16", "Prefetch buffer entries");
KNOB<UINT64> KnobPFDegree(KNOB_MODE_WRITEONCE, "pintool", "pf_degree", "1", "Prefetch degree");
KNOB<UINT64> KnobPFDistance(KNOB_MODE_WRITEONCE, "pintool", "pf_distance", "1", "Prefetch distance (strides ahead)");
KNOB<UINT64> KnobVcEntries(KNOB_MODE_WRITEONCE, "pintool","vc_entries", "0", "Victim cache entries (0 = disabled).");

// cache frameowrk
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

//prefetch buffer
class prefetch_buffer : public cache {
public:
    prefetch_buffer(int blockSize, int totalCacheSize) : cache(blockSize, totalCacheSize, totalCacheSize / blockSize, nullptr, false), pfHits(0), pfInserts(0) {}

    bool probe(unsigned long address) {
        unsigned int tag = getTag(address);
        unsigned int set = getSet(address);
        int idx = isHit(tag, set);
        if (idx != -1) { pfHits++; updateLRU(set, idx); return true; }
        return false;
    }

    bool contains(unsigned long address) {
        unsigned int tag = getTag(address);
        unsigned int set = getSet(address);
        return isHit(tag, set) != -1;
    }

    void insert(unsigned long address) {
        unsigned int tag = getTag(address);
        unsigned int set = getSet(address);
        int idx = isHit(tag, set);
        if (idx != -1) { updateLRU(set, idx); return; }
        int lruIndex = getLRU(set);
        cacheMem[lruIndex + set * assoc].Tag   = tag;
        cacheMem[lruIndex + set * assoc].Valid  = true;
        updateLRU(set, lruIndex);
        pfInserts++;
    }

    UINT64 getPFHits() { return pfHits; }
    UINT64 getPFInserts() { return pfInserts; }

private:
    UINT64 pfHits, pfInserts;
};

class l1icache : public cache {
public:
    l1icache(int blockSize, int totalCacheSize, int associativity, cache *nextLevel) : cache(blockSize, totalCacheSize, associativity, nextLevel, false) {}
};

class l2cache : public cache {
public:
    l2cache(int blockSize, int totalCacheSize, int associativity, cache *nextLevel) : cache(blockSize, totalCacheSize, associativity, nextLevel, true) {}
};

class l1dcache : public cache {
public:
    l1dcache(int blockSize, int totalCacheSize, int associativity, cache *nextLevel, victim_cache *vc, prefetch_buffer *pfb) : cache(blockSize, totalCacheSize, associativity, nextLevel, true), vc(vc), pfb(pfb) {}
    void addressRequest(unsigned long address) override;
    void prefetchLine(unsigned long address);

private:
    victim_cache *vc;
    prefetch_buffer *pfb;
};

// stride prefetcher (chen-baer RPT approach)
enum RPTState { RPT_INIT = 0, RPT_TRANSIENT, RPT_STEADY, RPT_NO_PRED };

struct RPTEntry {
    unsigned long pc;
    unsigned long last_addr;
    long stride;
    int state;
    bool valid;
};

class stride_prefetcher {
public:
    stride_prefetcher(UINT64 nEntries, l1dcache *dcache, int blockSize) : nEntries(nEntries), dcache(dcache), blockSize(blockSize),prefetches_issued(0), correct_predictions(0)
    {
        rpt = new RPTEntry[nEntries];
        for (UINT64 i = 0; i < nEntries; i++) {
            rpt[i].valid = false;
            rpt[i].state = RPT_INIT;
        }
    }

    void access(unsigned long pc, unsigned long addr, UINT64 degree, UINT64 distance);
    UINT64 getPrefetchesIssued()   { return prefetches_issued; }
    UINT64 getCorrectPredictions() { return correct_predictions; }

private:
    UINT64 nEntries;
    RPTEntry *rpt;
    l1dcache *dcache;
    int blockSize;
    UINT64 prefetches_issued;
    UINT64 correct_predictions;
    UINT64 lookupOrAllocate(unsigned long pc) {
        return (pc >> 2) & (nEntries - 1);
    }
};

void stride_prefetcher::access(unsigned long pc, unsigned long addr,UINT64 degree, UINT64 distance) {
    UINT64 idx = lookupOrAllocate(pc);
    RPTEntry &e = rpt[idx];

    if (!e.valid || e.pc != pc) {
        e.valid = true; e.pc = pc; e.last_addr = addr;
        e.stride = 0; e.state = RPT_INIT;
        return;
    }

    long observed_stride = (long)addr - (long)e.last_addr;

    switch (e.state) {
        case RPT_INIT:
            e.stride = observed_stride;
            e.state  = RPT_TRANSIENT;
            break;
        case RPT_TRANSIENT:
            if (observed_stride == e.stride) e.state = RPT_STEADY;
            else { e.stride = observed_stride; e.state = RPT_INIT; }
            break;
        case RPT_STEADY:
            if (observed_stride == e.stride) correct_predictions++;
            else e.state = RPT_NO_PRED;
            break;
        case RPT_NO_PRED:
            if (observed_stride == e.stride) e.state = RPT_TRANSIENT;
            else { e.stride = observed_stride; e.state = RPT_INIT; }
            break;
    }
    e.last_addr = addr;

    if (e.state == RPT_STEADY && e.stride != 0) {
        for (UINT64 d = 0; d < degree; d++) {
            long ahead = e.stride * (long)(distance + d);
            unsigned long pf_block = ((unsigned long)((long)addr + ahead)) & ~((unsigned long)blockSize - 1);
            if (dcache != nullptr) {
                dcache->prefetchLine(pf_block);
                prefetches_issued++;
            }
        }
    }
}

// global vars
l1icache *icache = nullptr;
l1dcache *dcache = nullptr;
victim_cache *vcache = nullptr;
prefetch_buffer *pfbuf = nullptr;
l2cache *llcache = nullptr;
memory *mem = nullptr;
stride_prefetcher *sp = nullptr;

UINT64 icount = 0;
UINT64 dcacheMissesAvoidedByPF = 0;

void PrintResults();

// l1d access
void l1dcache::addressRequest(unsigned long address) {
    unsigned long tagField = getTag(address);
    unsigned long setField = getSet(address);

    addRequest();
    int index = isHit(tagField, setField);
    if (index != -1) { addHit(); updateLRU(setField, index); return; }

    // l1d miss
    addTotalMiss();

    // step 1: check vc
    if (vc != nullptr && vc->probe(address)) {
        int lruIndex = getLRU(setField);
        if (cacheMem[lruIndex + setField * assoc].Valid) {
            unsigned long evictTag  = cacheMem[lruIndex + setField * assoc].Tag;
            unsigned long evictAddr = evictTag << (getSetSize() + getBlockOffsetSize());
            evictAddr |= (setField << getBlockOffsetSize());
            vc->insert(evictAddr);
        }
        cacheMem[lruIndex + setField * assoc].Tag   = tagField;
        cacheMem[lruIndex + setField * assoc].Valid  = true;
        updateLRU(setField, lruIndex);
        return;
    }

    // step 2: check pf buf
    if (pfb != nullptr && pfb->probe(address)) {
        dcacheMissesAvoidedByPF++;
        int lruIndex = getLRU(setField);
        if (cacheMem[lruIndex + setField * assoc].Valid) {
            addEntryRemoved();
            unsigned long evictTag  = cacheMem[lruIndex + setField * assoc].Tag;
            unsigned long evictAddr = evictTag << (getSetSize() + getBlockOffsetSize());
            evictAddr |= (setField << getBlockOffsetSize());
            if (vc != nullptr) {
                unsigned long displaced = vc->insert(evictAddr);
                if (displaced != 0) nextLevel->addressRequest(displaced);
            } else {
                nextLevel->addressRequest(evictAddr);
            }
        }
        cacheMem[lruIndex + setField * assoc].Tag   = tagField;
        cacheMem[lruIndex + setField * assoc].Valid  = true;
        updateLRU(setField, lruIndex);
        return;
    }

    // step 3: real miss so go to L2
    int lruIndex = getLRU(setField);
    if (cacheMem[lruIndex + setField * assoc].Valid) {
        addEntryRemoved();
        unsigned long evictTag  = cacheMem[lruIndex + setField * assoc].Tag;
        unsigned long evictAddr = evictTag << (getSetSize() + getBlockOffsetSize());
        evictAddr |= (setField << getBlockOffsetSize());
        if (vc != nullptr) {
            unsigned long displaced = vc->insert(evictAddr);
            if (displaced != 0) nextLevel->addressRequest(displaced);
        } else {
            nextLevel->addressRequest(evictAddr);
        }
    }
    nextLevel->addressRequest(address);
    cacheMem[lruIndex + setField * assoc].Tag   = tagField;
    cacheMem[lruIndex + setField * assoc].Valid  = true;
    updateLRU(setField, lruIndex);
}

void l1dcache::prefetchLine(unsigned long address) {
    unsigned long tagField = getTag(address);
    unsigned long setField = getSet(address);
    if (isHit(tagField, setField) != -1) return;
    if (vc  != nullptr && vc->contains(address)) return;
    if (pfb != nullptr && pfb->contains(address)) return;
    nextLevel->addressRequest(address);
    if (pfb != nullptr) pfb->insert(address);
}

// Default cache implemenation
cache::cache(int blockSize, int totalCacheSize, int associativity, cache* nextLevel_, bool writebackDirty_)
  : blockSz(blockSize), totalCacheSz(totalCacheSize), assoc(associativity),
    blockOffsetSize((unsigned)log2(blockSize)),
    setSize((unsigned)log2(totalCacheSize / (blockSize * associativity))),
    tagSize(ADDRESS_SIZE - blockOffsetSize - setSize),
    tagMask((1u << tagSize) - 1u),
    setMask((1u << setSize) - 1u),
    maxSetValue((int)1 << setSize),
    nextLevel(nextLevel_), writebackDirty(writebackDirty_)
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

unsigned int cache::getTag(unsigned long address) {
    return (unsigned int)((address >> (blockOffsetSize + setSize)) & tagMask);
}
unsigned int cache::getSet(unsigned long address) {
    return (unsigned int)((address >> blockOffsetSize) & setMask);
}

int cache::isHit(unsigned int tagBits, unsigned int setIndex) {
    for (int i = 0; i < assoc; i++)
        if (cacheMem[i + setIndex * assoc].Valid && cacheMem[i + setIndex * assoc].Tag == tagBits) return i;
    return -1;
}

void cache::updateLRU(int setBits, int MRU_index) {
    int upperBounds = assoc - 1;
    for (int i = 0; i < assoc; i++)
        if (cacheMem[i + setBits*assoc].LRU_status >= 0 && cacheMem[i + setBits*assoc].LRU_status < upperBounds)
            cacheMem[i + setBits*assoc].LRU_status++;
    cacheMem[MRU_index + setBits*assoc].LRU_status = 0;
}

int cache::getLRU(int setBits) {
    for (int i = 0; i < assoc; i++)
        if (cacheMem[i + setBits*assoc].LRU_status == (assoc - 1)) return i;
    return -1;
}

int cache::getMRU(int setBits) {
    for (int i = 0; i < assoc; i++)
        if (cacheMem[i + setBits*assoc].LRU_status == 0) return i;
    return -1;
}

void cache::addressRequest(unsigned long address) {
    unsigned long tagField = getTag(address);
    unsigned long setField = getSet(address);
    int index = isHit(tagField, setField);
    addRequest();
    if (index == -1) {
        int indexLRU = getLRU(setField);
        if (cacheMem[indexLRU + setField*assoc].Valid) addEntryRemoved();
        addTotalMiss();
        assert(nextLevel != nullptr);
        if (writebackDirty && cacheMem[indexLRU + setField*assoc].Valid) {
            unsigned long et = cacheMem[indexLRU + setField*assoc].Tag;
            unsigned long evictAddr = (et << (getSetSize() + getBlockOffsetSize()))  | ((unsigned long)setField << getBlockOffsetSize());
            nextLevel->addressRequest(evictAddr);
        }
        nextLevel->addressRequest(address);
        cacheMem[indexLRU + setField*assoc].Tag = tagField;
        cacheMem[indexLRU + setField*assoc].Valid = true;
        updateLRU(setField, indexLRU);
    } else {
        addHit();
        updateLRU(setField, index);
    }
}

// steup and instrumentation

INT32 Usage() {
    cerr << "Stride prefetcher cache simulator (CS1411 final project)\n";
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
        int bsize, csize, assoc, vsize = 0; char comma = ',';
        switch (i) {
            case 0:
                parser >> bsize >> comma >> csize >> comma >> assoc;
                llcache = new l2cache(bsize, csize, assoc, mem);
                break;
            case 1:
                parser >> bsize >> comma >> csize >> comma >> assoc;
                icache = new l1icache(bsize, csize, assoc, llcache);
                break;
            case 2: {
                parser >> bsize >> comma >> csize >> comma >> assoc >> comma >> vsize;
                int eff_vsize = (int)KnobVcEntries.Value();
                if (eff_vsize == 0) eff_vsize = vsize;
                vcache = (eff_vsize > 0) ? new victim_cache(bsize, bsize * eff_vsize) : nullptr;
                if (KnobPrefetch.Value())
                    pfbuf = new prefetch_buffer(bsize, bsize * (int)KnobPFBufEntries.Value());
                dcache = new l1dcache(bsize, csize, assoc, llcache, vcache, pfbuf);
                break;
            }
            default: break;
        }
        i++;
    }
    if (KnobPrefetch.Value())
        sp = new stride_prefetcher(KnobRPTSize.Value(), dcache, dcache->getCacheBlockSize());
}

void CheckInstructionLimits() {
    if (KnobInstructionCount.Value() > 0 && icount > KnobInstructionCount.Value()) {
        PrintResults(); PIN_ExitProcess(EXIT_SUCCESS);
    }
}

void MemoryOp(ADDRINT pc, ADDRINT address) {
    dcache->addressRequest(address);
    if (sp != nullptr)
        sp->access((unsigned long)pc, (unsigned long)address, KnobPFDegree.Value(), KnobPFDistance.Value());
}

void AllInstructions(ADDRINT ins_ptr) {
    icount++;
    icache->addressRequest(ins_ptr);
    CheckInstructionLimits();
}

void PrintResults() {
    ofstream out(KnobOutputFile.Value().c_str());
    out.setf(ios::fixed, ios::floatfield); out.precision(2);

    out << "\t\tSimulation Results\n"
        << "Memory system->\n"
        << "\t\tDcache size (bytes)         : " << dcache->getCacheSize()      << "\n"
        << "\t\tDcache ways                 : " << dcache->getCacheAssoc()     << "\n"
        << "\t\tDcache block size (bytes)   : " << dcache->getCacheBlockSize() << "\n"
        << "\t\tIcache size (bytes)         : " << icache->getCacheSize()      << "\n"
        << "\t\tIcache ways                 : " << icache->getCacheAssoc()     << "\n"
        << "\t\tIcache block size (bytes)   : " << icache->getCacheBlockSize() << "\n"
        << "\t\tL2-cache size (bytes)       : " << llcache->getCacheSize()     << "\n"
        << "\t\tL2-cache ways               : " << llcache->getCacheAssoc()    << "\n"
        << "\t\tL2-cache block size (bytes) : " << llcache->getCacheBlockSize()<< "\n";

    out << "Simulated events->\n" << "\t\t I-Cache Miss: " << icache->getTotalMiss() << " out of " << icache->getRequest()  << "\n" << "\t\t D-Cache Miss: "  << dcache->getTotalMiss() << " out of " << dcache->getRequest()  << "\n" << "\t\t L2-Cache Miss: " << llcache->getTotalMiss() << " out of " << llcache->getRequest() << "\n\n" << "\t\t Requests resulted in " << icache->getRequest() + dcache->getRequest() << " L1 requests, " << llcache->getRequest() << " L2 requests, " << mem->getRequest() << " mem requests\n";

    UINT64 vcHits = 0;
    if (vcache != nullptr) {
        vcHits = vcache->getVCHits();
        out << "\t\t Victim Cache Hits: " << vcHits << " entries: " << vcache->getCacheAssoc() << "\n";
    }
    if (pfbuf != nullptr) {
        out << "\t\t Prefetch Buffer Hits: " << pfbuf->getPFHits() << "\n" << "\t\t Prefetch Buffer Inserts: " << pfbuf->getPFInserts() << "\n" << "\t\t D-Cache Misses Serviced By Prefetch: " << dcacheMissesAvoidedByPF << "\n";
    }
    if (sp != nullptr) {
        out << "\t\t Stride Prefetches Issued: " << sp->getPrefetchesIssued() << "\n" << "\t\t Stride Correct Predictions: "  << sp->getCorrectPredictions() << "\n";
    }

    // effective miss = raw misses - VC hits - prefetch misses
    UINT64 rawMisses = dcache->getTotalMiss();
    UINT64 avoided   = vcHits + dcacheMissesAvoidedByPF;
    UINT64 effMisses = (avoided <= rawMisses) ? (rawMisses - avoided) : 0;
    out << "\t\t EFFECTIVE D-Cache Misses (after VC + Prefetch): " << effMisses << "\n";
}

void Instruction(INS ins, VOID *v) {
    (void)v;
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AllInstructions,IARG_INST_PTR, IARG_END);
    if (INS_IsMemoryRead(ins))
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryOp, IARG_INST_PTR, IARG_MEMORYREAD_EA, IARG_END);
    if (INS_IsMemoryWrite(ins))
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)MemoryOp,IARG_INST_PTR, IARG_MEMORYWRITE_EA, IARG_END);
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
