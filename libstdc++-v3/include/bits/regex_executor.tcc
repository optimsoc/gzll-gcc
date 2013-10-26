// class template regex -*- C++ -*-

// Copyright (C) 2013 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// Under Section 7 of GPL version 3, you are granted additional
// permissions described in the GCC Runtime Library Exception, version
// 3.1, as published by the Free Software Foundation.

// You should have received a copy of the GNU General Public License and
// a copy of the GCC Runtime Library Exception along with this program;
// see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
// <http://www.gnu.org/licenses/>.

/**
 *  @file bits/regex_executor.tcc
 *  This is an internal header file, included by other library headers.
 *  Do not attempt to use it directly. @headername{regex}
 */

namespace std _GLIBCXX_VISIBILITY(default)
{
namespace __detail
{
_GLIBCXX_BEGIN_NAMESPACE_VERSION

  template<typename _BiIter, typename _Alloc, typename _TraitsT,
    bool __dfs_mode>
    bool _Executor<_BiIter, _Alloc, _TraitsT, __dfs_mode>::
    _M_search()
    {
      if (_M_flags & regex_constants::match_continuous)
	return _M_search_from_first();
      auto __cur = _M_begin;
      do
	{
	  _M_current = __cur;
	  if (_M_main<false>())
	    return true;
	}
      // Continue when __cur == _M_end
      while (__cur++ != _M_end);
      return false;
    }

  template<typename _BiIter, typename _Alloc, typename _TraitsT,
    bool __dfs_mode>
  template<bool __match_mode>
    bool _Executor<_BiIter, _Alloc, _TraitsT, __dfs_mode>::
    _M_main()
    {
      if (__dfs_mode)
	{
	  _M_has_sol = false;
	  _M_cur_results = _M_results;
	  _M_dfs<__match_mode>(_M_start_state);
	  return _M_has_sol;
	}
      else
	{
	  // Like the DFS approach, it try every possible state transition;
	  // Unlike DFS, it uses a queue instead of a stack to store matching
	  // states. It's a BFS approach.
	  //
	  // Russ Cox's article(http://swtch.com/~rsc/regexp/regexp1.html)
	  // explained this algorithm clearly.
	  //
	  // Time complexity: o(match_length * match_results.size())
	  //                  O(match_length * _M_nfa.size()
	  //                    * match_results.size())
	  // Space complexity: o(_M_nfa.size() + match_results.size())
	  //                   O(_M_nfa.size() * match_results.size())
	  _M_match_queue->push(make_pair(_M_start_state, _M_results));
	  bool __ret = false;
	  while (1)
	    {
	      _M_has_sol = false;
	      if (_M_match_queue->empty())
		break;
	      _M_visited->assign(_M_visited->size(), false);
	      auto _M_old_queue = std::move(*_M_match_queue);
	      while (!_M_old_queue.empty())
		{
		  auto __task = _M_old_queue.front();
		  _M_old_queue.pop();
		  _M_cur_results = __task.second;
		  _M_dfs<__match_mode>(__task.first);
		}
	      if (!__match_mode)
		__ret |= _M_has_sol;
	      if (_M_current == _M_end)
		break;
	      ++_M_current;
	    }
	  if (__match_mode)
	    __ret = _M_has_sol;
	  return __ret;
	}
    }

  // Return whether now match the given sub-NFA.
  template<typename _BiIter, typename _Alloc, typename _TraitsT,
    bool __dfs_mode>
    bool _Executor<_BiIter, _Alloc, _TraitsT, __dfs_mode>::
    _M_lookahead(_State<_Executor<_BiIter, _Alloc, _TraitsT, __dfs_mode>::
		 _CharT, _TraitsT> __state)
    {
      _ResultsVec __what(_M_cur_results.size());
      auto __sub = std::unique_ptr<_Executor>(new _Executor(_M_current,
							    _M_end,
							    __what,
							    _M_re,
							    _M_flags));
      __sub->_M_start_state = __state._M_alt;
      if (__sub->_M_search_from_first())
	{
	  for (size_t __i = 0; __i < __what.size(); __i++)
	    if (__what[__i].matched)
	      _M_cur_results[__i] = __what[__i];
	  return true;
	}
      return false;
    }

  // A _DFSExecutor perform a DFS on given NFA and input string. At the very
  // beginning the executor stands in the start state, then it try every
  // possible state transition in current state recursively. Some state
  // transitions consume input string, say, a single-char-matcher or a
  // back-reference matcher; some not, like assertion or other anchor nodes.
  // When the input is exhausted and the current state is an accepting state,
  // the whole executor return true.
  //
  // TODO: This approach is exponentially slow for certain input.
  //       Try to compile the NFA to a DFA.
  //
  // Time complexity: o(match_length), O(2^(_M_nfa.size()))
  // Space complexity: \theta(match_results.size() + match_length)
  //
  template<typename _BiIter, typename _Alloc, typename _TraitsT,
    bool __dfs_mode>
  template<bool __match_mode>
    void _Executor<_BiIter, _Alloc, _TraitsT, __dfs_mode>::
    _M_dfs(_StateIdT __i)
    {
      if (!__dfs_mode)
	{
	  if ((*_M_visited)[__i])
	    return;
	  (*_M_visited)[__i] = true;
	}

      const auto& __state = _M_nfa[__i];
      switch (__state._M_opcode)
	{
	case _S_opcode_alternative:
	  // Greedy or not, this is a question ;)
	  if (!__state._M_neg)
	    {
	      _M_dfs<__match_mode>(__state._M_alt);
	      if (!__dfs_mode || !_M_has_sol)
		_M_dfs<__match_mode>(__state._M_next);
	    }
	  else
	    {
	      if (__dfs_mode)
		{
		  _M_dfs<__match_mode>(__state._M_next);
		  if (!_M_has_sol)
		    _M_dfs<__match_mode>(__state._M_alt);
		}
	      else
		{
		  if (!_M_has_sol)
		    {
		      _M_dfs<__match_mode>(__state._M_next);
		      if (!_M_has_sol)
			_M_dfs<__match_mode>(__state._M_alt);
		    }
		}
	    }
	  break;
	case _S_opcode_subexpr_begin:
	  // Here's the critical part: if there's nothing changed since last
	  // visit, do NOT continue. This prevents the executor from get into
	  // infinite loop when use "()*" to match "".
	  //
	  // Every change on _M_cur_results will be roll back after the
	  // recursion step finished.
	  if (!_M_cur_results[__state._M_subexpr].matched
	      || _M_cur_results[__state._M_subexpr].first != _M_current)
	    {
	      auto& __res = _M_cur_results[__state._M_subexpr];
	      auto __back = __res.first;
	      __res.first = _M_current;
	      _M_dfs<__match_mode>(__state._M_next);
	      __res.first = __back;
	    }
	  break;
	case _S_opcode_subexpr_end:
	  if (_M_cur_results[__state._M_subexpr].second != _M_current
	      || _M_cur_results[__state._M_subexpr].matched != true)
	    {
	      auto& __res = _M_cur_results[__state._M_subexpr];
	      auto __back = __res;
	      __res.second = _M_current;
	      __res.matched = true;
	      _M_dfs<__match_mode>(__state._M_next);
	      __res = __back;
	    }
	  else
	    _M_dfs<__match_mode>(__state._M_next);
	  break;
	case _S_opcode_line_begin_assertion:
	  if (_M_at_begin())
	    _M_dfs<__match_mode>(__state._M_next);
	  break;
	case _S_opcode_line_end_assertion:
	  if (_M_at_end())
	    _M_dfs<__match_mode>(__state._M_next);
	  break;
	case _S_opcode_word_boundry:
	  if (_M_word_boundry(__state) == !__state._M_neg)
	    _M_dfs<__match_mode>(__state._M_next);
	  break;
	  // Here __state._M_alt offers a single start node for a sub-NFA.
	  // We recursivly invoke our algorithm to match the sub-NFA.
	case _S_opcode_subexpr_lookahead:
	  if (_M_lookahead(__state) == !__state._M_neg)
	    _M_dfs<__match_mode>(__state._M_next);
	  break;
	case _S_opcode_match:
	  if (__dfs_mode)
	    {
	      if (_M_current != _M_end && __state._M_matches(*_M_current))
		{
		  ++_M_current;
		  _M_dfs<__match_mode>(__state._M_next);
		  --_M_current;
		}
	    }
	  else
	    if (__state._M_matches(*_M_current))
	      _M_match_queue->push(make_pair(__state._M_next, _M_cur_results));
	  break;
	// First fetch the matched result from _M_cur_results as __submatch;
	// then compare it with
	// (_M_current, _M_current + (__submatch.second - __submatch.first))
	// If matched, keep going; else just return to try another state.
	case _S_opcode_backref:
	  {
	    _GLIBCXX_DEBUG_ASSERT(__dfs_mode);
	    auto& __submatch = _M_cur_results[__state._M_backref_index];
	    if (!__submatch.matched)
	      break;
	    auto __last = _M_current;
	    for (auto __tmp = __submatch.first;
		 __last != _M_end && __tmp != __submatch.second;
		 ++__tmp)
	      ++__last;
	    if (_M_re._M_traits.transform(__submatch.first,
						__submatch.second)
		== _M_re._M_traits.transform(_M_current, __last))
	      {
		if (__last != _M_current)
		  {
		    auto __backup = _M_current;
		    _M_current = __last;
		    _M_dfs<__match_mode>(__state._M_next);
		    _M_current = __backup;
		  }
		else
		  _M_dfs<__match_mode>(__state._M_next);
	      }
	  }
	  break;
	case _S_opcode_accept:
	  if (__dfs_mode)
	    {
	      _GLIBCXX_DEBUG_ASSERT(!_M_has_sol);
	      if (__match_mode)
		_M_has_sol = _M_current == _M_end;
	      else
		_M_has_sol = true;
	      if (_M_current == _M_begin
		  && (_M_flags & regex_constants::match_not_null))
		_M_has_sol = false;
	      if (_M_has_sol)
		_M_results = _M_cur_results;
	    }
	  else
	    {
	      if (_M_current == _M_begin
		  && (_M_flags & regex_constants::match_not_null))
		break;
	      if (!__match_mode || _M_current == _M_end)
		if (!_M_has_sol)
		  {
		    _M_has_sol = true;
		    _M_results = _M_cur_results;
		  }
	    }
	  break;
	default:
	  _GLIBCXX_DEBUG_ASSERT(false);
	}
    }

  // Return whether now is at some word boundry.
  template<typename _BiIter, typename _Alloc, typename _TraitsT,
    bool __dfs_mode>
    bool _Executor<_BiIter, _Alloc, _TraitsT, __dfs_mode>::
    _M_word_boundry(_State<_CharT, _TraitsT> __state) const
    {
      // By definition.
      bool __ans = false;
      auto __pre = _M_current;
      --__pre;
      if (!(_M_at_begin() && _M_at_end()))
	{
	  if (_M_at_begin())
	    __ans = _M_is_word(*_M_current)
	      && !(_M_flags & regex_constants::match_not_bow);
	  else if (_M_at_end())
	    __ans = _M_is_word(*__pre)
	      && !(_M_flags & regex_constants::match_not_eow);
	  else
	    __ans = _M_is_word(*_M_current)
	      != _M_is_word(*__pre);
	}
      return __ans;
    }

_GLIBCXX_END_NAMESPACE_VERSION
} // namespace __detail
} // namespace
