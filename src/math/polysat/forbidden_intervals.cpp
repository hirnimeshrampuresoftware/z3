/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    Conflict explanation using forbidden intervals as described in
    "Solving bitvectors with MCSAT: explanations from bits and pieces"
    by S. Graham-Lengrand, D. Jovanovic, B. Dutertre.

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-6



--*/
#include "math/polysat/forbidden_intervals.h"
#include "math/polysat/interval.h"
#include "math/polysat/solver.h"
#include "math/polysat/log.h"

namespace polysat {

    /** Precondition: all variables other than v are assigned.
     *
     * \param[out] out_interval     The forbidden interval for this constraint
     * \param[out] out_neg_cond     Negation of the side condition (the side condition is true when the forbidden interval is trivial). May be NULL if the condition is constant.
     * \returns True iff a forbidden interval exists and the output parameters were set.
     */

    bool forbidden_intervals::get_interval(signed_constraint const& c, pvar v, rational & coeff, eval_interval& out_interval, vector<signed_constraint>& out_side_cond) {
        if (!c->is_ule())
            return false;

        coeff = 1;
        
        struct backtrack {
            bool released = false;
            vector<signed_constraint>& side_cond;
            unsigned sz;
            backtrack(vector<signed_constraint>& s):side_cond(s), sz(s.size()) {}
            ~backtrack() {
                if (!released)
                    side_cond.shrink(sz);
            }
        };

        backtrack _backtrack(out_side_cond);
         
        /**
        * TODO: to express the interval for coefficient 2^i symbolically,
        * we need right-shift/upper-bits-extract in the language.
        * So currently we can only do it if the coefficient is 1 or -1.
        */

        auto [ok1, a1, e1, b1] = linear_decompose(v, c->to_ule().lhs(), out_side_cond);
        auto [ok2, a2, e2, b2] = linear_decompose(v, c->to_ule().rhs(), out_side_cond);
        if (!ok1 || !ok2 || (a1.is_zero() && a2.is_zero()))
            return false;
        if (a1 != a2 && !a1.is_zero() && !a2.is_zero())
            return false;
        SASSERT(b1.is_val());
        SASSERT(b2.is_val());    

        coeff = a1.is_zero() ? a2 : a1;

        _backtrack.released = true;

        // LOG("add " << c << " " << a1 << " " << b1 << " " << a2 << " " << b2);

        if (match_linear1(c, coeff, b1, e1, a2, b2, e2, out_interval, out_side_cond))
            return true;
        if (match_linear2(c, coeff, b1, e1, a2, b2, e2, out_interval, out_side_cond))
            return true;
        if (match_linear3(c, coeff, b1, e1, a2, b2, e2, out_interval, out_side_cond))
            return true;


        _backtrack.released = false;
        return false;
    }

    void forbidden_intervals::push_eq(bool is_zero, pdd const& p, vector<signed_constraint>& side_cond) {
        SASSERT(!p.is_val() || (is_zero == p.is_zero()));
        if (p.is_val())
            return;
        else if (is_zero)
            side_cond.push_back(s.m_constraints.eq(p));
        else
            side_cond.push_back(~s.m_constraints.eq(p));
    }

    std::tuple<bool, rational, pdd, pdd> forbidden_intervals::linear_decompose(pvar v, pdd const& p, vector<signed_constraint>& out_side_cond) {
        auto& m = p.manager();
        pdd q = m.zero();
        pdd e = m.zero();
        unsigned const deg = p.degree(v);
        if (deg == 0)
            e = p;
        else if (deg == 1)
            p.factor(v, 1, q, e);
        else
            return std::tuple(false, rational(0), q, e);

        if (!q.is_val()) {
            pdd r = q.subst_val(s.assignment());
            if (!r.is_val())
                return std::tuple(false, rational(0), q, e);
            out_side_cond.push_back(s.eq(q, r));
            q = r;
        }
        auto b = e.subst_val(s.assignment());
        return std::tuple(b.is_val(), q.val(), e, b);
    };

    eval_interval forbidden_intervals::to_interval(
        signed_constraint const& c, bool is_trivial, rational & coeff,
        rational & lo_val, pdd & lo,
        rational & hi_val, pdd & hi) {
        
        dd::pdd_manager& m = lo.manager();

        if (is_trivial) {
            if (c.is_positive())
                // TODO: we cannot use empty intervals for interpolation. So we
                // can remove the empty case (make it represent 'full' instead),
                // and return 'false' here. Then we do not need the proper/full
                // tag on intervals.
                return eval_interval::empty(m);
            else
                return eval_interval::full();
        }

        rational pow2 = m.max_value() + 1;

        if (coeff > pow2/2) {
            
            coeff = pow2 - coeff;
            SASSERT(coeff > 0);
            // Transform according to:  y \in [l;u[  <=>  -y \in [1-u;1-l[
            //      -y \in [1-u;1-l[
            //      <=>  -y - (1 - u) < (1 - l) - (1 - u)    { by: y \in [l;u[  <=>  y - l < u - l }
            //      <=>  u - y - 1 < u - l                   { simplified }
            //      <=>  (u-l) - (u-y-1) - 1 < u-l           { by: a < b  <=>  b - a - 1 < b }
            //      <=>  y - l < u - l                       { simplified }
            //      <=>  y \in [l;u[.
            lo = 1 - lo;
            hi = 1 - hi;
            swap(lo, hi);
            lo_val = mod(1 - lo_val, pow2);
            hi_val = mod(1 - hi_val, pow2);
            lo_val.swap(hi_val);
        }

        if (c.is_positive())
            return eval_interval::proper(lo, lo_val, hi, hi_val);
        else
            return eval_interval::proper(hi, hi_val, lo, lo_val);
    }

    /**
    * Match  e1 + t <= e2, with t = a1*y
    * condition for empty/full: e2 == -1
    */
    bool forbidden_intervals::match_linear1(signed_constraint const& c,
        rational & a1, pdd const& b1, pdd const& e1, 
        rational const & a2, pdd const& b2, pdd const& e2,
        eval_interval& interval, vector<signed_constraint>& side_cond) {
        if (a2.is_zero() && !a1.is_zero()) {
            SASSERT(!a1.is_zero());
            bool is_trivial = (b2 + 1).is_zero();
            push_eq(is_trivial, e2 + 1, side_cond);
            auto lo = e2 - e1 + 1;
            rational lo_val = (b2 - b1 + 1).val();
            auto hi = -e1;
            rational hi_val = (-b1).val();
            interval = to_interval(c, is_trivial, a1, lo_val, lo, hi_val, hi);
            return true;
        }
        return false;
    }

    /**
     * e1 <= e2 + t, with t = a2*y
     * condition for empty/full: e1 == 0
     */
    bool forbidden_intervals::match_linear2(signed_constraint const& c,
        rational & a1, pdd const& b1, pdd const& e1,
        rational const & a2, pdd const& b2, pdd const& e2,
        eval_interval& interval, vector<signed_constraint>& side_cond) {
        if (a1.is_zero() && !a2.is_zero()) {
            SASSERT(!a2.is_zero());
            a1 = a2;
            bool is_trivial = b1.is_zero();
            push_eq(is_trivial, e1, side_cond);
            auto lo = -e2;
            rational lo_val = (-b2).val();
            auto hi = e1 - e2;
            rational hi_val = (b1 - b2).val();
            interval = to_interval(c, is_trivial, a1, lo_val, lo, hi_val, hi);
            return true;
        }
        return false;
    }

    /**
     * e1 + t <= e2 + t, with t = a1*y = a2*y
     * condition for empty/full: e1 == e2
     */
    bool forbidden_intervals::match_linear3(signed_constraint const& c,
        rational & a1, pdd const& b1, pdd const& e1,
        rational const & a2, pdd const& b2, pdd const& e2,
        eval_interval& interval, vector<signed_constraint>& side_cond) {
        if (a1 == a2 && !a1.is_zero()) {
            bool is_trivial = b1.val() == b2.val();
            push_eq(is_trivial, e1 - e2, side_cond);
            auto lo = -e2;
            rational lo_val = (-b2).val();
            auto hi = -e1;
            rational hi_val = (-b1).val();
            interval = to_interval(c, is_trivial, a1, lo_val, lo, hi_val, hi);
            return true;
        }
        return false;
    }
}
