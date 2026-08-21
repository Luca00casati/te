% Search/replace state machine, in Prolog rather than as C statics -- PCRE2
% (regex) and memmem (literal) stay native (te_find_matches/3,
% te_regex_substitute/5, both wrapping src/main.c's findMatches/
% editorRegexSubstitute), since PCRE2 is a C library. This file is the
% decision logic on top: which match is selected, what next/prev/replace do,
% wrap-around, replace-all's ordering. The minibuffer widget itself (typing
% into the prompt, Tab-complete, the modal shell) stays in main.c -- that's
% UI plumbing orthogonal to search, so main.c still resolves "what's the
% active query" (live prompt text vs. last committed search) and passes the
% bytes in here as an argument, same as it already hands byte ranges to
% record_edit/5 in src/undo_history.pl.
%
% Facts, single-clause-with-retract/assert (same style as undo_stack/1):
% match_list holds the current match(Start,End) terms in buffer order;
% search_index is which one is selected (for "i/N"); search_bad_regex/
% match_truncated mirror te_find_matches's Result. search_params remembers
% the session that started the current search (IsRegex, Reverse, Origin);
% replace_params remembers a query-replace session's pattern/replacement so
% replace_current_match/1 (no arguments -- called once per Enter in the
% loop) can re-run it. `last_search` deliberately does NOT live here -- it's
% minibuffer-adjacent (remembering the last submitted prompt text for
% empty-Enter repeat), so it stays a plain C static next to mb_input.

match_list([]).
search_index(0).
search_bad_regex(false).
match_truncated(false).
search_params(false, false, 0).       % IsRegex, Reverse, Origin
replace_params([], [], false, false). % PatternCodes, ReplacementCodes, IsRegex, AllMode

set_match_list(L) :- retract(match_list(_)), assertz(match_list(L)).
set_search_index(I) :- retract(search_index(_)), assertz(search_index(I)).
set_bad_regex(B) :- retract(search_bad_regex(_)), assertz(search_bad_regex(B)).
set_truncated(T) :- retract(match_truncated(_)), assertz(match_truncated(T)).
set_search_params(R, Rev, O) :- retract(search_params(_, _, _)), assertz(search_params(R, Rev, O)).
set_replace_params(F, T, R, A) :- retract(replace_params(_, _, _, _)), assertz(replace_params(F, T, R, A)).

% Where the current search/replace session began -- main.c reads this back
% to restore the caret on an aborted (Esc) search prompt.
search_origin(Origin) :- search_params(_, _, Origin).
% Whether the current session searches backward -- main.c reads this back
% for empty-Enter repeat-search, which reverses direction each time.
search_reverse(Reverse) :- search_params(_, Reverse, _).

% Read-back for the minibuffer draw code / echo messages -- re-derived
% fresh each call rather than cached, same philosophy as the rest of the
% engine's C-facing surface.
search_status(Index, Count, Truncated, BadRegex) :-
    search_index(Index),
    match_list(List), length(List, Count),
    match_truncated(Truncated),
    search_bad_regex(BadRegex).

clear_search :-
    set_match_list([]),
    set_search_index(0),
    set_bad_regex(false),
    set_truncated(false),
    set_search_params(false, false, 0),
    set_replace_params([], [], false, false).

% Recomputes match_list/search_bad_regex/match_truncated for Query/IsRegex.
recompute(Query, IsRegex) :-
    te_find_matches(Query, IsRegex, Result),
    ( Result = matches(List, Trunc)
    -> set_match_list(List), set_bad_regex(false), set_truncated(Trunc)
    ;  set_match_list([]), set_bad_regex(true), set_truncated(false)
    ).

% Selects match_list's Index-th entry and records it as the current one.
goto_match(Index) :-
    match_list(List),
    nth0(Index, List, match(Start, End)),
    te_set_selection(Start, End),
    set_search_index(Index).

% --- index-picking helpers, shared by search_update/search_step/
% enter_replace_query/replace_current_match ---------------------------------

% First index (0-based) with Start >= Pos, or 0 if none qualifies -- the
% "0" default is not a bug: it's how a forward search wraps to the first
% match when nothing at/after the origin matched, matching today's C loops
% (which simply never advance `idx` past its 0 initializer in that case).
first_at_or_after(List, Pos, Idx) :-
    ( first_at_or_after_strict(List, Pos, 0, I) -> Idx = I ; Idx = 0 ).
% Same, but FAILS instead of defaulting to 0 -- replace_current_match needs
% to distinguish "nothing left after this point" (stop) from "wrap to the
% first match" (enter_replace_query/search_update's behavior).
first_at_or_after_strict([match(S, _) | _], Pos, I, I) :- S >= Pos, !.
first_at_or_after_strict([_ | T], Pos, I0, I) :- I1 is I0 + 1, first_at_or_after_strict(T, Pos, I1, I).

% First index with Start > Pos, or 0 if none -- search_step's forward jump
% from a bare cursor position (not currently on any match).
first_after(List, Pos, Idx) :-
    ( first_after_strict(List, Pos, 0, I) -> Idx = I ; Idx = 0 ).
first_after_strict([match(S, _) | _], Pos, I, I) :- S > Pos, !.
first_after_strict([_ | T], Pos, I0, I) :- I1 is I0 + 1, first_after_strict(T, Pos, I1, I).

% Last index (0-based) with Start < Pos, or the last index if none qualifies
% -- reverse search's wraparound, and search_step's backward jump.
last_before(List, Pos, Idx) :-
    ( last_before_strict(List, Pos, I) -> Idx = I ; length(List, Len), Idx is Len - 1 ).
last_before_strict(List, Pos, Idx) :-
    last_before_acc(List, Pos, 0, -1, Best),
    Best >= 0,
    Idx = Best.
last_before_acc([], _, _, Best, Best).
last_before_acc([match(S, _) | T], Pos, I, Best0, Best) :-
    ( S < Pos -> Best1 is I ; Best1 = Best0 ),
    I1 is I + 1,
    last_before_acc(T, Pos, I1, Best1, Best).

% Index of the match spanning exactly [Anchor, Cursor) -- i.e. "is the
% current selection already sitting on one of these matches" -- fails if not.
find_current_index(List, Anchor, Cursor, Idx) :- find_current_index_(List, Anchor, Cursor, 0, Idx).
find_current_index_([match(Anchor, Cursor) | _], Anchor, Cursor, I, I) :- !.
find_current_index_([_ | T], Anchor, Cursor, I0, I) :- I1 is I0 + 1, find_current_index_(T, Anchor, Cursor, I1, I).

% --- session setup -----------------------------------------------------------

start_search(IsRegex, Reverse, Origin) :- set_search_params(IsRegex, Reverse, Origin).
start_replace(IsRegex, AllMode, Origin) :-
    set_search_params(IsRegex, false, Origin),
    set_replace_params([], [], IsRegex, AllMode).

% --- incremental search ------------------------------------------------------

% Re-run the search after the query changed and jump to the first match at
% or after where the search began (wrapping), or reverse's mirror. An empty,
% invalid, or zero-match query snaps back to the origin rather than leaving
% the caret on a stale match -- the live minibuffer draw reads
% search_bad_regex/match_list to show why.
search_update(QueryCodes) :-
    search_params(IsRegex, Reverse, Origin),
    length(QueryCodes, Len),
    ( Len =:= 0
    -> te_set_selection(Origin, Origin), set_match_list([]), set_bad_regex(false), set_truncated(false)
    ;  recompute(QueryCodes, IsRegex),
       ( search_bad_regex(true)
       -> te_set_selection(Origin, Origin)
       ;  match_list(List),
          ( List == []
          -> te_set_selection(Origin, Origin)
          ;  ( Reverse == true -> last_before(List, Origin, Idx) ; first_at_or_after(List, Origin, Idx) ),
             goto_match(Idx)
          )
       )
    ).

% Jump to the next/previous match of QueryCodes (wrapping) -- C-n/C-p while
% the search/replace-pattern prompt is open, or an empty-Enter repeat.
search_step(QueryCodes, Forward) :-
    search_params(IsRegex, _, _),
    recompute(QueryCodes, IsRegex),
    match_list(List),
    ( search_bad_regex(true) -> true
    ; List == [] -> true
    ; te_anchor(Anchor), te_cursor(Cursor),
      ( find_current_index(List, Anchor, Cursor, Cur)
      -> length(List, Len),
         ( Forward == true -> Idx is (Cur + 1) mod Len ; Idx is (Cur - 1 + Len) mod Len )
      ;  ( Forward == true -> first_after(List, Cursor, Idx) ; last_before(List, Cursor, Idx) )
      ),
      goto_match(Idx)
    ).

% --- replace ------------------------------------------------------------

% Literal replace uses the replacement text verbatim; regex applies it as a
% PCRE2 substitution against the matched span (so $1/$0 etc. expand).
compute_replacement(false, _Pattern, _Ms, _Me, Replacement, Replacement) :- !.
compute_replacement(true, Pattern, Ms, Me, Replacement, RepCodes) :-
    te_regex_substitute(Pattern, Ms, Me, Replacement, ok(RepCodes)).

% Enter the interactive query-replace loop: record the session, jump to the
% first match at/after the origin (no wraparound -- matching today exactly).
% Fails on a bad pattern or zero matches; the caller (scriptEnterReplaceQuery)
% checks search_bad_regex/1 to pick "Invalid pattern" vs. "No matches" and
% closes the minibuffer either way, same division of labor as before.
enter_replace_query(PatternCodes, ReplacementCodes, IsRegex) :-
    set_replace_params(PatternCodes, ReplacementCodes, IsRegex, false),
    search_params(_, _, Origin),
    recompute(PatternCodes, IsRegex),
    \+ search_bad_regex(true),
    match_list(List),
    List \= [],
    first_at_or_after(List, Origin, Idx),
    goto_match(Idx).

% Move to the next/previous match without replacing (n/p in replace mode) --
% unlike search_step, doesn't recompute: just steps the existing match_list.
replace_step(Forward) :-
    match_list(List), length(List, Len), Len > 0,
    search_index(Cur),
    ( Forward == true -> Idx is (Cur + 1) mod Len ; Idx is (Cur - 1 + Len) mod Len ),
    goto_match(Idx).

% Replace the current match, then advance past the insertion (so the
% replacement text is never re-matched) to the next remaining match. Result
% unifies with `ok` (applied, advanced -- caller reads match_list/
% search_index for "Match i/N"), `done` (nothing left -- caller echoes
% "Replace done" and closes the loop), or `failed` (substitution failed for
% this match -- caller echoes "Replace failed" and leaves the loop open,
% same asymmetry the old C had).
replace_current_match(Result) :-
    match_list(List),
    ( List == []
    -> Result = done
    ;  search_index(Idx),
       nth0(Idx, List, match(Ms, Me)),
       replace_params(Pattern, Replacement, IsRegex, _),
       ( compute_replacement(IsRegex, Pattern, Ms, Me, Replacement, RepCodes)
       -> length(RepCodes, RepLen),
          te_apply_replace(Ms, Me, RepCodes),
          From is Ms + RepLen,
          recompute(Pattern, IsRegex),
          match_list(List2),
          ( first_at_or_after_strict(List2, From, 0, Idx2)
          -> goto_match(Idx2), Result = ok
          ;  Result = done
          )
       ;  Result = failed
       )
    ).

% Replace every match of Pattern with Replacement in one native call each
% (so every applied match gets its own undo step via te_apply_replace/edit(),
% same as ordinary typing). Applied from the last match to the first so
% earlier offsets stay valid as later ones are edited. Count is the total
% match count regardless of any individual substitution failure (matching
% today's C exactly -- a skipped match still counts toward "Replaced N").
replace_all(PatternCodes, ReplacementCodes, IsRegex, Count) :-
    recompute(PatternCodes, IsRegex),
    ( search_bad_regex(true)
    -> Count = 0
    ;  match_list(List),
       length(List, Count),
       ( Count =:= 0
       -> true
       ;  te_cursor(Saved),
          reverse(List, RevList),
          apply_all(RevList, PatternCodes, ReplacementCodes, IsRegex),
          te_set_selection(Saved, Saved) % editorSetSelection clamps to the (possibly now shorter) buffer
       )
    ).
apply_all([], _, _, _).
apply_all([match(Ms, Me) | T], Pattern, Replacement, IsRegex) :-
    ( compute_replacement(IsRegex, Pattern, Ms, Me, Replacement, RepCodes)
    -> te_apply_replace(Ms, Me, RepCodes)
    ;  true % substitution failed for this match: skip it, matching the C `continue`
    ),
    apply_all(T, Pattern, Replacement, IsRegex).
