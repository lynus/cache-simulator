#include <cmath>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <list>
#include <math.h>
#include <memory>
#include <vector>
#include <map>
#include <boost/optional.hpp>
#include <boost/intrusive/list.hpp>
#include <unistd.h>
#include "sequence.h"

#include <stdlib.h>
#include <time.h>

#include "rule.hpp"
using namespace std;
using namespace boost;
using namespace boost::archive;
#define ASSIGN(arr,index,d1,d2,d3,d4)	arr[index][0] = d1;	\
					arr[index][1] = d2;	\
					arr[index][2] = d3;	\
					arr[index][3] = d4;	

#define C40(array)	ASSIGN(array, 0, 0,0,0,0);
#define C41(array)	ASSIGN(array, 0, 1,0,0,0);	\
			ASSIGN(array, 1, 0,1,0,0);	\
			ASSIGN(array, 2, 0,0,1,0);	\
			ASSIGN(array, 3, 0,0,0,1);

#define C42(array)	ASSIGN(array, 0, 1,1,0,0);	\
			ASSIGN(array, 1, 1,0,1,0);	\
			ASSIGN(array, 2, 1,0,0,1);	\
			ASSIGN(array, 3, 0,1,1,0);	\
			ASSIGN(array, 4, 0,1,0,1);	\
			ASSIGN(array, 5, 0,0,1,1);

#define C43(array)	ASSIGN(array, 0, 1,1,1,0);	\
			ASSIGN(array, 1, 1,1,0,1);	\
			ASSIGN(array, 2, 1,0,1,1);	\
			ASSIGN(array, 3, 0,1,1,1);

#define C44(array)	ASSIGN(array, 0, 1,1,1,1);
			
char *trace_file=NULL;
char *rule_map_file=NULL;
//----------------------------------------------------------------

namespace {
	typedef bnr_t	block;
	class prefetch_map;
	void mk_candidate(vector<seq> &candi, seq &ante, int i)
	{
		static int times[] = {1, 4, 6, 4, 1};
		int index[6][4];
		candi.clear();
		switch (i) {
			case 0:
				C40(index);
				break;
			case 1:
				C41(index);
				break;
			case 2:
				C42(index);
				break;
			case 3:
				C43(index);
				break;
			case 4:
				C44(index);
				break;
		}
		for(int j = 0; j < times[i]; j++) {
			int ii;
			ii = 0;
			seq t;
			for (auto it = ante.begin(); it != ante.end(); it++) {
				if (index[j][ii]) 
					t.push_back(*it);
				ii++;
			}
			t.push_back(ante.back());
			t.sort();
			candi.push_back(t);
			t.clear();
		}
	}

	//--------------------------------

	// FIXME: these random generators are not good enough, use the
	// Mersenne Twister from boost.
	double rand01(void) {
		return static_cast<double>(rand()) / static_cast<double>(RAND_MAX);
	}

	block rand_block(block max) {

		return rand() % max;
	}

	//--------------------------------

	void renormalise(vector<double> &pdfs) {
		double total = 0.0;

		for (unsigned i = 0; i < pdfs.size(); i++)
			total += pdfs[i];

		if (total == 0.0)
			return;

		for (unsigned i = 0; i < pdfs.size(); i++)
			pdfs[i] /= total;
	}

	template <typename T>
	T square(T x) {
		return x * x;
	}

	// probability density function
	class pdf {
	public:
		typedef std::shared_ptr<pdf> ptr;

		virtual ~pdf() {}
		virtual void generate(vector<double> &pdfs) const = 0;
	};

	class uniform : public pdf {
	public:
		virtual void generate(vector<double> &pdfs) const {
			if (pdfs.size() == 0)
				return;

			double value = 1.0 / static_cast<double>(pdfs.size());
			for (unsigned i = 0; i < pdfs.size(); i++)
				pdfs[i] = value;
		}
	};

	class gaussian : public pdf {
	public:
		typedef std::shared_ptr<gaussian> ptr;

		gaussian(double mean, double variance)
			: mean_(mean),
			  variance_(variance) {
		}

		virtual void generate(vector<double> &pdfs) const {
			double standard_deviation = sqrt(variance_);

			for (unsigned i = 0; i < pdfs.size(); i++) {
				double x = static_cast<double>(i);
				double exponent = -1.0 * (square(x - mean_) / (2 * variance_));

				pdfs[i] = (1.0 / (standard_deviation * sqrt(2.0 * M_PI))) *
					exp(exponent);
			}

			renormalise(pdfs);
		}

	private:
		double mean_;
		double variance_;
	};

	class mixture : public pdf {
	public:
		typedef std::shared_ptr<mixture> ptr;

		void add_component(double weight, pdf::ptr pdf) {
			components_.push_back(make_pair(weight, pdf));
		};

		virtual void generate(vector<double> &pdfs) const {
			vector<double> tmp(pdfs.size(), 0.0);

			for (unsigned i = 0; i < pdfs.size(); i++)
				pdfs[i] = 0.0;

			for (auto cs = components_.begin(); cs != components_.end(); ++cs) {
				cs->second->generate(tmp);

				for (unsigned i = 0; i < pdfs.size(); i++)
					pdfs[i] += cs->first * tmp[i];
			}

			renormalise(pdfs);
		}

	private:
		typedef pair<double, pdf::ptr> component;

		list<component> components_;
	};

	unsigned rand_pdf(vector<double> const &pdf) {
		double n = rand01();

		double total = 0.0;
		unsigned i;
		for (i = 0; i < pdf.size(); i++) {
			total += pdf[i];
			if (total >= n)
				return i;
		}

		return i;
	}

	//--------------------------------

	class cache {
	public:
		typedef std::shared_ptr<cache> ptr;

		cache(unsigned size)
			: hits_(0),
			  misses_(0),
			  evictions_(0),
			  cache_size_(size) {
		}

		block size() const {
			return cache_size_;
		}

		int request(block origin_block, bool prefetch = false) {
			if (to_cache_.count(origin_block) > 0) {
				if (!prefetch)
					hits_++;
				return 1;
			}
			else {
				if (!prefetch)
					misses_++;
				return 0;
			}
		};

		unsigned get_hits() const {
			return hits_;
		}

		unsigned get_misses() const {
			return misses_;
		}

		unsigned get_evictions() const {
			return evictions_;
		}

		void insert(block cache_block, block origin_block) {
			auto i = to_origin_.find(cache_block);
			if (i != to_origin_.end()) {
				if (i->second == origin_block)
					return; // mapping hasn't changed

				evictions_++;
				to_cache_.erase(i->second);
				to_origin_.erase(i->first);
			}

			to_cache_.insert(make_pair(origin_block, cache_block));
			to_origin_.insert(make_pair(cache_block, origin_block));
		}
		void clear() {
			to_cache_.clear();
			to_origin_.clear();
			hits_ = misses_ = evictions_ = 0;
		}
	private:
		typedef map<block, block> block_map;

		block_map to_cache_;
		block_map to_origin_;

		unsigned hits_;
		unsigned misses_;
		unsigned evictions_;

		unsigned cache_size_;
	};

	//--------------------------------

	class policy {
	public:
		typedef std::shared_ptr<policy> ptr;

		policy(block origin_size, cache::ptr cache, bool check)
			: origin_size_(origin_size),
			  cache_(cache),check_request(check) {
		}
		bool need_check_request() {
			return check_request;
		}
		virtual ~policy() {}
		virtual void map(block origin_block) = 0;

	protected:
		unsigned origin_size() const {
			return origin_size_;
		}

		cache::ptr get_cache() {
			return cache_;
		}

	private:
		block origin_size_;
		cache::ptr cache_;
		bool check_request;
	};

	// A silly little policy that gives us a baseline to compare the
	// more sophisticated policies against.  Actually it works quite
	// well!
	class random_policy : public policy {
	public:
		random_policy(double prob_of_promotion, block origin_size, 
				cache::ptr cache, bool check = true)
			: policy(origin_size, cache, check),
			  prob_(prob_of_promotion) {
		}

		virtual void map(block origin_block) {
			double r = rand01();
			if (r < prob_ ) {
				cache::ptr c = get_cache();
				c->insert(rand_block(c->size()), origin_block);
			}
		}

	private:
		double prob_;
	};

	class hash_policy : public policy {
	public:
		hash_policy(unsigned hash_size, double prob, block origin_size,
				cache::ptr cache, bool check = true)
			: policy(origin_size, cache, check),
			  hash_(hash_size, 0),
			  prob_(prob) {
		}

		virtual void map(block origin_block) {
			double p = prob_;
			unsigned h = hash(origin_block);

			if (hash_[h] > 0)
				p *= 100;

			double r = rand01();
			if (r < p) {
				cache::ptr c = get_cache();
				c->insert(rand_block(c->size()), origin_block);
				hash_[h] = 0;
			} else
				hash_[h]++;
		}

	private:
		unsigned hash(block b) const {
			const unsigned long BIG_PRIME = 4294967291UL;
			uint64_t hash = b * BIG_PRIME;
			return hash % hash_.size();
		}

		vector<unsigned> hash_;
		double prob_;
	};


	class lru_policy : public policy {
	public:
		lru_policy(block origin_size, cache::ptr cache, bool check = true)
			: policy(origin_size, cache, check),
			  entries_(cache->size()) {
		}

		virtual void map(block origin_block) {
			optional<lru_policy::entry &> oe = find_by_origin(origin_block);
			if (oe)
				return;

			// always promote if it's not in the cache
			cache::ptr c = get_cache();
			if (map_.size() < c->size()) {
				// create a new entry
				entry &e = entries_[map_.size()];
				e.origin_ = origin_block;
				e.cache_ = map_.size();
				lru_.push_back(e);
				map_.insert(make_pair(origin_block, ref(e)));
				c->insert(e.cache_, e.origin_);

			} else {
				// overwrite the lru
				entry &e = find_lru();
				map_.erase(e.origin_);
				e.origin_ = origin_block;
				map_.insert(make_pair(origin_block, ref(e)));
				mark_mru(e);
				c->insert(e.cache_, e.origin_);
			}
		}

	private:
		struct entry {
			intrusive::list_member_hook<> list_;
			intrusive::list_member_hook<> hash_;
			block origin_;
			block cache_;
		};

		typedef intrusive::member_hook<entry, intrusive::list_member_hook<>, &entry::list_> lru_option;
		typedef intrusive::list<entry, lru_option> lru_list;

		typedef std::map<block, entry &> entry_map;

		optional<entry &> find_by_origin(block origin) {
			entry_map::iterator it = map_.find(origin);
			if (it != map_.end())
				return optional<entry &>(it->second);

			return optional<entry &>();
		}

		entry &find_lru() {
			entry &e = lru_.front();
			return e;
		}

		void mark_mru(struct entry &e) {
			lru_.erase(lru_.iterator_to(e));
			lru_.push_back(e);
		}

		vector<entry> entries_;
		entry_map map_;
		lru_list lru_;
	};

	class arc_policy : public policy {
	private:
		enum entry_state {
			T1,
			T2,
			B1,
			B2
		};

		struct entry {
			entry_state state_;
			intrusive::list_member_hook<> list_;
			block origin_;
			block cache_;
		};

		typedef intrusive::member_hook<entry, intrusive::list_member_hook<>, &entry::list_> lru_option;
		typedef intrusive::list<entry, lru_option> lru_list;

		typedef std::map<block, entry &> entry_map;

	public:
		arc_policy(block origin_size, cache::ptr cache,bool check = false)
			: policy(origin_size, cache, check),
			  entries_(cache->size() * 2),
			  p_(0),
			  allocated_(0),
			  allocated_entries_(0) {
		}

		virtual void map(block origin_block) {
			cache::ptr c = get_cache();

			optional<entry &> oe = find_by_origin(origin_block);
			if (oe) {
				entry &e = *oe;
				block new_cache;
				block delta;
				block b1_size = b1_.size();
				block b2_size = b2_.size();

				switch (e.state_) {
				case T1:
					t1_.erase(t1_.iterator_to(e));
					break;

				case T2:
					t2_.erase(t2_.iterator_to(e));
					break;

				case B1:
					delta = (b1_size > b2_size) ? 1 : max<block>(b2_size / b1_size, 1);
					p_ = min(p_ + delta, c->size());
					new_cache = demote(optional<entry &>(e));

					b1_.erase(b1_.iterator_to(e));

					e.cache_ = new_cache;
					e.origin_ = origin_block;

					break;

				case B2:
					delta = b2_size >= b1_size ? 1 : max<block>(b1_size / b2_size, 1);
					p_ = max(p_ - delta, static_cast<block>(0));
					new_cache = demote(optional<entry &>(e));

					b2_.erase(b2_.iterator_to(e));

					e.cache_ = new_cache;
					e.origin_ = origin_block;

					break;
				}

				push(T2, e);
				return;
			}

			if (!interesting_block(origin_block))
				return;

			entry *e;
			block l1_size = t1_.size() + b1_.size();
			block l2_size = t2_.size() + b2_.size();
			if (l1_size == c->size()) {
				if (t1_.size() < c->size()) {
					e = &pop(B1);

					block new_cache = demote();
					e->cache_ = new_cache;
					e->origin_ = origin_block;

				} else {
					e = &pop(T1);
					e->origin_ = origin_block;
				}

			} else if (l1_size < c->size() && (l1_size + l2_size >= c->size())) {
				if (l1_size + l2_size == 2 * c->size()) {
					e = &pop(B2);
					e->cache_ = demote();
					e->origin_ = origin_block;

				} else {
					e = &alloc_entry();
					e->cache_ = demote();
					e->origin_ = origin_block;
				}

			} else {
				BOOST_ASSERT(allocated_ < c->size());
				e = &alloc_entry();
				e->origin_ = origin_block;
				e->cache_ = allocated_++;
			}

			push(T1, *e);
		}

	private:
		virtual bool interesting_block(block origin) {
			return true;
		}

		void insert(entry &e) {
			map_.insert(make_pair(e.origin_, ref(e)));
			get_cache()->insert(e.cache_, e.origin_);
		}

		optional<entry &> find_by_origin(block origin) {
			entry_map::iterator it = map_.find(origin);
			if (it != map_.end())
				return optional<entry &>(it->second);

			return optional<entry &>();
		}

		void push(entry_state s, entry &e) {
			e.state_ = s;

			switch (s) {
			case T1:
				t1_.push_back(e);
				insert(e);
				break;

			case T2:
				t2_.push_back(e);
				insert(e);
				break;

			case B1:
				b1_.push_back(e);
				break;

			case B2:
				b2_.push_back(e);
				break;
			}
		}

		entry &pop(entry_state s) {
			entry *e;

			switch (s) {
			case T1:
				e = &t1_.front();
				t1_.pop_front();
				map_.erase(e->origin_);
				break;

			case T2:
				e = &t2_.front();
				t2_.pop_front();
				map_.erase(e->origin_);
				break;

			case B1:
				e = &b1_.front();
				b1_.pop_front();
				break;

			case B2:
				e = &b2_.front();
				b2_.pop_front();
				break;
			}

			return *e;
		}

		block demote(optional<entry &> oe = optional<entry &>()) {
			entry *e;

			if ((t1_.size() > 0) &&
			    ((t1_.size() > p_) || (oe && oe->state_ == B2 && t1_.size() == p_))) {
				e = &pop(T1);
				push(B1, *e);

			} else {
				e = &pop(T2);
				push(B2, *e);
			}

			return e->cache_;
		}

		entry &alloc_entry() {
			BOOST_ASSERT(allocated_entries_ < 2 * get_cache()->size());
			return entries_[allocated_entries_++];
		}

		vector<entry> entries_;
		entry_map map_;
		lru_list t1_, b1_, t2_, b2_;
		block p_;
		block allocated_;
		block allocated_entries_;
	};

	class arc_window_policy : public arc_policy {
	public:
		arc_window_policy(block interesting_size, block origin_size,
				cache::ptr cache, bool check = false)
			: arc_policy(origin_size, cache, check),
			  entries_(interesting_size),
			  allocated_(0) {
		}

	private:
		virtual bool interesting_block(block origin) {
			auto it = map_.find(origin);
			if (it == map_.end()) {
				if (allocated_ == entries_.size()) {
					entry &e = lru_.front();
					lru_.pop_front();
					map_.erase(e.origin_);

					e.origin_ = origin;
					lru_.push_back(e);
					map_.insert(make_pair(origin, ref(e)));

				} else {
					entry &e = entries_[allocated_++];
					e.origin_ = origin;
					lru_.push_back(e);
					map_.insert(make_pair(origin, ref(e)));
				}

				return false;
			}

			// this block is interesting so we forget about it,
			// relying on the arc policy to manage it.
			map_.erase(it->second.origin_);
			return true;
		}

		struct entry {
			intrusive::list_member_hook<> list_;
			block origin_;
		};

		typedef intrusive::member_hook<entry, intrusive::list_member_hook<>, &entry::list_> lru_option;
		typedef intrusive::list<entry, lru_option> lru_list;

		typedef std::map<block, entry &> entry_map;

		vector<entry> entries_;
		block allocated_;
		lru_list lru_;
		entry_map map_;
	};

	class arc_hash_policy : public arc_policy {
	public:
		arc_hash_policy(block interesting_size, block origin_size,
				cache::ptr cache, bool check = false)
			: arc_policy(origin_size, cache, check),
			  table_(interesting_size, 0) {
		}

	private:
		unsigned hash(block b) {
			const block BIG_PRIME = 4294967291UL;
			block h = b * BIG_PRIME;

			return static_cast<unsigned>(h % table_.size());

		}

		virtual bool interesting_block(block origin) {
			unsigned h = hash(origin);
			if (table_[h] == origin)
				return true;

			table_[h] = origin;
			return false;
		}

		vector<block> table_;
	};

	//--------------------------------

	class pdf_sequence : public sequence<block> {
	public:
		pdf_sequence(pdf::ptr pdf, block size)
			: probs_(size, 0.0) {
			pdf->generate(probs_);
		}

		virtual block operator()() {
			return rand_pdf(probs_);
		}

	private:
		vector<double> probs_;
	};

	class linear_sequence : public sequence<block> {
	public:
		linear_sequence(block begin, block end)
			: begin_(begin),
			  end_(end),
			  current_(begin_) {
		}

		virtual block operator()() {
			block r = current_;
			if (++current_ >= end_)
				current_ = begin_;

			return r;
		}

	private:
		block begin_, end_, current_;
	};
	class trace_sequence : public sequence<block> {
	public:
		trace_sequence(char const *fname, int begin, int end)
			: current_(0) {
			int i = 1;
			fstream fin(fname, ios::in);
			if (!fin.is_open()) 
				throw "can not open trace file";
			while(!fin.eof()) {
				block t;
				fin>>t;
				if (!fin || i >= end) break;
				if (i >= begin)
					trace.push_back(t);
				i++;
			}
			if (i < end)
				throw "reading file error";
			fin.close();
		}
		virtual block operator() () {
			block r = trace.at(current_);
			if (++current_ >= trace.size())
				current_ = 0;
			return r;
		}
	private:
		size_t current_;
		vector<block> trace;
	};
	
	class prefetcher {
	public:
		typedef std::shared_ptr<prefetcher> ptr;
		prefetcher(policy::ptr p, cache::ptr c): policy_(p),cache_(c) {
			pref_times = 0; pref_blocks = 0;
		}
		virtual void prefetch(block b) = 0;
		virtual void clear() {
			pref_blocks = 0;
			pref_times = 0;
		}
		void set_policy(policy::ptr p) {
			policy_ = p;
		}
		void display_stats() {
			cout<<"prefetch times: "<<pref_times;
			cout<<"  prefetch blocks: "<<pref_blocks<<endl;
			clear();
		}

	protected:
		policy::ptr policy_;
		cache::ptr  cache_;
		unsigned long pref_times;
		unsigned long pref_blocks;
	};
	
	class readahead : public prefetcher {
	public:
		readahead(policy::ptr p, cache::ptr c, block max_win):
			prefetcher(p,c), max_win_(max_win), cur_win_(0) { }
		virtual void prefetch(block b) {
			if (cur_win_ == 0)
				cur_win_ = 2; //initial win size
			else {
				if (b == last_blk+1) {
					cur_win_ *= 2;
					cur_win_ = min(max_win_, cur_win_);
					pref_times++;
					pref_blocks += cur_win_;
				} else 
					cur_win_ = 0;
			}
			last_blk = b;
			for(block blk = b+1; blk <= b+cur_win_; blk++) {
				bool hit;
				hit = cache_->request(blk, true);
				if (policy_->need_check_request() && hit)
					continue;
				policy_->map(blk);
			}

		}
		virtual void clear() {
			prefetcher::clear();
			cur_win_ = 0;
		}
	private:
		block max_win_;
		block cur_win_;
		block last_blk;
	};
	class prefetch_map : public rule_map, public prefetcher {
	public:
		typedef std::shared_ptr<prefetch_map> ptr;
		prefetch_map(policy::ptr p, cache::ptr c):rule_map(), prefetcher(p, c){
		}
		virtual void prefetch(block b) {
			if (win.size()< 5) {
				win.push_back(b);
				return;
			}
			win.pop_front();
			win.push_back(b);
			seq secc = do_predict(win);
			if (secc.size()){
				pref_times++;
				pref_blocks += secc.size();
			}
#ifdef DEBUG
			{
				cerr<<"window: ";
				for(auto it = win.begin(); it != win.end(); it++)
					cerr<< *it<< "  ";
				cerr<<"predicted: ";
				if (secc.size() == 0)
					cerr<< "none"<< endl;
				else 
					for(auto it = secc.begin(); it != secc.end();
							it++)
						cerr<< *it<< "  ";
				cerr<<endl;
			}
#endif
			for (auto it = secc.begin(); it != secc.end(); it++) {
				bool hit;
				hit = cache_->request(*it,true);
				if (policy_->need_check_request() && hit)
					continue;
				policy_->map(*it);
			}
		}
		virtual void clear() {
			prefetcher::clear();
			win.clear();
		}
	private:
		seq do_predict(seq ante) 
		{
			vector<seq> candidates;
			seq ret;
			for(int i = 4; i>=0; i--) {
				mk_candidate(candidates, ante, i);
				for(auto it = candidates.begin(); it != candidates.end();
						it++) {
					auto mit = seq_map.find(*it);
					if (mit != seq_map.end()) {
#ifdef DEBUG
						{
							cerr<< '{';
							for(auto _it = mit->first.begin();
									_it != mit->first.end();
									_it++)
								cerr<< *_it<< "  ";
							cerr<< "} ";
						}
#endif
						ret = mit->second;
						return ret;
					}
				}
			}
		}
		seq win;
	};

	void
	run_sequence(block run_length, sequence<block>::ptr seq, cache::ptr c, policy::ptr p,
			prefetcher::ptr pm = NULL) {
		for (unsigned i = 0; i < run_length; i++) {
			block b = (*seq)();
			bool hit;
			hit = c->request(b);
			if (!p->need_check_request() || !hit)
				p->map(b);
			if (pm)
				pm->prefetch(b);
		}
	}

	void
	run_simulation(block run_length,
		       sequence<block>::ptr seq1,
		       sequence<block>::ptr seq2,
		       sequence<block>::ptr lseq,
		       cache::ptr c, policy::ptr p) {

		run_sequence(run_length, seq1, c, p);
		run_sequence(run_length, lseq, c, p);
		run_sequence(run_length, seq1, c, p);

		run_sequence(run_length, seq2, c, p);
		run_sequence(run_length, lseq, c, p);
		run_sequence(run_length, seq2, c, p);
	}

	template <typename T,typename T1>
	double percentage(T1 n, T tot) {
		return (static_cast<double>(n) / static_cast<double>(tot)) * 100.0;
	}

	void
	display_cache_stats(string const &label, cache::ptr c) {
		block total = c->get_hits() + c->get_misses();
 
		cout << label << ": "<< c->get_hits()<<" blocks, "
		     << setprecision(3) << percentage(c->get_hits(), total)
		     << "% hits, "<< c->get_evictions()<<" blocks, "
		     << setprecision(3) << percentage(c->get_evictions(), total)
		     << "% evictions"
		     << endl;
	}

}
void show_usage(const char *prog)
{
	cerr<<"[usage] "<<prog<<" -t trace_file [-r rule_map_file -h -s startpoint -l run_length]"<<endl;
	exit(-1);
}
//----------------------------------------------------------------

int main(int argc, char **argv)
{
	srand(time(NULL));
	mixture::ptr blend1, blend2;

	{
		pdf::ptr g1(new gaussian(120.0, 40.0));
		pdf::ptr g2(new gaussian(700.0, 100.0));
		pdf::ptr u(new uniform);

		blend1 = mixture::ptr(new mixture);
		blend1->add_component(0.2, g1);
		blend1->add_component(0.3, g2);
		blend1->add_component(0.1, u);
	}

	{
		pdf::ptr g1(new gaussian(500.0, 100.0));
		pdf::ptr g2(new gaussian(900.0, 10.0));
		pdf::ptr u(new uniform);

		blend2 = mixture::ptr(new mixture);
		blend2->add_component(0.4, g1);
		blend2->add_component(0.5, g2);
		blend2->add_component(0.1, u);
	}

	block origin_size = 1000;
	block cache_size = 50;
	block run_length = 10000;

	sequence<block>::ptr seq1(new pdf_sequence(blend1, origin_size));
	sequence<block>::ptr seq2(new pdf_sequence(blend2, origin_size));
	sequence<block>::ptr lseq(new linear_sequence(0, origin_size));
	int opt;
	int start = 0;
	while((opt = getopt(argc, argv, "t:r:s:l:h")) != -1) {
		switch(opt) {
			case 't':
				trace_file = (char *)malloc(sizeof(char)*32);
				strncpy(trace_file, optarg, 32);
				break;
			case 'r':
				rule_map_file = (char *)malloc(sizeof(char)*32);
				strncpy(rule_map_file, optarg, 32);
				break;
			case 'h':
				show_usage(argv[0]);
				break;
			case 's':
				start = atoi(optarg);
				break;
			case 'l':
				run_length = atoi(optarg);
				break;
			case '?':
				show_usage(argv[0]);
				break;
		}
	}
	if (trace_file == NULL) {
		cerr<<"error: not specify trace file"<<endl;
		show_usage(argv[0]);
	}
	{
		block cache_size = 1<<18; //128M cache , one block is 512bytes
		block origin_size = 1l<<25; //16G in bytes 
		cache::ptr c(new cache(cache_size));
		policy::ptr p(new random_policy(1.0, origin_size, c));
		sequence<block>::ptr myseq;
		prefetch_map::ptr  pm(new prefetch_map(p, c));
		readahead::ptr ra(new readahead(p, c, 512));
		try {
		myseq = sequence<block>::ptr(new trace_sequence(trace_file, start, start+run_length));
		if (rule_map_file == NULL)
			pm->load();
		else
			pm->load(rule_map_file);
		}catch(const char *msg) {
			cerr<<msg<<endl;
			exit(-1);
		}
		cout<<"sequence run length: "<<run_length<<endl;
cout<<"/*===================random test================*/"<<endl;
		run_sequence(run_length, myseq, c, p);
		display_cache_stats("my trace random policy ",c);
		c->clear();

		run_sequence(run_length, myseq, c, p, pm);
		display_cache_stats("my trace random policy with prefetch",c);
		pm->display_stats();
		c->clear();

		run_sequence(run_length, myseq, c, p, ra);
		display_cache_stats("my trace randowm policy with readahead",c);
		ra->display_stats();
		c->clear();
cout<<"/*===================lru test====================*/"<<endl;
		p = policy::ptr(new lru_policy(origin_size, c));
		run_sequence(run_length, myseq, c, p);
		display_cache_stats("my trace lru policy", c);
		c->clear();

		p = policy::ptr(new lru_policy(origin_size, c));
		pm->set_policy(p);
		run_sequence(run_length, myseq, c, p, pm);
		display_cache_stats("my trace lru policy wich prefetch", c);
		pm->display_stats();
		c->clear();

		run_sequence(run_length, myseq, c, p, ra);
		display_cache_stats("my trace lru policy with readahead",c);
		ra->display_stats();
		c->clear();
cout<<"/*===================hash test===================*/"<<endl;
		p = policy::ptr(new hash_policy(10000, 0.01, origin_size, c));
		run_sequence(run_length, myseq, c, p);
		display_cache_stats("my trace hash policy", c);
		c->clear();
		
		p = policy::ptr(new hash_policy(10000, 0.01, origin_size, c));
		pm->set_policy(p);
		run_sequence(run_length, myseq, c, p, pm);
		display_cache_stats("my trace hash policy with prefetch", c);
		c->clear();
	
		run_sequence(run_length, myseq, c, p, ra);
		display_cache_stats("my trace hash policy with readahead",c);
		ra->display_stats();
		c->clear();
cout<<"/*===================arc_hash test===============*/"<<endl;
		p = policy::ptr(new arc_hash_policy(c->size()/2, origin_size, c));
		run_sequence(run_length, myseq, c, p);
		display_cache_stats("my trace arc hash policy", c);
		c->clear();

		p = policy::ptr(new arc_hash_policy(c->size()/2, origin_size, c));
		pm->set_policy(p);
		run_sequence(run_length, myseq, c, p, pm);
		display_cache_stats("my trace arc hash policy with prefetch", c);

		run_sequence(run_length, myseq, c, p, ra);
		display_cache_stats("my trace arc_hash policy with readahead",c);
		ra->display_stats();
		c->clear();

	}

	return 0;
}

//----------------------------------------------------------------
