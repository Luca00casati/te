% Fixture for the script-binding test in tests/unit_te.c. Checks use direct
% unification (te_text("") only succeeds if the buffer really is empty) plus
% throw/1 for a custom message, so a failed check surfaces as a
% "prolog error: ..." echo instead of the expected final buffer/echo state,
% which the C test checks for.
:- ( te_text("") -> true ; throw('expected empty buffer at start') ).

:- te_insert("hello").
:- ( te_text("hello") -> true ; throw('te_insert did not update the buffer') ).
:- ( te_cursor(5) -> true ; throw('cursor should follow the insert') ).

:- te_action(undo).
:- ( te_text("") -> true ; throw('te_action(undo) should revert the insert') ).

% Registering bindings should not error, for both a custom handler predicate
% and an existing action name -- top-level and leader chords alike.
bound_handler :- te_insert("bound!").
bound_leader_handler :- te_insert("bound-leader!").

key_binding(j, ctrl_shift, bound_handler).
key_binding(u, ctrl, te_action(undo)).
leader_binding(d, any, bound_leader_handler).
leader_binding(s, ctrl, te_action(save)).

:- te_echo("hello from init.pl").
