/*
 * PatternLink.cc
 *
 * Copyright (C) 2009, 2014, 2015 Linas Vepstas
 *
 * Author: Linas Vepstas <linasvepstas@gmail.com>  January 2009
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the
 * exceptions at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public
 * License along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <opencog/util/Logger.h>
#include <opencog/util/oc_assert.h>

#include <opencog/atoms/atom_types/NameServer.h>
#include <opencog/atoms/base/Node.h>
#include <opencog/atoms/core/FindUtils.h>
#include <opencog/atoms/core/FreeLink.h>

#include "BindLink.h"
#include "DualLink.h"
#include "PatternLink.h"
#include "PatternUtils.h"

using namespace opencog;

void PatternLink::common_init(void)
{
	locate_defines(_pat.undeclared_clauses);

	// If there are any defines in the pattern, then all bets are off
	// as to whether it is connected or not, what's virtual, what isn't.
	// The analysis will have to be performed at run-time, so we will
	// skip doing it here.
	if (0 < _pat.defined_terms.size())
	{
		_num_virts = 0;
		_num_comps = 1;
		return;
	}

	remove_constants(_variables.varset, _pat);

	// Compute the intersection of literal clauses, and mandatory
	// clauses. This is the set of mandatory clauses that must be
	// present in thier literal form.
	for (const Handle& h : _pat.literal_clauses)
	{
		if (std::find(_pat.mandatory.begin(), _pat.mandatory.end(), h)
			 != _pat.mandatory.end())
		_fixed.push_back(h);
	}

	// Locate the black-box and clear-box clauses.
	unbundle_virtual(_pat.undeclared_clauses);
	_num_virts = _virtual.size();

	// Make sure every variable appears in some mandatory clause.
	HandleSeq all_clauses(_pat.undeclared_clauses);
	all_clauses.insert(all_clauses.end(),
	    _pat.literal_clauses.begin(), _pat.literal_clauses.end());
	validate_variables(_variables.varset, all_clauses);

	// unbundle_virtual does not handle connectives. Here, we assume that
	// we are being run with the TermMatchMixin, and so we assume
	// that the logical connectives are AndLink, OrLink and NotLink.
	// Tweak the evaluatable_holders to reflect this.
	// XXX FIXME; long-term, this should be replaced by a check to
	// see if a link inherits from EvaluatableLink. However, this can
	// only be done after all existing BindLinks have been converted to
	// use PresentLink... so this might not be practical to fix, for a
	// while.
	TypeSet connectives({AND_LINK, SEQUENTIAL_AND_LINK,
	                     OR_LINK, SEQUENTIAL_OR_LINK,
	                     NOT_LINK, TRUE_LINK, FALSE_LINK});

	// Icky. Yuck. Some pre-historic API's do not set the pattern body.
	// These appear only in the unit tests, never in real code. For now,
	// just work around this, but eventually XXX FIXME.
	if (nullptr == _pat.body)
	{
		for (const Handle& term : _pat.undeclared_clauses)
			trace_connectives(connectives, term);
	}
	else
	{
		trace_connectives(connectives, _pat.body);
	}

	add_dummies();

	// Split the non-virtual clauses into connected components
	get_bridged_components(_variables.varset, _fixed, _pat.optionals,
	                       _components, _component_vars);

	// Make sure every variable is in some component.
	check_satisfiability(_variables.varset, _component_vars);

	_num_comps = _components.size();

	// If there is only one connected component, then this can be
	// handled during search by a single PatternLink. The multi-clause
	// grounding mechanism is not required for that case.
	if (1 == _num_comps)
	   make_connectivity_map(_pat.mandatory);

	make_term_trees();

	get_clause_variables(_pat.literal_clauses);
	get_clause_variables(_pat.undeclared_clauses);
	get_clause_variables(_pat.mandatory);

	// Find prunable terms.
	locate_cacheable(all_clauses);
}


/// The second half of the common initialization sequence
void PatternLink::setup_components(void)
{
	if (_num_comps <= 1) return;

	// If we are here, then set up a PatternLink for each connected
	// component.
	_component_patterns.reserve(_num_comps);
	for (size_t i = 0; i < _num_comps; i++)
	{
		Handle h(createPatternLink(_component_vars[i],
		                           _variables,
		                           _components[i],
		                           _pat.optionals));
		_component_patterns.emplace_back(h);
	}
}

void PatternLink::init(void)
{
	_pat.redex_name = "anonymous PatternLink";
	ScopeLink::extract_variables(_outgoing);

	// If the _body has not been initialized by ScopeLink, that's
	// because the PatternLink itself was quoted, and thus not
	// actually initializable. This seems ... weird... to me.
	// I'm not convinced its a valid use of Quoting. It seems
	// like a bug. But whatever. System crashes if the body is
	// not set.
	if (nullptr == _body) return;

	if (2 < _outgoing.size() or
	   (2 == _outgoing.size() and _outgoing[1] != _body))
	{
		throw InvalidParamException(TRACE_INFO,
		      "Expecting (optional) variable decls and a body; got %s",
		      to_string().c_str());
	}

	unbundle_clauses(_body);
	common_init();
	setup_components();

#ifdef QDEBUG
	logger().fine("Pattern: %s", to_long_string("").c_str());
#endif
}

/* ================================================================= */

/// Special constructor used during just-in-time pattern compilation.
///
/// It assumes that the variables have already been correctly extracted
/// from the body, as appropriate.
PatternLink::PatternLink(const Variables& vars, const Handle& body)
	: PrenexLink(HandleSeq(), PATTERN_LINK)
{
	_pat.redex_name = "jit PatternLink";

	_variables = vars;
	_body = body;
	unbundle_clauses(_body);
	common_init();
	setup_components();
}

/* ================================================================= */

/// Special constructor used only to make single concrete pattern
/// components.  We are given the pre-computed components; we only
/// have to store them.
PatternLink::PatternLink(const HandleSet& vars,
                         const Variables& varspec,
                         const HandleSeq& compo,
                         const HandleSeq& opts)
	: PrenexLink(HandleSeq(), PATTERN_LINK)
{
	// First, lets deal with the vars. We have discarded the original
	// order of the variables, and I think that's OK, because we will
	// never have the substitute aka beta-redex aka putlink method
	// called on us, not directly, at least.  If we need it, then the
	// API will need to be changed...
	// So all we need is the varset, and the subset of the typemap.
	_variables.varset = vars;
	_variables.varseq.clear();
	for (const Handle& v : vars)
	{
		_variables.varseq.emplace_back(v);

		auto it = varspec._simple_typemap.find(v);
		if (it != varspec._simple_typemap.end())
			_variables._simple_typemap.insert(*it);

		auto dit = varspec._deep_typemap.find(v);
		if (dit != varspec._deep_typemap.end())
			_variables._deep_typemap.insert(*dit);

		auto fit = varspec._fuzzy_typemap.find(v);
		if (fit != varspec._fuzzy_typemap.end())
			_variables._fuzzy_typemap.insert(*fit);

		auto imit = varspec._glob_intervalmap.find(v);
		if (imit != varspec._glob_intervalmap.end())
			_variables._glob_intervalmap.insert(*imit);
	}

	// Next, the body... there's no `_body` for lambda. The compo is
	// the mandatory clauses; we have to reconstruct the optionals.
	for (const Handle& h : compo)
	{
		auto h_is_in = [&](const Handle& opt) { return is_atom_in_tree(opt, h); };
		auto it = std::find_if(opts.begin(), opts.end(), h_is_in);
		if (it != opts.end())
		{
			_pat.optionals.emplace_back(*it);
		}
		else
		{
			_pat.mandatory.emplace_back(h);
		}
	}
	locate_defines(compo);

	// The rest is easy: the evaluatables and the connection map
	unbundle_virtual(_pat.mandatory);
	_num_virts = _virtual.size();
	OC_ASSERT (0 == _num_virts, "Must not have any virtuals!");

	_components.emplace_back(compo);
	_num_comps = 1;

	make_connectivity_map(_pat.mandatory);
	_pat.redex_name = "Unpacked component of a virtual link";

	make_term_trees();
	get_clause_variables(_pat.mandatory);
	get_clause_variables(_pat.optionals);
}

/* ================================================================= */

/// Constructor that takes a pre-determined set of variables, and
/// a list of clauses to solve.  This is currently kind-of crippled,
/// since no variable type restricions are possible, and no optionals,
/// either.  This is used only for backwards-compatibility API's.
/// XXX No one, except unit tests, use these deprecated API's. These
/// old unit tests should be removed.
PatternLink::PatternLink(const HandleSet& vars,
                         const HandleSeq& clauses)
	: PrenexLink(HandleSeq(), PATTERN_LINK)
{
	_variables.varset = vars;
	_pat.undeclared_clauses = clauses;
	_pat.mandatory = clauses;
	common_init();
	setup_components();
}

/* ================================================================= */

PatternLink::PatternLink(const Handle& body)
	: PrenexLink(HandleSeq({body}), PATTERN_LINK)
{
	init();
}

PatternLink::PatternLink(const Handle& vars, const Handle& body)
	: PrenexLink(HandleSeq({vars, body}), PATTERN_LINK)
{
	init();
}

PatternLink::PatternLink(const HandleSeq&& hseq, Type t)
	: PrenexLink(std::move(hseq), t)
{
	// Type must be as expected
	if (not nameserver().isA(t, PATTERN_LINK))
	{
		const std::string& tname = nameserver().getTypeName(t);
		throw InvalidParamException(TRACE_INFO,
			"Expecting a PatternLink, got %s", tname.c_str());
	}

	// QueryLink, BindLink use a different initialization sequence.
	if (nameserver().isA(t, QUERY_LINK)) return;
	if (DUAL_LINK == t) return;
	init();
}

/* ================================================================= */

/// Make a note of any clauses that must be present (or absent)
/// in the pattern in their literal form, i.e. uninterpreted.
/// Any evaluatable terms appearing in these clauses are NOT evaluated,
/// but are taken as a request to search for and ground these terms in
/// the form they are given, in their literal form, without evaluation.
bool PatternLink::record_literal(const Handle& h, bool reverse)
{
	Type typ = h->get_type();

	// Pull clauses out of a PresentLink
	if ((not reverse and PRESENT_LINK == typ) or
	    (reverse and ABSENT_LINK == typ))
	{
		for (const Handle& ph : h->getOutgoingSet())
		{
			_pat.literal_clauses.emplace_back(ph);
			_pat.mandatory.emplace_back(ph);
		}
		return true;
	}

	// Everything under Choice is either a literal, or a grouping of
	// PresentLinks. They are not mandatory, since they exist only in
	// some of the choice branches, but not others.
	if (not reverse and CHOICE_LINK == typ)
	{
		for (const Handle& ph : h->getOutgoingSet())
		{
			Type pht = ph->get_type();
			if (PRESENT_LINK == pht)
			{
				for (const Handle& php : ph->getOutgoingSet())
					_pat.literal_clauses.emplace_back(php);
				continue;
			}

			if (ABSENT_LINK == pht)
				throw InvalidParamException(TRACE_INFO,
					"AbsentLink under a Choice is not supported yet!");

			_pat.literal_clauses.emplace_back(ph);
		}

// XXX FIXME both statements below are wrong, but they are needed for
// the unit tests. More bu0fxing to straighten this stuff out. That
// is why this code is badly indented!
_pat.literal_clauses.emplace_back(h);
_pat.mandatory.emplace_back(h);
		return true;
	}

	// Pull clauses out of an AbsentLink
	if ((not reverse and ABSENT_LINK == typ) or
	    (reverse and PRESENT_LINK == typ))
	{
		// We insist on an arity of 1, because anything else is
		// ambiguous: consider absent(A B) is that: "both A and B must
		// be absent"?  Or is it "if any of A and B are absent, then .."
		if (1 != h->get_arity())
			throw InvalidParamException(TRACE_INFO,
				"AbsentLink can have an arity of one only!");

		const Handle& inv(h->getOutgoingAtom(0));
		_pat.optionals.emplace_back(inv);
		_pat.literal_clauses.emplace_back(inv);
		return true;
	}

	if (reverse and CHOICE_LINK == typ)
		throw InvalidParamException(TRACE_INFO,
			"NotLink-ChoiceLink is not supported yet!");

	// Pull clauses out of an AlwaysLink
	if (not reverse and ALWAYS_LINK == typ)
	    // or (reverse and NEVER_LINK == typ))
	{
		for (const Handle& ah: h->getOutgoingSet())
		{
			_pat.always.emplace_back(ah);
			_pat.undeclared_clauses.emplace_back(ah);
		}
		return true;
	}

	// Handle in-line variable declarations.
	if (not reverse and TYPED_VARIABLE_LINK == typ)
	{
		// We have to erase first, else it gets duplicated.
		_variables.erase(h->getOutgoingAtom(0));
		_variables.validate_vardecl(h);
		return true;
	}

	return false;
}

/// Unpack the clauses.
///
/// The predicate is either an AndLink of clauses to be satisfied, or a
/// single clause. Other link types, such as OrLink and SequentialAnd,
/// are treated here as single clauses; unpacking them here would lead
/// to confusion in the pattern matcher.  This is partly because, after
/// unpacking, clauses can be grounded in an arbitrary order; thus,
/// SequentialAnd's must be grounded and evaluated sequentially, and
/// thus, not unpacked.  In the case of OrLinks, there is no flag to
/// say that "these are disjoined", so again, that has to happen later.
///
/// This makes built-in assumptions about using the TermMatchMixin,
/// which are not going to be true in general. However, the vast
/// majority of users expect to be able to use the boolean operators
/// in a naive, classical-logic manner, and so we cater to those users.
/// More sophisticated users are SOL and will have to come here and fix
/// things when bugs appear. Sorry.
void PatternLink::unbundle_clauses(const Handle& hbody)
{
	Type t = hbody->get_type();

	// Start by fishing out the PresentLink's, and adding them to the
	// list of clauses to be grounded.
	_pat.body = hbody;
	if (record_literal(hbody))
	{
		/* no-op */
	}
	else if (AND_LINK == t)
	{
		// XXX FIXME Handle of OrLink is incorrect, here.
		TypeSet connectives({AND_LINK, OR_LINK, NOT_LINK});

		const HandleSeq& oset = hbody->getOutgoingSet();

		// De-duplicate repeated clauses in the search pattern.
		HandleSet dedupe;
		for (const Handle& ho : oset)
			dedupe.insert(ho);

		for (const Handle& ho : dedupe)
		{
			if (not record_literal(ho) and
			    not unbundle_clauses_rec(ho, connectives))
			{
				_pat.undeclared_clauses.emplace_back(ho);
				_pat.mandatory.emplace_back(ho);
			}
		}
	}
	else if (SEQUENTIAL_AND_LINK == t or SEQUENTIAL_OR_LINK == t)
	{
		// Just like in trace_connectives, assume we are working with
		// the TermMatchMixin, which uses these. Some other
		// yet-to-be-specified callback may want to use a different
		// set of connectives...
		TypeSet connectives({AND_LINK, SEQUENTIAL_AND_LINK,
		                     OR_LINK, SEQUENTIAL_OR_LINK, NOT_LINK});

		// BUG - XXX FIXME. This extracts PresentLink's from the
		// Sequentials. This is not really correct, because the
		// evaluation of the sequential might terminate *before*
		// the PresentLink is reached. Whereas the current design
		// of the clause-walking will run the PresentLink before
		// running the sequential. So that's a bug.
		unbundle_clauses_rec(hbody, connectives);

		_pat.undeclared_clauses.emplace_back(hbody);
		_pat.mandatory.emplace_back(hbody);
	}

	// Sigh. Handle a top-level OrLink with a single member.
	// This assumes the TermMatchMixin, so its broken
	// for anyone giving alternative interpretations. Yuck.
	else if (OR_LINK == t and 1 == hbody->get_arity())
	{
		// BUG - XXX FIXME Handling of OrLink is incorrect, here.
		// See also FIXME above.
		TypeSet connectives({AND_LINK, OR_LINK, NOT_LINK});
		unbundle_clauses_rec(hbody, connectives);
		if (not unbundle_clauses_rec(hbody, connectives))
		{
			_pat.undeclared_clauses.emplace_back(hbody);
			_pat.mandatory.emplace_back(hbody);
		}
	}

	// A single top-level clause that is a NotLink.
	// This assumes the TermMatchMixin, so its broken
	// for anyone giving alternative interpretations. Yuck.
	else if (NOT_LINK == t)
	{
		// XXX FIXME Handle of OrLink is incorrect, here.
		TypeSet connectives({AND_LINK, OR_LINK, NOT_LINK});
		if (not unbundle_clauses_rec(hbody, connectives))
		{
			_pat.undeclared_clauses.emplace_back(hbody);
			_pat.mandatory.emplace_back(hbody);
		}
	}
	else
	{
		// There's just one single clause!
		_pat.undeclared_clauses.emplace_back(hbody);
		_pat.mandatory.emplace_back(hbody);
	}
}

/// Search for any PRESENT_LINK, ABSENT_LINK and CHOICE_LINK's that are
/// recusively embedded inside some evaluatable clause.  Note these as
/// literal, groundable clauses. `record_literal` does this.
///
/// If there weren't any literal Present, Absent or Choice Links, (i.e.
/// if `record_literal` didn't spot anything) then take some guesses.
/// This guessing is slightly convoluted, but seems to make sense.
/// So:
/// * If a clause is not evaluatable, then assume `Present` was intended.
/// * If it is evaluatable, then assume some later stage will evaluate it.
/// * If it is a variable, then assume something else will ground it, and
///   that some later stage will evaluate it.
/// * If it is an EvalutationLink-PredicateNode combination, then demand
///   that it be Present. A later stage will *also* treat this as
///   evaluatable, and will look at the TV on the EvaluationLink.
/// * Other EvalutationLink styles (e.g. with GPN or DPN or Lambda
///   as the predicate) are evaluatable, and cannot be treated as
///   Present.
///
bool PatternLink::unbundle_clauses_rec(const Handle& bdy,
                                       const TypeSet& connectives,
                                       bool reverse)
{
	Type t = bdy->get_type();

	if (connectives.find(t) == connectives.end())
		return false;

	if (NOT_LINK == t) reverse = not reverse;

	bool recorded = true;
	for (const Handle& ho : bdy->getOutgoingSet())
	{
		if (record_literal(ho, reverse)) continue;

		bool did_rec = unbundle_clauses_rec(ho, connectives, reverse);
		recorded = recorded and did_rec;
		Type ot = ho->get_type();
		if ((not did_rec and
		     not (ot == VARIABLE_NODE) and
		     not nameserver().isA(ot, EVALUATABLE_LINK)) or
		    (ot == EVALUATION_LINK and
		     0 < ho->get_arity() and
		     ho->getOutgoingAtom(0)->get_type() == PREDICATE_NODE))
		{
			_pat.undeclared_clauses.emplace_back(ho);
			_pat.mandatory.emplace_back(ho);
		}
	}
	return recorded;
}

/* ================================================================= */

void PatternLink::locate_defines(const HandleSeq& clauses)
{
	for (const Handle& clause: clauses)
	{
		FindAtoms fdpn(DEFINED_PREDICATE_NODE, DEFINED_SCHEMA_NODE, true);
		fdpn.stopset.insert(SCOPE_LINK);
		fdpn.search_set(clause);

		for (const Handle& sh : fdpn.varset)
		{
			_pat.defined_terms.insert(sh);
		}
	}
}

/* ================================================================= */
/**
 * Locate cacheable clauses. These are clauses whose groundings can be
 * cached for later re-use. To qualify for being cacheable, the clause
 * must not contain any evaluatable terms (as that would result in a
 * changing grounding), cannot contain any unordered or choice links
 * (as these have multiple groundings) and the clause can only contain
 * one variable (there is no use-cases for two or more variables, at
 * this time. Similarly, no Globs either.)
 *
 * Caching is used to "prune" or "cut" clauses during search; once the
 * joining variable is known, the "up" exploration can be skipped, and
 * pruned clause re-attached with the known grounding.
 *
 * This is kind-of the opposite of `is_virtual()`.
 */
void PatternLink::locate_cacheable(const HandleSeq& clauses)
{
	for (const Handle& claw: clauses)
	{
		// Skip over anything unsuitable.
		if (_pat.evaluatable_holders.find(claw) !=
		    _pat.evaluatable_holders.end()) continue;

		if (1 == num_unquoted_unscoped_in_tree(claw, _variables.varset))
		{
			_pat.cacheable_clauses.insert(claw);
			continue;
		}

		// Caching works fine, if there are UnorderedLinks. However,
		// if there is a lot of them, so that the engine is exploring
		// a combinatorially deep arrangement, then caching becomes
		// counter-productive.  Based on running UnorderedUTest, the
		// knee in the curve is at 4 or fewer UnorderedLinks in a clause.
		// Note that UnorderedUTest has some very unusual patterns,
		// exploring many tens of thousands of combinations, something
		// that most ussers will surely almost never do :-)
		if (4 < contains_atomtype_count(claw, UNORDERED_LINK)) continue;

		_pat.cacheable_multi.insert(claw);
	}
}

/* ================================================================= */

/// get_clause_variables -- for every clause, record the variables in it.
/// This is used at runtime, to determine if the clause has been fully
/// grounded (or not).
void PatternLink::get_clause_variables(const HandleSeq& clauses)
{
	for (const Handle& hcl : clauses)
	{
		HandleSet vset = get_free_variables(hcl);

		// Put them into a sequence; any fixed sequence will do.
		HandleSeq vseq;
		for (const Handle& v: vset)
		{
			if (_variables.varset.end() != _variables.varset.find(v))
				vseq.emplace_back(v);
		}

		_pat.clause_variables.insert({hcl, vseq});
	}
}

/* ================================================================= */
/**
 * Make sure that each declared variable appears in some clause.
 * We can't ground variables that don't show up in a clause; there's
 * just no way to know.  Throw, because they are presumably there due
 * to programmer error. Quoted variables are constants, and so don't
 * count.
 */
void PatternLink::validate_variables(HandleSet& vars,
                                     const HandleSeq& clauses)
{
	for (const Handle& v : vars)
	{
		if (not is_unquoted_in_any_tree(clauses, v))
		{
			vars.erase(v);
			throw InvalidParamException(TRACE_INFO,
			   "The variable %s does not appear (unquoted) in any clause!",
			   v->to_short_string().c_str());
		}
	}
}

/* ================================================================= */

/* utility -- every variable in the key term will get the value. */
static void add_to_map(std::unordered_multimap<Handle, Handle>& map,
                       const Handle& key, const Handle& value)
{
	if (key->get_type() == VARIABLE_NODE) map.insert({key, value});
	if (not key->is_link()) return;
	const HandleSeq& oset = key->getOutgoingSet();
	for (const Handle& ho : oset) add_to_map(map, ho, value);
}

/// is_virtual -- check to see if a clause is virtual.
///
/// A clause is virtual if it has two or more unquoted, unscoped
/// variables in it. Otherwise, it can be evaluated on the spot.
///
/// At this time, the pattern matcher does not support mathematical
/// optimzation within virtual clauses.
/// See https://en.wikipedia.org/wiki/Mathematical_optimization
///
/// So, virtual clauses are already one step towards full support
/// for optimization, as they do enable certain kinds of brute-force
/// search across disconnected components. So, there is partial
/// support, for simple kinds of optimization problems. It would
/// take more work and refactoring to support more.  Thus, for now,
/// just throw an error when the more complex optimzation problems
/// are encountered.
///
/// To add support, we would have to split executable clauses into
/// component graphs, the same way we currently split VirtualLinks.
///
bool PatternLink::is_virtual(const Handle& clause)
{
	size_t nfree = num_unquoted_unscoped_in_tree(clause, _variables.varset);
	if (2 > nfree) return false;

	size_t nsub = 0;
	size_t nsolv = 0;
	size_t nvar = 0;
	for (const Handle& sub: clause->getOutgoingSet())
	{
		size_t nv = num_unquoted_unscoped_in_tree(sub, _variables.varset);
		if (0 < nv)
		{
			nsub++;
			if (sub->is_executable()) nsolv++;
			if (VARIABLE_NODE == sub->get_type()) nvar++;
		}
	}
	if (2 <= nsolv or (1 == nsolv and 0 < nvar))
	{
		throw InvalidParamException(TRACE_INFO,
			"This optimization problem currently not supported!");
	}

	return true;
}

/// Sort out the list of clauses into four classes:
/// virtual, evaluatable, executable and concrete.
///
/// A term is "evalutable" if it contains a GroundedPredicateNode,
/// or if it inherits from VirtualLink (such as the GreaterThanLink).
/// Such terms need evaluation at grounding time, to determine
/// their truth values.
///
/// A term may also be evaluatable if consists of connectives (such as
/// AndLink, OrLink, NotLink) used to join together evaluatable terms.
/// Normally, the above search for GPN's and VirtualLinks should be
/// enough, except when an entire term is a variable.  Thus, for
/// example, a term such as (NotLink (VariableNode $x)) needs to be
/// evaluated at grounding-time, even though it does not currently
/// contain a GPN or a VirtualLink: the grounding might contain it;
/// We don't know yet.  However, there's a gotcha: we don't yet know
/// what the connectives are. The actual connectives depend on the
/// callback; the default callback uses AndLink, OrLink, NotLink, but
/// other callbacks may pick something else.  Thus, we cannot do this
/// here. By contrast, GetLink, SatisfactionLink and BindLink
/// explicitly assume the default callback, so they do the additional
/// unbundling there.
///
/// A term is "executable" if it is an ExecutionOutputLink
/// or if it inherits from one (such as PlusLink, TimesLink).
/// Such terms need execution at grounding time, to determine
/// their actual form.  Note that executable terms may frequently
/// occur underneath evaluatable terms, e.g. if something is greater
/// than the sum of two other things.
///
/// A clause is "virtual" if it has an evaluatable term inside of it,
/// and that term has two or more variables in it. Such virtual
/// clauses not only require evaluation, but an evaluation must be
/// performed for each different variable grounding.  Virtual clauses
/// get a very different (and more complex) treatment from the pattern
/// matcher.
///
/// Virtual clauses are hard for the pattern matcher in two different
/// ways: first, any variables in them must be grounded before they can
/// be evaluated, and that grounding has to occur *before* evaluation.
/// Thus, non-virtual clasues must be grounded first. Another problem
/// arises when a virtual clause has two or more variables in it, and
/// those variables are grounded by different disconnected graph
/// components; the combinatoric explosion has to be handled...
///
void PatternLink::unbundle_virtual(const HandleSeq& clauses)
{
	for (const Handle& clause: clauses)
	{
		bool is_virtu = false;
		bool is_black = false;

		// ----------
		FindAtoms fgpn(GROUNDED_PREDICATE_NODE, true);
		fgpn.stopset.insert(SCOPE_LINK);
		fgpn.search_set(clause);
		for (const Handle& sh : fgpn.least_holders)
		{
			_pat.evaluatable_terms.insert(sh);
			add_to_map(_pat.in_evaluatable, sh, sh);
			if (is_virtual(sh))
			{
				is_virtu = true;
				is_black = true;
			}
		}
		for (const Handle& sh : fgpn.holders)
			_pat.evaluatable_holders.insert(sh);

		// ----------
		// One might hope to fish out all EvaluatableLinks, and handle
		// them in just one loop.  Unfortunately, doing this causes
		// multiple unit tests to fail, and so instead, two special
		// cases are broken out: PredicateFormula, below, and
		// VirtualLink, further down.
		//
		// FindAtoms fpfl(EVALUATABLE_LINK, true);
		FindAtoms fpfl(PREDICATE_FORMULA_LINK, true);
		fpfl.search_set(clause);
		for (const Handle& sh : fpfl.least_holders)
		{
			_pat.evaluatable_terms.insert(sh);
			add_to_map(_pat.in_evaluatable, sh, sh);
			if (is_virtual(sh))
				is_virtu = true;
		}
		for (const Handle& sh : fpfl.holders)
			_pat.evaluatable_holders.insert(sh);

		// ----------
		// Subclasses of VirtualLink, e.g. GreaterThanLink, which
		// are essentially a kind of EvaluationLink holding a GPN
		FindAtoms fgtl(VIRTUAL_LINK, true);
		fgtl.search_set(clause);
		// Unlike the above, its varset, not least_holders...
		// because its a link...
		for (const Handle& sh : fgtl.varset)
		{
			_pat.evaluatable_terms.insert(sh);
			_pat.evaluatable_holders.insert(sh);
			add_to_map(_pat.in_evaluatable, sh, sh);

			if (is_virtual(sh)) is_virtu = true;
		}
		for (const Handle& sh : fgtl.holders)
			_pat.evaluatable_holders.insert(sh);

		// For each virtual link, look at it's members. If they
		// are concrete terms, then add them to the _fixed set.
		// For example, `(Equal (Var X) (List (Var A) (Var B)))`
		// the `(List (Var A) (Var B))` must be implcitly present.
		for (const Handle& sh : fgtl.varset)
		{
			if (SATISFACTION_LINK == sh->get_type()) continue;
			for (const Handle& term : sh->getOutgoingSet())
			{
				if (can_evaluate(term)) continue;
				if (not any_unquoted_unscoped_in_tree(term, _variables.varset))
					continue;
				_fixed.emplace_back(term);
			}
		}

		// ----------
		if (is_virtu)
			_virtual.emplace_back(clause);
		else
			_fixed.emplace_back(clause);

		if (is_black)
			_pat.black.insert(clause);
	}
}

/* ================================================================= */

/// Add dummy clauses for patterns that would otherwise not have any
/// non-evaluatable clauses.  One example of such is
///
///    (GetLink (GreaterThan (Number 42) (Variable $x)))
///
/// The only clause here is the GreaterThan, and it is virtual
/// (evaluatable) so we know that in general it cannot be found in
/// the atomspace.   Due to the pattern-matcher design, matching will
/// fail unless there is at least one PresentLink/AbsentLink clause.
/// We can infer, from the above, that the Variable $x must be in the
/// atomspace, so we just add it here, as a dummy clause that can be
/// trivially satisfied.  This can be done generically, without changing
/// search results, for any variable that is not in an AbsentLink.
/// (Always adding can harm performance, so we don't do it unless we
/// absolutely have to.)
///
/// Another example is
///
///    (GetLink (Equal (Variable "$whole") (Implication ...)))
///
/// where the ImplicationLink may itself contain more variables.
/// If the ImplicationLink is suitably simple, it can be added
/// as an ordinary clause, and searched for as if it was "present".
///
/// XXX FIXME: the code here assumes that the situation is indeed
/// simple: more complex cases are not handled correctly.  Doing this
/// correctly would require iterating again, and examining the
/// contents of the left and right side of the EqualLink... ugh.
///
/// XXX The situation here is also very dangerous: without any
/// type onstraints, we risk searching atoms created in the scratch
/// atomspace, resulting in infinite recursion and a blown stack.
/// Not clear how to avoid that...
///
bool PatternLink::add_dummies()
{
	if (0 < _pat.literal_clauses.size()) return false;

	// The below is almost but not quite the same as
	// if (0 < _fixed.size()) return; because fixed can be
	// non-zero, if the virtual term has only one variable
	// in it.
	for (const Handle& cl : _pat.undeclared_clauses)
	{
		// if (0 == _pat.evaluatable_holders.count(cl)) return;
		if (0 == _pat.evaluatable_terms.count(cl)) return false;
	}

	for (const Handle& t : _pat.evaluatable_terms)
	{
		Type tt = t->get_type();
		if (EQUAL_LINK == tt or
		    GREATER_THAN_LINK == tt or
		    IDENTICAL_LINK == tt)
		{
			const Handle& left = t->getOutgoingAtom(0);
			const Handle& right = t->getOutgoingAtom(1);

			for (const Handle& v : _variables.varset)
			{
				if (is_free_in_tree(left, v) or
				    is_free_in_tree(right, v))
				{
					_pat.mandatory.emplace_back(v);
					_fixed.emplace_back(v);
				}
			}
		}
	}

	return true;
}

/* ================================================================= */

/// Starting from the main body, trace down through the tree of (logical)
/// connectives.  If a term appears under a (logic) connective, and there
/// is a path of connectives all the way to the top, then we have to
/// assume the term is evaluatable, as the whole point of connectives
/// to to connect evaluatable terms.  Thus, for example, for a clause
/// having the form (AndLink stuff (OrLink more-stuff (NotLink not-stuff)))
/// we have to assume that stuff, more-stuff and not-stuff are all
/// evaluatable. Tracing halts as soon as something that isn't a
/// connective is encountered.
void PatternLink::trace_connectives(const TypeSet& connectives,
                                    const Handle& term,
                                    Quotation quotation)
{
	Type t = term->get_type();

	quotation.update(t);

	if (quotation.is_quoted()) return;
	if (connectives.find(t) == connectives.end()) return;

	_pat.evaluatable_holders.insert(term);
	add_to_map(_pat.in_evaluatable, term, term);
	for (const Handle& pred: term->getOutgoingSet())
	{
		if (pred->is_link())
			trace_connectives(connectives, pred, quotation);
	}
}

/* ================================================================= */
/**
 * Create a map that holds all of the clauses that a given atom
 * participates in.  In other words, it indicates all the places
 * where an atom is shared by multiple trees, and thus establishes
 * how the trees are connected.
 *
 * This is used for only one purpose: to find the next unsolved
 * clause. Perhaps this could be simplified somehow ...
 */
void PatternLink::make_connectivity_map(const HandleSeq& component)
{
	for (const Handle& h : _pat.mandatory)
		make_map_recursive(h, h);
	for (const Handle& h : _pat.optionals)
		make_map_recursive(h, h);

	// Save some minor amount of space by erasing those atoms that
	// participate in only one clause. These atoms cannot be used
	// to determine connectivity between clauses, and so are un-needed.
	auto it = _pat.connectivity_map.begin();
	auto end = _pat.connectivity_map.end();
	while (it != end)
	{
		if (1 == _pat.connectivity_map.count(it->first))
			it = _pat.connectivity_map.erase(it);
		else
			it++;
	}
}

void PatternLink::make_map_recursive(const Handle& root, const Handle& h)
{
	_pat.connectivity_map.emplace(h, root);

	if (h->is_link())
	{
		for (const Handle& ho: h->getOutgoingSet())
			make_map_recursive(root, ho);
	}
}

/// Make sure that every variable appears in some groundable clause.
/// Variables have to be grounded before an evaluatable clause
/// containing them can be evaluated. Throw an error if some variable
/// wasn't explicitly specified in a groundable clause.
void PatternLink::check_satisfiability(const HandleSet& vars,
                                       const HandleSetSeq& compvars)
{
	// Compute the set-union of all component vars.
	HandleSet vunion;
	for (const HandleSet& vset : compvars)
		vunion.insert(vset.begin(), vset.end());

	// Is every variable in some component? The user can give
	// us mal-formed things like this:
	//    (Bind (Evaluation (GroundedPredicate "scm: foo")
	//               (List (Variable "X") (Variable "Y")))
	//          (Concept "OK"))
	// and we could add `(Variable "X") (Variable "Y")` with the
	// `add_dummies()` method above. But if we did, it would be
	// an info loop that blows out the stack, because X and Y
	// are not constrained, and match everything, including the
	// temorary terms created during search... so we do NOT
	// `add_dummies()` this case. See issue #1420 for more.
	for (const Handle& v : vars)
	{
		const auto& it = vunion.find(v);
		if (vunion.end() == it)
			throw SyntaxException(TRACE_INFO,
				"Poorly-formed query; a variable declaration for %s is needed!",
				v->to_short_string().c_str());
	}
}

void PatternLink::make_term_trees()
{
	for (const Handle& clause : _pat.mandatory)
	{
		PatternTermPtr root_term(createPatternTerm());
		make_term_tree_recursive(clause, clause, root_term);
		root_term->makeLiteral();
	}
	for (const Handle& clause : _pat.optionals)
	{
		PatternTermPtr root_term(createPatternTerm());
		make_term_tree_recursive(clause, clause, root_term);
		root_term->makeLiteral();
	}
	for (const Handle& clause : _pat.always)
	{
		PatternTermPtr root_term(createPatternTerm());
		make_term_tree_recursive(clause, clause, root_term);
		root_term->makeLiteral();
	}
}

void PatternLink::make_term_tree_recursive(const Handle& root,
                                           const Handle& term,
                                           PatternTermPtr& parent)
{
	PatternTermPtr ptm(parent->addOutgoingTerm(term));

	// `h` is usually the same as `term`, unless there's quotation.
	Handle h(ptm->getHandle());
	_pat.connected_terms_map[{h, root}].emplace_back(ptm);

	// If the current node is a bound variable, store this as a
	// bool flag, recursively, up into the parent tree, for later use.
	// The addBoundVariable() method walks upwards into the parent to
	// set this flag.
	Type t = h->get_type();
	if ((VARIABLE_NODE == t or GLOB_NODE == t)
	    and not ptm->getQuotation().is_quoted()
	    and _variables.varset.end() != _variables.varset.find(h))
	{
		ptm->addBoundVariable();
		if (GLOB_NODE == t) ptm->addGlobbyVar();
		return;
	}

	// If the term is unordered, all parents must know about it.
	if (nameserver().isA(t, UNORDERED_LINK))
		ptm->addUnorderedLink();

	if (h->is_link())
	{
		for (const Handle& ho: h->getOutgoingSet())
			make_term_tree_recursive(root, ho, ptm);
	}
}

/* ================================================================= */
/**
 * Check that all clauses are connected
 */
void PatternLink::check_connectivity(const HandleSeqSeq& components)
{
	if (1 == components.size()) return;

	// Users are going to be stumped by this one, so print
	// out a verbose, user-freindly debug message to help
	// them out.
	std::stringstream ss;
	ss << "Pattern is not connected! Found "
	   << components.size() << " components:";
	int cnt = 1;
	for (const auto& comp : components)
	{
		ss << std::endl << "Connected component " << cnt
		   << " consists of ----------------:";
		for (Handle h : comp) ss << std::endl << h->to_string();
		cnt++;
	}
	throw InvalidParamException(TRACE_INFO, "%s", ss.str().c_str());
}

/* ================================================================= */

void PatternLink::debug_log(void) const
{
	if (not logger().is_fine_enabled())
		return;

	// Log the pattern ...
	logger().fine("Pattern '%s' summary:",
	              _pat.redex_name.c_str());
	logger().fine("%lu mandatory clauses", _pat.mandatory.size());
	logger().fine("%lu optionals clauses", _pat.optionals.size());
	logger().fine("%lu always clauses", _pat.always.size());
	logger().fine("%lu fixed clauses", _fixed.size());
	logger().fine("%lu virtual clauses", _num_virts);
	logger().fine("%lu components", _num_comps);
	logger().fine("%lu variables\n", _variables.varset.size());

	int cl = 0;
	for (const Handle& h : _pat.mandatory)
	{
		std::stringstream ss;
		ss << "Mandatory " << cl << ":";
		if (_pat.evaluatable_holders.find(h) != _pat.evaluatable_holders.end())
			ss << " (evaluatable)";
		ss << std::endl;
		ss << h->to_short_string();
		logger().fine() << ss.str();
		cl++;
	}

	if (0 < _pat.optionals.size())
	{
		logger().fine("Pattern has optional clauses:");
		cl = 0;
		for (const Handle& h : _pat.optionals)
		{
			std::stringstream ss;
			ss << "Optional clause " << cl << ":" << std::endl;
			OC_ASSERT(_pat.evaluatable_holders.find(h) ==
			          _pat.evaluatable_holders.end(),
			          "Optional clauses cannot be evaluatable!");
			ss << h->to_short_string();
			logger().fine() << ss.str();
			cl++;
		}
	}
	else
		logger().fine("No optional clauses");

	if (0 < _pat.always.size())
	{
		logger().fine("Pattern has for-all clauses:");
		cl = 0;
		for (const Handle& h : _pat.always)
		{
			std::stringstream ss;
			ss << "Always clause " << cl << ":";
			if (_pat.evaluatable_holders.find(h) != _pat.evaluatable_holders.end())
				ss << " (evaluatable)";
			ss << std::endl;
			ss << h->to_short_string();
			logger().fine() << ss.str();
			cl++;
		}
	}
	else
		logger().fine("No always clauses");

	// Print out the bound variables in the predicate.
	for (const Handle& h : _variables.varset)
	{
		if (h->is_node())
			logger().fine() << "Bound var: " << h->to_short_string();
	}

	if (_variables.varset.empty())
		logger().fine("There are no bound vars in this pattern");
	else
		logger().fine() << "Type declarations are:\n"
		                << oc_to_string(_variables);
}

DEFINE_LINK_FACTORY(PatternLink, PATTERN_LINK)

std::string PatternLink::to_long_string(const std::string& indent) const
{
	std::string indent_p = indent + oc_to_string_indent;
	std::stringstream ss;
	ss << to_string(indent) << std::endl;
	ss << indent << "_pat:" << std::endl
	   << oc_to_string(_pat, indent_p) << std::endl;
	ss << indent << "_fixed:" << std::endl
	   << oc_to_string(_fixed, indent_p) << std::endl;
	ss << indent << "_num_virts = " << _num_virts << std::endl;
	ss << indent << "_virtual:" << std::endl
	   << oc_to_string(_virtual, indent_p) << std::endl;
	ss << indent << "_num_comps = " << _num_comps << std::endl;
	ss << indent << "_components:" << std::endl
	   << oc_to_string(_components, indent_p) << std::endl;
	ss << indent << "_component_vars:" << std::endl
	   << oc_to_string(_component_vars, indent_p) << std::endl;
	ss << indent << "_component_patterns:" << std::endl
	   << oc_to_string(_component_patterns, indent_p);
	return ss.str();
}

std::string oc_to_string(const PatternLink& pl, const std::string& indent)
{
	return pl.to_long_string(indent);
}

/* ===================== END OF FILE ===================== */
