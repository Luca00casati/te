% Cursor movement, in Prolog rather than as static C functions -- the raw
% UTF-8/line/column math stays native (te_step_left/2, te_step_right/2,
% te_word_start_left/2, te_word_start_right/2, te_word_end_right/2,
% te_line_start/2, te_line_end/2, te_cols_in/3, te_byte_at_col/4,
% te_vis_rows/3, te_view_cols/1, te_page_lines/1, te_buffer_len/1 -- all in
% src/script.c, wrapping src/main.c's lineStart/lineEnd/colsIn/byteAtCol/
% visRows/stepLeft/stepRight/wordStart*/wordEndRight, the same functions
% rendering/scrolling call directly every frame), since that's raw buffer
% work no different in kind from te_find_matches' PCRE2/memmem. This file
% is what to do with the result: which direction, how far, and the
% goal-column bookkeeping a run of up/down presses needs.
%
% goal_col_set/goal_col_val replace the old C statics of the same name: a
% "run" of up/down presses tries to keep the same on-screen column even
% through lines of different lengths; any other action clears it
% (scriptClearGoalColumn, called from main.c's applyAction).

goal_col_set(false).
goal_col_val(0).

clear_goal_col :-
    retract(goal_col_set(_)), assertz(goal_col_set(false)),
    retract(goal_col_val(_)), assertz(goal_col_val(0)).
set_goal_col(Col) :-
    retract(goal_col_set(_)), assertz(goal_col_set(true)),
    retract(goal_col_val(_)), assertz(goal_col_val(Col)).

move_left :- te_cursor(Pos), te_step_left(Pos, New), te_set_cursor(New).
move_right :- te_cursor(Pos), te_step_right(Pos, New), te_set_cursor(New).
move_word_start_left :- te_cursor(Pos), te_word_start_left(Pos, New), te_set_cursor(New).
move_word_start_right :- te_cursor(Pos), te_word_start_right(Pos, New), te_set_cursor(New).
move_word_end_right :- te_cursor(Pos), te_word_end_right(Pos, New), te_set_cursor(New).
move_home :- te_cursor(Pos), te_line_start(Pos, New), te_set_cursor(New).
move_end :- te_cursor(Pos), te_line_end(Pos, New), te_set_cursor(New).
move_buffer_start :- te_set_cursor(0).
move_buffer_end :- te_buffer_len(Len), te_set_cursor(Len).
select_all :- te_buffer_len(Len), te_set_selection(0, Len).

% Delta: -1 (up) or 1 (down). Wrap/Cols come from main.c's `wrap` config
% and current view_cols -- viewport state isn't tracked in Prolog itself.
move_vertical(Delta, Wrap, Cols) :-
    ( Wrap == true -> move_visual(Delta, Cols) ; move_vertical_plain(Delta) ).

% Non-wrap: step to the same on-screen column in the previous/next logical
% line, remembering that column (goal_col) across a run of repeated calls.
move_vertical_plain(Delta) :-
    te_cursor(Cursor),
    te_line_start(Cursor, Ls),
    ( goal_col_set(true) -> goal_col_val(Col) ; te_cols_in(Ls, Cursor, Col) ),
    set_goal_col(Col),
    ( Delta < 0
    -> ( Ls =:= 0
       -> true
       ;  PrevEnd is Ls - 1,
          te_line_start(PrevEnd, PrevStart),
          te_byte_at_col(PrevStart, PrevEnd, Col, New),
          te_set_cursor(New)
       )
    ;  te_line_end(Cursor, Le),
       te_buffer_len(Len),
       ( Le >= Len
       -> true
       ;  NextStart is Le + 1,
          te_line_end(NextStart, NextEnd),
          te_byte_at_col(NextStart, NextEnd, Col, New),
          te_set_cursor(New)
       )
    ).

% Wrap-aware: move one visual (wrapped) row, keeping the same on-screen
% column. Within a long logical line this steps between its segments; at a
% segment edge it crosses to the adjacent logical line's nearest row.
move_visual(Delta, ColsIn) :-
    ( ColsIn > 1 -> Cols = ColsIn ; Cols = 1 ),
    te_cursor(Cursor),
    te_line_start(Cursor, Ls),
    te_line_end(Cursor, Le),
    te_cols_in(Ls, Cursor, Col), % display column within this logical line
    Sub is Col // Cols,          % which visual row within this logical line
    ( goal_col_set(true) -> true ; Rem is Col mod Cols, set_goal_col(Rem) ),
    goal_col_val(GoalVal),
    ( GoalVal < Cols - 1 -> Vcol = GoalVal ; Vcol is Cols - 1 ), % on-screen column to preserve
    ( Delta < 0
    -> ( Sub > 0
       -> Target is (Sub - 1) * Cols + Vcol,
          te_byte_at_col(Ls, Le, Target, New), te_set_cursor(New)
       ;  ( Ls =:= 0
          -> true
          ;  PrevEnd is Ls - 1,
             te_line_start(PrevEnd, PrevStart),
             te_cols_in(PrevStart, PrevEnd, PrevCols),
             te_vis_rows(PrevCols, Cols, PrevRows),
             LastSub is PrevRows - 1,
             Target is LastSub * Cols + Vcol,
             te_byte_at_col(PrevStart, PrevEnd, Target, New), te_set_cursor(New)
          )
       )
    ;  te_cols_in(Ls, Le, Llen),
       te_vis_rows(Llen, Cols, Rows),
       LastSub is Rows - 1,
       ( Sub < LastSub
       -> Target is (Sub + 1) * Cols + Vcol,
          te_byte_at_col(Ls, Le, Target, New), te_set_cursor(New)
       ;  te_buffer_len(Len),
          ( Le >= Len
          -> true
          ;  NextStart is Le + 1,
             te_line_end(NextStart, NextEnd),
             te_byte_at_col(NextStart, NextEnd, Vcol, New), te_set_cursor(New)
          )
       )
    ).

page_up(Wrap, Cols, Lines) :- forall(between(1, Lines, _), move_vertical(-1, Wrap, Cols)).
page_down(Wrap, Cols, Lines) :- forall(between(1, Lines, _), move_vertical(1, Wrap, Cols)).
