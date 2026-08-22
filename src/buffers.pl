% Buffer-local variables: a generic mechanism for state that should be
% independent per buffer -- undo/redo history (src/undo_history.pl) and
% search/replace session state (src/search.pl) are the first users, so
% opening a second file doesn't share (or clobber) the first one's undo
% stack or in-progress search. `blocal(BufId, Key, Value)` is one fact per
% (buffer, key), maintained with the same "retract old, assertz new"
% singleton idiom src/undo_history.pl/src/search.pl already used for their
% (formerly global) state -- safe to do this per key now that retract/1
% reclaims its arena bytes (see prolog.c's compactProgram) instead of
% leaking, which every one of these updates would otherwise do on nearly
% every keystroke.
%
% goal_col_set/1 and goal_col_val/1 (src/movement.pl) deliberately do NOT
% move here: cursor/anchor/scroll are per-*window* now (see main.c's Window
% struct), not per-buffer, and goal-column tracking is a window's own
% vertical-movement bookkeeping -- if the same buffer is ever split across
% two windows, buffer-local would wrongly make them share one sticky
% column. There's only ever one window until a later commit adds real
% window objects/switching, so this doesn't matter yet in practice; it'll
% get its own window-local equivalent once te_selected_window/1 exists,
% rather than being squeezed into the wrong scope now.

% This engine has no dynamic/1 declaration -- a predicate that's never been
% asserted at all doesn't just fail when queried, it throws an existence
% error (findPred returns NULL; a predicate that's been asserted then fully
% retracted stays a known, empty predicate and fails normally instead). On
% a long-running engine blocal/3 always ends up asserted by something before
% it matters, but a freshly created one (scriptInitFromFile, used by tests
% and any future per-buffer/per-window reinit) has never asserted it at
% all, so its very first blocal_get/blocal_set throws instead of finding
% "nothing here yet". assert-then-retract a throwaway fact at load time so
% blocal/3 always exists (with zero real clauses) from the start. Confirmed
% the hard way: te_insert("hello") right after a fresh scriptInitFromFile
% silently never recorded an undo step, because record_edit's first-ever
% blocal_get threw "existence error: procedure blocal/3" (caught and
% echoed, not fatal, so nothing *looked* wrong -- the buffer still updated
% correctly since that splice happens independently of undo recording).
:- assertz(blocal(0, '$init', 0)), retract(blocal(0, '$init', 0)).

% Fetches into a fresh variable first, THEN unifies with the caller's
% Value -- not `( blocal(Id, Key, Value) -> true ; default_blocal(...) )`
% directly. A caller's Value argument is often already partly or fully
% bound (undo_stack_empty checks `blocal_get(undo_stack, [])`; undo_step
% destructures `blocal_get(undo_stack, [entry(...)|Rest])`), and passing
% that straight into the blocal/3 lookup makes "the stored value doesn't
% match what the caller asked for" indistinguishable from "no fact exists
% for this key yet" -- both just fail the lookup and fall through to
% default_blocal, silently returning the *default* instead of the real
% (non-matching) stored value. Confirmed the hard way: undo_stack_empty
% reported true right after a real, non-empty push, because `[]` never
% unified with the actual stack, so it kept "finding" default_blocal's `[]`.
blocal_get(Key, Value) :-
    te_current_buffer(Id),
    ( blocal(Id, Key, Stored) -> Value = Stored ; default_blocal(Key, Value) ).
blocal_set(Key, Value) :-
    te_current_buffer(Id),
    ( retract(blocal(Id, Key, _)) -> true ; true ),
    assertz(blocal(Id, Key, Value)).

% Starting value for a buffer-local key that's never been set for the
% current buffer yet (a freshly created/opened buffer) -- one line per key,
% matching what used to be that predicate's own base fact before it moved
% here (e.g. undo_history.pl's old `undo_stack([]).`).
default_blocal(undo_stack, []).
default_blocal(redo_stack, []).
default_blocal(match_list, []).
default_blocal(search_index, 0).
default_blocal(search_bad_regex, false).
default_blocal(match_truncated, false).
default_blocal(search_params, params(false, false, 0)).
default_blocal(replace_params, params([], [], false, false)).

% --- policy on top of the native buffer/window primitives -------------------
% Structural work (create/free a Buffer, keep every Window's `buf` pointer
% valid) stays native (script.c); this is the decision logic -- reuse an
% already-open buffer instead of re-reading the file, cycle order, and
% cleaning up a killed buffer's own blocal/3 facts so they don't linger
% forever across a long session of opening and closing files.

switch_buffer(Id) :- te_selected_window(W), te_window_set_buffer(W, Id).

% Reuses the buffer already showing Path instead of re-reading it from disk
% -- a real behavior change from the old single-buffer ACTION_OPEN, which
% always clobbered the current buffer in place (see README). Opening the
% same file twice no longer discards unsaved changes in the first buffer.
open_file(Path) :-
    ( te_buffer_find_by_path(Path, Id) -> true ; te_buffer_open_file(Path, Id) ),
    switch_buffer(Id).

next_buffer :- te_buffer_list(Ids), te_current_buffer(Cur), cycle_buffer(Ids, Cur, 1).
prev_buffer :- te_buffer_list(Ids), te_current_buffer(Cur), cycle_buffer(Ids, Cur, -1).
% Finds Cur's own position in Ids (nth0 with an unbound index backtracks
% through the list until Cur unifies -- see bootstrap.pl), then switches to
% the buffer Delta positions away, wrapping around either end. With a single
% buffer this always lands back on itself.
cycle_buffer(Ids, Cur, Delta) :-
    nth0(Idx, Ids, Cur), !,
    length(Ids, Len),
    NewIdx is (Idx + Delta + Len) mod Len,
    nth0(NewIdx, Ids, NewId),
    switch_buffer(NewId).

% Clears the killed buffer's own buffer-local state first -- otherwise every
% key this buffer ever used (undo_stack, search_params, ...) leaks forever
% as blocal/3 facts nothing can reach again (a real leak the arena
% compaction fix, prolog.c's compactProgram, can't help with: those facts
% are still *live*, just permanently unreachable via te_current_buffer).
kill_buffer(Id) :- retractall(blocal(Id, _, _)), te_buffer_kill(Id).
