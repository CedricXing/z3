/*++
Copyright (c) 2015 Microsoft Corporation

Module Name:

    theory_seq.h

Abstract:

    Native theory solver for sequences.

Author:

    Nikolaj Bjorner (nbjorner) 2015-6-12

Revision History:

--*/

#include "value_factory.h"
#include "smt_context.h"
#include "smt_model_generator.h"
#include "theory_seq.h"
#include "seq_rewriter.h"

using namespace smt;

void theory_seq::solution_map::update(expr* e, expr* r, enode_pair_dependency* d) {
    std::pair<expr*, enode_pair_dependency*> value;
    if (m_map.find(e, value)) {
        add_trail(DEL, e, value.first, value.second);
    }
    value.first = r;
    value.second = d;
    m_map.insert(e, value);
    add_trail(INS, e, r, d);
}

void theory_seq::solution_map::add_trail(map_update op, expr* l, expr* r, enode_pair_dependency* d) {
    m_updates.push_back(op);
    m_lhs.push_back(l);
    m_rhs.push_back(r);
    m_deps.push_back(d);
}

expr* theory_seq::solution_map::find(expr* e, enode_pair_dependency*& d) {
    std::pair<expr*, enode_pair_dependency*> value;
    d = 0;
    unsigned num_finds = 0;
    expr* result = e;
    while (m_map.find(result, value)) {
        d = m_dm.mk_join(d, value.second);
        result = value.first;
        ++num_finds;
    }
    if (num_finds > 1) { // path compression for original key only.
        update(e, result, d);
    }
    return result;
}

void theory_seq::solution_map::pop_scope(unsigned num_scopes) {
    if (num_scopes == 0) return;    
    unsigned start = m_limit[m_limit.size() - num_scopes];
    for (unsigned i = m_updates.size(); i > start; ) {
        --i;
        if (m_updates[i] == INS) {
            m_map.remove(m_lhs[i].get());
        }
        else {
            m_map.insert(m_lhs[i].get(), std::make_pair(m_rhs[i].get(), m_deps[i]));
        }
    }
    m_updates.resize(start);
    m_lhs.resize(start);
    m_rhs.resize(start);
    m_deps.resize(start);
    m_limit.resize(m_limit.size() - num_scopes);
}

void theory_seq::solution_map::display(std::ostream& out) const {
    map_t::iterator it = m_map.begin(), end = m_map.end();
    for (; it != end; ++it) {
        out << mk_pp(it->m_key, m) << " |-> " << mk_pp(it->m_value.first, m) << "\n";
    }
}

void theory_seq::exclusion_table::update(expr* e, expr* r) {
    if (e->get_id() > r->get_id()) {
        std::swap(e, r);
    }
    if (e != r && !m_table.contains(std::make_pair(e, r))) {
        m_lhs.push_back(e);
        m_rhs.push_back(r);
        m_table.insert(std::make_pair(e, r));
    }
}

void theory_seq::exclusion_table::pop_scope(unsigned num_scopes) {
    if (num_scopes == 0) return;
    unsigned start = m_limit[m_limit.size() - num_scopes];
    for (unsigned i = start; i < m_lhs.size(); ++i) {
        m_table.erase(std::make_pair(m_lhs[i].get(), m_rhs[i].get()));
    }
    m_lhs.resize(start);
    m_rhs.resize(start);
    m_limit.resize(m_limit.size() - num_scopes);
}

void theory_seq::exclusion_table::display(std::ostream& out) const {
    table_t::iterator it = m_table.begin(), end = m_table.end();
    for (; it != end; ++it) {
        out << mk_pp(it->first, m) << " != " << mk_pp(it->second, m) << "\n";
    }
}

theory_seq::theory_seq(ast_manager& m):
    theory(m.mk_family_id("seq")), 
    m(m),
    m_dam(m_dep_array_value_manager, m_alloc),
    m_rep(m, m_dm),    
    m_ineqs(m),
    m_exclude(m),
    m_axioms(m),
    m_axioms_head(0),
    m_branch_variable_head(0),
    m_incomplete(false), 
    m_model_completion(false),
    m_rewrite(m), 
    m_util(m),
    m_autil(m),
    m_trail_stack(*this) {
    m_lhs.push_back(expr_array());
    m_rhs.push_back(expr_array());
    m_deps.push_back(enode_pair_dependency_array());
    m_prefix_sym = "prefix";
    m_suffix_sym = "suffix";
    m_left_sym = "left";
    m_right_sym = "right";
    m_contains_left_sym = "contains_left";
    m_contains_right_sym = "contains_right";
}

theory_seq::~theory_seq() {
    unsigned num_scopes = m_lhs.size()-1;
    if (num_scopes > 0) pop_scope_eh(num_scopes);
    m.del(m_lhs.back());
    m.del(m_rhs.back());
    m_dam.del(m_deps.back());
}


final_check_status theory_seq::final_check_eh() { 
    context & ctx   = get_context();
    TRACE("seq", display(tout););
    if (!check_ineqs()) {
        return FC_CONTINUE;
    }
    if (simplify_and_solve_eqs()) {
        return FC_CONTINUE;
    }
    if (ctx.inconsistent()) {
        return FC_CONTINUE;
    }
    if (branch_variable()) {
        return FC_CONTINUE;
    }
    if (split_variable()) {
        return FC_CONTINUE;
    }
    if (ctx.inconsistent()) {
        return FC_CONTINUE;
    }
    if (m.size(m_lhs.back()) > 0 || m_incomplete) {
        return FC_GIVEUP;        
    }
    return FC_DONE; 
}

bool theory_seq::check_ineqs() {
    context & ctx   = get_context();
    for (unsigned i = 0; i < m_ineqs.size(); ++i) {
        expr* a = m_ineqs[i].get();
        enode_pair_dependency* eqs = 0;
        expr_ref b = canonize(a, eqs);
        if (m.is_true(b)) {
            TRACE("seq", tout << "Evaluates to false: " << mk_pp(a,m) << "\n";);
            ctx.internalize(a, false);
            propagate_lit(eqs, ctx.get_literal(a));
            return false;
        }
    }
    return true;
}

bool theory_seq::branch_variable() {
    context& ctx = get_context();
    TRACE("seq", ctx.display(tout););
    expr_array& lhs = m_lhs.back();
    expr_array& rhs = m_rhs.back();
    unsigned sz = m.size(lhs);
    ptr_vector<expr> ls, rs;
    for (unsigned i = 0; i < sz; ++i) {
        unsigned k = (i + m_branch_variable_head) % sz;
        expr* l = m.get(lhs, k);
        expr* r = m.get(rhs, k);
        TRACE("seq", tout << mk_pp(l, m) << " = " << mk_pp(r, m) << "\n";);
        ls.reset(); rs.reset();
        m_util.str.get_concat(l, ls);
        m_util.str.get_concat(r, rs);
        
        if (!ls.empty() && find_branch_candidate(ls[0], rs)) {
            m_branch_variable_head = k;
            return true;
        }
        if (!rs.empty() && find_branch_candidate(rs[0], ls)) {
            m_branch_variable_head = k;
            return true;
        }
    }
    return false;
}

bool theory_seq::find_branch_candidate(expr* l, ptr_vector<expr> const& rs) {

    TRACE("seq", tout << mk_pp(l, m) << " " 
          << (is_var(l)?"var":"not var") << "\n";);

    if (!is_var(l)) {
        return false;
    }

    expr_ref v0(m), v(m);
    v0 = m_util.str.mk_empty(m.get_sort(l));
    if (assume_equality(l, v0)) {
        return true;
    }
    for (unsigned j = 0; j < rs.size(); ++j) {
        if (occurs(l, rs[j])) {
            return false;
        }
        std::string s;
        if (m_util.str.is_string(rs[j], s)) {
            for (size_t k = 1; k < s.length(); ++k) {
                v = m_util.str.mk_string(std::string(s.c_str(), k));
                if (v0) v = m_util.str.mk_concat(v0, v);
                if (assume_equality(l, v)) {
                    return true;
                }
            }
        }
        v0 = (j == 0)? rs[0] : m_util.str.mk_concat(v0, rs[j]); 
        if (assume_equality(l, v0)) {
            return true;
        }
    }           
    return false;
}

bool theory_seq::assume_equality(expr* l, expr* r) {
    TRACE("seq", tout << mk_pp(l, m) << " = " << mk_pp(r, m) << "\n";);
    context& ctx = get_context();
    if (m_exclude.contains(l, r)) {
        return false;
    }
    else {
        SASSERT(ctx.e_internalized(l));
        if (!ctx.e_internalized(r)) ctx.internalize(r, false);
        enode* n1 = ctx.get_enode(l);
        enode* n2 = ctx.get_enode(r);
        ctx.assume_eq(n1, n2);        
    }
    return true;
}

bool theory_seq::split_variable() {

    return false;
}

void theory_seq::propagate_lit(enode_pair_dependency* dep, literal lit) {
    context& ctx = get_context();
    ctx.mark_as_relevant(lit);
    vector<enode_pair, false> _eqs;
    m_dm.linearize(dep, _eqs);
    TRACE("seq", ctx.display_detailed_literal(tout, lit); 
          tout << " <-\n"; display_deps(tout, dep);); 
    justification* js = 
        ctx.mk_justification(
            ext_theory_propagation_justification(
                get_id(), ctx.get_region(), 0, 0, _eqs.size(), _eqs.c_ptr(), lit));

    ctx.assign(lit, js);
}

void theory_seq::set_conflict(enode_pair_dependency* dep) {
    context& ctx = get_context();
    vector<enode_pair, false> _eqs;
    m_dm.linearize(dep, _eqs);
    TRACE("seq", display_deps(tout, dep);); 
    ctx.set_conflict(
        ctx.mk_justification(
            ext_theory_conflict_justification(
                get_id(), ctx.get_region(), 0, 0, _eqs.size(), _eqs.c_ptr(), 0, 0)));
}

void theory_seq::propagate_eq(enode_pair_dependency* dep, enode* n1, enode* n2) {
    context& ctx = get_context();
    vector<enode_pair, false> _eqs;
    m_dm.linearize(dep, _eqs);
    TRACE("seq",
          tout << mk_pp(n1->get_owner(), m) << " " << mk_pp(n2->get_owner(), m) << " <- ";
          display_deps(tout, dep);
          ); 
    
    justification* js = ctx.mk_justification(
        ext_theory_eq_propagation_justification(
            get_id(), ctx.get_region(), 0, 0, _eqs.size(), _eqs.c_ptr(), n1, n2));
    ctx.assign_eq(n1, n2, eq_justification(js));
}



bool theory_seq::simplify_eq(expr* l, expr* r, enode_pair_dependency* deps) {
    context& ctx = get_context();
    seq_rewriter rw(m);
    expr_ref_vector lhs(m), rhs(m);
    expr_ref lh = canonize(l, deps);
    expr_ref rh = canonize(r, deps);
    if (!rw.reduce_eq(lh, rh, lhs, rhs)) {
        // equality is inconsistent.
        TRACE("seq", tout << lh << " != " << rh << "\n";);
        set_conflict(deps);
        return true;
    }
    if (lhs.size() == 1 && l == lhs[0].get() &&
        rhs.size() == 1 && r == rhs[0].get()) {
        return false;
    }
    SASSERT(lhs.size() == rhs.size());
    for (unsigned i = 0; i < lhs.size(); ++i) {
        m.push_back(m_lhs.back(), lhs[i].get());
        m.push_back(m_rhs.back(), rhs[i].get());
        m_dam.push_back(m_deps.back(), deps);
    }    
    TRACE("seq",
          tout << mk_pp(l, m) << " = " << mk_pp(r, m) << " => ";
          for (unsigned i = 0; i < lhs.size(); ++i) {
              tout << mk_pp(lhs[i].get(), m) << " = " << mk_pp(rhs[i].get(), m) << "; ";
          }
          tout << "\n";
          );
    return true;
}

bool theory_seq::solve_unit_eq(expr* l, expr* r, enode_pair_dependency* deps) {
    expr_ref lh = canonize(l, deps);
    expr_ref rh = canonize(r, deps);
    if (lh == rh) {
        return true;
    }
    if (is_var(lh) && !occurs(lh, rh)) {
        add_solution(lh, rh, deps);
        return true;
    }
    if (is_var(rh) && !occurs(rh, lh)) {
        add_solution(rh, lh, deps);
        return true;
    }
    // Use instead reference counts for dependencies to GC?

    // TBD: Solutions to units are not necessarily variables, but
    // they may induce new equations.

    return false;
}

bool theory_seq::occurs(expr* a, expr* b) {
    // true if a occurs under an interpreted function or under left/right selector.    
    SASSERT(is_var(a));
    expr* e1, *e2;
    while (is_left_select(a, e1) || is_right_select(a, e1)) {
        a = e1;
    }
    if (m_util.str.is_concat(b, e1, e2)) {
        return occurs(a, e1) || occurs(a, e2);
    }
    while (is_left_select(b, e1) || is_right_select(b, e1)) {
        b = e1;
    }          
    if (a == b) {
        return true;
    }  
    return false;
}

bool theory_seq::is_var(expr* a) {
    return is_uninterp(a) || m_util.is_skolem(a);
}

bool theory_seq::is_left_select(expr* a, expr*& b) {
    return m_util.is_skolem(a) && 
        to_app(a)->get_decl()->get_parameter(0).get_symbol() == m_left_sym && (b = to_app(a)->get_arg(0), true);
}

bool theory_seq::is_right_select(expr* a, expr*& b) {
    return m_util.is_skolem(a) &&
        to_app(a)->get_decl()->get_parameter(0).get_symbol() == m_right_sym && (b = to_app(a)->get_arg(0), true);
}


void theory_seq::add_solution(expr* l, expr* r, enode_pair_dependency* deps) {
    context& ctx = get_context();
    m_rep.update(l, r, deps);
    // TBD: skip new equalities for non-internalized terms.
    if (ctx.e_internalized(l) && ctx.e_internalized(r)) {
        enode* n1 = ctx.get_enode(l);
        enode* n2 = ctx.get_enode(r);
        propagate_eq(deps, n1, n2);
    }
}

bool theory_seq::simplify_eqs() {
    return pre_process_eqs(true);
}

bool theory_seq::solve_basic_eqs() {
    return pre_process_eqs(false);
}

bool theory_seq::pre_process_eqs(bool simplify_or_solve) {
    context& ctx = get_context();
    bool change = false;
    expr_array& lhs = m_lhs.back();
    expr_array& rhs = m_rhs.back();
    enode_pair_dependency_array& deps = m_deps.back();
    for (unsigned i = 0; !ctx.inconsistent() && i < m.size(lhs); ++i) {
        if (simplify_or_solve?
            simplify_eq(m.get(lhs, i), m.get(rhs, i), m_dam.get(deps, i)):
            solve_unit_eq(m.get(lhs, i), m.get(rhs, i), m_dam.get(deps, i))) {  
            if (i + 1 != m.size(lhs)) {
                m.set(lhs, i, m.get(lhs, m.size(lhs)-1));
                m.set(rhs, i, m.get(rhs, m.size(rhs)-1));
                m_dam.set(deps, i, m_dam.get(deps, m_dam.size(deps)-1));
                --i;
                ++m_stats.m_num_reductions;
            }
            m.pop_back(lhs);
            m.pop_back(rhs);
            m_dam.pop_back(deps);
            change = true;
        }
    }    
    return change;
}

bool theory_seq::simplify_and_solve_eqs() {
    context & ctx   = get_context();
    bool change = simplify_eqs();
    while (!ctx.inconsistent() && solve_basic_eqs()) {
        simplify_eqs();
        change = true;
    }
    return change;
}



bool theory_seq::internalize_atom(app* a, bool) { 
    return internalize_term(a);
}

bool theory_seq::internalize_term(app* term) { 
    TRACE("seq", tout << mk_pp(term, m) << "\n";);
    context & ctx   = get_context();
    unsigned num_args = term->get_num_args();
    for (unsigned i = 0; i < num_args; i++) {
        expr* arg = term->get_arg(i);
        ctx.internalize(arg, false);
        if (ctx.e_internalized(arg)) {
            mk_var(ctx.get_enode(arg));
        }
    }
    enode* e = 0;
    if (ctx.e_internalized(term)) {
        e = ctx.get_enode(term);
    }
    if (m.is_bool(term)) {
        bool_var bv = ctx.mk_bool_var(term);
        ctx.set_var_theory(bv, get_id());
        ctx.set_enode_flag(bv, true);
    }
    else {
        if (!e) {
            e = ctx.mk_enode(term, false, m.is_bool(term), true);
        } 
        mk_var(e);
    }
    if (!m_util.str.is_concat(term) &&
        !m_util.str.is_string(term) &&
        !m_util.str.is_empty(term)  && 
        !m_util.str.is_unit(term)   &&
        !m_util.str.is_suffix(term) &&
        !m_util.str.is_prefix(term) &&
        !m_util.str.is_contains(term) &&
        !m_util.is_skolem(term)) {
        set_incomplete(term);
    }
    return true;
}

void theory_seq::apply_sort_cnstr(enode* n, sort* s) {
    mk_var(n);    
}

void theory_seq::display(std::ostream & out) const {
    display_equations(out);
    if (!m_ineqs.empty()) {
        out << "Negative constraints:\n";
        for (unsigned i = 0; i < m_ineqs.size(); ++i) {
            out << mk_pp(m_ineqs[i], m) << "\n";
        }
    }
    out << "Solved equations:\n";
    m_rep.display(out);
    m_exclude.display(out);
}

void theory_seq::display_equations(std::ostream& out) const {
    expr_array const& lhs = m_lhs.back();
    expr_array const& rhs = m_rhs.back();
    enode_pair_dependency_array const& deps = m_deps.back();
    if (m.size(lhs) == 0) {
        return;
    }
    out << "Equations:\n";
    for (unsigned i = 0; i < m.size(lhs); ++i) {
        out << mk_pp(m.get(lhs, i), m) << " = " << mk_pp(m.get(rhs, i), m) << " <-\n";
        display_deps(out, m_dam.get(deps, i));
    }       
}

void theory_seq::display_deps(std::ostream& out, enode_pair_dependency* dep) const {
    if (!dep) return;
    vector<enode_pair, false> _eqs;
    const_cast<enode_pair_dependency_manager&>(m_dm).linearize(dep, _eqs);
    for (unsigned i = 0; i < _eqs.size(); ++i) {
        out << " " << mk_pp(_eqs[i].first->get_owner(), m) << " = " << mk_pp(_eqs[i].second->get_owner(), m) << "\n";
    }
}

void theory_seq::collect_statistics(::statistics & st) const {
    st.update("seq num splits", m_stats.m_num_splits);
    st.update("seq num reductions", m_stats.m_num_reductions);
}

void theory_seq::init_model(model_generator & mg) {
    m_factory = alloc(seq_factory, get_manager(), 
                      get_family_id(), mg.get_model());
    mg.register_factory(m_factory);
}

model_value_proc * theory_seq::mk_value(enode * n, model_generator & mg) {
    enode_pair_dependency* deps = 0;
    expr_ref e(n->get_owner(), m);
    flet<bool> _model_completion(m_model_completion, true);
    e = canonize(e, deps);
    SASSERT(is_app(e));
    m_factory->add_trail(e);
    return alloc(expr_wrapper_proc, to_app(e));
}



void theory_seq::set_incomplete(app* term) {
    TRACE("seq", tout << "No support for: " << mk_pp(term, m) << "\n";);
    if (!m_incomplete) { 
        m_trail_stack.push(value_trail<theory_seq,bool>(m_incomplete)); 
        m_incomplete = true; 
    } 
}

theory_var theory_seq::mk_var(enode* n) {
    if (is_attached_to_var(n)) {
        return n->get_th_var(get_id());
    }
    else {
        theory_var v = theory::mk_var(n);
        get_context().attach_th_var(n, this, v);
        return v;
    }
}

bool theory_seq::can_propagate() {
    return m_axioms_head < m_axioms.size();
}

expr_ref theory_seq::canonize(expr* e, enode_pair_dependency*& eqs) {
    expr_ref result = expand(e, eqs);
    m_rewrite(result);
    return result;
}

expr_ref theory_seq::expand(expr* e, enode_pair_dependency*& eqs) {
    enode_pair_dependency* deps = 0;
    e = m_rep.find(e, deps);
    expr* e1, *e2;
    eqs = m_dm.mk_join(eqs, deps);
    if (m_util.str.is_concat(e, e1, e2)) {
        return expr_ref(m_util.str.mk_concat(expand(e1, eqs), expand(e2, eqs)), m);
    }        
    if (m_util.str.is_empty(e) || m_util.str.is_string(e)) {
        return expr_ref(e, m);
    }
    if (m.is_eq(e, e1, e2)) {
        return expr_ref(m.mk_eq(expand(e1, eqs), expand(e2, eqs)), m);
    }
    if (m_util.str.is_prefix(e, e1, e2)) {
        return expr_ref(m_util.str.mk_prefix(expand(e1, eqs), expand(e2, eqs)), m);
    }
    if (m_util.str.is_suffix(e, e1, e2)) {
        return expr_ref(m_util.str.mk_suffix(expand(e1, eqs), expand(e2, eqs)), m);
    }
    if (m_util.str.is_contains(e, e1, e2)) {
        return expr_ref(m_util.str.mk_contains(expand(e1, eqs), expand(e2, eqs)), m);
    }
    if (m_model_completion && is_var(e)) {
        SASSERT(m_factory);
        expr_ref val(m);
        val = m_factory->get_fresh_value(m.get_sort(e));
        if (val) {
            m_rep.update(e, val, 0);
            return val;
        }
    }
    return expr_ref(e, m);
}

void theory_seq::add_dependency(enode_pair_dependency*& dep, enode* a, enode* b) {
    if (a != b) {
        dep = m_dm.mk_join(dep, m_dm.mk_leaf(std::make_pair(a, b)));
    }
}


void theory_seq::propagate() {
    context & ctx = get_context();
    while (m_axioms_head < m_axioms.size() && !ctx.inconsistent()) {
        expr_ref e(m);
        e = m_axioms[m_axioms_head].get();
        assert_axiom(e);
        ++m_axioms_head;
    }
}

void theory_seq::create_axiom(expr_ref& e) {
    m_trail_stack.push(push_back_vector<theory_seq, expr_ref_vector>(m_axioms));
    m_axioms.push_back(e);
}

/*
  encode that s is not a proper prefix of xs
*/
expr_ref theory_seq::tightest_prefix(expr* s, expr* x) {
    expr_ref s1 = mk_skolem(symbol("first"), s);
    expr_ref c  = mk_skolem(symbol("last"),  s);
    expr_ref fml(m);

    fml = m.mk_and(m.mk_eq(s, m_util.str.mk_concat(s1, c)),
                   m.mk_eq(m_util.str.mk_length(c), m_autil.mk_int(1)),
                   m.mk_not(m_util.str.mk_contains(s, m_util.str.mk_concat(x, s1))));

    return fml;
}


void theory_seq::new_eq_len_concat(enode* n1, enode* n2) {
    context& ctx = get_context();
    // TBD: walk use list of n1 for concat, walk use-list of n2 for length.
    // instantiate length distributes over concatenation axiom.
    SASSERT(n1->get_root() != n2->get_root());
    if (!m_util.is_seq(n1->get_owner())) {
        return;
    }
    func_decl* f_len = 0;

    // TBD: extract length function for sort if it is used.
    // TBD: add filter for already processed length equivalence classes
    if (!f_len) {
        return;
    }
    enode_vector::const_iterator it = ctx.begin_enodes_of(f_len);
    enode_vector::const_iterator end = ctx.end_enodes_of(f_len);
    bool has_concat = true;
    for (; has_concat && it != end; ++it) {
        if ((*it)->get_root() == n1->get_root()) {
            enode* start2 = n2;
            do {
                if (m_util.str.is_concat(n2->get_owner())) {
                    has_concat = true;
                    add_len_concat_axiom(n2->get_owner());
                }
                n2 = n2->get_next();
            }
            while (n2 != start2);
        }
    }
}

void theory_seq::add_len_concat_axiom(expr* c) {
    // or just internalize lc and have relevancy propagation create axiom?
    expr *a, *b;
    expr_ref la(m), lb(m), lc(m), fml(m);
    VERIFY(m_util.str.is_concat(c, a, b));
    la = m_util.str.mk_length(a);
    lb = m_util.str.mk_length(b);
    lc = m_util.str.mk_length(c);
    fml = m.mk_eq(m_autil.mk_add(la, lb), lc);
    create_axiom(fml);
}

/*
  let i = Index(s, t)

  (!contains(s, t) -> i = -1)
  (contains(s, t) & s = empty -> i = 0)
  (contains(s, t) & s != empty -> t = xsy & tightest_prefix(s, x))

  optional lemmas:
  (len(s) > len(t)  -> i = -1) 
  (len(s) <= len(t) -> i <= len(t)-len(s))  
*/
void theory_seq::add_indexof_axiom(expr* i) {
    expr* s, *t;
    VERIFY(m_util.str.is_index(i, s, t));
    expr_ref cnt(m), fml(m), eq_empty(m);
    expr_ref x  = mk_skolem(m_contains_left_sym, s, t);
    expr_ref y  = mk_skolem(m_contains_right_sym, s, t);    
    eq_empty = m.mk_eq(s, m_util.str.mk_empty(m.get_sort(s)));
    cnt = m_util.str.mk_contains(s, t);
    fml = m.mk_or(cnt, m.mk_eq(i, m_autil.mk_int(-1)));
    create_axiom(fml);
    fml = m.mk_or(m.mk_not(cnt), m.mk_not(eq_empty), m.mk_eq(i, m_autil.mk_int(0)));
    create_axiom(fml);
    fml = m.mk_or(m.mk_not(cnt), eq_empty, m.mk_eq(t, m_util.str.mk_concat(x,s,y)));
    create_axiom(fml);
    fml = m.mk_or(m.mk_not(cnt), eq_empty, tightest_prefix(s, x));
    create_axiom(fml);
}

/*
  let r = replace(a, s, t)
  
  (contains(s, a) -> r = xty & a = xsy & tightest_prefix(s,xs)) & 
  (!contains(s, a) -> r = a)
   
*/
void theory_seq::add_replace_axiom(expr* r) {
    expr* a, *s, *t;
    VERIFY(m_util.str.is_replace(r, a, s, t));
    expr_ref cnt(m), fml(m);
    cnt = m_util.str.mk_contains(s, a);
    expr_ref x  = mk_skolem(m_contains_left_sym, s, a);
    expr_ref y  = mk_skolem(m_contains_right_sym, s, a);    
    fml = m.mk_or(m.mk_not(cnt), m.mk_eq(a, m_util.str.mk_concat(x, s, y)));
    create_axiom(fml);
    fml = m.mk_or(m.mk_not(cnt), m.mk_eq(r, m_util.str.mk_concat(x, t, y)));
    create_axiom(fml);
    fml = m.mk_or(m.mk_not(cnt), tightest_prefix(s, x));
    create_axiom(fml);
    fml = m.mk_or(cnt, m.mk_eq(r, a));
    create_axiom(fml);
}

/*
    let n = len(x)

    len(x) >= 0
    len(x) = 0 => x = ""
    x = "" => len(x) = 0
    len(x) = rewrite(len(x))
 */
void theory_seq::add_len_axiom(expr* n) {
    expr* x;
    VERIFY(m_util.str.is_length(n, x));
    expr_ref fml(m), eq1(m), eq2(m), nr(m);
    eq1 = m.mk_eq(m_autil.mk_int(0), n);
    eq2 = m.mk_eq(x, m_util.str.mk_empty(m.get_sort(x)));
    fml = m_autil.mk_le(m_autil.mk_int(0), n);
    create_axiom(fml);
    fml = m.mk_or(m.mk_not(eq1), eq2);
    create_axiom(fml);
    fml = m.mk_or(m.mk_not(eq2), eq1);
    create_axiom(fml);
    nr = n;
    m_rewrite(nr);
    if (nr != n) {
        fml = m.mk_eq(n, nr);
        create_axiom(fml);
    }
}

/*
  check semantics of extract.

  let e = extract(s, i, l)

  0 <= i < len(s) -> prefix(xe,s) & len(x) = i
  0 <= i < len(s) & l >= len(s) - i -> len(e) = len(s) - i
  0 <= i < len(s) & 0 <= l < len(s) - i -> len(e) = l
  0 <= i < len(s) & l < 0 -> len(e) = 0
  i < 0 -> e = s
  i >= len(s) -> e = empty
*/

void theory_seq::add_extract_axiom(expr* e) {
    expr* s, *i, *j;
    VERIFY(m_util.str.is_extract(e, s, i, j));
    expr_ref i_ge_0(m), i_le_j(m), j_lt_s(m);

    NOT_IMPLEMENTED_YET();
    
}

void theory_seq::assert_axiom(expr_ref& e) {
    context & ctx = get_context();
    if (m.is_true(e)) return;
    TRACE("seq", tout << "asserting " << e << "\n";);
    ctx.internalize(e, false);
    literal lit(ctx.get_literal(e));
    ctx.mark_as_relevant(lit);
    ctx.mk_th_axiom(get_id(), 1, &lit);
}

expr_ref theory_seq::mk_skolem(symbol const& name, expr* e1, expr* e2) {
    expr* es[2] = { e1, e2 };
    return expr_ref(m_util.mk_skolem(name, e2?2:1, es, m.get_sort(e1)), m);
}

void theory_seq::propagate_eq(bool_var v, expr* e1, expr* e2) {
    context& ctx = get_context();
    TRACE("seq", 
          tout << mk_pp(ctx.bool_var2enode(v)->get_owner(), m) << " => " 
          << mk_pp(e1, m) << " = " << mk_pp(e2, m) << "\n";); 

    ctx.internalize(e1, false);
    SASSERT(ctx.e_internalized(e2));
    enode* n1 = ctx.get_enode(e1);
    enode* n2 = ctx.get_enode(e2);
    literal lit(v);
    justification* js = 
        ctx.mk_justification(
            ext_theory_eq_propagation_justification(
                get_id(), ctx.get_region(), 1, &lit, 0, 0, n1, n2));

    ctx.assign_eq(n1, n2, eq_justification(js));
}

void theory_seq::assign_eq(bool_var v, bool is_true) {
    context & ctx = get_context();
    enode* n = ctx.bool_var2enode(v);
    app*  e = n->get_owner();
    if (is_true) {
        expr* e1, *e2;
        expr_ref f(m);
        if (m_util.str.is_prefix(e, e1, e2)) {
            f = mk_skolem(m_prefix_sym, e1, e2);
            f = m_util.str.mk_concat(e1, f);
            propagate_eq(v, f, e2);
        }
        else if (m_util.str.is_suffix(e, e1, e2)) {
            f = mk_skolem(m_suffix_sym, e1, e2);
            f = m_util.str.mk_concat(f, e1);
            propagate_eq(v, f, e2);
        }
        else if (m_util.str.is_contains(e, e1, e2)) {
            expr_ref f1 = mk_skolem(m_contains_left_sym, e1, e2);
            expr_ref f2 = mk_skolem(m_contains_right_sym, e1, e2);
            f = m_util.str.mk_concat(m_util.str.mk_concat(f1, e1), f2);
            propagate_eq(v, f, e2);
        }
        else if (m_util.str.is_in_re(e, e1, e2)) {
            NOT_IMPLEMENTED_YET();
        }
        else {
            UNREACHABLE();
        }
    }
    else {
        m_trail_stack.push(push_back_vector<theory_seq, expr_ref_vector>(m_ineqs));
        m_ineqs.push_back(e);
    }
}

void theory_seq::new_eq_eh(theory_var v1, theory_var v2) { 
    enode* n1 = get_enode(v1);
    enode* n2 = get_enode(v2);
    if (n1 != n2) {
        m.push_back(m_lhs.back(), n1->get_owner());
        m.push_back(m_rhs.back(), n2->get_owner());
        m_dam.push_back(m_deps.back(), m_dm.mk_leaf(enode_pair(n1, n2)));

        new_eq_len_concat(n1, n2);
        new_eq_len_concat(n2, n1);
    }
}

void theory_seq::new_diseq_eh(theory_var v1, theory_var v2) {
    expr* e1 = get_enode(v1)->get_owner();
    expr* e2 = get_enode(v2)->get_owner();
    m_trail_stack.push(push_back_vector<theory_seq, expr_ref_vector>(m_ineqs));
    m_ineqs.push_back(mk_eq_atom(e1, e2));
    m_exclude.update(e1, e2);
}

void theory_seq::push_scope_eh() {
    TRACE("seq", tout << "push " << m_lhs.size() << "\n";);
    theory::push_scope_eh();
    m_rep.push_scope();
    m_exclude.push_scope();
    m_dm.push_scope();
    m_trail_stack.push_scope();
    m_trail_stack.push(value_trail<theory_seq, unsigned>(m_axioms_head));
    expr_array lhs, rhs;
    enode_pair_dependency_array deps;
    m.copy(m_lhs.back(), lhs);
    m.copy(m_rhs.back(), rhs);
    m_dam.copy(m_deps.back(), deps);
    m_lhs.push_back(lhs);
    m_rhs.push_back(rhs);
    m_deps.push_back(deps);
}

void theory_seq::pop_scope_eh(unsigned num_scopes) {
    TRACE("seq", tout << "pop " << m_lhs.size() << "\n";);
    m_trail_stack.pop_scope(num_scopes);
    theory::pop_scope_eh(num_scopes);   
    m_dm.pop_scope(num_scopes); 
    m_rep.pop_scope(num_scopes);
    m_exclude.pop_scope(num_scopes);
    while (num_scopes > 0) {
        --num_scopes;
        m.del(m_lhs.back());
        m.del(m_rhs.back());
        m_dam.del(m_deps.back());
        m_lhs.pop_back();
        m_rhs.pop_back();
        m_deps.pop_back();
    }
}

void theory_seq::restart_eh() {
#if 0
    m.del(m_lhs.back());
    m.del(m_rhs.back());
    m_dam.del(m_deps.back());
    m_lhs.reset();
    m_rhs.reset();
    m_deps.reset();
    m_lhs.push_back(expr_array());
    m_rhs.push_back(expr_array());
    m_deps.push_back(enode_pair_dependency_array());
#endif
}

void theory_seq::relevant_eh(app* n) {
    if (m_util.str.is_length(n)) {
        add_len_axiom(n);
    }
}

