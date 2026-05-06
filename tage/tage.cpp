/*
 * ive made two operating modes for tage
 *
 * single-config mode (default):
 *      $PIN_ROOT/pin -t tage.so -m 4 -o out.txt -- benchmark
 *
 * multi-config mode (-multi 1):
 *      run all configs at the same time in one benchmark pass
 *      wil write one .out file per config into -outdir.
 *      $PIN_ROOT/pin -t tage.so -multi 1 -outdir results/libquantum \
 *          -l 100000000 -- benchmark
 *
 * modes (-m flag):
 *      0=1bit  1=2bit  2=2level  3=gshare  4=TAGE
 */

#include <unistd.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstdlib>
#include <cstdint>
#include <sys/stat.h>
#include "pin.H"

using std::ofstream;
using std::cerr;
using std::endl;
using std::string;
using std::vector;
using std::ostringstream;

// Knobs
KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "o", "tage.out", "output file (single-config mode)");
KNOB<string> KnobOutDir(KNOB_MODE_WRITEONCE, "pintool", "outdir", "results", "output directory (multi-config mode)");
KNOB<BOOL>   KnobMulti(KNOB_MODE_WRITEONCE, "pintool", "multi", "0", "1 = run all configs in one pass");
KNOB<UINT64> KnobBranchLimit(KNOB_MODE_WRITEONCE, "pintool", "l", "0", "branch limit (0 = run full benchmark)");
KNOB<UINT64> KnobMode(KNOB_MODE_WRITEONCE, "pintool", "m", "4", "predictor mode (single-config): 0=1bit 1=2bit 2=2level 3=gshare 4=TAGE");

// single-config knobs
KNOB<UINT64> KnobBPBSize(KNOB_MODE_WRITEONCE, "pintool", "bpb", "1024", "BPB entries");
KNOB<UINT64> KnobHHRTSize(KNOB_MODE_WRITEONCE, "pintool", "hhrt", "1024", "HHRT entries");
KNOB<UINT64> KnobPTSize(KNOB_MODE_WRITEONCE, "pintool", "pt", "1024", "PT entries");
KNOB<UINT64> KnobGshareSize(KNOB_MODE_WRITEONCE, "pintool", "gshare_size", "16384", "gshare entries");
KNOB<UINT64> KnobGshareH(KNOB_MODE_WRITEONCE, "pintool", "gshare_hlen", "14", "gshare history bits");
KNOB<UINT64> KnobTageBase(KNOB_MODE_WRITEONCE, "pintool", "tage_base", "16384", "TAGE base entries");
KNOB<UINT64> KnobTageTS(KNOB_MODE_WRITEONCE, "pintool", "tage_tag_size", "2048", "TAGE tagged-table entries");
KNOB<UINT64> KnobTageBits(KNOB_MODE_WRITEONCE, "pintool", "tage_tag_bits", "11", "TAGE tag width");
KNOB<UINT64> KnobTageN(KNOB_MODE_WRITEONCE, "pintool", "tage_n", "4", "TAGE tagged tables");

// Global history shared by all predictors
static const int GHIST_LEN = 256;
static bool ghist[GHIST_LEN];
static UINT64 path_hist = 0;

static void update_ghist(bool taken, ADDRINT pc) {
    for (int i = GHIST_LEN-1; i > 0; i--) ghist[i] = ghist[i-1];
    ghist[0] = taken;
    path_hist = ((path_hist << 1) | ((UINT64)pc & 1ULL)) & 0xFFFFULL;
}

static UINT64 hist_bits(int len) {
    UINT64 h = 0;
    if (len > GHIST_LEN) len = GHIST_LEN;
    for (int i = 0; i < len; i++)
        if (ghist[i]) h |= (1ULL << i);
    return h;
}
// Predictor base class
struct Predictor {
    string label;
    UINT64 seen, taken, correct;
    Predictor(const string& l) : label(l), seen(0), taken(0), correct(0) {}
    virtual ~Predictor() {}
    virtual bool predict(ADDRINT pc) = 0;
    virtual void update(ADDRINT pc, bool t) = 0;
    void observe(ADDRINT pc, bool t) {
        seen++; if (t) taken++;
        if (predict(pc) == t) correct++;
        update(pc, t);
    }
    void write(const string& dir) {
        string path = dir + "/" + label + ".out";
        ofstream f(path.c_str());
        f << "Label: " << label << "\n"
          << "Count Seen: " << seen << "\n"
          << "Count Taken: " << taken << "\n"
          << "Count Correct: " << correct << "\n";
        if (seen > 0)
            f << "Accuracy: " << 100.0 * (double)correct / (double)seen << "%\n";
        f.close();
    }
};

// 1 bit bpb
struct BPB1 : Predictor {
    UINT64 mask; bool* tbl;
    BPB1(UINT64 sz) : Predictor(""), mask(sz-1) {
        tbl = new bool[sz]();
        ostringstream s; s << "1bit_bpb" << sz; label = s.str();
    }
    bool predict(ADDRINT pc) override { return tbl[mask & (UINT64)pc]; }
    void update(ADDRINT pc, bool t) override { tbl[mask & (UINT64)pc] = t; }
};

// 2 bit bpb (A2 saturating counter)
struct BPB2 : Predictor {
    UINT64 mask; UINT8* tbl;
    BPB2(UINT64 sz) : Predictor(""), mask(sz-1) {
        tbl = new UINT8[sz];
        for (UINT64 i = 0; i < sz; i++) tbl[i] = 2;
        ostringstream s; s << "2bit_bpb" << sz; label = s.str();
    }
    bool predict(ADDRINT pc) override { return tbl[mask & (UINT64)pc] >= 2; }
    void update(ADDRINT pc, bool t) override {
        UINT64 i = mask & (UINT64)pc;
        if (t) { if (tbl[i] < 3) tbl[i]++; } else { if (tbl[i] > 0) tbl[i]--; }
    }
};

// 2-level adaptive (HHRT + global PT, A2)
struct TwoLevel : Predictor {
    UINT64 hm, pm; UINT64* hhrt; UINT8* pt;
    TwoLevel(UINT64 hs, UINT64 ps) : Predictor(""), hm(hs-1), pm(ps-1) {
        hhrt = new UINT64[hs]();
        pt   = new UINT8[ps];
        for (UINT64 i = 0; i < ps; i++) pt[i] = 2;
        ostringstream s; s << "2lvl_" << hs; label = s.str();
    }
    bool predict(ADDRINT pc) override {
        return pt[pm & hhrt[hm & (UINT64)pc]] >= 2;
    }
    void update(ADDRINT pc, bool t) override {
        UINT64 hi = hm & (UINT64)pc;
        UINT64 pi = pm & hhrt[hi];
        if (t) { if (pt[pi] < 3) pt[pi]++; } else { if (pt[pi] > 0) pt[pi]--; }
        hhrt[hi] = ((hhrt[hi] << 1) | (t ? 1ULL : 0ULL)) & pm;
    }
};

//gshare
struct Gshare : Predictor {
    UINT64 mask; int hlen; UINT8* tbl;
    Gshare(UINT64 sz, int h) : Predictor(""), mask(sz-1), hlen(h) {
        tbl = new UINT8[sz];
        for (UINT64 i = 0; i < sz; i++) tbl[i] = 2;
        ostringstream s; s << "gshare_" << sz << "_h" << h; label = s.str();
    }
    UINT64 idx(ADDRINT pc) { return ((UINT64)pc ^ hist_bits(hlen)) & mask; }
    bool predict(ADDRINT pc) override { return tbl[idx(pc)] >= 2; }
    void update(ADDRINT pc, bool t) override {
        UINT64 i = idx(pc);
        if (t) { if (tbl[i] < 3) tbl[i]++; } else { if (tbl[i] > 0) tbl[i]--; }
    }
};

// TAGE
#define TAGE_MAX_TABLES 8
static const int TAGE_HL[TAGE_MAX_TABLES] = {5, 13, 32, 80, 200, 200, 200, 200};

struct TageEntry { int8_t ctr; uint16_t tag; uint8_t u; };

struct Tage : Predictor {
    int N; UINT64 bsz, tsz; int tbits;
    UINT64 tmask, bmask;
    UINT8*      base;
    TageEntry*  tabs[TAGE_MAX_TABLES];
    int lp, lap; bool lpred, lapred;
    UINT64 tick;

    Tage(int n, UINT64 bs, UINT64 ts, int tb)
      : Predictor(""), N(n), bsz(bs), tsz(ts), tbits(tb),
        tmask((1ULL<<tb)-1ULL), bmask(bs-1ULL),
        lp(-1), lap(-1), lpred(false), lapred(false), tick(0)
    {
        ostringstream s; s << "tage_n" << n << "_ts" << ts; label = s.str();
        base = new UINT8[bs];
        for (UINT64 i = 0; i < bs; i++) base[i] = 2;
        for (int t = 0; t < N; t++) {
            tabs[t] = new TageEntry[ts];
            for (UINT64 i = 0; i < ts; i++) { tabs[t][i] = {0, 0, 0}; }
        }
        for (int t = N; t < TAGE_MAX_TABLES; t++) tabs[t] = nullptr;
    }

    int idx_width() {
        int b = 0; UINT64 m = tsz - 1; while (m) { b++; m >>= 1; } return b;
    }

    UINT64 fold(int len, int w) {
        if (len > GHIST_LEN) len = GHIST_LEN;
        UINT64 a = 0; int p = 0;
        for (int i = 0; i < len; i++) {
            if (ghist[i]) a ^= (1ULL << p);
            if (++p >= w) p = 0;
        }
        return a;
    }

    UINT64 tidx(int t, ADDRINT pc) {
        int w = idx_width();
        return ((UINT64)pc ^ fold(TAGE_HL[t], w) ^ ((UINT64)pc >> w) ^ path_hist) & (tsz-1);
    }

    UINT64 ttag(int t, ADDRINT pc) {
        return ((UINT64)pc ^ fold(TAGE_HL[t], tbits)
                            ^ (fold(TAGE_HL[t], tbits-1) << 1)) & tmask;
    }

    bool predict(ADDRINT pc) override {
        lp = -1; lap = -1;
        for (int t = N-1; t >= 0; t--) {
            if ((UINT64)tabs[t][tidx(t,pc)].tag == ttag(t,pc)) {
                if (lp == -1) lp = t;
                else { lap = t; break; }
            }
        }
        lapred = (lap >= 0) ? tabs[lap][tidx(lap,pc)].ctr >= 0 : base[bmask & (UINT64)pc] >= 2;
        lpred  = (lp  >= 0) ? tabs[lp][tidx(lp,pc)].ctr >= 0 : base[bmask & (UINT64)pc] >= 2;
        return lpred;
    }

    void update(ADDRINT pc, bool t) override {
        bool ok = (lpred == t);
        if (lp == -1) {
            UINT64 bi = bmask & (UINT64)pc;
            if (t) { if (base[bi]<3) base[bi]++; } else { if (base[bi]>0) base[bi]--; }
        } else {
            TageEntry& e = tabs[lp][tidx(lp,pc)];
            if (t) { if (e.ctr < 3) e.ctr++; } else { if (e.ctr > -4) e.ctr--; }
        }
        if (lp >= 0 && lpred != lapred) {
            TageEntry& e = tabs[lp][tidx(lp,pc)];
            if (ok) { if (e.u < 3) e.u++; } else { if (e.u > 0) e.u--; }
        }
        if (!ok && lp < N-1) {
            int at = -1;
            for (int t2 = lp+1; t2 < N; t2++)
                if (tabs[t2][tidx(t2,pc)].u == 0) { at = t2; break; }
            if (at == -1) {
                for (int t2 = lp+1; t2 < N; t2++)
                    if (tabs[t2][tidx(t2,pc)].u > 0) tabs[t2][tidx(t2,pc)].u--;
            } else {
                TageEntry& e = tabs[at][tidx(at,pc)];
                e.tag = (uint16_t)ttag(at,pc);
                e.ctr = t ? 0 : -1;
                e.u   = 0;
            }
        }
        if (((++tick) & ((1ULL<<18)-1)) == 0)
            for (int t2 = 0; t2 < N; t2++)
                for (UINT64 i = 0; i < tsz; i++) tabs[t2][i].u >>= 1;
    }
};

// global state
UINT64 CountSeen = 0, CountTaken = 0, CountCorrect = 0;
vector<Predictor*> all_preds;
bool multi_mode = false;
string g_outdir;
Predictor* single_pred = nullptr;

void build_all_predictors() {
    for (UINT64 sz : {256ULL,1024ULL,4096ULL,16384ULL})
        all_preds.push_back(new BPB1(sz));
    for (UINT64 sz : {256ULL,1024ULL,4096ULL,16384ULL})
        all_preds.push_back(new BPB2(sz));
    for (UINT64 sz : {1024ULL,4096ULL,16384ULL})
        all_preds.push_back(new TwoLevel(sz, sz));
    for (UINT64 sz : {4096ULL,16384ULL,65536ULL})
        for (int h : {12,14,16})
            all_preds.push_back(new Gshare(sz, h));
    for (int n : {2,4,6})
        for (UINT64 ts : {512ULL,2048ULL,8192ULL})
            all_preds.push_back(new Tage(n, 16384, ts, 11));
}

void write_all(bool limit_reached) {
    (void)limit_reached;
    if (multi_mode) {
        mkdir(g_outdir.c_str(), 0755);
        for (Predictor* p : all_preds) p->write(g_outdir);
    } else {
        ofstream f(KnobOutputFile.Value().c_str());
        f << "Count Seen: " << CountSeen << "\n"
          << "Count Taken: " << CountTaken << "\n"
          << "Count Correct: " << CountCorrect << "\n";
        if (CountSeen > 0)
            f << "Accuracy: " << 100.0*(double)CountCorrect/(double)CountSeen << "%\n";
    }
}

// pin analysis
VOID br_predict(ADDRINT pc, INT32 taken) {
    bool t = (taken != 0);
    CountSeen++; if (t) CountTaken++;

    if (multi_mode) {
        for (Predictor* p : all_preds) p->observe(pc, t);
    } else {
        if (single_pred->predict(pc) == t) CountCorrect++;
        single_pred->update(pc, t);
    }
    update_ghist(t, pc);

    if (KnobBranchLimit.Value() > 0 && CountSeen >= KnobBranchLimit.Value()) {
        write_all(true);
        PIN_ExitApplication(0);
    }
}

VOID Instruction(INS ins, void* v) {
    (void)v;
    if (INS_IsRet(ins) || INS_IsSyscall(ins) || INS_IsBranch(ins))
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)br_predict,
                       IARG_INST_PTR, IARG_BRANCH_TAKEN, IARG_END);
}

VOID Fini(int n, void* v) { (void)n; (void)v; write_all(false); }

INT32 Usage() {
    cerr << "TAGE branch predictor \n";
    cerr << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}

int main(int argc, char* argv[]) {
    if (PIN_Init(argc, argv)) return Usage();

    for (int i = 0; i < GHIST_LEN; i++) ghist[i] = false;

    multi_mode = (bool)KnobMulti.Value();
    g_outdir   = KnobOutDir.Value();

    if (multi_mode) {
        build_all_predictors();
    } else {
        switch (KnobMode.Value()) {
            case 0: single_pred = new BPB1(KnobBPBSize.Value()); break;
            case 1: single_pred = new BPB2(KnobBPBSize.Value()); break;
            case 2: single_pred = new TwoLevel(KnobHHRTSize.Value(), KnobPTSize.Value()); break;
            case 3: single_pred = new Gshare(KnobGshareSize.Value(), (int)KnobGshareH.Value()); break;
            case 4: default:
                single_pred = new Tage((int)KnobTageN.Value(), KnobTageBase.Value(), KnobTageTS.Value(), (int)KnobTageBits.Value());
        }
    }

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_StartProgram();
    return 0;
}
