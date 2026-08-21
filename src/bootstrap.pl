% The engine's own standard library, written in Prolog rather than C --
% consulted once at prologCreate (see BOOTSTRAP_PL_SRC / bootstrap_pl.h,
% generated from this file by nob.c). Most of these predicates don't need
% anything a native C function can do that an ordinary clause can't, so the
% engine dogfeeds its own unification/backtracking instead of duplicating
% that logic in C. between/3 is the one case where this isn't just a style
% choice: it needs real backtracking (nondeterministic generate-and-test)
% that a native predicate's single-bool-return ABI can't express.

between(Low, High, Low) :- Low =< High.
between(Low, High, X) :- Low < High, Low1 is Low + 1, between(Low1, High, X).

% succ/2: on top of var/1 and integer/1 (native type checks) plus throw/1
% for its ISO error cases, rather than a hand-written native.
succ(X, Y) :-
    var(X), !,
    ( var(Y) -> throw(error(instantiation_error, _))
    ; \+ integer(Y) -> throw(error(type_error(integer, Y), _))
    ; Y < 0 -> throw(error(domain_error(not_less_than_zero, Y), _))
    ; Y =:= 0 -> fail
    ; X is Y - 1
    ).
succ(X, Y) :-
    integer(X), !,
    ( X < 0 -> throw(error(domain_error(not_less_than_zero, X), _))
    ; Y is X + 1
    ).
succ(X, _) :- throw(error(type_error(integer, X), _)).

% --- lists -----------------------------------------------------------

member(X, [X|_]).
member(X, [_|T]) :- member(X, T).
memberchk(X, L) :- member(X, L), !.

append([], L, L).
append([H|T], L, [H|R]) :- append(T, L, R).

reverse(L, R) :- reverse_(L, [], R).
reverse_([], Acc, Acc).
reverse_([H|T], Acc, R) :- reverse_(T, [H|Acc], R).

last([X], X) :- !.
last([_|T], X) :- last(T, X).

% nth0/nth1 support both the common "index given" mode and, when the index
% is unbound, generating (Index, Elem) pairs by backtracking through List.
nth0(N, List, Elem) :- integer(N), !, N >= 0, nth0_det(N, List, Elem).
nth0(N, List, Elem) :- var(N), !, nth0_gen(List, Elem, 0, N).
nth0_det(0, [X|_], X) :- !.
nth0_det(N, [_|T], X) :- N > 0, N1 is N - 1, nth0_det(N1, T, X).
nth0_gen([X|_], X, I, I).
nth0_gen([_|T], X, I0, I) :- I1 is I0 + 1, nth0_gen(T, X, I1, I).

nth1(N, List, Elem) :- integer(N), !, N >= 1, N0 is N - 1, nth0_det(N0, List, Elem).
nth1(N, List, Elem) :- var(N), !, nth0_gen(List, Elem, 1, N).

sum_list(L, S) :- sum_list_(L, 0, S).
sum_list_([], S, S).
sum_list_([H|T], Acc, S) :- Acc1 is Acc + H, sum_list_(T, Acc1, S).

max_list([X], X) :- !.
max_list([H|T], M) :- max_list(T, M0), ( H > M0 -> M = H ; M = M0 ).
min_list([X], X) :- !.
min_list([H|T], M) :- min_list(T, M0), ( H < M0 -> M = H ; M = M0 ).

numlist(Low, High, []) :- Low > High, !.
numlist(Low, High, [Low|Rest]) :- Low =< High, Low1 is Low + 1, numlist(Low1, High, Rest).

delete([], _, []).
delete([X|Xs], Y, Result) :- \+ X \= Y, !, delete(Xs, Y, Result).
delete([X|Xs], Y, [X|Result]) :- delete(Xs, Y, Result).

subtract([], _, []).
subtract([X|Xs], L, Result) :- memberchk(X, L), !, subtract(Xs, L, Result).
subtract([X|Xs], L, [X|Result]) :- subtract(Xs, L, Result).

intersection([], _, []).
intersection([X|Xs], L, [X|Result]) :- memberchk(X, L), !, intersection(Xs, L, Result).
intersection([_|Xs], L, Result) :- intersection(Xs, L, Result).

union([], L, L).
union([X|Xs], L, Result) :- memberchk(X, L), !, union(Xs, L, Result).
union([X|Xs], L, [X|Result]) :- union(Xs, L, Result).

% --- control / higher-order --------------------------------------------

maplist(_, []).
maplist(G, [X|Xs]) :- call(G, X), maplist(G, Xs).
maplist(_, [], []).
maplist(G, [X|Xs], [Y|Ys]) :- call(G, X, Y), maplist(G, Xs, Ys).
maplist(_, [], [], []).
maplist(G, [X|Xs], [Y|Ys], [Z|Zs]) :- call(G, X, Y, Z), maplist(G, Xs, Ys, Zs).

forall(Cond, Action) :- \+ (Cond, \+ Action).

include(_, [], []).
include(G, [X|Xs], Result) :-
    ( call(G, X) -> Result = [X|Rest] ; Result = Rest ),
    include(G, Xs, Rest).

exclude(_, [], []).
exclude(G, [X|Xs], Result) :-
    ( call(G, X) -> Result = Rest ; Result = [X|Rest] ),
    exclude(G, Xs, Rest).

foldl(_, [], Acc, Acc).
foldl(G, [X|Xs], Acc0, Acc) :- call(G, X, Acc0, Acc1), foldl(G, Xs, Acc1, Acc).
