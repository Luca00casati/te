% Undo/redo history, in Prolog rather than as C arrays -- the actual
% byte-level buffer splice stays a native primitive (te_replace_range/3;
% te_set_cursor/1 already existed), so this file is the bookkeeping: what
% to remember, when to coalesce a run of typing into one undo step, when to
% evict the oldest entry past te_undo_depth/1, and what undo/redo actually
% do. te-specific (uses te_* natives), so it doesn't belong in the
% editor-agnostic scripts/bootstrap.pl -- consulted from scriptSetup instead.
%
% Each stack is a buffer-local variable (scripts/buffers.pl's blocal_get/2,
% blocal_set/2) holding a list of entry(Pos, Removed, Inserted, CurBefore,
% CurAfter) terms, newest first -- buffer-local rather than a single global
% fact so each buffer gets its own independent undo history (see
% default_blocal(undo_stack, [])/default_blocal(redo_stack, []) in
% buffers.pl for the empty-buffer starting state).

record_edit(Pos, Removed, Inserted, CurBefore, CurAfter) :-
    blocal_get(undo_stack, Stack),
    blocal_set(redo_stack, []), % a new edit invalidates redo
    ( coalesce(Stack, Pos, Removed, Inserted, CurAfter, NewStack)
    -> true
    ;  te_undo_depth(Depth), length(Stack, Len),
       ( Len >= Depth -> trim_last(Stack, Trimmed) ; Trimmed = Stack ),
       NewStack = [entry(Pos, Removed, Inserted, CurBefore, CurAfter) | Trimmed]
    ),
    blocal_set(undo_stack, NewStack).

% Coalesce a run of single-character typing (no deletion, exactly one
% inserted char, not a newline, immediately after the previous entry's
% insertion) into the existing top entry, growing its Inserted codes.
coalesce([entry(TopPos, [], TopIns, TopCurBefore, _)|Rest], Pos, [], [Ch], CurAfter, NewStack) :-
    Ch =\= 10, % 10 = newline: don't coalesce across it, matches the old C behavior
    length(TopIns, TopInsLen),
    Pos =:= TopPos + TopInsLen,
    append(TopIns, [Ch], NewIns),
    NewStack = [entry(TopPos, [], NewIns, TopCurBefore, CurAfter) | Rest].

trim_last([_], []) :- !.
trim_last([X|Xs], [X|Ys]) :- trim_last(Xs, Ys).

% Undo: put Removed back where Inserted currently is, restore the
% pre-edit cursor, and move this entry to the redo stack.
undo_step :-
    blocal_get(undo_stack, [entry(Pos, Removed, Inserted, CurBefore, CurAfter)|Rest]),
    length(Inserted, InsLen), End is Pos + InsLen,
    te_replace_range(Pos, End, Removed),
    te_set_cursor(CurBefore),
    blocal_set(undo_stack, Rest),
    blocal_get(redo_stack, RStack),
    blocal_set(redo_stack, [entry(Pos, Removed, Inserted, CurBefore, CurAfter) | RStack]).

% Redo: re-apply Inserted where Removed currently is, restore the
% post-edit cursor, and move this entry back to the undo stack.
redo_step :-
    blocal_get(redo_stack, [entry(Pos, Removed, Inserted, CurBefore, CurAfter)|Rest]),
    length(Removed, RemLen), End is Pos + RemLen,
    te_replace_range(Pos, End, Inserted),
    te_set_cursor(CurAfter),
    blocal_set(redo_stack, Rest),
    blocal_get(undo_stack, UStack),
    blocal_set(undo_stack, [entry(Pos, Removed, Inserted, CurBefore, CurAfter) | UStack]).

undo_stack_empty :- blocal_get(undo_stack, []).

clear_history :-
    blocal_set(undo_stack, []),
    blocal_set(redo_stack, []).
