% Undo/redo history, in Prolog rather than as C arrays -- the actual
% byte-level buffer splice stays a native primitive (te_replace_range/3;
% te_set_cursor/1 already existed), so this file is the bookkeeping: what
% to remember, when to coalesce a run of typing into one undo step, when to
% evict the oldest entry past te_undo_depth/1, and what undo/redo actually
% do. te-specific (uses te_* natives), so it doesn't belong in the
% editor-agnostic src/bootstrap.pl -- consulted from scriptSetup instead.
%
% Each stack is the single argument of a fact (undo_stack([...])), a list
% of entry(Pos, Removed, Inserted, CurBefore, CurAfter) terms, newest first.
% Exactly one clause of undo_stack/1 (and redo_stack/1) exists at any time,
% maintained by always retracting the old one before asserting the new one
% -- so clearing history is just as simple a retract+assertz, no retractall
% needed.

undo_stack([]).
redo_stack([]).

record_edit(Pos, Removed, Inserted, CurBefore, CurAfter) :-
    retract(undo_stack(Stack)),
    retract(redo_stack(_)), assertz(redo_stack([])), % a new edit invalidates redo
    ( coalesce(Stack, Pos, Removed, Inserted, CurAfter, NewStack)
    -> true
    ;  te_undo_depth(Depth), length(Stack, Len),
       ( Len >= Depth -> trim_last(Stack, Trimmed) ; Trimmed = Stack ),
       NewStack = [entry(Pos, Removed, Inserted, CurBefore, CurAfter) | Trimmed]
    ),
    assertz(undo_stack(NewStack)).

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
    retract(undo_stack([entry(Pos, Removed, Inserted, CurBefore, CurAfter)|Rest])),
    length(Inserted, InsLen), End is Pos + InsLen,
    te_replace_range(Pos, End, Removed),
    te_set_cursor(CurBefore),
    assertz(undo_stack(Rest)),
    retract(redo_stack(RStack)),
    assertz(redo_stack([entry(Pos, Removed, Inserted, CurBefore, CurAfter) | RStack])).

% Redo: re-apply Inserted where Removed currently is, restore the
% post-edit cursor, and move this entry back to the undo stack.
redo_step :-
    retract(redo_stack([entry(Pos, Removed, Inserted, CurBefore, CurAfter)|Rest])),
    length(Removed, RemLen), End is Pos + RemLen,
    te_replace_range(Pos, End, Inserted),
    te_set_cursor(CurAfter),
    assertz(redo_stack(Rest)),
    retract(undo_stack(UStack)),
    assertz(undo_stack([entry(Pos, Removed, Inserted, CurBefore, CurAfter) | UStack])).

undo_stack_empty :- undo_stack([]).

clear_history :-
    retract(undo_stack(_)), assertz(undo_stack([])),
    retract(redo_stack(_)), assertz(redo_stack([])).
