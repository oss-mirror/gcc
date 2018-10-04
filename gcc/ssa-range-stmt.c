/* SSA range statement summary.
   Copyright (C) 2017-2018 Free Software Foundation, Inc.
   Contributed by Andrew MacLeod <amacleod@redhat.com>.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "insn-codes.h"
#include "rtl.h"
#include "tree.h"
#include "gimple.h"
#include "cfghooks.h"
#include "tree-pass.h"
#include "ssa.h"
#include "optabs-tree.h"
#include "gimple-pretty-print.h"
#include "diagnostic-core.h"
#include "flags.h"
#include "fold-const.h"
#include "stor-layout.h"
#include "calls.h"
#include "cfganal.h"
#include "gimple-fold.h"
#include "tree-eh.h"
#include "gimple-iterator.h"
#include "gimple-walk.h"
#include "tree-cfg.h"
#include "wide-int.h"
#include "ssa-range-stmt.h"
#include "fold-const.h"

/* Return the First operand of the statement if it is a valid SSA_NAME which
   is supported by class irange. Otherwise return NULL_TREE.  */
tree
range_stmt::operand1 () const
{
  switch (gimple_code (m_g))
    {
      case GIMPLE_COND:
        return gimple_cond_lhs (m_g);
      case GIMPLE_ASSIGN:
        {
	  tree expr = gimple_assign_rhs1 (m_g);
	  if (get_code() == ADDR_EXPR)
	    {
	      // If the base address is an SSA_NAME, return it. 
	      // if its range is non-zero, then we know the pointer is non-zero.
	      bool strict_ov;
	      tree base = get_base_address (TREE_OPERAND (expr, 0));
	      if (base != NULL_TREE && TREE_CODE (base) == MEM_REF
		  && TREE_CODE (TREE_OPERAND (base, 0)) == SSA_NAME)
		return TREE_OPERAND (base, 0);
	      
	      // Otherwise, check to see if the RHS is always non-null
	      // and return a constant that will equate to a non-zero range.
	      if (tree_single_nonzero_warnv_p (expr, &strict_ov))
		return integer_one_node;
	      // Otherwise this is not something complex we care about. Return
	      // the RHS and allow validate_stmt to have a look.
	    }
	  return expr;
	}
      default:
        break;
    }
  return NULL;
}

/* Validate that the statement and all operands of this expression are
   operable on iranges. If it is valid, set the stmt pointer.  */
void
range_stmt::validate_stmt (gimple *s)
{
  // Check for supported statements
  switch (gimple_code (s))
    {
      case GIMPLE_COND:
      case GIMPLE_ASSIGN:
	m_g = s;
        break;

      default:
        m_g = NULL;
    }

  // Must have a ranger operation handler as well.
  if (m_g && handler ())
    {
      // Now verify all the operanmds are compatible
      tree op1 = operand1 ();
      tree op2 = operand2 ();
      tree ssa1 = valid_irange_ssa (op1);
      tree ssa2 = valid_irange_ssa (op2);

      if (ssa1 || (TREE_CODE (op1) == INTEGER_CST && !TREE_OVERFLOW (op1))) 
	{
	  // IF this is a unary operation, we are done. 
	  if (!op2)
	   return;
	  if (ssa2 || (TREE_CODE (op2) == INTEGER_CST && !TREE_OVERFLOW (op2)))
	   return;
	}
    }
  m_g = NULL;
}


/* This method will attempt to resolve a unary expression with value R1 to
   a range.  If the expression can be resolved, true is returned, and the
   range is returned in RES.  */
bool
range_stmt::fold (irange &res, const irange& r1) const
{
  irange r2;
  tree lhs = gimple_get_lhs (m_g);
  /* Single ssa operations require the LHS type as the second range.  */
  if (lhs)
    r2.set_varying (TREE_TYPE (lhs));
  else
    r2.set_undefined (r1.type ());

  return handler()->fold_range (res, r1, r2);
}

/* This method will attempt to resolve a binary expression with operand
   values R1 tnd R2 to a range.  If the expression can be resolved, true is
   returned, and the range is returned in RES.  */
bool
range_stmt::fold (irange &res, const irange& r1, const irange& r2) const
{
  // Make sure this isnt a unary operation being passed a second range.
  gcc_assert (operand2 ());
  return handler() ->fold_range (res, r1, r2);
}

/* This method will evaluate a range for the operand of a unary expression
   given a range for the LHS of the expression in LHS_RANGE. If it can be
   evaluated, TRUE is returned and the resulting range returned in RES.  */
bool
range_stmt::op1_irange (irange& r, const irange& lhs_range) const
{  
  irange type_range;
  // An empty range is viral, so return an empty range.
  if (lhs_range.undefined_p ())
    {
      r.set_undefined (TREE_TYPE (operand1 ()));
      return true;
    }
  type_range.set_varying (TREE_TYPE (operand1 ()));
  return handler ()->op1_range (r, lhs_range, type_range);
}

/* This method will evaluate a range for operand 1 of a binary expression
   given a range for the LHS in LHS_RANGE and a range for operand 2 in
   OP2_RANGE. If it can be evaluated, TRUE is returned and the resulting
   range returned in RES.  */
bool
range_stmt::op1_irange (irange& r, const irange& lhs_range,
			const irange& op2_range) const
{  
  // Changing the API to say unary ops can be called with the range of the
  // RHS instead of the type if the caller wants.
  // This is more flexible since we can still get the type from the range if
  // that is all we want, but getting an actual range will let us sometimes
  // determine more. see operator_cast::op1_irange.()
  
  // gcc_assert (operand2 () != NULL);
  
  // An empty range is viral, so return an empty range.
  if (op2_range.undefined_p () || lhs_range.undefined_p ())
    {
      r.set_undefined (op2_range.type ());
      return true;
    }
  return handler ()->op1_range (r, lhs_range, op2_range);
}

/* This method will evaluate a range for operand 2 of a binary expression
   given a range for the LHS in LHS_RANGE and a range for operand 1 in
   OP1_RANGE. If it can be evaluated, TRUE is returned and the resulting
   range returned in RES.  */
bool
range_stmt::op2_irange (irange& r, const irange& lhs_range,
			const irange& op1_range) const
{  
  // An empty range is viral, so return an empty range.
  if (op1_range.undefined_p () || lhs_range.undefined_p ())
    {
      r.set_undefined (op1_range.type ());
      return true;
    }
  return handler ()->op2_range (r, lhs_range, op1_range);
}

/* This method will dump the internal state of the statement summary.  */
void
range_stmt::dump (FILE *f) const
{
  tree lhs = gimple_get_lhs (m_g);
  tree op1 = operand1 ();
  tree op2 = operand2 ();

  if (lhs)
    {
      print_generic_expr (f, lhs, TDF_SLIM);
      fprintf (f, " = ");
    }

  if (!op2)
    handler ()->dump (f);

  print_generic_expr (f, op1, TDF_SLIM);

  if (op2)
    {
      handler ()->dump (f);
      print_generic_expr (f, op2, TDF_SLIM);
    }

}


/* Return TRUE if code with operands of type TYPE is a boolean
   evaluation.  These are important to identify as both sides of a logical
   binary expression must be evaluated in order to calculate a range.  */
bool
logical_combine_stmt_p (gimple *s)
{
  range_stmt rn(s);
  if (!rn.valid ())
    return false;

  /* Look for boolean and/or condition.  */
  switch (gimple_expr_code (s))
    {
      case TRUTH_AND_EXPR:
      case TRUTH_OR_EXPR:
        return true;

      case BIT_AND_EXPR:
      case BIT_IOR_EXPR:
        // Bitwise operations on single bits are logical too.
        if (types_compatible_p (TREE_TYPE (rn.operand1 ()), boolean_type_node))
	  return true;
	break;

      default:
        break;
    }
  return false;
}


/* Evaluate a binary logical expression given true and false ranges for each
   of the operands. Base the result on the value in the LHS.  */
bool
logical_combine_stmt_fold (irange& r, gimple *s, const irange& lhs,
			   const irange& op1_true,
			   const irange& op1_false,
			   const irange& op2_true,
			   const irange& op2_false)
{
  range_stmt stmt(s);
  gcc_checking_assert (logical_combine_stmt_p (s));
 
  /* If the LHS can be TRUE OR FALSE, then both need to be evaluated and
     combined, otherwise any range restrictions that have been determined
     leading up to this point would be lost.  */
  if (!wi::eq_p (lhs.lower_bound(), lhs.upper_bound()))
    {
      irange r1;
      unsigned prec = TYPE_PRECISION (boolean_type_node);
      irange bool_zero (boolean_type_node, wi::zero (prec), wi::zero (prec));
      irange bool_one (boolean_type_node, wi::one (prec), wi::one (prec));
      if (logical_combine_stmt_fold (r1, s, bool_zero, op1_true, op1_false,
				     op2_true, op2_false) &&
	  logical_combine_stmt_fold (r, s, bool_one, op1_true, op1_false,
				     op2_true, op2_false))
	{
	  r.union_ (r1);
	  return true;
	}
      return false;

    }

  /* Now combine based on whether the result is TRUE or FALSE.  */
  switch (gimple_expr_code (s))
    {

      /* A logical operation on two ranges is executed with operand ranges that
	 have been determined for both a TRUE and FALSE result..
	 Assuming x_8 is an unsigned char:
		b_1 = x_8 < 20
		b_2 = x_8 > 5
	 if we are looking for the range of x_8, the operand ranges will be:
	 will be: 
	 b_1 TRUE	x_8 = [0, 19]
	 b_1 FALSE  	x_8 = [20, 255]
	 b_2 TRUE 	x_8 = [6, 255]
	 b_2 FALSE	x_8 = [0,5]. */
	       
      /*	c_2 = b_1 && b_2
	 The result of an AND operation with a TRUE result is the intersection
	 of the 2 TRUE ranges, [0,19] intersect [6,255]  ->   [6, 19]. */
      case TRUTH_AND_EXPR:
      case BIT_AND_EXPR:
        if (!lhs.zero_p ())
	  r = range_intersect (op1_true, op2_true);
	else
	  {
	    /* The FALSE side is the union of the other 3 cases.  */
	    irange ff = range_intersect (op1_false, op2_false);
	    irange tf = range_intersect (op1_true, op2_false);
	    irange ft = range_intersect (op1_false, op2_true);
	    r = range_union (ff, tf);
	    r.union_ (ft);
	  }
        break;

      /* 	c_2 = b_1 || b_2
	 An OR operation will only take the FALSE path if both operands are
	 false, so [20, 255] intersect [0, 5] is the union: [0,5][20,255].  */
      case TRUTH_OR_EXPR:
      case BIT_IOR_EXPR:
        if (lhs.zero_p ())
	  r = range_intersect (op1_false, op2_false);
	else
	  {
	    /* The TRUE side of the OR operation will be the union of the other
	       three combinations.  */
	    irange tt = range_intersect (op1_true, op2_true);
	    irange tf = range_intersect (op1_true, op2_false);
	    irange ft = range_intersect (op1_false, op2_true);
	    r = range_union (tt, tf);
	    r.union_ (ft);
	  }
	break;

      default:
        gcc_unreachable ();
    }

  return true;
}

/* Return the range implied by edge E for a branch statement in R.  */
void gori_branch_edge_range (irange& r, edge e)
{
  gcc_assert (gori_branch_stmt_p (last_stmt (e->src)));

  if (e->flags & EDGE_TRUE_VALUE)
    r = irange (boolean_type_node, boolean_true_node, boolean_true_node);
  else if (e->flags & EDGE_FALSE_VALUE)
    r = irange (boolean_type_node, boolean_false_node, boolean_false_node);
  else
    gcc_unreachable ();

  return;
}


