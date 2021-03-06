// Copyright (C) 2013 Alexandre Rames <alexandre@coreperf.com>
// rejit is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "globals.h"
#include "parser.h"
#include "macro-assembler.h"

namespace rejit {
namespace internal {

// Called from the generated code to register matches.

// Simply push a match with no further check.
void MatchAllAppendRaw(vector<Match>* matches, Match new_match);
// Push a match and delete any previously registered matches rendered invalid by
// the new match.
void MatchAllAppendFilter(vector<Match>* matches, Match new_match);


// A simple regexp visitor, which walks the tree and assigns entry and ouput
// indexes to the regexps.
class RegexpIndexer : public RealRegexpVisitor<void> {
 public:
  explicit RegexpIndexer(RegexpInfo* rinfo,
                            int entry_state = 0,
                            int last_state = 0)
    : rinfo_(rinfo), entry_state_(entry_state), last_state_(last_state) {}

  void Index(Regexp* regexp);
  // By default index from 0 and create the output state.
  // If specified force the entry and/or output states.
  void IndexSub(Regexp* regexp, int entry_state = 0, int output_state = -1);

#define DECLARE_REGEXP_VISITORS(RegexpType) \
  virtual void Visit##RegexpType(RegexpType* r);
  LIST_FLOW_REGEXP_TYPES(DECLARE_REGEXP_VISITORS)
#undef DECLARE_REGEXP_VISITORS

  virtual void VisitRegexp(Regexp* re);
  inline virtual void VisitMultipleChar(MultipleChar* re) { VisitRegexp(re); }
  inline virtual void VisitPeriod(Period* re) { VisitRegexp(re); }
  inline virtual void VisitBracket(Bracket* re) { VisitRegexp(re); }
  inline virtual void VisitStartOfLine(StartOfLine* re) { VisitRegexp(re); }
  inline virtual void VisitEndOfLine(EndOfLine* re) { VisitRegexp(re); }
  // Epsilon transitions are generated explicitly by the RegexpLister and should
  // not appear before that stage.
  inline virtual void VisitEpsilon(Epsilon* epsilon) { UNREACHABLE(); }

  
  RegexpInfo* rinfo() const { return rinfo_; }
  int entry_state() const { return entry_state_; }
  int last_state() const { return last_state_; }

 private:
  RegexpInfo* rinfo_;
  int entry_state_;
  int last_state_;
};


// Walks the regexp tree and lists regexp for which the Codegen needs to
// generate code.
class RegexpLister : public RealRegexpVisitor<void> {
 public:
  explicit RegexpLister(RegexpInfo* rinfo) :
    rinfo_(rinfo) {}

  void List(Regexp* re) {
    if (re->IsControlRegexp()) {
      rinfo_->re_control_list()->push_back(re->AsControlRegexp());
    } else {
      rinfo_->re_matching_list()->push_back(re->AsMatchingRegexp());
    }
  }
  // List a regexp allocated by the lister.
  // Register it in the RegexpInfo to correctly delete it later.
  inline void ListNew(Regexp* re) {
    rinfo_->extra_allocated()->push_back(re);
    List(re);
  }

#define DECLARE_REGEXP_VISITORS(RegexpType) \
  virtual void Visit##RegexpType(RegexpType* r);
  LIST_FLOW_REGEXP_TYPES(DECLARE_REGEXP_VISITORS)
#undef DECLARE_REGEXP_VISITORS

  inline virtual void VisitRegexp(Regexp* re) { List(re); }
  inline virtual void VisitMultipleChar(MultipleChar* re) { VisitRegexp(re); }
  inline virtual void VisitPeriod(Period* re) { VisitRegexp(re); }
  inline virtual void VisitBracket(Bracket* re) { VisitRegexp(re); }
  inline virtual void VisitStartOfLine(StartOfLine* re) { VisitRegexp(re); }
  inline virtual void VisitEndOfLine(EndOfLine* re) { VisitRegexp(re); }
  // Epsilon transitions are generated explicitly.
  inline virtual void VisitEpsilon(Epsilon* epsilon) { UNREACHABLE(); }


  RegexpInfo* rinfo() const { return rinfo_; }

 private:
  RegexpInfo* rinfo_;

  DISALLOW_COPY_AND_ASSIGN(RegexpLister);
};


// Walks the regexp tree to find the regexps to use as fast forward elements.
// TODO: Merge FF into the parsing stage.
class FF_finder : public RealRegexpVisitor<bool> {
 public:
  FF_finder(RegexpInfo *rinfo) :
    rinfo_(rinfo) {
      ff_list_ = rinfo_->ff_list();
    }

  void FindFFElements();

#define DECLARE_REGEXP_VISITORS(RegexpType) \
  virtual bool Visit##RegexpType(RegexpType* r);
  LIST_FLOW_REGEXP_TYPES(DECLARE_REGEXP_VISITORS)
#undef DECLARE_REGEXP_VISITORS

  inline virtual bool VisitRegexp(Regexp* re) {
    ff_list_->push_back(re);
    return true;
  }
#define DEFINE_FF_FINDER_SIMPLE_VISITOR(RType)                                 \
  inline virtual bool Visit##RType(RType* re) { return VisitRegexp(re); }
  DEFINE_FF_FINDER_SIMPLE_VISITOR(MultipleChar)
  DEFINE_FF_FINDER_SIMPLE_VISITOR(Period)
  DEFINE_FF_FINDER_SIMPLE_VISITOR(Bracket)
  DEFINE_FF_FINDER_SIMPLE_VISITOR(StartOfLine)
  DEFINE_FF_FINDER_SIMPLE_VISITOR(EndOfLine)
  // There are no epsilon transitions at this point.
  inline virtual bool VisitEpsilon(Epsilon* epsilon) {
    UNREACHABLE();
    return false;
  }

  // Try to reduce the block of regular expressions to more efficient fast
  // forward elements.
  void ff_alternation_reduce(size_t *start, size_t *end);
  // Reduce the two blocks of regular expressions [i1:i2] and
  // [i2:ff_list_->size()] and chose the most efficient of the two.
  // A positive return value indicates that [i1:i2] is more efficient than
  // [i2:ff_list_->size()].
  int ff_reduce_cmp(size_t *i1, size_t *i2);

 private:
  RegexpInfo *rinfo_;
  vector<Regexp*>* ff_list_;
  DISALLOW_COPY_AND_ASSIGN(FF_finder);
};


class Codegen : public PhysicalRegexpVisitor<void> {
 public:
  Codegen();

  VirtualMemory* Compile(RegexpInfo* rinfo, MatchType match_type);

  // Code generation.
  void Generate();

  void FlowTime();
  void TestTimeFlow();
  void CheckTimeFlow(Direction direction,
                     Label *exit,
                     Label *limit);

  bool GenerateFastForward_(bool early = false);
  inline bool GenerateFastForward() { return GenerateFastForward_(false); }
  // Start looking for potential matches before setting up the stack.
  inline bool GenerateFastForwardEarly() { return GenerateFastForward_(true); }
  void HandleControlRegexps();

  void CheckMatch(Direction direction, Label* limit);
  void RegisterMatch();

  void set_direction(Direction dir);

  void GenerateMatchDirection(Direction direction);
  void GenerateMatchBackward();
  inline void GenerateMatchForward() {
    GenerateMatchDirection(kForward);
  }

  void GenerateTransitions(Direction direction);

#define DECLARE_REGEXP_VISITORS(RegexpType) \
  virtual void Visit##RegexpType(RegexpType* r);
  LIST_PHYSICAL_REGEXP_TYPES(DECLARE_REGEXP_VISITORS)
#undef DECLARE_REGEXP_VISITORS

  inline void Advance(unsigned n_chars) {
    masm_->Advance(n_chars, direction(), string_pointer);
  }

  void TestState(int time, int state_index);
  void SetState(int target_time, int target_index, int current_index);
  // Set target state with the current string_pointer as the match source.
  void SetStateForce(int target_time, int target_index);
  void SetStateForce(int target_time, Register target_index);
  void SetEntryStates(vector<Regexp*> *re_list);

  void RestoreFFFoundStates();

  void DirectionSetOutputFromEntry(int time, Regexp* regexp);

  // Only use if certain that the access will not overflow the ring_state.
  // Typically with time == 0.
  Operand StateOperand(int time, int state_index);
  Operand StateOperand(int time, Register state_index);
  void ComputeStateOperandOffset(Register offset, int time, int index);
  Operand StateOperand(Register offset);


  void ClearTime(int time);
  void ClearAllTimes();
  void ClearStates(Register begin, Register end = no_reg);

  int TimeSummaryBaseOffsetFromFrame();
  Operand TimeSummaryOperand(int time);
  Operand TimeSummary(int offset);

  Operand StateRingBase();
  int StateRingBaseOffsetFromFrame();

  // Accessors.
  MacroAssembler *masm() const { return masm_; }
  RegexpInfo *rinfo() const { return rinfo_; }
  MatchType match_type() const { return match_type_; }
  Direction direction() const { return direction_; }
  int state_ring_time_size() const { return state_ring_time_size_; }
  int state_ring_times() const { return state_ring_times_; }
  int state_ring_size() const { return state_ring_size_; }
  int time_summary_size() const { return time_summary_size_; }

 private:
  MacroAssembler *masm_;
  RegexpInfo *rinfo_;
  MatchType match_type_;
  Direction direction_;

  // The size in bytes of a time of the ring state.
  int state_ring_time_size_;
  // The number of times in the ring state.
  int state_ring_times_;
  // The total size (in bytes) of the ring state.
  int state_ring_size_;
  int time_summary_size_;
  Operand ring_base_;

  Label *fast_forward_;
  Label *unwind_and_return_;
};


// Walks the tree to find what regexps can be used as fast-forward elements.
class FastForwardGen : public PhysicalRegexpVisitor<void> {
 public:
  FastForwardGen(Codegen* codegen, vector<Regexp*>* list,
                 Label *unwind_and_return) :
    codegen_(codegen),
    masm_(codegen->masm()),
    ff_list_(list),
    potential_match_(NULL),
    unwind_and_return_(unwind_and_return) {}

  enum Behaviour {
    // Set the entry states of potential matches and fall through.
    SetStateFallThrough,
    // Simply fall through if any potential match is found.
    FallThrough,
  };
  void Generate(Behaviour on_match_behaviour = SetStateFallThrough);

  void FoundState(int time, int state);
  void PotentialMatches(vector<Regexp*> *regexps) {
    if (behaviour_ == SetStateFallThrough) {
      codegen_->SetEntryStates(regexps);
    } else {
      ASSERT(behaviour_ == FallThrough);
    }
  }
  void PotentialMatch(Regexp *re) {
    if (behaviour_ == SetStateFallThrough) {
      FoundState(0, re->entry_state());
    } else {
      ASSERT(behaviour_ == FallThrough);
    }
  }

#define DECLARE_REGEXP_VISITORS(RegexpType) \
  void Visit##RegexpType(RegexpType* r);
  LIST_PHYSICAL_REGEXP_TYPES(DECLARE_REGEXP_VISITORS)
#undef DECLARE_REGEXP_VISITORS

#define DECLARE_SINGLE_REGEXP_VISITORS(RegexpType) \
  void VisitSingle##RegexpType(RegexpType* r);
  LIST_PHYSICAL_REGEXP_TYPES(DECLARE_SINGLE_REGEXP_VISITORS)
#undef DECLARE_REGEXP_VISITORS

  void VisitSingleStartOrEndOfLine(ControlRegexp* seol);

  void VisitSingle(Regexp* regexp) {
    switch (regexp->type()) {
#define TYPE_CASE(RegexpType)                                             \
      case k##RegexpType:                                                 \
        VisitSingle##RegexpType(reinterpret_cast<RegexpType*>(regexp));   \
        break;
      LIST_PHYSICAL_REGEXP_TYPES(TYPE_CASE)
#undef TYPE_CASE
      default:
        UNREACHABLE();
    }
  }

 private:
  Codegen* codegen_;
  MacroAssembler* masm_;
  vector<Regexp*>* ff_list_;
  Label* potential_match_;
  Label* unwind_and_return_;
  Behaviour behaviour_;

  DISALLOW_COPY_AND_ASSIGN(FastForwardGen);
};


} }  // namespace rejit::internal

