// my_predictor.h
// Branch predictors for the CBP-2 infrastructure.
//

#include <string.h>

// ---------------------------------------------------------------------------
// Sample gshare predictor (unchanged from the infrastructure)
// ---------------------------------------------------------------------------

class gshare_update : public branch_update {
public:
	unsigned int index;
};

class gshare_predictor : public branch_predictor {
public:
#define HISTORY_LENGTH	15
#define TABLE_BITS	15
	gshare_update u;
	branch_info bi;
	unsigned int history;
	unsigned char tab[1<<TABLE_BITS];

	gshare_predictor (void) : history(0) {
		memset (tab, 0, sizeof (tab));
	}

	branch_update *predict (branch_info & b) {
		bi = b;
		if (b.br_flags & BR_CONDITIONAL) {
			u.index =
				  (history << (TABLE_BITS - HISTORY_LENGTH))
				^ (b.address & ((1<<TABLE_BITS)-1));
			u.direction_prediction (tab[u.index] >> 1);
		} else {
			u.direction_prediction (true);
		}
		u.target_prediction (0);
		return &u;
	}

	void update (branch_update *u, bool taken, unsigned int target) {
		if (bi.br_flags & BR_CONDITIONAL) {
			unsigned char *c = &tab[((gshare_update*)u)->index];
			if (taken) {
				if (*c < 3) (*c)++;
			} else {
				if (*c > 0) (*c)--;
			}
			history <<= 1;
			history |= taken;
			history &= (1<<HISTORY_LENGTH)-1;
		}
	}
};

// ---------------------------------------------------------------------------
// Shared building blocks
// ---------------------------------------------------------------------------

// 2-bit saturating counter helpers. 0,1 = predict not taken; 2,3 = taken.
static inline void ctr_update (unsigned char &c, bool taken) {
	if (taken) { if (c < 3) c++; }
	else       { if (c > 0) c--; }
}
static inline bool ctr_taken (unsigned char c) { return c >= 2; }

// Bimodal table: 4096 2-bit counters indexed by IP[11:0].
#define BIM_BITS 12
struct bimodal_table {
	unsigned char ctr[1 << BIM_BITS];
	bimodal_table (void) { memset (ctr, 1, sizeof (ctr)); } // weakly not taken
	static unsigned int index (unsigned int ip) { return ip & ((1 << BIM_BITS) - 1); }
	bool predict (unsigned int ip) { return ctr_taken (ctr[index (ip)]); }
	void update (unsigned int ip, bool taken) { ctr_update (ctr[index (ip)], taken); }
};

// Tagged, set-associative global predictor with true-LRU replacement.
// 512 sets; HASH is 15 bits: index = HASH[14:6] (9 bits), tag = HASH[5:0].
#define GP_SETS_BITS 9
#define GP_TAG_BITS  6
#define GP_MAX_WAYS  8
struct global_table {
	struct entry {
		bool valid;
		unsigned char tag;   // 6-bit tag
		unsigned char ctr;   // 2-bit counter
		unsigned char lru;   // 0 = most recently used
	};
	int ways;
	entry set[1 << GP_SETS_BITS][GP_MAX_WAYS];

	global_table (int w) : ways (w) {
		for (int s = 0; s < (1 << GP_SETS_BITS); s++)
			for (int i = 0; i < GP_MAX_WAYS; i++) {
				set[s][i].valid = false;
				set[s][i].tag = 0;
				set[s][i].ctr = 1;
				set[s][i].lru = i;
			}
	}

	static unsigned int index (unsigned int hash) {
		return (hash >> GP_TAG_BITS) & ((1 << GP_SETS_BITS) - 1);
	}
	static unsigned int tag (unsigned int hash) {
		return hash & ((1 << GP_TAG_BITS) - 1);
	}

	// returns the hitting way, or -1 on a miss
	int lookup (unsigned int hash) {
		entry *e = set[index (hash)];
		unsigned int t = tag (hash);
		for (int i = 0; i < ways; i++)
			if (e[i].valid && e[i].tag == t) return i;
		return -1;
	}

	void touch (unsigned int s, int way) {
		entry *e = set[s];
		unsigned char old = e[way].lru;
		for (int i = 0; i < ways; i++)
			if (e[i].lru < old) e[i].lru++;
		e[way].lru = 0;
	}

	// update on a hit
	void update_hit (unsigned int hash, int way, bool taken) {
		unsigned int s = index (hash);
		ctr_update (set[s][way].ctr, taken);
		touch (s, way);
	}

	// allocate a new entry in the LRU way, counter initialised weakly
	// toward the actual outcome
	void allocate (unsigned int hash, bool taken) {
		unsigned int s = index (hash);
		entry *e = set[s];
		int victim = 0;
		for (int i = 0; i < ways; i++) {
			if (!e[i].valid) { victim = i; break; }
			if (e[i].lru > e[victim].lru) victim = i;
		}
		e[victim].valid = true;
		e[victim].tag = tag (hash);
		e[victim].ctr = taken ? 2 : 1;
		touch (s, victim);
	}
};

// ---------------------------------------------------------------------------
// REQUIRED: Pentium M hybrid outcome predictor (bimodal + global)
// ---------------------------------------------------------------------------
//
// Simplifications allowed by the handout:
//   * no PIR; the global hash is IP[14:0] XOR a global history register (GHR)
//     of the last 15 conditional-branch outcomes;
//   * no loop predictor and no target prediction.
//
// Global predictor allocation policy: a new entry is allocated only when the
// branch misses in the global predictor AND the bimodal table mispredicted it.
// This keeps branches that the bimodal table already handles well out of the
// global table (as in the real Pentium M). Set -DPM_ALLOC_ALWAYS=1 to allocate
// on every miss instead (the behaviour used in the worked example in the
// handout); the report compares both.

#ifndef PM_GLOBAL_WAYS
#define PM_GLOBAL_WAYS 4        // the handout allows 2; 4 matches Pentium M
#endif
#ifndef PM_HIST_LEN
#define PM_HIST_LEN 15
#endif
#ifndef PM_ALLOC_ALWAYS
#define PM_ALLOC_ALWAYS 0
#endif

class pm_update : public branch_update {
public:
	unsigned int index;     // bimodal index
	unsigned int hash;      // global predictor hash
	int gway;               // hitting way in the global predictor, -1 = miss
	bool bim_pred;          // bimodal prediction
};

class pm_predictor : public branch_predictor {
public:
	pm_update u;
	branch_info bi;
	unsigned int ghr;
	bimodal_table bim;
	global_table gp;

	// statistics (printed to stderr on destruction)
	long long n_cond, n_global_hit;

	pm_predictor (void) : ghr (0), gp (PM_GLOBAL_WAYS), n_cond (0), n_global_hit (0) {}

	~pm_predictor (void) {
#ifdef PM_STATS
		if (n_cond)
			fprintf (stderr, "pm: global hit rate %.2f%%\n", 100.0 * n_global_hit / n_cond);
#endif
	}

	// fold the global history down to 15 bits (only matters if PM_HIST_LEN > 15)
	unsigned int folded_history (void) {
		unsigned int h = 0, g = ghr;
		for (int i = 0; i < PM_HIST_LEN; i += 15) { h ^= g & 0x7fff; g >>= 15; }
		return h;
	}

	branch_update *predict (branch_info & b) {
		bi = b;
		if (b.br_flags & BR_CONDITIONAL) {
			// bimodal: index = IP[11:0]
			u.index = bimodal_table::index (b.address);
			u.bim_pred = bim.predict (b.address);

			// global: HASH = IP[14:0] ^ GHR
			u.hash = (b.address ^ folded_history ()) & 0x7fff;
			u.gway = gp.lookup (u.hash);

			// final prediction: global on a hit, otherwise bimodal
			if (u.gway >= 0)
				u.direction_prediction (ctr_taken (gp.set[global_table::index (u.hash)][u.gway].ctr));
			else
				u.direction_prediction (u.bim_pred);
		} else {
			// unconditional branches are always taken
			u.direction_prediction (true);
		}
		u.target_prediction (0);
		return &u;
	}

	void update (branch_update *bu, bool taken, unsigned int target) {
		if (!(bi.br_flags & BR_CONDITIONAL)) return;
		pm_update *pu = (pm_update *) bu;
		n_cond++;

		// global predictor
		if (pu->gway >= 0) {
			n_global_hit++;
			gp.update_hit (pu->hash, pu->gway, taken);
		} else if (PM_ALLOC_ALWAYS || pu->bim_pred != taken) {
			gp.allocate (pu->hash, taken);
		}

		// bimodal is always updated
		bim.update (bi.address, taken);

		// shift the outcome into the global history
		ghr = (ghr << 1) | (taken ? 1 : 0);
		if (PM_HIST_LEN < 32) ghr &= (1u << PM_HIST_LEN) - 1;
	}
};

// ---------------------------------------------------------------------------
// EXTRA CREDIT: complete Pentium M branch prediction unit
// ---------------------------------------------------------------------------
//
// Structures (from the handout diagram):
//   BTB    : 512 sets x 4 ways, index IP[12:4], tag IP[21:13] + offset IP[3:0],
//            stores branch type, 32-bit target, LRU replacement.
//   LPB    : loop predictor, 64 sets x 2 ways, index IP[9:4], tag IP[15:10],
//            6-bit count, 6-bit limit, 1-bit prediction (+ confidence).
//   Bimodal: 4096 2-bit counters, IP[11:0].
//   Global : 512 sets x 4 ways, HASH = IP[14:0] ^ PIR; index HASH[14:6],
//            tag HASH[5:0].
//   iBTB   : 256 entries, index HASH[13:6], tag {HASH[14], HASH[5:0]}.
//   PIR    : 15-bit path information register (see update_pir()).
//   RAS    : 16-entry return address stack (Pentium M has one; it is not in
//            the diagram).
//
// Outcome selection follows the diagram: the loop predictor overrides the
// bimodal table when the LPB and the BTB both hit; the global predictor
// overrides both when it hits.
//
// Target selection follows Appendix A of the handout:
//   predicted taken + BTB hit  -> BTB target (iBTB target for an indirect
//                                 branch that hits in the iBTB, RAS for returns)
//   predicted not taken        -> fall-through (IP + instruction length). The
//                                 fetch unit knows instruction lengths from
//                                 predecode bits; we model that with a 4K x
//                                 1-bit table indexed by IP[11:0] that learns
//                                 whether a Jcc is 2 or 6 bytes long.
//   predicted taken + BTB miss -> no front-end target is available; the
//                                 decoder would resteer later. We predict the
//                                 fall-through and count it as a target miss.

#ifndef CPM_GLOBAL_WAYS
#define CPM_GLOBAL_WAYS 4
#endif
#ifndef CPM_USE_LOOP
#define CPM_USE_LOOP 1
#endif
#ifndef CPM_USE_PIR
#define CPM_USE_PIR 1          // 0 = use the conditional-outcome GHR instead
#endif
#ifndef CPM_LOOP_CONF
#define CPM_LOOP_CONF 2        // # of repeated identical trip counts before use
#endif
#ifndef CPM_LOOP_OVER_GLOBAL
#define CPM_LOOP_OVER_GLOBAL 0 // 1 = confident loop prediction beats global hit
#endif
#ifndef CPM_PIR_COND_ONLY
#define CPM_PIR_COND_ONLY 0    // 1 = only taken conditional branches update PIR
#endif

#define BTB_SETS_BITS 9
#define BTB_WAYS      4
#define LPB_SETS_BITS 6
#define LPB_WAYS      2
#define IBTB_BITS     8
#define RAS_DEPTH     16

enum { T_COND = 0, T_UNCOND, T_INDIRECT, T_CALL, T_ICALL, T_RET };

struct btb_table {
	struct entry {
		bool valid;
		unsigned int tag;      // IP[21:13]
		unsigned char offset;  // IP[3:0]
		unsigned char type;
		unsigned int target;
		unsigned char lru;
	};
	entry set[1 << BTB_SETS_BITS][BTB_WAYS];

	btb_table (void) {
		for (int s = 0; s < (1 << BTB_SETS_BITS); s++)
			for (int i = 0; i < BTB_WAYS; i++) {
				memset (&set[s][i], 0, sizeof (entry));
				set[s][i].lru = i;
			}
	}
	static unsigned int index (unsigned int ip) { return (ip >> 4) & ((1 << BTB_SETS_BITS) - 1); }
	static unsigned int tag (unsigned int ip) { return (ip >> 13) & 0x1ff; }

	entry *lookup (unsigned int ip) {
		entry *e = set[index (ip)];
		for (int i = 0; i < BTB_WAYS; i++)
			if (e[i].valid && e[i].tag == tag (ip) && e[i].offset == (ip & 15))
				return &e[i];
		return 0;
	}
	void touch (entry *hit, unsigned int ip) {
		entry *e = set[index (ip)];
		unsigned char old = hit->lru;
		for (int i = 0; i < BTB_WAYS; i++) if (e[i].lru < old) e[i].lru++;
		hit->lru = 0;
	}
	entry *allocate (unsigned int ip) {
		entry *e = set[index (ip)];
		entry *v = &e[0];
		for (int i = 0; i < BTB_WAYS; i++) {
			if (!e[i].valid) { v = &e[i]; break; }
			if (e[i].lru > v->lru) v = &e[i];
		}
		v->valid = true;
		v->tag = tag (ip);
		v->offset = ip & 15;
		touch (v, ip);
		return v;
	}
};

// Loop predictor. 'dir' is the direction of the loop body (the outcome seen
// on every iteration but the last). 'limit' is the learned trip count.
struct loop_table {
	struct entry {
		bool valid;
		unsigned char tag;     // IP[15:10]
		unsigned char count;   // 6 bits
		unsigned char limit;   // 6 bits
		bool dir;              // 1-bit prediction (body direction)
		unsigned char conf;    // confidence (not in the diagram; see report)
		unsigned char lru;
	};
	entry set[1 << LPB_SETS_BITS][LPB_WAYS];

	loop_table (void) {
		for (int s = 0; s < (1 << LPB_SETS_BITS); s++)
			for (int i = 0; i < LPB_WAYS; i++) {
				memset (&set[s][i], 0, sizeof (entry));
				set[s][i].lru = i;
			}
	}
	static unsigned int index (unsigned int ip) { return (ip >> 4) & ((1 << LPB_SETS_BITS) - 1); }
	static unsigned int tag (unsigned int ip) { return (ip >> 10) & 0x3f; }

	entry *lookup (unsigned int ip) {
		entry *e = set[index (ip)];
		for (int i = 0; i < LPB_WAYS; i++)
			if (e[i].valid && e[i].tag == tag (ip)) return &e[i];
		return 0;
	}
	void touch (entry *hit, unsigned int ip) {
		entry *e = set[index (ip)];
		for (int i = 0; i < LPB_WAYS; i++) e[i].lru = 1;
		hit->lru = 0;
	}
	entry *allocate (unsigned int ip, bool taken) {
		entry *e = set[index (ip)];
		entry *v = &e[0];
		for (int i = 0; i < LPB_WAYS; i++) {
			if (!e[i].valid) { v = &e[i]; break; }
			if (e[i].lru > v->lru) v = &e[i];
		}
		v->valid = true;
		v->tag = tag (ip);
		v->count = 0;
		v->limit = 0;
		v->dir = !taken;   // assume the mispredicted outcome was a loop exit
		v->conf = 0;
		touch (v, ip);
		return v;
	}
	// usable prediction?
	static bool confident (entry *e) { return e->conf >= CPM_LOOP_CONF && e->limit > 0; }
	static bool predict (entry *e) { return (e->count == e->limit) ? !e->dir : e->dir; }

	static void update (entry *e, bool taken) {
		if (taken == e->dir) {
			// another iteration of the loop body
			if (e->count == 63) { e->count = 0; e->limit = 0; e->conf = 0; return; }
			e->count++;
		} else {
			// loop exit
			if (e->count == 0) {
				// two exits in a row: we guessed the body direction wrong
				e->dir = !e->dir;
				e->limit = 0;
				e->conf = 0;
			} else if (e->count == e->limit) {
				if (e->conf < 3) e->conf++;
			} else {
				e->limit = e->count;
				e->conf = 0;
			}
			e->count = 0;
		}
	}
};

struct ibtb_table {
	struct entry { bool valid; unsigned char tag; unsigned int target; };
	entry tab[1 << IBTB_BITS];
	ibtb_table (void) { memset (tab, 0, sizeof (tab)); }
	static unsigned int index (unsigned int h) { return (h >> 6) & ((1 << IBTB_BITS) - 1); }
	static unsigned int tag (unsigned int h) { return (((h >> 14) & 1) << 6) | (h & 0x3f); }
	bool lookup (unsigned int h, unsigned int &t) {
		entry &e = tab[index (h)];
		if (e.valid && e.tag == tag (h)) { t = e.target; return true; }
		return false;
	}
	void update (unsigned int h, unsigned int t) {
		entry &e = tab[index (h)];
		e.valid = true; e.tag = tag (h); e.target = t;
	}
};

class cpm_update : public branch_update {
public:
	unsigned int index;          // bimodal index
	unsigned int hash;           // IP ^ PIR
	int gway;                    // global hit way or -1
	bool bim_pred;
	bool loop_used;              // loop predictor supplied the prediction
	bool btb_hit;
	unsigned int pred_target;
};

class cpm_predictor : public branch_predictor {
public:
	cpm_update u;
	branch_info bi;
	unsigned int pir;            // 15-bit path information register
	unsigned int ghr;            // used only when CPM_USE_PIR == 0

	bimodal_table bim;
	global_table gp;
	btb_table btb;
	loop_table lpb;
	ibtb_table ibtb;
	unsigned char long_jcc[1 << 12];   // predecode: 1 = 6-byte Jcc, 0 = 2-byte
	unsigned int ras[RAS_DEPTH];
	int ras_top;                 // number of valid entries (circular)
	int ras_pos;

	long long n_cond, n_global_hit, n_loop_used;

	cpm_predictor (void) : pir (0), ghr (0), gp (CPM_GLOBAL_WAYS), ras_top (0), ras_pos (0),
		n_cond (0), n_global_hit (0), n_loop_used (0) {
		memset (ras, 0, sizeof (ras));
		memset (long_jcc, 0, sizeof (long_jcc));
	}

	~cpm_predictor (void) {
#ifdef PM_STATS
		if (n_cond)
			fprintf (stderr, "cpm: global hit %.2f%%, loop used %.2f%%\n",
				100.0 * n_global_hit / n_cond, 100.0 * n_loop_used / n_cond);
#endif
	}

	static unsigned char type_of (const branch_info &b) {
		unsigned int f = b.br_flags;
		if (f & BR_RETURN) return T_RET;
		if ((f & BR_CALL) && (f & BR_INDIRECT)) return T_ICALL;
		if (f & BR_CALL) return T_CALL;
		if (f & BR_INDIRECT) return T_INDIRECT;
		if (f & BR_CONDITIONAL) return T_COND;
		return T_UNCOND;
	}

	unsigned int history (void) { return CPM_USE_PIR ? pir : ghr; }

	// PIR update (approximation of the Pentium M scheme described by
	// Uzelac & Milenkovic): on every taken branch the PIR is shifted left by
	// two and XORed with address bits IP[18:4]; for indirect branches the
	// target address is folded in as well so different targets produce
	// different paths.
	void update_pir (const branch_info &b, bool taken, unsigned int target) {
		if (!taken) return;
		if (CPM_PIR_COND_ONLY && !(b.br_flags & BR_CONDITIONAL)) return;
		unsigned int v = (b.address >> 4) & 0x7fff;
		if (b.br_flags & BR_INDIRECT) v ^= (target >> 4) & 0x7fff;
		pir = ((pir << 2) ^ v) & 0x7fff;
	}

	void ras_push (unsigned int a) {
		ras_pos = (ras_pos + 1) % RAS_DEPTH;
		ras[ras_pos] = a;
		if (ras_top < RAS_DEPTH) ras_top++;
	}
	unsigned int ras_peek (void) { return ras_top ? ras[ras_pos] : 0; }
	void ras_pop (void) {
		if (ras_top) { ras_pos = (ras_pos + RAS_DEPTH - 1) % RAS_DEPTH; ras_top--; }
	}

	branch_update *predict (branch_info & b) {
		bi = b;
		unsigned int ip = b.address;
		btb_table::entry *be = btb.lookup (ip);
		u.btb_hit = be != 0;
		u.hash = (ip ^ history ()) & 0x7fff;
		u.loop_used = false;
		u.gway = -1;

		// ---- direction ----
		bool dir = true;
		if (b.br_flags & BR_CONDITIONAL) {
			u.index = bimodal_table::index (ip);
			u.bim_pred = bim.predict (ip);
			dir = u.bim_pred;

			// loop predictor overrides bimodal (needs LPB hit AND BTB hit)
			if (CPM_USE_LOOP && u.btb_hit) {
				loop_table::entry *le = lpb.lookup (ip);
				if (le && loop_table::confident (le)) {
					dir = loop_table::predict (le);
					u.loop_used = true;
				}
			}

			// global predictor overrides both
			u.gway = gp.lookup (u.hash);
			if (u.gway >= 0 && !(CPM_LOOP_OVER_GLOBAL && u.loop_used)) {
				dir = ctr_taken (gp.set[global_table::index (u.hash)][u.gway].ctr);
				u.loop_used = false;
			}
		}
		u.direction_prediction (dir);

		// ---- target ----
		unsigned int fallthrough = ip + (long_jcc[ip & 0xfff] ? 6 : 2);
		unsigned int tgt = fallthrough;
		if (dir && be) {
			tgt = be->target;
			if (be->type == T_RET) {
				if (ras_top) tgt = ras_peek ();
			} else if (be->type == T_INDIRECT || be->type == T_ICALL) {
				unsigned int it;
				if (ibtb.lookup (u.hash, it)) tgt = it;
			}
		}
		u.pred_target = tgt;
		u.target_prediction (tgt);
		return &u;
	}

	void update (branch_update *bu, bool taken, unsigned int target) {
		cpm_update *pu = (cpm_update *) bu;
		unsigned int ip = bi.address;
		unsigned char type = type_of (bi);

		if (bi.br_flags & BR_CONDITIONAL) {
			n_cond++;
			bool final_pred = pu->direction_prediction ();

			// global predictor
			if (pu->gway >= 0) {
				n_global_hit++;
				gp.update_hit (pu->hash, pu->gway, taken);
			} else if (pu->bim_pred != taken) {
				gp.allocate (pu->hash, taken);
			}
			if (pu->loop_used) n_loop_used++;

			// loop predictor: update existing entry, allocate on a mispredict
			if (CPM_USE_LOOP) {
				loop_table::entry *le = lpb.lookup (ip);
				if (le) {
					loop_table::update (le, taken);
					lpb.touch (le, ip);
				} else if (final_pred != taken) {
					lpb.allocate (ip, taken);
				}
			}

			bim.update (ip, taken);
			ghr = ((ghr << 1) | (taken ? 1 : 0)) & 0x7fff;
		}

		// BTB: allocate on taken branches, refresh target/type
		btb_table::entry *be = btb.lookup (ip);
		if (taken) {
			if (!be) be = btb.allocate (ip);
			else btb.touch (be, ip);
			be->type = type;
			if (type != T_RET) be->target = target;
		}
		if ((bi.br_flags & BR_CONDITIONAL) && !taken)
			long_jcc[ip & 0xfff] = (target - ip) == 6;

		// iBTB for indirect jumps/calls
		if (type == T_INDIRECT || type == T_ICALL)
			ibtb.update (pu->hash, target);

		// return address stack
		if (type == T_CALL) ras_push (ip + 5);
		else if (type == T_ICALL) ras_push (ip + 2);
		else if (type == T_RET) ras_pop ();

		update_pir (bi, taken, target);
	}
};